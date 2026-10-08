#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sample.h"
#include "util.h"

static char big[65536]; /* /proc/stat and /proc/meminfo; cpu lines come first if truncated */

static void read_ident(struct sampler *sp)
{
	char *nl;

	if (gethostname(sp->host, sizeof sp->host - 1) != 0)
		strcpy(sp->host, "?");
	if (read_path("/sys/class/dmi/id/product_name", sp->product, sizeof sp->product) <= 0)
		sp->product[0] = '\0';
	if ((nl = strchr(sp->product, '\n')))
		*nl = '\0';
}

int sampler_init(struct sampler *sp, int want_gpu)
{
	memset(sp, 0, sizeof *sp);
	sp->fd_stat = open_ro("/proc/stat");
	sp->fd_meminfo = open_ro("/proc/meminfo");
	if (sp->fd_stat < 0 || sp->fd_meminfo < 0)
		return -1;
	sp->fd_loadavg = open_ro("/proc/loadavg");
	sp->fd_psi_cpu = open_ro("/proc/pressure/cpu");
	sp->fd_psi_mem = open_ro("/proc/pressure/memory");
	sp->fd_uptime = open_ro("/proc/uptime");
	sp->fd_statm = open_ro("/proc/self/statm");
	sp->fd_schedstat = open_ro("/proc/self/schedstat");
	read_ident(sp);
	topo_init(&sp->topo);
	thermal_init(&sp->th);
	sp->gpu_on = want_gpu && gpu_init(&sp->gpu) == 0;
	return 0;
}

void sampler_close(struct sampler *sp)
{
	if (sp->gpu_on)
		gpu_shutdown(&sp->gpu);
	sp->gpu_on = 0;
}

static double read_psi(int fd)
{
	char buf[256];
	return read_fd(fd, buf, sizeof buf) > 0 ? parse_psi_avg10(buf) : -1;
}

void sampler_read(struct sampler *sp, struct sample *s)
{
	char buf[256];
	struct rusage ru;
	uint64_t pages;
	const char *p;

	s->t = mono_now();
	if (read_fd(sp->fd_stat, big, sizeof big) > 0)
		parse_stat(big, s);
	if (read_fd(sp->fd_meminfo, big, sizeof big) > 0)
		parse_meminfo(big, &s->mem);
	if (read_fd(sp->fd_loadavg, buf, sizeof buf) <= 0 ||
	    parse_loadavg(buf, s->load, &s->running, &s->tasks) != 0)
		s->load[0] = s->load[1] = s->load[2] = -1;
	s->psi_cpu = read_psi(sp->fd_psi_cpu);
	s->psi_mem = read_psi(sp->fd_psi_mem);
	if (read_fd(sp->fd_uptime, buf, sizeof buf) <= 0 || !parse_dec(buf, &s->uptime))
		s->uptime = -1;
	topo_read(&sp->topo, s);
	thermal_read(&sp->th, s);
	if (sp->gpu_on)
		gpu_read(&sp->gpu, &s->gpu, s->t);
	else
		s->gpu.have = 0;

	/* schedstat is ns-exact; rusage is sampled at HZ and overstates sub-ms work */
	if (read_fd(sp->fd_schedstat, buf, sizeof buf) > 0 && parse_u64(buf, &pages)) {
		s->self_cpu_s = (double)pages * 1e-9;
	} else {
		getrusage(RUSAGE_SELF, &ru);
		s->self_cpu_s = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6 +
				(double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec * 1e-6;
	}
	s->self_rss_kb = 0;
	if (read_fd(sp->fd_statm, buf, sizeof buf) > 0 && (p = parse_u64(buf, &pages)) &&
	    parse_u64(p, &pages))
		s->self_rss_kb = pages * (uint64_t)sysconf(_SC_PAGESIZE) / 1024;
}

static double pct(uint64_t part, uint64_t whole)
{
	return whole ? 100.0 * (double)part / (double)whole : 0;
}

static uint64_t sub(uint64_t a, uint64_t b)
{
	return a > b ? a - b : 0;
}

/* busy = user+nice+system+irq+softirq+steal, idle = idle+iowait (htop semantics) */
static void cpu_delta(const struct cpu_times *a, const struct cpu_times *b, double *user,
		      double *sys, double *iowait)
{
	uint64_t u = sub(b->user + b->nice, a->user + a->nice);
	uint64_t s = sub(b->system + b->irq + b->softirq + b->steal,
			 a->system + a->irq + a->softirq + a->steal);
	uint64_t w = sub(b->iowait, a->iowait);
	uint64_t total = u + s + w + sub(b->idle, a->idle);

	*user = pct(u, total);
	*sys = pct(s, total);
	if (iowait)
		*iowait = pct(w, total);
}

/*
 * GPU allocations on unified memory are invisible to every meminfo counter except MemFree,
 * so the part of "used" not explained by known consumers is attributed to the GPU/driver.
 */
void mem_split(const struct meminfo *m, struct view *v)
{
	uint64_t cache = m->buffers + sub(m->cached, m->shmem) + m->sreclaimable;
	uint64_t used = sub(sub(m->total, m->free), cache);
	uint64_t kernel = m->sunreclaim + m->kstack + m->pagetables + m->secpagetables +
			  m->percpu + m->vmalloc_used;
	uint64_t apps = m->anon + m->shmem;

	v->m_total = m->total;
	v->m_free = m->free;
	v->m_avail = m->avail;
	v->m_cache = cache;
	v->m_used = used;
	v->m_apps = apps < used ? apps : used;
	v->m_kernel = kernel < used - v->m_apps ? kernel : used - v->m_apps;
	v->m_gpu = used - v->m_apps - v->m_kernel;
}

static struct pid_entry *pid_lookup(struct sampler *sp, uint32_t pid, double now)
{
	struct pid_entry *slot = &sp->pids[0];
	char path[64], buf[256];
	struct stat st;
	ssize_t n;

	for (int i = 0; i < MAX_GPU_PROCS; i++) {
		if (sp->pids[i].pid == pid) {
			sp->pids[i].seen = now;
			return &sp->pids[i];
		}
		if (sp->pids[i].seen < slot->seen)
			slot = &sp->pids[i];
	}
	memset(slot, 0, sizeof *slot);
	slot->pid = pid;
	slot->seen = now;
	strcpy(slot->name, "?");
	strcpy(slot->user, "?");

	/* argv[0] keeps names set via setproctitle (comm is cut at 15 chars) */
	snprintf(path, sizeof path, "/proc/%u/cmdline", pid);
	n = read_path(path, buf, sizeof buf);
	if (n <= 0) {
		snprintf(path, sizeof path, "/proc/%u/comm", pid);
		n = read_path(path, buf, sizeof buf);
	}
	if (n > 0) {
		char *base, *end = strpbrk(buf, "\n ");
		if (end)
			*end = '\0';
		base = strrchr(buf, '/');
		snprintf(slot->name, sizeof slot->name, "%.31s", base && base[1] ? base + 1 : buf);
	}
	snprintf(path, sizeof path, "/proc/%u", pid);
	if (stat(path, &st) == 0) {
		struct passwd *pw = getpwuid(st.st_uid);
		slot->uid = st.st_uid;
		if (pw)
			snprintf(slot->user, sizeof slot->user, "%.15s", pw->pw_name);
		else
			snprintf(slot->user, sizeof slot->user, "%u", st.st_uid);
	}
	return slot;
}

void compute_view(struct sampler *sp, const struct sample *prev, const struct sample *cur,
		  struct view *v)
{
	double wall = cur->t - prev->t;
	int best = TEMP_NONE;

	v->s = cur;
	v->sp = sp;
	for (int c = 0; c < cur->ncpu; c++)
		cpu_delta(&prev->cpu[c], &cur->cpu[c], &v->core_user[c], &v->core_sys[c], NULL);
	cpu_delta(&prev->total, &cur->total, &v->tot_user, &v->tot_sys, &v->tot_iowait);
	mem_split(&cur->mem, v);

	for (int i = 0; i < cur->nzones; i++)
		if (cur->zone_mc[i] > best)
			best = cur->zone_mc[i];
	v->cpu_temp_mc = best;
	v->self_cpu_pct = wall > 0 ? 100.0 * (cur->self_cpu_s - prev->self_cpu_s) / wall : 0;

	v->nproc = 0;
	v->gpu_proc_kib = 0;
	for (int i = 0; i < cur->gpu.nproc; i++) {
		const struct gpu_proc *gp = &cur->gpu.procs[i];
		struct pid_entry *e = pid_lookup(sp, gp->pid, cur->t);
		struct view_proc vp = {gp->pid, gp->mem_bytes, e->name, e->user};
		int j = v->nproc++;

		v->gpu_proc_kib += gp->mem_bytes / 1024;
		for (; j > 0 && v->procs[j - 1].mem_bytes < vp.mem_bytes; j--)
			v->procs[j] = v->procs[j - 1];
		v->procs[j] = vp;
	}
}

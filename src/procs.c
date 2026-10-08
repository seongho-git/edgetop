#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sample.h"
#include "util.h"

static int cmp_pid(const void *a, const void *b)
{
	uint32_t x = ((const struct proc_entry *)a)->pid, y = ((const struct proc_entry *)b)->pid;
	return x < y ? -1 : x > y;
}

void procs_init(struct proc_table *t)
{
	/* no memset: the lists are large and must not be touched before the first scan */
	t->a.n = t->b.n = 0;
	t->a.t = t->b.t = 0;
	t->nscans = 0;
	memset(t->users, 0, sizeof t->users);
	t->cur = &t->a;
	t->prev = &t->b;
	t->clk_tck = sysconf(_SC_CLK_TCK);
	t->page_kib = sysconf(_SC_PAGESIZE) / 1024;
	if (t->clk_tck <= 0)
		t->clk_tck = 100;
}

static const char *user_name(struct proc_table *t, uint32_t uid)
{
	struct uid_name *slot = &t->users[0];
	struct passwd *pw;

	for (int i = 0; i < MAX_USERS; i++) {
		if (t->users[i].used && t->users[i].uid == uid)
			return t->users[i].name;
		if (!t->users[i].used) {
			slot = &t->users[i];
			break;
		}
	}
	slot->used = 1;
	slot->uid = uid;
	pw = getpwuid(uid);
	if (pw)
		snprintf(slot->name, sizeof slot->name, "%.15s", pw->pw_name);
	else
		snprintf(slot->name, sizeof slot->name, "%u", uid);
	return slot->name;
}

/* Fills name and uid for a pid seen for the first time. */
static void identify(struct proc_table *t, struct proc_entry *e, const char *comm)
{
	char path[48], buf[256];
	struct stat st;
	ssize_t n;

	snprintf(path, sizeof path, "/proc/%u/cmdline", e->pid);
	n = read_path(path, buf, sizeof buf);
	e->cmd[0] = '\0';
	if (n > 0 && buf[0]) {
		/* argv is NUL-separated; keep a space-joined copy for the COMMAND column */
		size_t k = 0;
		for (ssize_t i = 0; i < n && k + 1 < sizeof e->cmd; i++)
			e->cmd[k++] = buf[i] ? buf[i] : ' ';
		while (k && e->cmd[k - 1] == ' ')
			k--;
		e->cmd[k] = '\0';
	}
	if (n > 0 && buf[0]) {
		/* argv[0] keeps names set via setproctitle; comm is cut at 15 chars */
		char *base, *end = strpbrk(buf, "\n ");
		if (end)
			*end = '\0';
		base = strrchr(buf, '/');
		if (end && end > buf && end[-1] == ':') /* "avahi-daemon: running" style titles */
			end[-1] = '\0';
		snprintf(e->name, sizeof e->name, "%.*s", (int)sizeof e->name - 1,
			 base && base[1] ? base + 1 : buf);
	} else {
		snprintf(e->name, sizeof e->name, "%.*s", (int)sizeof e->name - 1, comm);
	}
	snprintf(path, sizeof path, "/proc/%u", e->pid);
	e->uid = stat(path, &st) == 0 ? st.st_uid : 0;
	e->user = user_name(t, e->uid);
}

/*
 * /proc/PID/stat: "pid (comm) state ppid ... utime(14) stime(15) ... rss(24)".
 * comm may contain spaces and parentheses, so fields are counted from the last ')'.
 */
static int parse_pid_stat(const char *buf, struct proc_entry *e, char *comm, size_t comm_n)
{
	const char *open = strchr(buf, '('), *close = strrchr(buf, ')'), *p;
	int64_t v, utime = 0, stime = 0, rss = 0; /* tpgid is -1 without a terminal, nice may be negative */
	size_t len;

	if (!open || !close || close < open)
		return -1;
	len = (size_t)(close - open - 1);
	if (len >= comm_n)
		len = comm_n - 1;
	memcpy(comm, open + 1, len);
	comm[len] = '\0';
	p = close + 2;
	e->state = *p;
	for (int field = 3; field <= 24 && p; field++) {
		if (field == 3) {
			p = strchr(p, ' ');
			continue;
		}
		p = parse_i64(p, &v);
		if (!p)
			return -1;
		if (field == 14)
			utime = v;
		else if (field == 15)
			stime = v;
		else if (field == 24)
			rss = v;
	}
	e->ticks = (uint64_t)(utime + stime);
	e->rss_pages = rss > 0 ? (uint64_t)rss : 0;
	return 0;
}

int procs_scan(struct proc_table *t, double now)
{
	struct proc_list *cur = t->prev, *prev = t->cur; /* swap: fill the older buffer */
	DIR *d = opendir("/proc");
	struct dirent *de;
	char path[48], buf[1024], comm[32];
	double window;
	int i, j;

	if (!d)
		return -1;
	cur->n = 0;
	while ((de = readdir(d)) && cur->n < PROC_TABLE) {
		struct proc_entry *e = &cur->e[cur->n];
		char *end;
		unsigned long pid;
		int fd;
		ssize_t r;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		pid = strtoul(de->d_name, &end, 10);
		if (*end || pid == 0 || pid > 0xffffffffUL)
			continue;
		snprintf(path, sizeof path, "/proc/%lu/stat", pid);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		r = read_fd(fd, buf, sizeof buf);
		close(fd);
		if (r <= 0)
			continue;
		memset(e, 0, sizeof *e);
		e->pid = (uint32_t)pid;
		if (parse_pid_stat(buf, e, comm, sizeof comm) != 0)
			continue;
		snprintf(e->comm, sizeof e->comm, "%.15s", comm);
		cur->n++;
	}
	closedir(d);
	qsort(cur->e, (size_t)cur->n, sizeof cur->e[0], cmp_pid);

	/* merge with the previous scan by pid for cpu deltas and cached identity */
	window = now - prev->t;
	for (i = 0, j = 0; i < cur->n; i++) {
		struct proc_entry *e = &cur->e[i];

		while (j < prev->n && prev->e[j].pid < e->pid)
			j++;
		if (j < prev->n && prev->e[j].pid == e->pid && prev->n) {
			const struct proc_entry *o = &prev->e[j];
			memcpy(e->name, o->name, sizeof e->name);
			memcpy(e->cmd, o->cmd, sizeof e->cmd);
			e->uid = o->uid;
			e->user = o->user;
			e->cpu_pct = window > 0 && e->ticks >= o->ticks
					     ? 100.0 * (double)(e->ticks - o->ticks) / (double)t->clk_tck / window
					     : 0;
		} else {
			identify(t, e, e->comm);
			e->cpu_pct = 0;
		}
	}
	cur->t = now;
	t->cur = cur;
	t->prev = prev;
	t->nscans++;
	return cur->n;
}

static const struct gpu_proc *gpu_of(const struct gpu_sample *g, uint32_t pid)
{
	for (int i = 0; i < g->nproc; i++)
		if (g->procs[i].pid == pid)
			return &g->procs[i];
	return NULL;
}

static int better(const struct view_proc *a, const struct view_proc *b, int key)
{
	switch (key) {
	case SORT_GPU:
		if (a->gpu_pct != b->gpu_pct)
			return a->gpu_pct > b->gpu_pct;
		if (a->gpu_bytes != b->gpu_bytes)
			return a->gpu_bytes > b->gpu_bytes;
		break;
	case SORT_RSS:
		if (a->rss_kib != b->rss_kib)
			return a->rss_kib > b->rss_kib;
		break;
	default:
		break;
	}
	if (a->cpu_pct != b->cpu_pct)
		return a->cpu_pct > b->cpu_pct;
	if (a->rss_kib != b->rss_kib)
		return a->rss_kib > b->rss_kib;
	return a->pid < b->pid;
}

int procs_top(const struct proc_table *t, const struct gpu_sample *g, int key, struct view_proc *out,
	      int max)
{
	const struct proc_list *l = t->cur;
	int n = 0;

	for (int i = 0; i < l->n; i++) {
		const struct proc_entry *e = &l->e[i];
		const struct gpu_proc *gp = gpu_of(g, e->pid);
		struct view_proc vp = {
			.pid = e->pid, .user = e->user, .name = e->name, .cmd = e->cmd, .cpu_pct = e->cpu_pct,
			.gpu_pct = gp ? gp->sm_pct : 0, .rss_kib = e->rss_pages * (uint64_t)t->page_kib,
			.gpu_bytes = gp ? gp->mem_bytes : 0, .state = e->state,
		};
		int j;

		if (key == SORT_GPU && !gp)
			continue;
		if (n < max)
			j = n++;
		else if (better(&vp, &out[n - 1], key))
			j = n - 1;
		else
			continue;
		for (; j > 0 && better(&vp, &out[j - 1], key); j--)
			out[j] = out[j - 1];
		out[j] = vp;
	}
	return n;
}

#include <stddef.h>
#include <string.h>

#include "sample.h"
#include "util.h"

int parse_stat(const char *buf, struct sample *s)
{
	const char *p = buf;
	int seen_total = 0;

	s->ncpu = 0;
	memset(s->online, 0, sizeof s->online);
	for (; p && *p; p = next_line(p)) {
		struct cpu_times *t;
		const char *q;
		uint64_t v[8] = {0};

		if (strncmp(p, "cpu", 3) != 0) {
			if (seen_total)
				break; /* cpu lines come first */
			continue;
		}
		q = p + 3;
		if (*q == ' ') {
			t = &s->total;
			seen_total = 1;
		} else {
			uint64_t id;

			q = parse_u64(q, &id);
			if (!q || id >= MAX_CPUS)
				continue;
			t = &s->cpu[id];
			s->online[id] = 1;
			if ((int)id + 1 > s->ncpu)
				s->ncpu = (int)id + 1;
		}
		for (int i = 0; i < 8; i++) {
			const char *r = parse_u64(q, &v[i]);
			if (!r)
				break;
			q = r;
		}
		/* guest time is already included in user/nice */
		t->user = v[0];
		t->nice = v[1];
		t->system = v[2];
		t->idle = v[3];
		t->iowait = v[4];
		t->irq = v[5];
		t->softirq = v[6];
		t->steal = v[7];
	}
	return seen_total ? 0 : -1;
}

static const struct {
	const char *key;
	size_t off;
} mem_keys[] = {
	{"MemTotal", offsetof(struct meminfo, total)},
	{"MemFree", offsetof(struct meminfo, free)},
	{"MemAvailable", offsetof(struct meminfo, avail)},
	{"Buffers", offsetof(struct meminfo, buffers)},
	{"Cached", offsetof(struct meminfo, cached)},
	{"SReclaimable", offsetof(struct meminfo, sreclaimable)},
	{"SUnreclaim", offsetof(struct meminfo, sunreclaim)},
	{"AnonPages", offsetof(struct meminfo, anon)},
	{"Shmem", offsetof(struct meminfo, shmem)},
	{"KernelStack", offsetof(struct meminfo, kstack)},
	{"PageTables", offsetof(struct meminfo, pagetables)},
	{"SecPageTables", offsetof(struct meminfo, secpagetables)},
	{"Percpu", offsetof(struct meminfo, percpu)},
	{"VmallocUsed", offsetof(struct meminfo, vmalloc_used)},
	{"SwapTotal", offsetof(struct meminfo, swap_total)},
	{"SwapFree", offsetof(struct meminfo, swap_free)},
};

int parse_meminfo(const char *buf, struct meminfo *m)
{
	const size_t nkeys = sizeof mem_keys / sizeof mem_keys[0];

	memset(m, 0, sizeof *m);
	for (const char *p = buf; p && *p; p = next_line(p)) {
		const char *colon = strchr(p, ':');
		size_t len;

		if (!colon)
			break;
		len = (size_t)(colon - p);
		for (size_t i = 0; i < nkeys; i++) {
			if (strlen(mem_keys[i].key) == len && memcmp(p, mem_keys[i].key, len) == 0) {
				parse_u64(colon + 1, (uint64_t *)(void *)((char *)m + mem_keys[i].off));
				break;
			}
		}
	}
	return m->total ? 0 : -1;
}

int parse_loadavg(const char *buf, double load[3], unsigned *running, unsigned *tasks)
{
	const char *p = buf;
	uint64_t r, t;

	for (int i = 0; i < 3; i++) {
		p = parse_dec(p, &load[i]);
		if (!p)
			return -1;
	}
	p = parse_u64(p, &r);
	if (!p || *p != '/' || !parse_u64(p + 1, &t))
		return -1;
	*running = (unsigned)r;
	*tasks = (unsigned)t;
	return 0;
}

double parse_psi_avg10(const char *buf)
{
	double v;

	if (strncmp(buf, "some avg10=", 11) != 0 || !parse_dec(buf + 11, &v))
		return -1;
	return v;
}

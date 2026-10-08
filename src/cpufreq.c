#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sample.h"
#include "util.h"

#define CPU_SYS "/sys/devices/system/cpu/cpu%d/"
#define FALLBACK_PERIOD 5.0

static const struct {
	unsigned impl, part;
	const char *name;
} core_names[] = {
	{0x41, 0xd03, "A53"},  {0x41, 0xd05, "A55"},  {0x41, 0xd08, "A72"},  {0x41, 0xd0b, "A76"},
	{0x41, 0xd0c, "N1"},   {0x41, 0xd40, "V1"},   {0x41, 0xd41, "A78"},  {0x41, 0xd44, "X1"},
	{0x41, 0xd46, "A510"}, {0x41, 0xd47, "A710"}, {0x41, 0xd48, "X2"},   {0x41, 0xd49, "N2"},
	{0x41, 0xd4d, "A715"}, {0x41, 0xd4e, "X3"},   {0x41, 0xd4f, "V2"},   {0x41, 0xd80, "A520"},
	{0x41, 0xd81, "A720"}, {0x41, 0xd82, "X4"},   {0x41, 0xd84, "V3"},   {0x41, 0xd85, "X925"},
	{0x41, 0xd87, "A725"}, {0x41, 0xd8e, "N3"},
};

const char *core_name(uint64_t midr, char *buf, unsigned n)
{
	unsigned impl = (unsigned)(midr >> 24) & 0xff;
	unsigned part = (unsigned)(midr >> 4) & 0xfff;

	for (size_t i = 0; i < sizeof core_names / sizeof core_names[0]; i++)
		if (core_names[i].impl == impl && core_names[i].part == part)
			return core_names[i].name;
	snprintf(buf, n, "%02x:%03x", impl, part);
	return buf;
}

uint32_t parse_freq(const char *buf, long r)
{
	uint64_t khz;

	/* cpuinfo_avg_freq returns EAGAIN when the core has been idle */
	if (r <= 0 || !parse_u64(buf, &khz))
		return 0;
	return (uint32_t)khz;
}

static uint64_t read_hex(const char *path)
{
	char buf[64];
	uint64_t v = 0;

	if (read_path(path, buf, sizeof buf) <= 0)
		return 0;
	for (const char *p = buf[0] == '0' && buf[1] == 'x' ? buf + 2 : buf;; p++) {
		unsigned d;
		if (*p >= '0' && *p <= '9')
			d = (unsigned)(*p - '0');
		else if (*p >= 'a' && *p <= 'f')
			d = (unsigned)(*p - 'a' + 10);
		else
			break;
		v = v << 4 | d;
	}
	return v;
}

static uint32_t read_u32(const char *path)
{
	char buf[32];
	uint64_t v = 0;

	if (read_path(path, buf, sizeof buf) > 0)
		parse_u64(buf, &v);
	return (uint32_t)v;
}

void topo_init(struct topo *t)
{
	long n = sysconf(_SC_NPROCESSORS_CONF);
	char path[128], nbuf[16];
	uint64_t key[MAX_GROUPS] = {0};

	memset(t, 0, sizeof *t);
	t->ncpu = n < 1 ? 1 : n > MAX_CPUS ? MAX_CPUS : (int)n;
	snprintf(path, sizeof path, CPU_SYS "cpufreq/cpuinfo_avg_freq", 0);
	t->fallback = access(path, F_OK) != 0;

	for (int c = 0; c < t->ncpu; c++) {
		uint64_t midr, k;
		int g;

		snprintf(path, sizeof path, CPU_SYS "cpufreq/cpuinfo_max_freq", c);
		t->max_khz[c] = read_u32(path);
		snprintf(path, sizeof path, CPU_SYS "cpufreq/%s", c,
			 t->fallback ? "scaling_cur_freq" : "cpuinfo_avg_freq");
		t->freq_fd[c] = open_ro(path);
		snprintf(path, sizeof path, CPU_SYS "regs/identification/midr_el1", c);
		midr = read_hex(path);
		/* group by core type; without MIDR every core lands in one group */
		k = midr & 0xff00fff0;
		for (g = 0; g < t->ngroups && key[g] != k; g++)
			;
		if (g == t->ngroups) {
			if (g == MAX_GROUPS)
				g--;
			else {
				t->ngroups++;
				key[g] = k;
				snprintf(t->groups[g].name, sizeof t->groups[g].name, "%s",
					 midr ? core_name(midr, nbuf, sizeof nbuf) : "CPU");
			}
		}
		t->groups[g].cpus[t->groups[g].n++] = c;
		if (t->max_khz[c] > t->groups[g].max_khz)
			t->groups[g].max_khz = t->max_khz[c];
	}
	/* fastest cluster first */
	for (int i = 1; i < t->ngroups; i++)
		for (int j = i; j > 0 && t->groups[j].max_khz > t->groups[j - 1].max_khz; j--) {
			struct core_group tmp = t->groups[j];
			t->groups[j] = t->groups[j - 1];
			t->groups[j - 1] = tmp;
		}
}

void topo_read(struct topo *t, struct sample *s)
{
	char buf[32];

	if (!t->fallback || s->t - t->last_slow >= FALLBACK_PERIOD) {
		for (int c = 0; c < t->ncpu; c++)
			t->last_khz[c] = parse_freq(buf, (long)read_fd(t->freq_fd[c], buf, sizeof buf));
		t->last_slow = s->t;
	}
	memcpy(s->freq_khz, t->last_khz, sizeof s->freq_khz);
}

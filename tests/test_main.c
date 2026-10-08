#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "render.h"
#include "sample.h"
#include "util.h"

static int failures, checks;

#define CHECK(cond)                                                              \
	do {                                                                     \
		checks++;                                                        \
		if (!(cond)) {                                                   \
			failures++;                                              \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

static char buf[1 << 16];
static struct sample s;

static const char *load(const char *path)
{
	if (read_path(path, buf, sizeof buf) <= 0) {
		fprintf(stderr, "  missing fixture %s\n", path);
		buf[0] = '\0';
	}
	return buf;
}

static void test_stat(void)
{
	CHECK(parse_stat(load("fixtures/stat_dgx_spark"), &s) == 0);
	CHECK(s.ncpu == 20);
	CHECK(s.total.user == 137123735ull && s.total.idle == 10741180071ull);
	CHECK(s.total.softirq == 3721622ull && s.total.steal == 0);
	CHECK(s.cpu[19].user == 59768663ull && s.cpu[19].iowait == 46126ull);
	CHECK(s.online[0] && s.online[19] && !s.online[20]);
}

static void test_stat_256(void)
{
	CHECK(parse_stat(load("fixtures/stat_256cpu"), &s) == 0);
	CHECK(s.ncpu == 256);
	CHECK(s.cpu[255].user == 765 && s.cpu[255].system == 255 && s.cpu[255].idle == 1255);
	CHECK(parse_stat("intr 1 2\n", &s) == -1);
}

static void test_meminfo(void)
{
	struct meminfo m;
	struct view v;

	CHECK(parse_meminfo(load("fixtures/meminfo_dgx_spark"), &m) == 0);
	CHECK(m.total == 127606860 && m.free == 8639536 && m.shmem == 469056);
	CHECK(m.secpagetables == 215564 && m.vmalloc_used == 704724 && m.swap_total == 0);

	mem_split(&m, &v);
	/* shmem lives in Cached but is not reclaimable, so it belongs to apps, not cache */
	CHECK(v.m_cache == 277664 + (5175188 - 469056) + 475760);
	CHECK(v.m_apps == 7419636 + 469056);
	CHECK(v.m_kernel == 1019404 + 25616 + 67740 + 215564 + 33360 + 704724);
	CHECK(v.m_used == v.m_total - v.m_free - v.m_cache);
	CHECK(v.m_apps + v.m_gpu + v.m_kernel == v.m_used);
	CHECK(v.m_gpu > 90ull * 1024 * 1024); /* ~98.8 GiB held by the sglang server at capture */
}

static void test_meminfo_no_gpu_underflow(void)
{
	struct meminfo m = {.total = 1000, .free = 900, .cached = 50, .anon = 80, .sunreclaim = 40};
	struct view v;

	mem_split(&m, &v);
	CHECK(v.m_used == 50);
	CHECK(v.m_apps == 50 && v.m_kernel == 0 && v.m_gpu == 0);
}

static void test_loadavg_psi(void)
{
	double l[3];
	unsigned r, t;

	CHECK(parse_loadavg("1.08 1.10 1.03 2/1583 332875\n", l, &r, &t) == 0);
	CHECK(l[0] > 1.079 && l[0] < 1.081 && l[2] > 1.029 && l[2] < 1.031);
	CHECK(r == 2 && t == 1583);
	CHECK(parse_loadavg("garbage", l, &r, &t) == -1);
	CHECK(parse_psi_avg10("some avg10=12.34 avg60=0.00 avg300=0.00 total=1\n") > 12.33);
	CHECK(parse_psi_avg10(load("fixtures/pressure_cpu")) == 0.0);
	CHECK(parse_psi_avg10("full avg10=1.00") < 0);
}

static void test_freq(void)
{
	CHECK(parse_freq("3894515\n", 8) == 3894515);
	CHECK(parse_freq("", -1) == 0); /* EAGAIN from an idle core */
	CHECK(parse_freq("x", 1) == 0);
}

static void test_core_name(void)
{
	char b[16];

	CHECK(strcmp(core_name(0x410fd851, b, sizeof b), "X925") == 0);
	CHECK(strcmp(core_name(0x410fd871, b, sizeof b), "A725") == 0);
	CHECK(strcmp(core_name(0x4e0f0040, b, sizeof b), "4e:004") == 0);
}

static void test_parsers(void)
{
	uint64_t u;
	int64_t i;
	double d;

	CHECK(parse_u64("  42x", &u) && u == 42);
	CHECK(!parse_u64("x", &u));
	CHECK(parse_i64("-17", &i) && i == -17);
	CHECK(parse_dec("0.05", &d) && d > 0.049 && d < 0.051);
	CHECK(parse_dec("-1.5", &d) && d < -1.49 && d > -1.51);
}

static void test_fmt(void)
{
	char b[16];

	fmt_kib(b, sizeof b, 512);
	CHECK(strcmp(b, "512K") == 0);
	fmt_kib(b, sizeof b, 170 * 1024);
	CHECK(strcmp(b, "170M") == 0);
	fmt_kib(b, sizeof b, 103079215ull);
	CHECK(strcmp(b, "98.3G") == 0);
}

static void test_render_fits(void)
{
	static struct frame f, prev;
	static struct sampler sp;
	static struct sample a, b;
	static struct view v;
	static char out[1 << 20];
	struct ui ui = {.show_procs = 1, .color = 1, .interval = 1};

	parse_stat(load("fixtures/stat_dgx_spark"), &a);
	b = a;
	b.cpu[3].user += 50;
	b.cpu[3].idle += 50;
	b.total.user += 50;
	b.total.idle += 50;
	parse_meminfo(load("fixtures/meminfo_dgx_spark"), &b.mem);
	sp.topo.ncpu = 20;
	sp.topo.ngroups = 1;
	strcpy(sp.topo.groups[0].name, "CPU");
	for (int c = 0; c < 20; c++)
		sp.topo.groups[0].cpus[sp.topo.groups[0].n++] = c;
	b.zone_mc[0] = 63000;
	b.nzones = 1;
	b.nvme_mc = TEMP_NONE;
	sp.th.zone[0].kind = Z_OTHER;
	compute_view(&sp, &a, &b, SORT_CPU, &v);
	CHECK(v.cpu_temp_mc == 63000 && !v.cpu_temp_labeled);
	sp.th.zone[0].kind = Z_PCORE;
	compute_view(&sp, &a, &b, SORT_CPU, &v);
	CHECK(v.cpu_temp_labeled);
	CHECK(v.core_user[3] > 49.9 && v.core_user[3] < 50.1);

	for (int d = 0; d < D_COUNT; d++) {
		ui.density = d;
		f.rows = 24;
		f.cols = 80;
		render(&f, &v, &ui);
		CHECK(frame_encode(&f, &prev, 1, &ui, out, sizeof out) > 0);
		CHECK(frame_encode(&f, &prev, 0, &ui, out, sizeof out) == 0); /* unchanged frame */
	}
	CHECK(render_json(&v, out, sizeof out) > 100 && out[0] == '{');
}

static void test_procs(void)
{
	static struct proc_table t;
	static struct view_proc top[8];
	struct gpu_sample g = {0};
	int n, found = 0;

	procs_init(&t);
	CHECK(procs_scan(&t, 1.0) > 1);
	CHECK(procs_scan(&t, 2.0) > 1);
	n = procs_top(&t, &g, SORT_CPU, top, 8);
	CHECK(n > 0 && n <= 8);
	for (int i = 1; i < n; i++)
		CHECK(top[i - 1].cpu_pct >= top[i].cpu_pct);
	for (int i = 0; i < t.cur->n; i++)
		if (t.cur->e[i].pid == (uint32_t)getpid()) {
			found = 1;
			CHECK(strcmp(t.cur->e[i].name, "run_tests") == 0);
			CHECK(t.cur->e[i].user && t.cur->e[i].user[0] != '?');
		}
	CHECK(found);
	g.nproc = 1;
	g.procs[0].pid = (uint32_t)getpid();
	g.procs[0].mem_bytes = 5 << 20;
	n = procs_top(&t, &g, SORT_GPU, top, 8);
	CHECK(n == 1 && top[0].pid == (uint32_t)getpid() && top[0].gpu_bytes == 5 << 20);
}

static const struct {
	const char *name;
	void (*fn)(void);
} tests[] = {
	{"procs", test_procs},
	{"stat", test_stat},
	{"stat_256", test_stat_256},
	{"meminfo", test_meminfo},
	{"meminfo_no_gpu_underflow", test_meminfo_no_gpu_underflow},
	{"loadavg_psi", test_loadavg_psi},
	{"freq", test_freq},
	{"core_name", test_core_name},
	{"parsers", test_parsers},
	{"fmt", test_fmt},
	{"render_fits", test_render_fits},
};

int main(int argc, char **argv)
{
	int ran = 0;

	for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
		int before = failures;

		if (argc > 1 && strcmp(argv[1], tests[i].name) != 0)
			continue;
		tests[i].fn();
		ran++;
		printf("%-26s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
	}
	if (!ran) {
		fprintf(stderr, "no test named '%s'\n", argc > 1 ? argv[1] : "");
		return 2;
	}
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}

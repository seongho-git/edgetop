#ifndef EDGETOP_SAMPLE_H
#define EDGETOP_SAMPLE_H

#include <limits.h>
#include <stdint.h>

#define MAX_CPUS 256
#define MAX_GROUPS 8
#define MAX_ZONES 16
#define MAX_GPU_PROCS 64
#define PROC_TABLE 4096
#define MAX_VIEW_PROCS 64
#define MAX_USERS 32
#define TEMP_NONE INT_MIN

enum sort_key { SORT_CPU, SORT_GPU, SORT_RSS, SORT_COUNT };

enum zone_kind { Z_OTHER, Z_SOC, Z_PCORE, Z_ECORE, Z_GPU, Z_UNCORE, Z_CPUISH };

struct cpu_times {
	uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
};

/* All values in KiB, as reported by /proc/meminfo. */
struct meminfo {
	uint64_t total, free, avail, buffers, cached, sreclaimable, sunreclaim;
	uint64_t anon, shmem, kstack, pagetables, secpagetables, percpu, vmalloc_used;
	uint64_t swap_total, swap_free;
};

enum gpu_field {
	GF_UTIL = 1 << 0,
	GF_POWER = 1 << 1,
	GF_TEMP = 1 << 2,
	GF_SM = 1 << 3,
	GF_PSTATE = 1 << 4,
	GF_REASONS = 1 << 5,
	GF_PROCS = 1 << 6,
};

struct gpu_proc {
	uint32_t pid;
	uint64_t mem_bytes;
};

struct gpu_sample {
	unsigned have; /* gpu_field bits valid in this sample */
	unsigned util, membw, sm_mhz, power_mw, temp_c, pstate;
	uint64_t reasons;
	int nproc;
	struct gpu_proc procs[MAX_GPU_PROCS];
};

struct sample {
	double t;
	int ncpu; /* highest cpu id in /proc/stat + 1 */
	struct cpu_times total;
	struct cpu_times cpu[MAX_CPUS];
	uint8_t online[MAX_CPUS];
	uint32_t freq_khz[MAX_CPUS]; /* 0 = idle or unknown */
	struct meminfo mem;
	double load[3];
	unsigned running, tasks;
	double psi_cpu, psi_mem; /* some avg10; < 0 when absent */
	double uptime;
	int nzones;
	int zone_mc[MAX_ZONES];
	int nvme_mc;
	struct gpu_sample gpu;
	int nprocs; /* processes seen by the last scan, 0 when scanning is off */
	double self_cpu_s;
	uint64_t self_rss_kb;
};

struct core_group {
	char name[16];
	uint32_t max_khz;
	int n;
	int cpus[MAX_CPUS];
};

struct topo {
	int ncpu;
	uint32_t max_khz[MAX_CPUS];
	int ngroups;
	struct core_group groups[MAX_GROUPS];
	int freq_fd[MAX_CPUS];
	int fallback; /* cpuinfo_avg_freq missing; scaling_cur_freq on a slow cadence */
	uint32_t last_khz[MAX_CPUS];
	double last_slow;
};

struct zone_info {
	char label[12];
	int kind; /* zone_kind */
	int cluster;
};

struct thermal {
	int nzones;
	int zone_fd[MAX_ZONES];
	struct zone_info zone[MAX_ZONES];
	int nvme_fd;
	int nvme_last;
	double nvme_t;
};

struct gpu {
	void *lib;
	void *dev;
	char name[64];
	unsigned supported; /* gpu_field bits not yet seen as NOT_SUPPORTED */
	double slow_t;
	int procs_ok, reasons_ok;
	uint64_t reasons;
	int nproc;
	struct gpu_proc procs[MAX_GPU_PROCS];
};

struct uid_name {
	int used;
	uint32_t uid;
	char name[16];
};

struct proc_entry {
	uint32_t pid;
	uint32_t uid;
	uint64_t ticks; /* utime + stime in clock ticks */
	uint64_t rss_pages;
	double cpu_pct;
	const char *user;
	char state;
	char comm[16];
	char name[24];
};

struct proc_list {
	int n;
	double t;
	struct proc_entry e[PROC_TABLE];
};

struct proc_table {
	struct proc_list a, b;
	struct proc_list *cur, *prev;
	long clk_tck, page_kib;
	unsigned nscans;
	struct uid_name users[MAX_USERS];
};

struct sampler {
	struct topo topo;
	struct thermal th;
	struct gpu gpu;
	int gpu_on;
	int fd_stat, fd_meminfo, fd_loadavg, fd_psi_cpu, fd_psi_mem, fd_uptime, fd_statm, fd_schedstat;
	struct proc_table *procs; /* static storage in sample.c; untouched (not resident) until the first scan */
	double procs_t;
	char host[64];
	char product[64];
};

struct view_proc {
	uint32_t pid;
	const char *user;
	const char *name;
	double cpu_pct;
	uint64_t rss_kib;
	uint64_t gpu_bytes;
	char state;
};

struct view {
	const struct sample *s;
	const struct sampler *sp;
	double core_user[MAX_CPUS], core_sys[MAX_CPUS]; /* percent */
	double tot_user, tot_sys, tot_iowait;
	uint64_t m_total, m_used, m_apps, m_gpu, m_kernel, m_cache, m_free, m_avail;
	uint64_t gpu_proc_kib;
	int gpu_proc_count;
	int cpu_temp_mc;
	int cpu_temp_labeled; /* 1 when cpu_temp_mc comes from zones known to be CPU sensors */
	double self_cpu_pct;
	int nproc;
	struct view_proc procs[MAX_VIEW_PROCS];
};

/* proc.c — pure parsers, exposed for tests */
int parse_stat(const char *buf, struct sample *s);
int parse_meminfo(const char *buf, struct meminfo *m);
int parse_loadavg(const char *buf, double load[3], unsigned *running, unsigned *tasks);
double parse_psi_avg10(const char *buf);

/* cpufreq.c */
const char *core_name(uint64_t midr, char *buf, unsigned n);
void topo_init(struct topo *t);
void topo_read(struct topo *t, struct sample *s);
uint32_t parse_freq(const char *buf, long r);

/* thermal.c */
void thermal_init(struct thermal *th);
void thermal_read(struct thermal *th, struct sample *s);

/* nvml.c */
int gpu_init(struct gpu *g);
void gpu_read(struct gpu *g, struct gpu_sample *out, double now);
void gpu_shutdown(struct gpu *g);

/* procs.c */
void procs_init(struct proc_table *t);
int procs_scan(struct proc_table *t, double now);
int procs_top(const struct proc_table *t, const struct gpu_sample *g, int key, struct view_proc *out,
	      int max);

/* sample.c */
int sampler_init(struct sampler *sp, int want_gpu);
/* want_procs: scan /proc for the process panel (every PROC_PERIOD seconds) */
void sampler_read(struct sampler *sp, struct sample *s, int want_procs);
void sampler_close(struct sampler *sp);
void mem_split(const struct meminfo *m, struct view *v);
void compute_view(struct sampler *sp, const struct sample *prev, const struct sample *cur, int sort,
		  struct view *v);

#endif

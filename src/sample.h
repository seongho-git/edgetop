#ifndef EDGETOP_SAMPLE_H
#define EDGETOP_SAMPLE_H

#include <limits.h>
#include <stdint.h>

#define MAX_CPUS 256
#define MAX_GROUPS 8
#define MAX_ZONES 16
#define MAX_GPU_PROCS 64
#define TEMP_NONE INT_MIN

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

struct thermal {
	int nzones;
	int zone_fd[MAX_ZONES];
	int zone_last[MAX_ZONES];
	double zone_t;
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

struct pid_entry {
	uint32_t pid;
	uint32_t uid;
	double seen;
	char name[32];
	char user[16];
};

struct sampler {
	struct topo topo;
	struct thermal th;
	struct gpu gpu;
	int gpu_on;
	int fd_stat, fd_meminfo, fd_loadavg, fd_psi_cpu, fd_psi_mem, fd_uptime, fd_statm, fd_schedstat;
	struct pid_entry pids[MAX_GPU_PROCS];
	char host[64];
	char product[64];
};

struct view_proc {
	uint32_t pid;
	uint64_t mem_bytes;
	const char *name;
	const char *user;
};

struct view {
	const struct sample *s;
	const struct sampler *sp;
	double core_user[MAX_CPUS], core_sys[MAX_CPUS]; /* percent */
	double tot_user, tot_sys, tot_iowait;
	uint64_t m_total, m_used, m_apps, m_gpu, m_kernel, m_cache, m_free, m_avail;
	uint64_t gpu_proc_kib;
	int cpu_temp_mc;
	double self_cpu_pct;
	int nproc;
	struct view_proc procs[MAX_GPU_PROCS];
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

/* sample.c */
int sampler_init(struct sampler *sp, int want_gpu);
void sampler_read(struct sampler *sp, struct sample *s);
void sampler_close(struct sampler *sp);
void mem_split(const struct meminfo *m, struct view *v);
void compute_view(struct sampler *sp, const struct sample *prev, const struct sample *cur,
		  struct view *v);

#endif

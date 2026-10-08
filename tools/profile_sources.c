/*
 * Per-source sampling cost, hot (back to back) and cold (after a 300 ms sleep, the realistic case).
 * Build and run with `make profile`; links against the edgetop objects.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sample.h"
#include "util.h"

static struct sampler sp;
static struct sample s;
static char big[65536], small[256];

static double cpu_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

#define ROWS 8
#define MEASURE(label, setup, stmt)                                                     \
	do {                                                                            \
		double hot = 0, cold = 0, t0;                                           \
		struct timespec nap = {0, 300000000};                                   \
		for (int i = 0; i < 200; i++) {                                         \
			setup;                                                          \
			t0 = cpu_ns();                                                  \
			stmt;                                                           \
			hot += cpu_ns() - t0;                                           \
		}                                                                       \
		for (int i = 0; i < ROWS; i++) {                                        \
			nanosleep(&nap, NULL);                                          \
			setup;                                                          \
			t0 = cpu_ns();                                                  \
			stmt;                                                           \
			cold += cpu_ns() - t0;                                          \
		}                                                                       \
		printf("%-36s %8.1f %8.1f\n", label, hot / 200 / 1e3, cold / ROWS / 1e3);  \
	} while (0)

int main(void)
{
	if (sampler_init(&sp, 1) != 0) {
		fprintf(stderr, "cannot open /proc\n");
		return 1;
	}
	printf("%-36s %8s %8s\n", "source (us)", "hot", "cold");
	MEASURE("/proc/stat read+parse", (void)0, read_fd(sp.fd_stat, big, sizeof big); parse_stat(big, &s));
	MEASURE("/proc/meminfo read+parse", (void)0, read_fd(sp.fd_meminfo, big, sizeof big); parse_meminfo(big, &s.mem));
	MEASURE("loadavg + psi x2 + uptime", (void)0,
		read_fd(sp.fd_loadavg, small, 256); read_fd(sp.fd_psi_cpu, small, 256);
		read_fd(sp.fd_psi_mem, small, 256); read_fd(sp.fd_uptime, small, 256));
	MEASURE("self statm + schedstat", (void)0, read_fd(sp.fd_statm, small, 256); read_fd(sp.fd_schedstat, small, 256));
	MEASURE("cpuinfo_avg_freq x ncpu", s.t = mono_now(), topo_read(&sp.topo, &s));
	MEASURE("thermal zones", sp.th.nvme_t = 1e9; s.t = 0, thermal_read(&sp.th, &s));
	MEASURE("thermal zones + nvme (forced)", sp.th.nvme_t = -1e9; s.t = 0, thermal_read(&sp.th, &s));
	if (!sp.gpu_on) {
		printf("(no GPU: NVML rows skipped)\n");
		return 0;
	}
	MEASURE("nvml util", sp.gpu.supported = GF_UTIL, gpu_read(&sp.gpu, &s.gpu, 0, 0));
	MEASURE("nvml power", sp.gpu.supported = GF_POWER, gpu_read(&sp.gpu, &s.gpu, 0, 0));
	MEASURE("nvml temp + sm clock + pstate", sp.gpu.supported = GF_TEMP | GF_SM | GF_PSTATE, gpu_read(&sp.gpu, &s.gpu, 0, 0));
	MEASURE("nvml clock-event reasons (forced)", sp.gpu.supported = GF_REASONS; sp.gpu.slow_t = -1e9, gpu_read(&sp.gpu, &s.gpu, 0, 0));
	MEASURE("nvml process lists (forced)", sp.gpu.supported = GF_PROCS; sp.gpu.slow_t = -1e9, gpu_read(&sp.gpu, &s.gpu, 0, 0));
	MEASURE("nvml process utilization (forced)", sp.gpu.supported = GF_PROCS | GF_PUTIL; sp.gpu.slow_t = -1e9, gpu_read(&sp.gpu, &s.gpu, 0, 1));
	sampler_close(&sp);
	return 0;
}

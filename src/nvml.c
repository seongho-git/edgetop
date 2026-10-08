#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include "sample.h"

/* Minimal NVML ABI; avoids depending on nvml.h at build time. */
#define NVML_SUCCESS 0
#define NVML_ERROR_NOT_SUPPORTED 3
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_SM 1
#define NVML_VALUE_NOT_AVAILABLE ((unsigned long long)-1)
#define SLOW_PERIOD 3.0 /* process list (~770 us cold) and clock-event reasons (~70 us cold) */

typedef void *nvml_dev;
typedef struct {
	unsigned gpu, memory;
} nvml_util;
typedef struct {
	unsigned pid;
	unsigned long long used;
	unsigned gi, ci;
} nvml_proc;

typedef int (*fn_void)(void);
typedef int (*fn_handle)(unsigned, nvml_dev *);
typedef int (*fn_name)(nvml_dev, char *, unsigned);
typedef int (*fn_util)(nvml_dev, nvml_util *);
typedef int (*fn_uint)(nvml_dev, unsigned *);
typedef int (*fn_sel_uint)(nvml_dev, int, unsigned *);
typedef int (*fn_ull)(nvml_dev, unsigned long long *);
typedef int (*fn_procs)(nvml_dev, unsigned *, nvml_proc *);

static struct {
	fn_void init, shutdown;
	fn_handle handle;
	fn_name name;
	fn_util util;
	fn_uint power, pstate;
	fn_sel_uint temp, clock;
	fn_ull reasons;
	fn_procs compute, graphics;
} f;

static void *sym(void *lib, const char *a, const char *b)
{
	void *p = dlsym(lib, a);
	return p || !b ? p : dlsym(lib, b);
}

int gpu_init(struct gpu *g)
{
	memset(g, 0, sizeof *g);
	g->lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!g->lib)
		return -1;
	/* POSIX dlsym returns void *; the cast through a union keeps -Wpedantic quiet */
#define LOAD(field, a, b)                                  \
	do {                                               \
		union { void *p; __typeof__(f.field) fn; } u; \
		u.p = sym(g->lib, a, b);                   \
		f.field = u.fn;                            \
	} while (0)
	LOAD(init, "nvmlInit_v2", "nvmlInit");
	LOAD(shutdown, "nvmlShutdown", NULL);
	LOAD(handle, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
	LOAD(name, "nvmlDeviceGetName", NULL);
	LOAD(util, "nvmlDeviceGetUtilizationRates", NULL);
	LOAD(power, "nvmlDeviceGetPowerUsage", NULL);
	LOAD(pstate, "nvmlDeviceGetPerformanceState", NULL);
	LOAD(temp, "nvmlDeviceGetTemperature", NULL);
	LOAD(clock, "nvmlDeviceGetClockInfo", NULL);
	LOAD(reasons, "nvmlDeviceGetCurrentClocksEventReasons",
	     "nvmlDeviceGetCurrentClocksThrottleReasons");
	LOAD(compute, "nvmlDeviceGetComputeRunningProcesses_v3", NULL);
	LOAD(graphics, "nvmlDeviceGetGraphicsRunningProcesses_v3", NULL);
#undef LOAD
	if (!f.init || !f.handle || f.init() != NVML_SUCCESS) {
		dlclose(g->lib);
		g->lib = NULL;
		return -1;
	}
	if (f.handle(0, &g->dev) != NVML_SUCCESS) {
		gpu_shutdown(g);
		return -1;
	}
	if (!f.name || f.name(g->dev, g->name, sizeof g->name) != NVML_SUCCESS)
		strcpy(g->name, "GPU");
	g->slow_t = -1e9;
	g->supported = GF_UTIL | GF_POWER | GF_TEMP | GF_SM | GF_PSTATE | GF_REASONS | GF_PROCS;
	return 0;
}

/* Returns 1 when the value is valid; NOT_SUPPORTED disables the field for the rest of the run. */
static int ok(struct gpu *g, unsigned field, int rc)
{
	if (rc == NVML_ERROR_NOT_SUPPORTED)
		g->supported &= ~field;
	return rc == NVML_SUCCESS;
}

static int merge_procs(nvml_dev dev, fn_procs fn, struct gpu *out)
{
	nvml_proc buf[MAX_GPU_PROCS];
	unsigned n = MAX_GPU_PROCS;
	int rc;

	if (!fn)
		return NVML_ERROR_NOT_SUPPORTED;
	rc = fn(dev, &n, buf);
	if (rc == NVML_ERROR_INSUFFICIENT_SIZE)
		n = MAX_GPU_PROCS;
	else if (rc != NVML_SUCCESS)
		return rc;
	for (unsigned i = 0; i < n && i < MAX_GPU_PROCS; i++) {
		uint64_t mem = buf[i].used == NVML_VALUE_NOT_AVAILABLE ? 0 : buf[i].used;
		int j;

		/* a pid can hold both a compute and a graphics context */
		for (j = 0; j < out->nproc && out->procs[j].pid != buf[i].pid; j++)
			;
		if (j < out->nproc) {
			if (mem > out->procs[j].mem_bytes)
				out->procs[j].mem_bytes = mem;
		} else if (out->nproc < MAX_GPU_PROCS) {
			out->procs[out->nproc].pid = buf[i].pid;
			out->procs[out->nproc++].mem_bytes = mem;
		}
	}
	return NVML_SUCCESS;
}

void gpu_read(struct gpu *g, struct gpu_sample *out, double now)
{
	unsigned s = g->supported;
	nvml_util u;

	out->have = 0;
	out->nproc = 0;
	if (!g->dev)
		return;
	if ((s & GF_UTIL) && f.util && ok(g, GF_UTIL, f.util(g->dev, &u))) {
		out->util = u.gpu;
		out->membw = u.memory;
		out->have |= GF_UTIL;
	}
	if ((s & GF_POWER) && f.power && ok(g, GF_POWER, f.power(g->dev, &out->power_mw)))
		out->have |= GF_POWER;
	if ((s & GF_TEMP) && f.temp && ok(g, GF_TEMP, f.temp(g->dev, NVML_TEMPERATURE_GPU, &out->temp_c)))
		out->have |= GF_TEMP;
	if ((s & GF_SM) && f.clock && ok(g, GF_SM, f.clock(g->dev, NVML_CLOCK_SM, &out->sm_mhz)))
		out->have |= GF_SM;
	if ((s & GF_PSTATE) && f.pstate && ok(g, GF_PSTATE, f.pstate(g->dev, &out->pstate)))
		out->have |= GF_PSTATE;
	if (now - g->slow_t >= SLOW_PERIOD) {
		unsigned long long r;
		int rc, rg;

		g->slow_t = now;
		g->reasons_ok = (s & GF_REASONS) && f.reasons && ok(g, GF_REASONS, f.reasons(g->dev, &r));
		if (g->reasons_ok)
			g->reasons = r;
		if (s & GF_PROCS) {
			g->nproc = 0;
			rc = merge_procs(g->dev, f.compute, g);
			rg = merge_procs(g->dev, f.graphics, g);
			g->procs_ok = rc == NVML_SUCCESS || rg == NVML_SUCCESS;
			if (rc == NVML_ERROR_NOT_SUPPORTED && rg == NVML_ERROR_NOT_SUPPORTED)
				g->supported &= ~(unsigned)GF_PROCS;
		}
	}
	if (g->reasons_ok) {
		out->reasons = g->reasons;
		out->have |= GF_REASONS;
	}
	if ((g->supported & GF_PROCS) && g->procs_ok) {
		out->nproc = g->nproc;
		memcpy(out->procs, g->procs, sizeof g->procs);
		out->have |= GF_PROCS;
	}
}

void gpu_shutdown(struct gpu *g)
{
	if (!g->lib)
		return;
	if (f.shutdown)
		f.shutdown();
	dlclose(g->lib);
	g->lib = NULL;
	g->dev = NULL;
}

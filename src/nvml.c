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
#define NVML_FI_DEV_POWER_INSTANT 186
#define NVML_PERF_POLICY_POWER 0
#define PUTIL_MAX 256
#define SLOW_PERIOD 3.0 /* process lists, per-process utilization and the power-cap counter cost 1-11 ms */

typedef void *nvml_dev;
typedef struct {
	unsigned gpu, memory;
} nvml_util;
typedef struct {
	unsigned pid;
	unsigned long long used;
	unsigned gi, ci;
} nvml_proc;
typedef struct {
	unsigned pid;
	unsigned long long ts;
	unsigned sm, mem, enc, dec;
} nvml_putil;

typedef int (*fn_void)(void);
typedef int (*fn_handle)(unsigned, nvml_dev *);
typedef int (*fn_name)(nvml_dev, char *, unsigned);
typedef int (*fn_util)(nvml_dev, nvml_util *);
typedef int (*fn_uint)(nvml_dev, unsigned *);
typedef int (*fn_sel_uint)(nvml_dev, int, unsigned *);
typedef int (*fn_ull)(nvml_dev, unsigned long long *);
typedef int (*fn_procs)(nvml_dev, unsigned *, nvml_proc *);
typedef int (*fn_putil)(nvml_dev, nvml_putil *, unsigned *, unsigned long long);
typedef struct {
	unsigned long long ref, viol;
} nvml_viol;
typedef int (*fn_viol)(nvml_dev, int, nvml_viol *);
typedef struct {
	unsigned field, scope;
	long long ts, latency;
	int type, rc;
	union {
		double d;
		unsigned u;
		unsigned long long ull;
		long long ll;
		int i;
	} v;
} nvml_fval;
typedef int (*fn_fields)(nvml_dev, int, nvml_fval *);

static struct {
	fn_void init, shutdown;
	fn_handle handle;
	fn_name name;
	fn_util util;
	fn_uint power, pstate;
	fn_sel_uint temp, clock;
	fn_ull reasons;
	fn_procs compute, graphics;
	fn_putil putil;
	fn_viol viol;
	fn_fields fields;
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
	LOAD(putil, "nvmlDeviceGetProcessUtilization", NULL);
	LOAD(viol, "nvmlDeviceGetViolationStatus", NULL);
	LOAD(fields, "nvmlDeviceGetFieldValues", NULL);
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
	g->capped_pct = -1;
	g->supported = GF_UTIL | GF_POWER | GF_TEMP | GF_SM | GF_PSTATE | GF_REASONS | GF_PROCS | GF_PUTIL |
		       GF_PINST | GF_VIOL;
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

void gpu_read(struct gpu *g, struct gpu_sample *out, double now, int want_putil)
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
	if ((s & GF_REASONS) && f.reasons) {
		unsigned long long r;
		if (ok(g, GF_REASONS, f.reasons(g->dev, &r))) {
			out->reasons = r;
			out->have |= GF_REASONS;
		}
	}
	if ((s & GF_PINST) && f.fields) {
		nvml_fval fv;
		memset(&fv, 0, sizeof fv);
		fv.field = NVML_FI_DEV_POWER_INSTANT;
		if (f.fields(g->dev, 1, &fv) == NVML_SUCCESS && ok(g, GF_PINST, fv.rc)) {
			out->power_inst_mw = fv.v.u;
			out->have |= GF_PINST;
		}
	}
	if (now - g->slow_t >= SLOW_PERIOD) {
		int rc, rg;

		g->slow_t = now;
		/*
		 * Power-cap share: the violation counter costs 1-2 ms to read and also advances while the
		 * driver merely holds idle clocks down, so it is sampled only while the GPU is busy.
		 */
		if ((s & GF_VIOL) && f.viol && (out->have & GF_UTIL) && out->util > 0) {
			nvml_viol vs;
			if (ok(g, GF_VIOL, f.viol(g->dev, NVML_PERF_POLICY_POWER, &vs))) {
				if (g->viol_t > 0 && now > g->viol_t && vs.viol >= g->viol_ns) {
					g->capped_pct = (double)(vs.viol - g->viol_ns) / 1e9 / (now - g->viol_t) * 100.0;
					if (g->capped_pct > 100)
						g->capped_pct = 100;
				} else {
					g->capped_pct = 0;
				}
				g->viol_ns = vs.viol;
				g->viol_t = now;
			}
		} else {
			g->viol_t = 0; /* idle: next busy sample starts a fresh window */
			g->capped_pct = -1;
		}
		if (s & GF_PROCS) {
			g->nproc = 0;
			rc = merge_procs(g->dev, f.compute, g);
			rg = merge_procs(g->dev, f.graphics, g);
			g->procs_ok = rc == NVML_SUCCESS || rg == NVML_SUCCESS;
			if (rc == NVML_ERROR_NOT_SUPPORTED && rg == NVML_ERROR_NOT_SUPPORTED)
				g->supported &= ~(unsigned)GF_PROCS;
		}
		/*
		 * 6-11 ms per call even when idle, so it runs only while the process panel is shown and the
		 * device reported activity (no process can have SM time at 0 % utilization).
		 */
		for (int i = 0; i < g->nproc; i++)
			g->procs[i].sm_pct = 0;
		if ((s & GF_PUTIL) && f.putil && g->procs_ok && want_putil &&
		    (!(out->have & GF_UTIL) || out->util > 0)) {
			static nvml_putil buf[PUTIL_MAX];
			unsigned n = PUTIL_MAX;

			rc = f.putil(g->dev, buf, &n, g->putil_ts);
			if (rc == NVML_SUCCESS) {
				for (unsigned i = 0; i < n && i < PUTIL_MAX; i++) {
					if (buf[i].ts > g->putil_ts)
						g->putil_ts = buf[i].ts;
					for (int j = 0; j < g->nproc; j++)
						if (g->procs[j].pid == buf[i].pid && buf[i].sm <= 100 &&
						    buf[i].sm > g->procs[j].sm_pct)
							g->procs[j].sm_pct = buf[i].sm;
				}
			} else if (rc == NVML_ERROR_NOT_SUPPORTED) {
				g->supported &= ~(unsigned)GF_PUTIL;
			}
		}
	}
	out->capped_pct = g->capped_pct;
	if (g->capped_pct >= 0)
		out->have |= GF_VIOL;
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

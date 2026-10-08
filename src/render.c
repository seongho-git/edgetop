#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "render.h"
#include "util.h"

#define LABEL_W 7
#define PROC_ROWS_TUI 5
#define PROC_ROWS_ONCE 10

static struct frame *F;

static void clear(struct frame *f)
{
	for (int r = 0; r < f->rows; r++) {
		memset(f->ch[r], ' ', (size_t)f->cols);
		memset(f->co[r], C_DEF, (size_t)f->cols);
	}
}

static int put(int r, int c, unsigned char color, const char *s)
{
	if (r < 0 || r >= F->rows)
		return c;
	for (; *s && c < F->cols; s++, c++) {
		if (c < 0)
			continue;
		F->ch[r][c] = (unsigned char)*s;
		F->co[r][c] = color;
	}
	return c;
}

__attribute__((format(printf, 4, 5))) static int putf(int r, int c, unsigned char color,
						     const char *fmt, ...)
{
	char buf[MAX_COLS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return put(r, c, color, buf);
}

static int iround(double x)
{
	return (int)(x + 0.5);
}

static unsigned char util_color(double p)
{
	return p >= 80 ? C_RED : p >= 50 ? C_YELLOW : C_GREEN;
}

static unsigned char temp_color(int mc)
{
	return mc >= 85000 ? C_RED : mc >= 70000 ? C_YELLOW : C_GREEN;
}

struct seg {
	double pct;
	unsigned char color;
	char mono; /* glyph used without color, so segments stay distinguishable */
};

/* Draws [bar text] in width w at (r, c); bar cells stop where the right-aligned text starts. */
static void bar(int r, int c, int w, const struct seg *segs, int nseg, const char *text,
		unsigned char text_color, const struct ui *ui)
{
	int inner = w - 2, tl = (int)strlen(text), bw, filled = 0, unicode;
	double acc = 0;

	if (inner < 1)
		return;
	if (tl > inner)
		tl = inner;
	bw = inner - tl;
	unicode = ui->unicode && ui->color;
	put(r, c, C_DIM, "[");
	put(r, c + w - 1, C_DIM, "]");
	for (int i = 0; i < nseg; i++) {
		int end, last = i == nseg - 1;
		double units;

		acc += segs[i].pct;
		if (acc > 100)
			acc = 100;
		units = acc / 100.0 * bw * (unicode && last ? 8 : 1);
		end = unicode && last ? (int)(units / 8) : iround(units);
		for (; filled < end && filled < bw; filled++) {
			F->ch[r][c + 1 + filled] = unicode ? GLYPH_BASE + 8
					    : ui->color ? '|' : (unsigned char)segs[i].mono;
			F->co[r][c + 1 + filled] = segs[i].color;
		}
		if (unicode && last && filled < bw) {
			int eighths = iround(units) - filled * 8;
			if (eighths > 0 && eighths < 8) {
				F->ch[r][c + 1 + filled] = (unsigned char)(GLYPH_BASE + eighths);
				F->co[r][c + 1 + filled] = segs[i].color;
			}
		}
	}
	put(r, c + 1 + inner - tl, text_color, text + strlen(text) - (size_t)tl);
}

static void fmt_uptime(char *b, size_t n, double up)
{
	unsigned long s = up < 0 ? 0 : (unsigned long)up;
	unsigned long d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;

	if (d)
		snprintf(b, n, "%lud %02lu:%02lu", d, h, m);
	else
		snprintf(b, n, "%02lu:%02lu", h, m);
}

static int panel_header(int r, const struct view *v, const struct ui *ui)
{
	const struct sample *s = v->s;
	char up[32], clock[16];
	time_t now = time(NULL);
	struct tm tm;
	int c;

	char load[64] = "";
	int room;

	localtime_r(&now, &tm);
	strftime(clock, sizeof clock, "%H:%M:%S", &tm);
	fmt_uptime(up, sizeof up, s->uptime);
	if (s->load[0] >= 0)
		snprintf(load, sizeof load, "  load %.2f %.2f %.2f  %u/%u", s->load[0], s->load[1],
			 s->load[2], s->running, s->tasks);
	/* the product name is the first thing dropped so the clock keeps its place */
	room = F->cols - (ui->once ? 0 : 10) - 9 - (int)strlen(v->sp->host) - 5 - (int)strlen(up) -
	       (int)strlen(load);
	c = put(r, 1, C_TITLE, "edgetop");
	c = putf(r, c + 2, C_BOLD, "%s", v->sp->host);
	if (v->sp->product[0] && (int)strlen(v->sp->product) + 3 <= room)
		c = putf(r, c + 1, C_DIM, "(%s)", v->sp->product);
	c = putf(r, c + 2, C_DEF, "up %s", up);
	c = put(r, c, C_DEF, load);
	if (!ui->once && c + 10 <= F->cols)
		put(r, F->cols - 9, C_DEF, clock);
	return r + 1;
}

static void core_cell(int r, int c, int cpu, const struct view *v, const struct ui *ui)
{
	const struct sample *s = v->s;
	double u = v->core_user[cpu], sy = v->core_sys[cpu], busy = u + sy;
	struct seg segs[2] = {{u, C_GREEN, '|'}, {sy, C_RED, '#'}};
	uint32_t khz = s->freq_khz[cpu], max = v->sp->topo.max_khz[cpu];
	char text[24];

	if (!s->online[cpu]) {
		putf(r, c, C_DIM, "%3d offline", cpu);
		return;
	}
	if (max && khz > max)
		khz = max;
	switch (ui->density) {
	case D_FULL:
		putf(r, c, C_CYAN, "%3d", cpu);
		if (khz)
			snprintf(text, sizeof text, "%3.0f%% %4u", busy, khz / 1000);
		else
			snprintf(text, sizeof text, "%3.0f%% idle", busy);
		bar(r, c + 3, 20, segs, 2, text, util_color(busy), ui);
		break;
	case D_BAR:
		putf(r, c, C_CYAN, "%3d", cpu);
		snprintf(text, sizeof text, "%3.0f%%", busy);
		bar(r, c + 3, 12, segs, 2, text, util_color(busy), ui);
		break;
	default:
		putf(r, c, C_CYAN, "%3d", cpu);
		putf(r, c + 4, util_color(busy), "%3.0f%%", busy);
	}
}

static const int cell_w[D_COUNT] = {24, 16, 9};

static int cores_per_row(int density)
{
	int per = (F->cols - LABEL_W) / cell_w[density];
	return per < 1 ? 1 : per;
}

static int core_rows(const struct topo *t, int density)
{
	int per = cores_per_row(density), rows = 0;

	for (int g = 0; g < t->ngroups; g++)
		rows += (t->groups[g].n + per - 1) / per;
	return rows;
}

static int panel_cores(int r, const struct view *v, const struct ui *ui)
{
	const struct topo *t = &v->sp->topo;
	int w = cell_w[ui->density];
	int per = cores_per_row(ui->density);

	for (int g = 0; g < t->ngroups; g++) {
		const struct core_group *grp = &t->groups[g];
		put(r, 1, C_TITLE, grp->name);
		for (int i = 0; i < grp->n; i++) {
			if (i && i % per == 0)
				r++;
			core_cell(r, LABEL_W + (i % per) * w, grp->cpus[i], v, ui);
		}
		r++;
	}
	return r;
}

static int panel_cpu_total(int r, const struct view *v, const struct ui *ui)
{
	const struct sample *s = v->s;
	struct seg segs[3] = {
		{v->tot_user, C_GREEN, '|'}, {v->tot_sys, C_RED, '#'}, {v->tot_iowait, C_DIM, '.'}};
	double busy = v->tot_user + v->tot_sys;
	char text[16], info[48];
	int bw;

	snprintf(text, sizeof text, "%.1f%%", busy);
	if (s->psi_cpu >= 0)
		snprintf(info, sizeof info, "  psi %.1f%%  %d cores", s->psi_cpu, v->sp->topo.ncpu);
	else
		snprintf(info, sizeof info, "  %d cores", v->sp->topo.ncpu);
	put(r, 1, C_TITLE, "CPU");
	bw = F->cols - LABEL_W - (int)strlen(info) - 1;
	bar(r, LABEL_W, bw, segs, 3, text, C_BOLD, ui);
	put(r, LABEL_W + bw, C_DEF, info);
	return r + 1;
}

static const struct {
	uint64_t bit;
	const char *name;
} reasons[] = {
	{0x4, "sw-power-cap"},  {0x8, "hw-slowdown"},      {0x20, "sw-thermal"},
	{0x40, "hw-thermal"},   {0x80, "hw-power-brake"},
};

static int panel_gpu(int r, const struct view *v, const struct ui *ui)
{
	const struct gpu_sample *g = &v->s->gpu;
	const char *name = v->sp->gpu.name;
	char text[8], info[96];
	int c, n = 0, bw;

	if (!v->sp->gpu_on)
		return r;
	if (strncmp(name, "NVIDIA ", 7) == 0)
		name += 7;
	put(r, 1, C_TITLE, "GPU");
	c = putf(r, LABEL_W, C_BOLD, "%s ", name);
	info[0] = '\0';
	if (g->have & GF_UTIL)
		n += snprintf(info + n, sizeof info - (size_t)n, "  membw %2u%%", g->membw);
	if (g->have & GF_SM)
		n += snprintf(info + n, sizeof info - (size_t)n, "  %uMHz", g->sm_mhz);
	if (g->have & GF_POWER)
		n += snprintf(info + n, sizeof info - (size_t)n, "  %.1fW", g->power_mw / 1000.0);
	if (g->have & GF_PSTATE)
		snprintf(info + n, sizeof info - (size_t)n, "  P%u", g->pstate);
	bw = F->cols - c - (int)strlen(info) - 1 - ((g->have & GF_TEMP) ? 5 : 0);
	if (g->have & GF_UTIL) {
		struct seg seg = {g->util, util_color(g->util), '|'};
		snprintf(text, sizeof text, "%u%%", g->util);
		bar(r, c, bw, &seg, 1, text, C_BOLD, ui);
	} else {
		put(r, c, C_DIM, "util n/a");
	}
	c = put(r, c + bw, C_DEF, info);
	if (g->have & GF_TEMP)
		putf(r, c + 2, temp_color((int)g->temp_c * 1000), "%uC", g->temp_c);
	r++;

	if ((g->have & GF_REASONS) && (g->reasons & 0xec)) {
		c = put(r, LABEL_W, C_YELLOW, "throttle:");
		for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++)
			if (g->reasons & reasons[i].bit)
				c = put(r, c + 1, C_RED, reasons[i].name);
		r++;
	}
	return r;
}

static int panel_mem(int r, const struct view *v, const struct ui *ui)
{
	const struct sample *s = v->s;
	double t = (double)v->m_total;
	const char *gpu_label = v->sp->gpu_on ? "gpu" : "other";
	struct seg segs[4] = {
		{100.0 * (double)v->m_apps / t, C_GREEN, '|'},
		{100.0 * (double)v->m_gpu / t, C_MAGENTA, '#'},
		{100.0 * (double)v->m_kernel / t, C_RED, '*'},
		{100.0 * (double)v->m_cache / t, C_YELLOW, ':'},
	};
	char used[16], total[16], a[16], g[16], k[16], ca[16], av[16], text[40];
	int c;

	if (!v->m_total)
		return r;
	fmt_kib(used, sizeof used, v->m_used);
	fmt_kib(total, sizeof total, v->m_total);
	snprintf(text, sizeof text, "%s/%s", used, total);
	put(r, 1, C_TITLE, "Mem");
	bar(r, LABEL_W, F->cols - LABEL_W - 1, segs, 4, text, C_BOLD, ui);
	r++;

	fmt_kib(a, sizeof a, v->m_apps);
	fmt_kib(g, sizeof g, v->m_gpu);
	fmt_kib(k, sizeof k, v->m_kernel);
	fmt_kib(ca, sizeof ca, v->m_cache);
	fmt_kib(av, sizeof av, v->m_avail);
	c = putf(r, LABEL_W, C_GREEN, "apps %s", a);
	c = putf(r, c + 2, C_MAGENTA, "%s %s", gpu_label, g);
	c = putf(r, c + 2, C_RED, "kernel %s", k);
	c = putf(r, c + 2, C_YELLOW, "cache %s", ca);
	c = putf(r, c + 2, C_BOLD, "avail %s", av);
	if (s->psi_mem >= 0)
		putf(r, c + 2, C_DEF, "psi %.1f%%", s->psi_mem);
	r++;

	if (s->mem.swap_total) {
		uint64_t su = s->mem.swap_total - s->mem.swap_free;
		struct seg seg = {100.0 * (double)su / (double)s->mem.swap_total, C_RED, '|'};
		fmt_kib(used, sizeof used, su);
		fmt_kib(total, sizeof total, s->mem.swap_total);
		snprintf(text, sizeof text, "%s/%s", used, total);
		put(r, 1, C_TITLE, "Swap");
		bar(r, LABEL_W, F->cols - LABEL_W - 1, &seg, 1, text, C_BOLD, ui);
		r++;
	}
	return r;
}

static int panel_temp(int r, const struct view *v)
{
	const struct sample *s = v->s;
	int c = LABEL_W;

	put(r, 1, C_TITLE, "Temp");
	if (v->cpu_temp_mc != TEMP_NONE)
		c = putf(r, c, temp_color(v->cpu_temp_mc), "cpu %dC  ", v->cpu_temp_mc / 1000);
	if (s->gpu.have & GF_TEMP)
		c = putf(r, c, temp_color((int)s->gpu.temp_c * 1000), "gpu %uC  ", s->gpu.temp_c);
	if (s->nvme_mc != TEMP_NONE)
		c = putf(r, c, temp_color(s->nvme_mc), "nvme %dC  ", s->nvme_mc / 1000);
	if (s->nzones) {
		c = put(r, c + 1, C_DIM, "zones");
		for (int i = 0; i < s->nzones; i++)
			if (s->zone_mc[i] != TEMP_NONE)
				c = putf(r, c + 1, temp_color(s->zone_mc[i]), "%d", s->zone_mc[i] / 1000);
	}
	return r + 1;
}

static int panel_procs(int r, int max_rows, const struct view *v, const struct ui *ui)
{
	const struct sample *s = v->s;
	int limit = ui->once ? PROC_ROWS_ONCE : PROC_ROWS_TUI;
	char mem[16], sum[16], resid[16];

	if (!ui->show_procs || !(s->gpu.have & GF_PROCS) || max_rows < 3)
		return r;
	if (limit > max_rows - 2)
		limit = max_rows - 2;
	put(r, 1, C_TITLE, "GPU procs");
	putf(r, 11, C_BOLD, "%8s  %-10s %7s  %s", "PID", "USER", "MEM", "NAME");
	r++;
	if (!v->nproc)
		put(r++, 21, C_DIM, "(none)");
	for (int i = 0; i < v->nproc && i < limit; i++, r++) {
		const struct view_proc *p = &v->procs[i];
		fmt_kib(mem, sizeof mem, p->mem_bytes / 1024);
		putf(r, 11, C_DEF, "%8u  %-10.10s %7s  %s", p->pid, p->user, mem, p->name);
	}
	fmt_kib(sum, sizeof sum, v->gpu_proc_kib);
	fmt_kib(resid, sizeof resid, v->m_gpu);
	putf(r, 11, C_DIM, "%d procs  nvml sum %s  meminfo resid %s", v->nproc, sum, resid);
	return r + 1;
}

static void panel_footer(int r, const struct view *v, const struct ui *ui)
{
	char rss[16];
	int c;

	fmt_kib(rss, sizeof rss, v->s->self_rss_kb);
	if (F->cols >= 76)
		c = putf(r, 1, C_DIM, "q quit  +/- %.2gs  p pause  g procs  c cells", ui->interval);
	else
		c = putf(r, 1, C_DIM, "q +/- %.2gs p g c", ui->interval);
	if (ui->paused)
		c = put(r, c + 2, C_YELLOW, "PAUSED");
	if (c + 20 <= F->cols)
		putf(r, F->cols - 19, C_DIM, "self %5.2f%% %6s", v->self_cpu_pct, rss);
}

void render(struct frame *f, const struct view *v, const struct ui *ui)
{
	int r = 0;

	F = f;
	clear(f);
	if (!ui->once && (f->cols < 60 || f->rows < 12)) {
		putf(f->rows / 2, 1, C_YELLOW, "terminal %dx%d is too small (need 60x12)", f->cols,
		     f->rows);
		return;
	}
	struct ui eff = *ui;
	const struct sample *s = v->s;
	int fixed = 1 + 1 + (v->sp->gpu_on ? 1 : 0) + 2 + (s->mem.swap_total ? 1 : 0) + 1 + 1;

	/* switch to denser core cells when the other panels would be pushed off screen */
	while (!ui->once && eff.density < D_COMPACT &&
	       fixed + core_rows(&v->sp->topo, eff.density) > f->rows)
		eff.density++;
	ui = &eff;
	r = panel_header(r, v, ui);
	r = panel_cores(r, v, ui);
	r = panel_cpu_total(r, v, ui);
	r = panel_gpu(r, v, ui);
	r = panel_mem(r, v, ui);
	r = panel_temp(r, v);
	r = panel_procs(r, (ui->once ? f->rows : f->rows - 1) - r, v, ui);
	if (!ui->once)
		panel_footer(f->rows - 1, v, ui);
}

static const char *const sgr[] = {
	[C_DEF] = "\033[0m",      [C_RED] = "\033[0;31m",  [C_GREEN] = "\033[0;32m",
	[C_YELLOW] = "\033[0;33m", [C_BLUE] = "\033[0;34m", [C_MAGENTA] = "\033[0;35m",
	[C_CYAN] = "\033[0;36m",  [C_DIM] = "\033[0;90m",  [C_BOLD] = "\033[0;1m",
	[C_TITLE] = "\033[0;1;36m",
};

/* U+258F..U+2588: left blocks of 1/8 .. 8/8 width */
static const char *const blocks[9] = {
	" ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█",
};

static size_t emit_row(const struct frame *f, int r, int width, const struct ui *ui, char *out,
		       size_t n, size_t cap)
{
	int cur = -1;

	for (int c = 0; c < width; c++) {
		unsigned char ch = f->ch[r][c], co = f->co[r][c];
		const char *s;
		size_t len;

		if (cap - n < 32)
			break;
		if (ui->color && co != cur && !(ch == ' ' && co == C_DEF && cur == C_DEF)) {
			len = strlen(sgr[co]);
			memcpy(out + n, sgr[co], len);
			n += len;
			cur = co;
		}
		if (ch >= GLYPH_BASE) {
			s = blocks[ch - GLYPH_BASE];
			len = strlen(s);
			memcpy(out + n, s, len);
			n += len;
		} else {
			out[n++] = (char)ch;
		}
	}
	if (ui->color && cur != C_DEF && cap - n > 8) {
		memcpy(out + n, sgr[C_DEF], 4);
		n += 4;
	}
	return n;
}

size_t frame_encode(struct frame *f, struct frame *prev, int full, const struct ui *ui, char *out,
		    size_t cap)
{
	size_t n = 0;

	for (int r = 0; r < f->rows; r++) {
		size_t w = (size_t)f->cols;

		if (!full && memcmp(f->ch[r], prev->ch[r], w) == 0 &&
		    memcmp(f->co[r], prev->co[r], w) == 0)
			continue;
		if (cap - n < 16)
			break;
		n += (size_t)snprintf(out + n, cap - n, "\033[%d;1H", r + 1);
		/* skip the bottom-right cell so the terminal never scrolls */
		n = emit_row(f, r, r == f->rows - 1 ? f->cols - 1 : f->cols, ui, out, n, cap);
		memcpy(prev->ch[r], f->ch[r], w);
		memcpy(prev->co[r], f->co[r], w);
	}
	return n;
}

static int row_blank(const struct frame *f, int r)
{
	for (int c = 0; c < f->cols; c++)
		if (f->ch[r][c] != ' ')
			return 0;
	return 1;
}

size_t frame_text(const struct frame *f, const struct ui *ui, char *out, size_t cap)
{
	size_t n = 0;
	int last = f->rows - 1;

	while (last > 0 && row_blank(f, last))
		last--;
	for (int r = 0; r <= last && cap - n > 2; r++) {
		int w = f->cols;
		while (w > 0 && f->ch[r][w - 1] == ' ')
			w--;
		n = emit_row(f, r, w, ui, out, n, cap - 1);
		out[n++] = '\n';
	}
	return n;
}

struct jbuf {
	char *p;
	size_t n, cap;
};

__attribute__((format(printf, 2, 3))) static void jp(struct jbuf *b, const char *fmt, ...)
{
	va_list ap;
	int k;

	if (b->n >= b->cap)
		return;
	va_start(ap, fmt);
	k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
	va_end(ap);
	b->n = k < 0 ? b->n : b->n + (size_t)k > b->cap ? b->cap : b->n + (size_t)k;
}

static void jtemp(struct jbuf *b, const char *key, int mc)
{
	if (mc == TEMP_NONE)
		jp(b, "\"%s\":null", key);
	else
		jp(b, "\"%s\":%.1f", key, mc / 1000.0);
}

/* JSON string escape for process names; control bytes are dropped. */
static void jstr(struct jbuf *b, const char *s)
{
	jp(b, "\"");
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			jp(b, "\\%c", *s);
		else if ((unsigned char)*s >= 0x20)
			jp(b, "%c", *s);
	}
	jp(b, "\"");
}

size_t render_json(const struct view *v, char *out, size_t cap)
{
	const struct sample *s = v->s;
	const struct topo *t = &v->sp->topo;
	const struct gpu_sample *g = &s->gpu;
	struct jbuf b = {out, 0, cap - 1};

	jp(&b, "{\"ts\":%ld,\"uptime_s\":%.0f,", (long)time(NULL), s->uptime);
	if (s->load[0] >= 0)
		jp(&b, "\"load\":[%.2f,%.2f,%.2f],\"tasks\":{\"running\":%u,\"total\":%u},",
		   s->load[0], s->load[1], s->load[2], s->running, s->tasks);
	else
		jp(&b, "\"load\":null,\"tasks\":null,");

	jp(&b, "\"cpu\":{\"total_pct\":%.2f,\"user_pct\":%.2f,\"system_pct\":%.2f,"
	       "\"iowait_pct\":%.2f,",
	   v->tot_user + v->tot_sys, v->tot_user, v->tot_sys, v->tot_iowait);
	if (s->psi_cpu >= 0)
		jp(&b, "\"psi_some10\":%.2f,", s->psi_cpu);
	else
		jp(&b, "\"psi_some10\":null,");
	jp(&b, "\"cores\":[");
	for (int gi = 0, first = 1; gi < t->ngroups; gi++)
		for (int i = 0; i < t->groups[gi].n; i++) {
			int c = t->groups[gi].cpus[i];
			uint32_t khz = s->freq_khz[c];
			if (t->max_khz[c] && khz > t->max_khz[c])
				khz = t->max_khz[c];
			jp(&b, "%s{\"id\":%d,\"cluster\":", first ? "" : ",", c);
			jstr(&b, t->groups[gi].name);
			if (!s->online[c])
				jp(&b, ",\"pct\":null,\"mhz\":null}");
			else if (khz)
				jp(&b, ",\"pct\":%.2f,\"mhz\":%u}", v->core_user[c] + v->core_sys[c],
				   khz / 1000);
			else
				jp(&b, ",\"pct\":%.2f,\"mhz\":null}", v->core_user[c] + v->core_sys[c]);
			first = 0;
		}
	jp(&b, "]},");

	if (!v->sp->gpu_on) {
		jp(&b, "\"gpu\":null,");
	} else {
		jp(&b, "\"gpu\":{\"name\":");
		jstr(&b, v->sp->gpu.name);
		if (g->have & GF_UTIL)
			jp(&b, ",\"util_pct\":%u,\"membw_pct\":%u", g->util, g->membw);
		else
			jp(&b, ",\"util_pct\":null,\"membw_pct\":null");
		if (g->have & GF_SM)
			jp(&b, ",\"sm_mhz\":%u", g->sm_mhz);
		else
			jp(&b, ",\"sm_mhz\":null");
		if (g->have & GF_POWER)
			jp(&b, ",\"power_w\":%.2f", g->power_mw / 1000.0);
		else
			jp(&b, ",\"power_w\":null");
		if (g->have & GF_TEMP)
			jp(&b, ",\"temp_c\":%u", g->temp_c);
		else
			jp(&b, ",\"temp_c\":null");
		if (g->have & GF_PSTATE)
			jp(&b, ",\"pstate\":%u", g->pstate);
		else
			jp(&b, ",\"pstate\":null");
		if (g->have & GF_REASONS)
			jp(&b, ",\"clock_event_reasons\":%llu", (unsigned long long)g->reasons);
		else
			jp(&b, ",\"clock_event_reasons\":null");
		jp(&b, ",\"procs\":[");
		for (int i = 0; i < v->nproc; i++) {
			jp(&b, "%s{\"pid\":%u,\"user\":", i ? "," : "", v->procs[i].pid);
			jstr(&b, v->procs[i].user);
			jp(&b, ",\"mem_mib\":%llu,\"name\":",
			   (unsigned long long)(v->procs[i].mem_bytes >> 20));
			jstr(&b, v->procs[i].name);
			jp(&b, "}");
		}
		jp(&b, "]},");
	}

	jp(&b, "\"mem\":{\"total_kib\":%llu,\"used_kib\":%llu,\"apps_kib\":%llu,\"gpu_kib\":%llu,"
	       "\"kernel_kib\":%llu,\"cache_kib\":%llu,\"free_kib\":%llu,\"avail_kib\":%llu,"
	       "\"gpu_procs_kib\":%llu,\"swap_total_kib\":%llu,\"swap_free_kib\":%llu,",
	   (unsigned long long)v->m_total, (unsigned long long)v->m_used,
	   (unsigned long long)v->m_apps, (unsigned long long)v->m_gpu,
	   (unsigned long long)v->m_kernel, (unsigned long long)v->m_cache,
	   (unsigned long long)v->m_free, (unsigned long long)v->m_avail,
	   (unsigned long long)v->gpu_proc_kib, (unsigned long long)s->mem.swap_total,
	   (unsigned long long)s->mem.swap_free);
	if (s->psi_mem >= 0)
		jp(&b, "\"psi_some10\":%.2f},", s->psi_mem);
	else
		jp(&b, "\"psi_some10\":null},");

	jp(&b, "\"temp\":{");
	jtemp(&b, "cpu_c", v->cpu_temp_mc);
	jp(&b, ",");
	if (s->gpu.have & GF_TEMP)
		jp(&b, "\"gpu_c\":%u,", s->gpu.temp_c);
	else
		jp(&b, "\"gpu_c\":null,");
	jtemp(&b, "nvme_c", s->nvme_mc);
	jp(&b, ",\"zones_c\":[");
	for (int i = 0; i < s->nzones; i++) {
		if (s->zone_mc[i] == TEMP_NONE)
			jp(&b, "%snull", i ? "," : "");
		else
			jp(&b, "%s%.1f", i ? "," : "", s->zone_mc[i] / 1000.0);
	}
	jp(&b, "]},\"self\":{\"cpu_pct\":%.3f,\"rss_kib\":%llu}}\n", v->self_cpu_pct,
	   (unsigned long long)s->self_rss_kb);
	return b.n;
}

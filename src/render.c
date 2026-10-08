#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "render.h"
#include "util.h"

#define LABEL_W 7
#define PROC_ROWS_ONCE 15
#define PROC_ROWS_JSON 20
#define PROC_MIN_ROWS 4    /* header + two processes + summary */
#define GRAPH_MIN_PROCS 8  /* graphs are a luxury: only when the process list keeps this many rows */
#define GPU_BOX_XL 16      /* GPU box rows including borders: 14-row graph */
#define GPU_BOX_TALL 12    /* 10-row graph */
#define GPU_BOX_SHORT 8    /* 6-row graph */
#define BOX_W 26           /* core box: 24 history columns plus two borders */
#define BOX_TALL 6         /* core box rows including borders: 4-row graph */
#define BOX_SHORT 4        /* 2-row graph */
#define MIN_COLS 40
#define MIN_ROWS 3

static struct frame *F;

static void clear(struct frame *f)
{
	for (int r = 0; r < f->rows; r++) {
		for (int c = 0; c < f->cols; c++)
			f->ch[r][c] = ' ';
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

static int uni(const struct ui *ui)
{
	return ui->unicode && ui->color;
}

/* Draws a box border; the title is overlaid on the top edge starting one cell in. */
static void box(int r, int c, int w, int h, const char *title, unsigned char title_color,
		const struct ui *ui)
{
	unsigned char hl = uni(ui) ? GLYPH_LINE_TRACK : '-', vl = uni(ui) ? GLYPH_BOX_V : '|';
	unsigned char tl = uni(ui) ? GLYPH_BOX_TL : '+', tr = uni(ui) ? GLYPH_BOX_TR : '+';
	unsigned char bl = uni(ui) ? GLYPH_BOX_BL : '+', br = uni(ui) ? GLYPH_BOX_BR : '+';

	if (w < 2 || h < 2 || r + h > F->rows)
		return;
	for (int k = 1; k < w - 1; k++) {
		F->ch[r][c + k] = hl;
		F->co[r][c + k] = C_DIM;
		F->ch[r + h - 1][c + k] = hl;
		F->co[r + h - 1][c + k] = C_DIM;
	}
	for (int k = 1; k < h - 1; k++) {
		F->ch[r + k][c] = vl;
		F->co[r + k][c] = C_DIM;
		F->ch[r + k][c + w - 1] = vl;
		F->co[r + k][c + w - 1] = C_DIM;
	}
	F->ch[r][c] = tl;
	F->ch[r][c + w - 1] = tr;
	F->ch[r + h - 1][c] = bl;
	F->ch[r + h - 1][c + w - 1] = br;
	F->co[r][c] = F->co[r][c + w - 1] = F->co[r + h - 1][c] = F->co[r + h - 1][c + w - 1] = C_DIM;
	if (title && *title) {
		int tw = (int)strlen(title);
		if (tw > w - 4)
			tw = w - 4;
		for (int k = 0; k < tw; k++) {
			F->ch[r][c + 2 + k] = (unsigned char)title[k];
			F->co[r][c + 2 + k] = title_color;
		}
	}
}

static void set_cell(int row, int col, uint16_t ch, unsigned char color)
{
	if (row >= 0 && row < F->rows && col >= 0 && col < F->cols) {
		F->ch[row][col] = ch;
		F->co[row][col] = color;
	}
}

#define MAX_SERIES 2

/*
 * Line plot after nvtop's plot.c: values are rounded to rows (0 % on the bottom row, 100 % on the
 * top), flat segments are ─, a change is drawn in one column as two corners joined by │. With n series
 * a tick spans n columns: in column k only series k moves, the others continue flat at their last row,
 * and where the moving line's vertical run meets another line a junction (┬ ┴ ┼) is drawn instead of
 * overwriting it. Newest tick at the right.
 */
static void plot_lines(int r_top, int rows, int c_left, int cells, int nser,
		       const uint8_t *const ring[], const unsigned char color[],
		       const struct history *h, const struct ui *ui)
{
	uint16_t hl = uni(ui) ? GLYPH_LINE_TRACK : '-', vl = uni(ui) ? GLYPH_BOX_V : '|';
	uint16_t tl = uni(ui) ? GLYPH_BOX_TL : '+', tr = uni(ui) ? GLYPH_BOX_TR : '+';
	uint16_t bl = uni(ui) ? GLYPH_BOX_BL : '+', br = uni(ui) ? GLYPH_BOX_BR : '+';
	uint16_t tt = uni(ui) ? GLYPH_BOX_TT : '+', bt = uni(ui) ? GLYPH_BOX_BT : '+';
	uint16_t cross = uni(ui) ? GLYPH_BOX_X : '+';
	int ticks = cells / nser, before[MAX_SERIES];

	if (ticks > HIST_LEN)
		ticks = HIST_LEN;
	if (ticks > h->len)
		ticks = h->len;
	if (ticks <= 0)
		return;

#define LEVEL(k, t) (r_top + rows - 1 - (history_at(ring[k], h, ticks - 1 - (t)) * (rows - 1) + 50) / 100)
	for (int k = 0; k < nser; k++)
		before[k] = LEVEL(k, 0);
	for (int t = 0; t < ticks; t++) {
		for (int k = 0; k < nser; k++) {
			int col = c_left + (cells - ticks * nser) + t * nser + k; /* right-aligned */
			int now = LEVEL(k, t);

			if (now != before[k]) {
				int down = now > before[k]; /* value fell: the line goes down the screen */
				int top = down ? before[k] : now, bottom = down ? now : before[k];

				set_cell(top, col, down ? tr : tl, color[k]);
				set_cell(bottom, col, down ? bl : br, color[k]);
				for (int rr = top + 1; rr < bottom; rr++)
					set_cell(rr, col, vl, color[k]);
				for (int o = 0; o < nser; o++) {
					if (o == k)
						continue;
					if (before[o] == bottom)
						set_cell(bottom, col, bt, color[k]);
					else if (before[o] == top)
						set_cell(top, col, tt, color[k]);
					else if (before[o] > top && before[o] < bottom)
						set_cell(before[o], col, cross, color[k]);
					else
						set_cell(before[o], col, hl, color[o]);
				}
			} else {
				set_cell(now, col, hl, color[k]);
				for (int o = 0; o < nser; o++)
					if (o != k && before[o] != now)
						set_cell(before[o], col, hl, color[o]);
			}
			before[k] = now;
		}
	}
#undef LEVEL
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

/*
 * Draws [bar text] in width w. Line style fills with ━ in half-cell steps (╸) over a dim ─ track;
 * block style uses 1/8-cell left blocks; ASCII uses | (or one glyph per segment without color).
 */
static void bar(int r, int c, int w, const struct seg *segs, int nseg, const char *text,
		unsigned char text_color, const struct ui *ui)
{
	int inner = w - 2, tl = (int)strlen(text), bw, filled = 0;
	int style = ui->color ? ui->bars : BARS_ASCII;
	int sub = style == BARS_BLOCKS ? 8 : style == BARS_LINE ? 2 : 1; /* sub-steps per cell */
	unsigned char full = style == BARS_BLOCKS ? GLYPH_BASE + 8
			     : style == BARS_LINE ? GLYPH_LINE_FULL : '|';
	double acc = 0;

	if (inner < 1)
		return;
	if (tl > inner)
		tl = inner;
	bw = inner - tl;
	if (style == BARS_LINE && bw > 1)
		bw--; /* keep one blank between the track and the text */
	put(r, c, C_DIM, "[");
	put(r, c + w - 1, C_DIM, "]");
	if (style == BARS_LINE)
		for (int k = 0; k < bw; k++) {
			F->ch[r][c + 1 + k] = GLYPH_LINE_TRACK;
			F->co[r][c + 1 + k] = C_DIM;
		}
	for (int i = 0; i < nseg; i++) {
		int last = i == nseg - 1, steps, end;

		acc += segs[i].pct;
		if (acc > 100)
			acc = 100;
		/* only the last segment gets a partial cell; inner boundaries round to whole cells */
		steps = iround(acc / 100.0 * bw * sub);
		end = last ? steps / sub : iround(acc / 100.0 * bw);
		for (; filled < end && filled < bw; filled++) {
			F->ch[r][c + 1 + filled] = style == BARS_ASCII && !ui->color
						    ? (unsigned char)segs[i].mono : full;
			F->co[r][c + 1 + filled] = segs[i].color;
		}
		if (last && sub > 1 && filled < bw && steps % sub) {
			F->ch[r][c + 1 + filled] = style == BARS_BLOCKS
						    ? (unsigned char)(GLYPH_BASE + steps % sub)
						    : GLYPH_LINE_HALF;
			F->co[r][c + 1 + filled] = segs[i].color;
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
	char up[32], clock[16], load[64] = "";
	time_t now = time(NULL);
	struct tm tm;
	int c, room;

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

static int cur_box_rows = BOX_SHORT; /* set by render() from the planned layout */

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
	case D_BOX: {
		const struct history *h = &v->sp->hist;
		char title[32];

		if (khz)
			snprintf(title, sizeof title, " %d  %3.0f%%  %uMHz ", cpu, busy, khz / 1000);
		else
			snprintf(title, sizeof title, " %d  %3.0f%%  idle ", cpu, busy);
		box(r, c, BOX_W, cur_box_rows, title, util_color(busy), ui);
		{
			/* the whole line takes the color of the current load */
			const uint8_t *const ring[1] = {h->core[cpu]};
			const unsigned char col[1] = {util_color(busy)};
			plot_lines(r + 1, cur_box_rows - 2, c + 1, BOX_W - 2, 1, ring, col, h, ui);
		}
		break;
	}
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

static const int cell_w[D_COUNT] = {BOX_W + 1, 24, 16, 9}; /* boxes get a 1-col gap */

static int cores_per_row(int density, int cols)
{
	int per = (cols - LABEL_W) / cell_w[density];
	return per < 1 ? 1 : per;
}

static int core_rows(const struct topo *t, int density, int cols, int box_rows)
{
	int per, rows = 0;

	if (density >= D_COUNT)
		return 0;
	per = cores_per_row(density, cols);
	for (int g = 0; g < t->ngroups; g++)
		rows += (t->groups[g].n + per - 1) / per;
	return rows * (density == D_BOX ? box_rows : 1);
}

static int panel_cores(int r, const struct view *v, const struct ui *ui)
{
	const struct topo *t = &v->sp->topo;
	int w = cell_w[ui->density];
	int per = cores_per_row(ui->density, F->cols);

	for (int g = 0; g < t->ngroups; g++) {
		const struct core_group *grp = &t->groups[g];
		int step = ui->density == D_BOX ? cur_box_rows : 1;
		put(r, 1, C_TITLE, grp->name);
		for (int i = 0; i < grp->n; i++) {
			if (i && i % per == 0)
				r += step;
			core_cell(r, LABEL_W + (i % per) * w, grp->cpus[i], v, ui);
		}
		r += step;
	}
	return r;
}

static int panel_cpu_total(int r, const struct view *v, const struct ui *ui)
{
	const struct sample *s = v->s;
	struct seg segs[3] = {
		{v->tot_user, C_GREEN, '|'}, {v->tot_sys, C_RED, '#'}, {v->tot_iowait, C_DIM, '.'}};
	double busy = v->tot_user + v->tot_sys;
	char text[16], info[80];
	int bw;

	snprintf(text, sizeof text, "%.1f%%", busy);
	if (F->cols >= 110)
		snprintf(info, sizeof info, "  usr %.1f%% sys %.1f%% io %.1f%%  psi %.1f%%  %d cores",
			 v->tot_user, v->tot_sys, v->tot_iowait, s->psi_cpu < 0 ? 0 : s->psi_cpu,
			 v->sp->topo.ncpu);
	else if (s->psi_cpu >= 0)
		snprintf(info, sizeof info, "  psi %.1f%%  %d cores", s->psi_cpu, v->sp->topo.ncpu);
	else
		snprintf(info, sizeof info, "  %d cores", v->sp->topo.ncpu);
	put(r, 1, C_TITLE, "CPU");
	bw = F->cols - LABEL_W - (int)strlen(info) - 1;
	bar(r, LABEL_W, bw, segs, 3, text, C_BOLD, ui);
	put(r, LABEL_W + bw, C_DEF, info);
	return r + 1;
}

#define REASON_IDLE 0x1
#define REASON_SW_POWER_CAP 0x4
#define REASON_SLOWDOWN 0xe8 /* hw-slowdown, sw-thermal, hw-thermal, hw-power-brake */

static const struct {
	uint64_t bit;
	const char *name;
} reasons[] = {
	{0x8, "hw-slowdown"}, {0x20, "sw-thermal"}, {0x40, "hw-thermal"}, {0x80, "hw-power-brake"},
};

static int panel_gpu(int r, int rows, const struct view *v, const struct ui *ui)
{
	const struct gpu_sample *g = &v->s->gpu;
	const char *name = v->sp->gpu.name;
	int busy = (g->have & GF_UTIL) && g->util > 0;
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
	if (g->have & GF_PINST)
		n += snprintf(info + n, sizeof info - (size_t)n, "  %.1fW", g->power_inst_mw / 1000.0);
	else if (g->have & GF_POWER)
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
	if (rows < 2)
		return r;

	/*
	 * Status row. The driver reports "SW power cap" and power-cap violation time while it merely
	 * holds idle clocks down, so those only count as throttling when the GPU is busy.
	 */
	c = put(r, LABEL_W, C_DIM, "throttle: ");
	if (!(g->have & GF_REASONS)) {
		c = put(r, c, C_DIM, "n/a");
	} else if (g->reasons & REASON_SLOWDOWN) {
		for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++)
			if (g->reasons & reasons[i].bit)
				c = putf(r, c, C_RED, "%s ", reasons[i].name);
		if (g->reasons & REASON_SW_POWER_CAP)
			c = put(r, c, C_YELLOW, "sw-power-cap ");
	} else if ((g->reasons & REASON_SW_POWER_CAP) && busy) {
		c = put(r, c, C_YELLOW, "sw-power-cap ");
	} else if (!busy) {
		c = put(r, c, C_DIM, "idle ");
	} else {
		c = put(r, c, C_GREEN, "none ");
	}
	c = put(r, c + 1, C_DIM, "power:");
	if (g->have & GF_POWER)
		c = putf(r, c + 1, C_DEF, "avg %.1fW", g->power_mw / 1000.0);
	c = put(r, c + 1, C_DIM, "cap n/a");
	if (busy && (g->have & GF_VIOL))
		putf(r, c + 2, g->capped_pct > 0 ? C_YELLOW : C_DIM, "capped %.0f%%", g->capped_pct);
	return r + 1;
}

/*
 * nvtop-style box: compute utilization and system memory use as two lines, two columns per tick as in
 * nvtop, newest at the right.
 */
static int panel_gpu_graph(int r, int rows, const struct view *v, const struct ui *ui)
{
	const struct history *h = &v->sp->hist;
	const struct gpu_sample *g = &v->s->gpu;
	const char *name = v->sp->gpu.name;
	int inner = rows - 2, w = F->cols - 2 - 6 - 1, left = 1 + 6;
	char title[160], used[16], total[16];

	if (strncmp(name, "NVIDIA ", 7) == 0)
		name += 7;
	fmt_kib(used, sizeof used, v->m_used);
	fmt_kib(total, sizeof total, v->m_total);
	snprintf(title, sizeof title, " %.40s  compute %u%% (green)  memory %u%% %s/%s (yellow) ", name,
		 (g->have & GF_UTIL) ? g->util : 0, history_at(h->mem, h, 0), used, total);
	box(r, 1, F->cols - 2, rows, title, C_BOLD, ui);
	putf(r + 1, 2, C_DIM, "%4d%%", 100);
	putf(r + rows - 2, 2, C_DIM, "%4d%%", 0);
	if (inner >= 4) /* the row whose value is nearest 50 % under the rows-1 mapping */
		putf(r + 1 + (inner - 1) / 2, 2, C_DIM, "%4d%%", 100 - 100 * ((inner - 1) / 2) / (inner - 1));
	{
		const uint8_t *const ring[2] = {h->gpu, h->mem};
		const unsigned char col[2] = {C_GREEN, C_YELLOW};
		plot_lines(r + 1, inner, left, w, 2, ring, col, h, ui);
	}
	return r + rows;
}

static int panel_mem(int r, int rows, const struct view *v, const struct ui *ui)
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
	if (rows < 2)
		return r;

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

static const char *cluster_name(const struct topo *t, int kind)
{
	/* fastest cluster first in topo, so P-cores map to group 0 when two types exist */
	if (t->ngroups >= 2)
		return t->groups[kind == Z_PCORE ? 0 : 1].name;
	return kind == Z_PCORE ? "pcore" : "ecore";
}

static int panel_temp(int r, int rows, const struct view *v)
{
	const struct sample *s = v->s;
	const struct thermal *th = &v->sp->th;
	int c = LABEL_W, labeled = 0;

	put(r, 1, C_TITLE, "Temp");
	if (v->cpu_temp_mc != TEMP_NONE)
		c = putf(r, c, temp_color(v->cpu_temp_mc), "cpu %dC%s  ", v->cpu_temp_mc / 1000,
			 v->cpu_temp_labeled ? "" : "?");
	if (s->gpu.have & GF_TEMP)
		c = putf(r, c, temp_color((int)s->gpu.temp_c * 1000), "gpu %uC  ", s->gpu.temp_c);
	if (s->nvme_mc != TEMP_NONE)
		c = putf(r, c, temp_color(s->nvme_mc), "nvme %dC  ", s->nvme_mc / 1000);
	for (int i = 0; i < s->nzones; i++)
		labeled |= th->zone[i].kind != Z_OTHER;
	if (!s->nzones || rows < 2)
		return r + 1;
	r++;
	c = LABEL_W;
	if (!labeled) {
		c = put(r, c, C_DIM, "zones");
		for (int i = 0; i < s->nzones; i++)
			if (s->zone_mc[i] != TEMP_NONE)
				c = putf(r, c + 1, temp_color(s->zone_mc[i]), "%d", s->zone_mc[i] / 1000);
		return r + 1;
	}
	/* one item per sensor kind; clustered sensors print as "name c0/c1" */
	for (int kind = Z_SOC; kind <= Z_CPUISH; kind++) {
		const char *name = NULL;
		int first = 1;

		for (int i = 0; i < s->nzones; i++) {
			const struct zone_info *z = &th->zone[i];
			int mc = s->zone_mc[i];

			if (z->kind != kind || mc == TEMP_NONE)
				continue;
			if (first) {
				name = kind == Z_PCORE || kind == Z_ECORE ? cluster_name(&v->sp->topo, kind)
				       : kind == Z_GPU ? "gpu(acpi)" : z->label;
				c = putf(r, c, C_DIM, "%s ", name);
				first = 0;
			} else {
				c = put(r, c, C_DIM, "/");
			}
			c = putf(r, c, temp_color(mc), "%d", mc / 1000);
		}
		if (!first)
			c = put(r, c, C_DEF, "  ");
	}
	for (int i = 0; i < s->nzones; i++)
		if (th->zone[i].kind == Z_OTHER && s->zone_mc[i] != TEMP_NONE)
			c = putf(r, c, temp_color(s->zone_mc[i]), "%s %d  ", th->zone[i].label,
				 s->zone_mc[i] / 1000);
	return r + 1;
}

static const char *const sort_names[SORT_COUNT] = {"cpu", "gpu", "rss"};

/* Header + rows + summary; takes whatever vertical room is left, down to nothing. */
static int panel_procs(int r, int avail, const struct view *v, const struct ui *ui)
{
	const struct sample *s = v->s;
	int limit = avail - 2, c;
	char rss[16], gpu[16], sum[16], resid[16];

	if (!ui->show_procs || !s->nprocs || avail < PROC_MIN_ROWS)
		return r;
	if (ui->once && limit > PROC_ROWS_ONCE)
		limit = PROC_ROWS_ONCE;
	if (limit > v->nproc)
		limit = v->nproc;
	put(r, 1, C_TITLE, "Procs");
	if (v->sp->gpu_on)
		putf(r, LABEL_W, C_BOLD, "%7s  %-10s %6s %5s %7s %7s  %-15s %s", "PID", "USER", "CPU%",
		     "GPU%", "RSS", "GPUMEM", "NAME", "COMMAND");
	else
		putf(r, LABEL_W, C_BOLD, "%7s  %-10s %6s %7s  %-15s %s", "PID", "USER", "CPU%", "RSS",
		     "NAME", "COMMAND");
	r++;
	for (int i = 0; i < limit; i++, r++) {
		const struct view_proc *p = &v->procs[i];
		fmt_kib(rss, sizeof rss, p->rss_kib);
		c = putf(r, LABEL_W, C_DEF, "%7u  %-10.10s ", p->pid, p->user);
		c = putf(r, c, util_color(p->cpu_pct), "%5.1f%%", p->cpu_pct);
		if (v->sp->gpu_on) {
			if (p->gpu_pct > 0)
				c = putf(r, c, util_color(p->gpu_pct), " %3.0f%%", p->gpu_pct);
			else
				c = put(r, c, C_DEF, p->gpu_bytes ? "   0%" : "     ");
			if (p->gpu_bytes)
				fmt_kib(gpu, sizeof gpu, p->gpu_bytes / 1024);
			else
				gpu[0] = '\0';
			c = putf(r, c, C_DEF, " %7s ", rss);
			c = putf(r, c, C_MAGENTA, "%7s", gpu);
		} else {
			c = putf(r, c, C_DEF, " %7s", rss);
		}
		c = putf(r, c, p->state == 'R' ? C_BOLD : C_DEF, "  %-15.15s", p->name);
		if (p->cmd && *p->cmd)
			put(r, c + 1, C_DIM, p->cmd);
	}
	if (!limit)
		put(r++, LABEL_W, C_DIM, "(none)");
	c = putf(r, LABEL_W, C_DIM, "%d procs, sort %s", s->nprocs, sort_names[ui->sort]);
	if (v->sp->gpu_on) {
		fmt_kib(sum, sizeof sum, v->gpu_proc_kib);
		fmt_kib(resid, sizeof resid, v->m_gpu);
		putf(r, c, C_DIM, "  |  %d on gpu: nvml %s, meminfo resid %s", v->gpu_proc_count, sum,
		     resid);
	}
	return r + 1;
}

static void panel_footer(int r, const struct view *v, const struct ui *ui)
{
	char rss[16];
	int c;

	fmt_kib(rss, sizeof rss, v->s->self_rss_kb);
	if (F->cols >= 80)
		c = putf(r, 1, C_DIM, "q quit  +/- %.2gs  p pause  g procs  s sort  c cells", ui->interval);
	else
		c = putf(r, 1, C_DIM, "q +/- %.2gs p g s c", ui->interval);
	if (ui->paused)
		c = put(r, c + 2, C_YELLOW, "PAUSED");
	if (c + 20 <= F->cols)
		putf(r, F->cols - 19, C_DIM, "self %5.2f%% %6s", v->self_cpu_pct, rss);
}

static int base_rows(const struct sample *s, const struct layout *L)
{
	return L->header + 1 + L->gpu_rows + L->gpu_graph + L->mem_rows +
	       (s && s->mem.swap_total ? 1 : 0) + L->temp_rows + L->footer;
}

/*
 * Starts from the richest layout and removes one thing at a time until everything fits: core boxes
 * (6 → 4 rows → plain cells), GPU box (16 → 12 → 8 rows → off), core cell density, the process list,
 * the core panel, the second temperature row, footer, header, temperature, the memory detail row,
 * the GPU status line. CPU, GPU and Mem bars always stay.
 */
void plan_layout(const struct sampler *sp, const struct sample *last, const struct ui *ui, int rows,
		 int cols, struct layout *L)
{
	int graphs = ui->graphs && !ui->once;
	int want_procs = ui->show_procs ? PROC_MIN_ROWS : 0;

	memset(L, 0, sizeof *L);
	if (cols < MIN_COLS || rows < MIN_ROWS) {
		L->too_small = 1;
		return;
	}
	L->header = L->footer = 1;
	L->mem_rows = 2;
	L->gpu_rows = sp->gpu_on ? 2 : 0; /* bar line plus the throttle/power status line */
	L->temp_rows = sp->th.nzones ? 2 : 1;
	L->density = ui->density;
	L->box_rows = BOX_TALL;
	if (graphs && ui->density == D_FULL && cols >= LABEL_W + 2 * cell_w[D_BOX])
		L->density = D_BOX;
	L->gpu_graph = graphs && sp->gpu_on ? GPU_BOX_XL : 0;

	for (;;) {
		int extras = L->gpu_graph || L->density == D_BOX;
		int need = base_rows(last, L) + core_rows(&sp->topo, L->density, cols, L->box_rows);
		int min_procs = want_procs && extras ? GRAPH_MIN_PROCS : want_procs;

		if (need + min_procs <= rows) {
			L->proc_rows = ui->show_procs ? rows - need : 0;
			if (L->proc_rows < PROC_MIN_ROWS)
				L->proc_rows = 0;
			return;
		}
		/* the GPU graph is worth more than core graphs: shrink the core boxes first, then the GPU box */
		if (L->density == D_BOX && L->box_rows == BOX_TALL)
			L->box_rows = BOX_SHORT;
		else if (L->density == D_BOX)
			L->density = D_FULL;
		else if (L->gpu_graph == GPU_BOX_XL)
			L->gpu_graph = GPU_BOX_TALL;
		else if (L->gpu_graph == GPU_BOX_TALL)
			L->gpu_graph = GPU_BOX_SHORT;
		else if (L->gpu_graph)
			L->gpu_graph = 0;
		else if (L->density == D_FULL)
			L->density = D_BAR;
		else if (L->density == D_BAR)
			L->density = D_COMPACT;
		else if (want_procs)
			want_procs = 0;
		else if (L->density == D_COMPACT)
			L->density = D_COUNT;
		else if (L->temp_rows == 2)
			L->temp_rows = 1;
		else if (L->footer)
			L->footer = 0;
		else if (L->header)
			L->header = 0;
		else if (L->temp_rows)
			L->temp_rows = 0;
		else if (L->mem_rows == 2)
			L->mem_rows = 1;
		else if (L->gpu_rows == 2)
			L->gpu_rows = 1; /* last resort: drop the status line so CPU, GPU, Mem fit in 3 rows */
		else {
			L->too_small = 1;
			return;
		}
	}
}

void render(struct frame *f, const struct view *v, const struct ui *ui)
{
	struct layout L;
	struct ui eff = *ui;
	int r = 0;

	F = f;
	clear(f);
	plan_layout(v->sp, v->s, ui, ui->once ? MAX_ROWS : f->rows, f->cols, &L);
	if (L.too_small) {
		putf(f->rows / 2, 1, C_YELLOW, "terminal %dx%d is too small (need %dx%d)", f->cols, f->rows,
		     MIN_COLS, MIN_ROWS);
		return;
	}
	eff.density = L.density;
	cur_box_rows = L.box_rows;
	ui = &eff;
	if (L.header)
		r = panel_header(r, v, ui);
	if (L.density < D_COUNT)
		r = panel_cores(r, v, ui);
	r = panel_cpu_total(r, v, ui);
	r = panel_gpu(r, L.gpu_rows, v, ui);
	if (L.gpu_graph)
		r = panel_gpu_graph(r, L.gpu_graph, v, ui);
	r = panel_mem(r, L.mem_rows, v, ui);
	if (L.temp_rows)
		r = panel_temp(r, L.temp_rows, v);
	if (L.proc_rows)
		r = panel_procs(r, L.proc_rows, v, ui);
	if (L.footer)
		panel_footer(f->rows - 1, v, ui);
}

static const char *const sgr[] = {
	[C_DEF] = "\033[0m",      [C_RED] = "\033[0;31m",  [C_GREEN] = "\033[0;32m",
	[C_YELLOW] = "\033[0;33m", [C_MAGENTA] = "\033[0;38;5;164m",
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
		uint16_t ch = f->ch[r][c];
		unsigned char co = f->co[r][c];
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
		if (ch >= GLYPH_LINE_FULL) {
			static const char *const lines[12] = {"\u2501", "\u2578", "\u2500", "?", "\u2502",
							      "\u250c", "\u2510", "\u2514", "\u2518", "\u252c",
							      "\u2534", "\u253c"};
			s = lines[ch - GLYPH_LINE_FULL];
			len = strlen(s);
			memcpy(out + n, s, len);
			n += len;
		} else if (ch >= GLYPH_BASE) {
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

		if (!full && memcmp(f->ch[r], prev->ch[r], w * sizeof f->ch[r][0]) == 0 &&
		    memcmp(f->co[r], prev->co[r], w) == 0)
			continue;
		if (cap - n < 16)
			break;
		n += (size_t)snprintf(out + n, cap - n, "\033[%d;1H", r + 1);
		/* skip the bottom-right cell so the terminal never scrolls */
		n = emit_row(f, r, r == f->rows - 1 ? f->cols - 1 : f->cols, ui, out, n, cap);
		memcpy(prev->ch[r], f->ch[r], w * sizeof f->ch[r][0]);
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
		if (g->have & GF_PINST)
			jp(&b, ",\"power_instant_w\":%.2f", g->power_inst_mw / 1000.0);
		else
			jp(&b, ",\"power_instant_w\":null");
		if (g->have & GF_VIOL)
			jp(&b, ",\"power_capped_pct\":%.1f", g->capped_pct);
		else
			jp(&b, ",\"power_capped_pct\":null");
		jp(&b, ",\"procs\":[");
		for (int i = 0; i < g->nproc; i++)
			jp(&b, "%s{\"pid\":%u,\"mem_mib\":%llu,\"sm_pct\":%u}", i ? "," : "",
			   g->procs[i].pid, (unsigned long long)(g->procs[i].mem_bytes >> 20),
			   g->procs[i].sm_pct);
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
	jp(&b, ",\"cpu_source\":\"%s\",\"zones\":[", v->cpu_temp_labeled ? "labeled" : "max_zone");
	for (int i = 0; i < s->nzones; i++) {
		jp(&b, "%s{\"label\":", i ? "," : "");
		jstr(&b, v->sp->th.zone[i].label);
		if (s->zone_mc[i] == TEMP_NONE)
			jp(&b, ",\"c\":null}");
		else
			jp(&b, ",\"c\":%.1f}", s->zone_mc[i] / 1000.0);
	}
	jp(&b, "]},\"procs\":[");
	for (int i = 0; i < v->nproc && i < PROC_ROWS_JSON; i++) {
		const struct view_proc *p = &v->procs[i];
		jp(&b, "%s{\"pid\":%u,\"user\":", i ? "," : "", p->pid);
		jstr(&b, p->user);
		jp(&b, ",\"cpu_pct\":%.2f,\"gpu_pct\":%.0f,\"rss_kib\":%llu,\"gpu_mib\":%llu,\"name\":",
		   p->cpu_pct, p->gpu_pct, (unsigned long long)p->rss_kib,
		   (unsigned long long)(p->gpu_bytes >> 20));
		jstr(&b, p->name);
		jp(&b, ",\"cmd\":");
		jstr(&b, p->cmd ? p->cmd : "");
		jp(&b, "}");
	}
	jp(&b, "],\"self\":{\"cpu_pct\":%.3f,\"rss_kib\":%llu}}\n", v->self_cpu_pct,
	   (unsigned long long)s->self_rss_kb);
	return b.n;
}

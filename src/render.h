#ifndef EDGETOP_RENDER_H
#define EDGETOP_RENDER_H

#include <stddef.h>
#include <stdint.h>

#include "sample.h"

#define MAX_ROWS 200
#define MAX_COLS 400

/* C_MAGENTA is 256-color index 164: palette magenta (35) renders purple in most themes. */
enum color { C_DEF, C_RED, C_GREEN, C_YELLOW, C_MAGENTA, C_CYAN, C_DIM, C_BOLD, C_TITLE };

/* Frame cells hold ASCII or one of these glyph codes. GLYPH_BASE + n is the n/8 left block. */
#define GLYPH_BASE 0x80
/* Line-style bars: full ━, half ╸, empty track ─ (─ doubles as the box horizontal). */
#define GLYPH_LINE_FULL 0xa0
#define GLYPH_LINE_HALF 0xa1
#define GLYPH_LINE_TRACK 0xa2
/* Box drawing: │ ┌ ┐ └ ┘ */
#define GLYPH_BOX_V 0xa4
#define GLYPH_BOX_TL 0xa5
#define GLYPH_BOX_TR 0xa6
#define GLYPH_BOX_BL 0xa7
#define GLYPH_BOX_BR 0xa8
/* Junctions where one graph line's vertical run crosses another line: ┬ ┴ ┼ */
#define GLYPH_BOX_TT 0xa9
#define GLYPH_BOX_BT 0xaa
#define GLYPH_BOX_X 0xab

enum bar_style { BARS_ASCII, BARS_LINE, BARS_BLOCKS };

struct frame {
	int rows, cols;
	uint16_t ch[MAX_ROWS][MAX_COLS];
	unsigned char co[MAX_ROWS][MAX_COLS];
};

/* D_BOX is a box per core with a line graph inside (6 or 4 rows); chosen automatically when there is room. */
enum density { D_BOX, D_FULL, D_BAR, D_COMPACT, D_COUNT };

/* What fits on the current screen, from the richest layout down to CPU/GPU/Mem alone. */
struct layout {
	int header, footer;
	int density;    /* enum density, or D_COUNT when the core panel is hidden */
	int box_rows;   /* rows per core box when density is D_BOX: 6 or 4 */
	int mem_rows;   /* 2 or 1 */
	int gpu_rows;   /* 2 (bar + status line) or 1; 0 without a GPU */
	int temp_rows;  /* 2, 1 or 0 */
	int gpu_graph;  /* rows for the GPU history box including borders: 16, 12, 8 or 0 */
	int proc_rows;  /* rows for the process panel, 0 when off */
	int too_small;
};

struct ui {
	int density;
	int graphs; /* allow core boxes and the GPU graph when the screen is large */
	int show_procs;
	int sort;
	int paused;
	int once;
	int color;
	int unicode; /* box-drawing and block glyphs instead of ASCII */
	int bars;    /* enum bar_style for the horizontal bars */
	double interval;
};

void render(struct frame *f, const struct view *v, const struct ui *ui);
void plan_layout(const struct sampler *sp, const struct sample *last, const struct ui *ui, int rows,
		 int cols, struct layout *out);
/* Encodes rows that differ from prev (all rows when full) into out; returns the byte count. */
size_t frame_encode(struct frame *f, struct frame *prev, int full, const struct ui *ui, char *out,
		    size_t cap);
/* Plain-text dump of the used rows, for --once. */
size_t frame_text(const struct frame *f, const struct ui *ui, char *out, size_t cap);
size_t render_json(const struct view *v, char *out, size_t cap);

#endif

#ifndef EDGETOP_RENDER_H
#define EDGETOP_RENDER_H

#include <stddef.h>

#include "sample.h"

#define MAX_ROWS 200
#define MAX_COLS 400

enum color { C_DEF, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_DIM, C_BOLD, C_TITLE };

/* Cells hold ASCII, or GLYPH_BASE + n for the n/8 left block in --unicode mode. */
#define GLYPH_BASE 0x80

struct frame {
	int rows, cols;
	unsigned char ch[MAX_ROWS][MAX_COLS];
	unsigned char co[MAX_ROWS][MAX_COLS];
};

enum density { D_FULL, D_BAR, D_COMPACT, D_COUNT };

struct ui {
	int density;
	int show_procs;
	int sort;
	int paused;
	int once;
	int color;
	int unicode;
	double interval;
};

void render(struct frame *f, const struct view *v, const struct ui *ui);
/* Rows the process panel would get on a rows x cols screen; < 3 means it is hidden. */
int proc_rows_available(const struct sampler *sp, const struct sample *last, const struct ui *ui,
			int rows, int cols);
/* Encodes rows that differ from prev (all rows when full) into out; returns the byte count. */
size_t frame_encode(struct frame *f, struct frame *prev, int full, const struct ui *ui, char *out,
		    size_t cap);
/* Plain-text dump of the used rows, for --once. */
size_t frame_text(const struct frame *f, const struct ui *ui, char *out, size_t cap);
size_t render_json(const struct view *v, char *out, size_t cap);

#endif

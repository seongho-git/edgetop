#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "term.h"

static const char ENTER[] = "\033[?1049h\033[?25l\033[2J";
static const char LEAVE[] = "\033[0m\033[?25h\033[?1049l";

static struct termios saved, raw;
static volatile int active;

static void put_seq(const char *s, size_t n)
{
	while (n) {
		ssize_t w = write(STDOUT_FILENO, s, n);
		if (w <= 0)
			return;
		s += w;
		n -= (size_t)w;
	}
}

int term_enter(void)
{
	if (tcgetattr(STDIN_FILENO, &saved) != 0)
		return -1;
	raw = saved;
	/* ISIG stays on so Ctrl-C/Ctrl-Z arrive as signals */
	raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	term_reenter_now();
	return 0;
}

void term_reenter_now(void)
{
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	put_seq(ENTER, sizeof ENTER - 1);
	active = 1;
}

void term_restore_now(void)
{
	if (!active)
		return;
	put_seq(LEAVE, sizeof LEAVE - 1);
	tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	active = 0;
}

void term_leave(void)
{
	term_restore_now();
}

void term_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	} else {
		*rows = 24;
		*cols = 80;
	}
}

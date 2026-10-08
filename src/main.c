#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "render.h"
#include "sample.h"
#include "term.h"
#include "util.h"

#define VERSION "0.1.0"
#define MIN_INTERVAL 0.25
#define MAX_INTERVAL 10.0
#define TIMER_SLACK_NS 5000000 /* lets the kernel merge our wakeup with other timers */

static struct sampler sp;
static struct sample samples[2];
static struct view view;
static struct frame frame, shown;
static char out[MAX_ROWS * MAX_COLS * 14];

static volatile sig_atomic_t g_quit, g_resize, g_cont;

static void on_quit(int sig)
{
	(void)sig;
	if (g_quit) {
		term_restore_now();
		_exit(130);
	}
	g_quit = 1;
}

static void on_winch(int sig)
{
	(void)sig;
	g_resize = 1;
}

static void on_tstp(int sig)
{
	(void)sig;
	term_restore_now();
	signal(SIGTSTP, SIG_DFL);
	raise(SIGTSTP);
}

static void on_cont(int sig);

static void install(int sig, void (*fn)(int))
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = fn;
	sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, NULL);
}

static void on_cont(int sig)
{
	(void)sig;
	install(SIGTSTP, on_tstp);
	term_reenter_now();
	g_cont = 1;
}

static void write_all(const char *p, size_t n)
{
	while (n) {
		ssize_t w = write(STDOUT_FILENO, p, n);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return;
		p += w;
		n -= (size_t)w;
	}
}

static void sleep_s(double s)
{
	struct timespec ts = {(time_t)s, (long)((s - (double)(time_t)s) * 1e9)};

	while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !g_quit)
		;
}

static void usage(FILE *fp)
{
	fprintf(fp,
		"usage: edgetop [options]\n"
		"  -d, --delay SEC    refresh interval, %.2g-%.0f s (default 1)\n"
		"      --once         print one sample as text and exit\n"
		"      --json         print one sample as JSON and exit\n"
		"      --watch SEC    print a JSON line every SEC seconds\n"
		"      --no-gpu       do not load NVML (saves ~15 MB)\n"
		"      --no-procs     skip the process list and its /proc scan\n"
		"      --no-color     disable colors (also honors NO_COLOR)\n"
		"      --unicode      draw bars with block glyphs\n"
		"      --bench N      time N sample+render ticks and exit\n"
		"  -h, --help         show this help\n"
		"  -V, --version      show version\n"
		"\nkeys: q quit, +/- interval, p pause, g process list, s sort (cpu/gpu/rss), c core cell density\n",
		MIN_INTERVAL, MAX_INTERVAL);
}

static double clamp_interval(double d)
{
	return d < MIN_INTERVAL ? MIN_INTERVAL : d > MAX_INTERVAL ? MAX_INTERVAL : d;
}

static int parse_seconds(const char *s, double *out_s)
{
	char *end;
	double d = strtod(s, &end);

	if (end == s || *end || !(d > 0))
		return -1;
	*out_s = d;
	return 0;
}

/* Takes two samples one window apart so utilization has a delta. */
static void sample_window(const struct ui *ui, double window)
{
	sampler_read(&sp, &samples[0], ui->show_procs);
	sleep_s(window);
	sp.procs_t = -1e9; /* rescan now so process cpu% covers this window */
	sampler_read(&sp, &samples[1], ui->show_procs);
	compute_view(&sp, &samples[0], &samples[1], ui->sort, &view);
}

static int run_once(struct ui *ui, int json)
{
	size_t n;

	sample_window(ui, ui->interval);
	if (json) {
		n = render_json(&view, out, sizeof out);
	} else {
		int rows;
		term_size(&rows, &frame.cols);
		if (!isatty(STDOUT_FILENO))
			frame.cols = 80;
		if (frame.cols > MAX_COLS)
			frame.cols = MAX_COLS;
		frame.rows = MAX_ROWS;
		render(&frame, &view, ui);
		n = frame_text(&frame, ui, out, sizeof out);
	}
	write_all(out, n);
	return 0;
}

static int run_watch(const struct ui *ui, double period)
{
	int cur = 0;

	prctl(PR_SET_TIMERSLACK, TIMER_SLACK_NS);
	install(SIGINT, on_quit);
	install(SIGTERM, on_quit);
	sampler_read(&sp, &samples[cur], ui->show_procs);
	while (!g_quit) {
		sleep_s(period);
		if (g_quit)
			break;
		cur ^= 1;
		sampler_read(&sp, &samples[cur], ui->show_procs);
		compute_view(&sp, &samples[cur ^ 1], &samples[cur], ui->sort, &view);
		write_all(out, render_json(&view, out, sizeof out));
	}
	return 0;
}

static int run_bench(struct ui *ui, int n)
{
	double t0, ts = 0, tr = 0;
	double cpu0, cpu1;
	int cur = 0;

	frame.rows = 24;
	frame.cols = 80;
	sampler_read(&sp, &samples[cur], ui->show_procs);
	cpu0 = samples[cur].self_cpu_s;
	t0 = mono_now();
	for (int i = 0; i < n; i++) {
		double a = mono_now(), b;
		cur ^= 1;
		sampler_read(&sp, &samples[cur], ui->show_procs);
		compute_view(&sp, &samples[cur ^ 1], &samples[cur], ui->sort, &view);
		b = mono_now();
		render(&frame, &view, ui);
		frame_encode(&frame, &shown, 0, ui, out, sizeof out);
		ts += b - a;
		tr += mono_now() - b;
	}
	sampler_read(&sp, &samples[cur ^ 1], ui->show_procs);
	cpu1 = samples[cur ^ 1].self_cpu_s;
	printf("ticks %d  wall %.1f us/tick  (sample %.1f, render %.1f)  cpu %.1f us/tick  rss %llu KiB  gpu %s\n",
	       n, (mono_now() - t0) / n * 1e6, ts / n * 1e6, tr / n * 1e6, (cpu1 - cpu0) / n * 1e6,
	       (unsigned long long)samples[cur ^ 1].self_rss_kb, sp.gpu_on ? "on" : "off");
	return 0;
}

static void draw(const struct ui *ui, int full)
{
	render(&frame, &view, ui);
	write_all(out, frame_encode(&frame, &shown, full, ui, out, sizeof out));
}

/* Returns 1 when the frame must be redrawn. */
static int handle_keys(struct ui *ui, double *deadline, double last)
{
	char buf[32];
	ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
	int redraw = 0;

	for (ssize_t i = 0; i < n; i++) {
		switch (buf[i]) {
		case 'q':
		case 'Q':
			g_quit = 1;
			break;
		case '+':
		case '=':
			ui->interval = clamp_interval(ui->interval * 2);
			*deadline = last + ui->interval;
			redraw = 1;
			break;
		case '-':
		case '_':
			ui->interval = clamp_interval(ui->interval / 2);
			*deadline = last + ui->interval;
			redraw = 1;
			break;
		case 'p':
		case 'P':
		case ' ':
			ui->paused = !ui->paused;
			*deadline = mono_now();
			redraw = 1;
			break;
		case 'g':
		case 'G':
			ui->show_procs = !ui->show_procs;
			redraw = 1;
			break;
		case 'c':
		case 'C':
			ui->density = (ui->density + 1) % D_COUNT;
			redraw = 1;
			break;
		case 's':
		case 'S':
			ui->sort = (ui->sort + 1) % SORT_COUNT;
			redraw = 1;
			break;
		case 0x1b: /* swallow escape sequences (arrows, mouse) */
			i = n;
			break;
		}
	}
	return redraw;
}

/* Skip the /proc scan entirely when the panel is off or the screen has no room for it. */
static int want_procs(const struct ui *ui)
{
	return ui->show_procs &&
	       proc_rows_available(&sp, view.s, ui, frame.rows, frame.cols) >= 3;
}

static int run_tui(struct ui *ui)
{
	int cur = 0, full = 1;
	double deadline, last;

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "edgetop: not a terminal; use --once or --json\n");
		return 1;
	}
	prctl(PR_SET_TIMERSLACK, TIMER_SLACK_NS);
	install(SIGINT, on_quit);
	install(SIGTERM, on_quit);
	install(SIGHUP, on_quit);
	install(SIGWINCH, on_winch);
	install(SIGTSTP, on_tstp);
	install(SIGCONT, on_cont);
	if (term_enter() != 0) {
		fprintf(stderr, "edgetop: cannot configure terminal\n");
		return 1;
	}
	term_size(&frame.rows, &frame.cols);

	sampler_read(&sp, &samples[cur], want_procs(ui));
	last = samples[cur].t;
	/* first frame after a short window so it shows real numbers */
	deadline = last + (ui->interval < 0.2 ? ui->interval : 0.2);

	while (!g_quit) {
		double now = mono_now();

		if (!ui->paused && now >= deadline) {
			cur ^= 1;
			sampler_read(&sp, &samples[cur], want_procs(ui));
			compute_view(&sp, &samples[cur ^ 1], &samples[cur], ui->sort, &view);
			last = samples[cur].t;
			deadline = last + ui->interval;
			draw(ui, full);
			full = 0;
			continue;
		}
		if (g_resize || g_cont) {
			g_resize = g_cont = 0;
			term_size(&frame.rows, &frame.cols);
			if (frame.rows > MAX_ROWS)
				frame.rows = MAX_ROWS;
			if (frame.cols > MAX_COLS)
				frame.cols = MAX_COLS;
			write_all("\033[2J", 4);
			if (view.s)
				draw(ui, 1);
			else
				full = 1;
			continue;
		}

		struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
		/* round up so we never wake just before the deadline and spin */
		int wait_ms = ui->paused ? -1 : (int)((deadline - now) * 1000.0) + 1;
		if (poll(&pfd, 1, wait_ms) > 0 && (pfd.revents & POLLIN) &&
		    handle_keys(ui, &deadline, last) && view.s && !g_quit) {
			/* sort or toggle changes need the view recomputed, not just redrawn */
			compute_view(&sp, &samples[cur ^ 1], &samples[cur], ui->sort, &view);
			draw(ui, 0);
		}
	}
	term_leave();
	return 0;
}

int main(int argc, char **argv)
{
	struct ui ui = {.density = D_FULL, .show_procs = 1, .sort = SORT_CPU, .color = 1, .interval = 1.0};
	int once = 0, json = 0, gpu = 1, bench = 0;
	double watch = 0;
	int rc;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val = i + 1 < argc ? argv[i + 1] : NULL;

		if ((!strcmp(a, "-d") || !strcmp(a, "--delay")) && val) {
			double d;
			if (parse_seconds(val, &d) != 0) {
				fprintf(stderr, "edgetop: invalid delay '%s'\n", val);
				return 2;
			}
			ui.interval = clamp_interval(d);
			i++;
		} else if (!strcmp(a, "--watch") && val) {
			if (parse_seconds(val, &watch) != 0) {
				fprintf(stderr, "edgetop: invalid watch period '%s'\n", val);
				return 2;
			}
			i++;
		} else if (!strcmp(a, "--bench") && val) {
			bench = atoi(val);
			if (bench < 1) {
				fprintf(stderr, "edgetop: invalid tick count '%s'\n", val);
				return 2;
			}
			i++;
		} else if (!strcmp(a, "--once")) {
			once = 1;
		} else if (!strcmp(a, "--json")) {
			json = 1;
		} else if (!strcmp(a, "--no-gpu")) {
			gpu = 0;
		} else if (!strcmp(a, "--no-procs")) {
			ui.show_procs = 0;
		} else if (!strcmp(a, "--no-color")) {
			ui.color = 0;
		} else if (!strcmp(a, "--unicode")) {
			ui.unicode = 1;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
			puts("edgetop " VERSION);
			return 0;
		} else {
			fprintf(stderr, "edgetop: unknown or incomplete option '%s'\n", a);
			usage(stderr);
			return 2;
		}
	}
	if (getenv("NO_COLOR") && *getenv("NO_COLOR"))
		ui.color = 0;
	if (once && !isatty(STDOUT_FILENO))
		ui.color = 0;
	ui.once = once || json;

	if (sampler_init(&sp, gpu) != 0) {
		fprintf(stderr, "edgetop: cannot read /proc\n");
		return 1;
	}
	if (bench)
		rc = run_bench(&ui, bench);
	else if (watch > 0)
		rc = run_watch(&ui, watch);
	else if (once || json)
		rc = run_once(&ui, json);
	else
		rc = run_tui(&ui);
	sampler_close(&sp);
	return rc;
}

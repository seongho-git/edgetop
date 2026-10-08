#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

double mono_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int open_ro(const char *path)
{
	return open(path, O_RDONLY | O_CLOEXEC);
}

ssize_t read_fd(int fd, char *buf, size_t cap)
{
	ssize_t r;

	if (fd < 0 || cap < 2) {
		errno = EBADF;
		return -1;
	}
	do
		r = pread(fd, buf, cap - 1, 0);
	while (r < 0 && errno == EINTR);
	if (r < 0)
		return -1;
	buf[r] = '\0';
	return r;
}

ssize_t read_path(const char *path, char *buf, size_t cap)
{
	int fd = open_ro(path);
	ssize_t r;
	int saved;

	if (fd < 0)
		return -1;
	r = read_fd(fd, buf, cap);
	saved = errno;
	close(fd);
	errno = saved;
	return r;
}

static const char *skip_blank(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

const char *parse_u64(const char *p, uint64_t *out)
{
	uint64_t v = 0;

	p = skip_blank(p);
	if (*p < '0' || *p > '9')
		return NULL;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + (uint64_t)(*p++ - '0');
	*out = v;
	return p;
}

const char *parse_i64(const char *p, int64_t *out)
{
	uint64_t v;
	int neg;

	p = skip_blank(p);
	neg = *p == '-';
	if (neg)
		p++;
	p = parse_u64(p, &v);
	if (!p)
		return NULL;
	*out = neg ? -(int64_t)v : (int64_t)v;
	return p;
}

const char *parse_dec(const char *p, double *out)
{
	int64_t ip;
	double frac = 0, scale = 0.1;
	int neg;

	p = skip_blank(p);
	neg = *p == '-';
	p = parse_i64(p, &ip);
	if (!p)
		return NULL;
	if (*p == '.') {
		for (p++; *p >= '0' && *p <= '9'; p++, scale *= 0.1)
			frac += (*p - '0') * scale;
	}
	*out = neg ? (double)ip - frac : (double)ip + frac;
	return p;
}

const char *next_line(const char *p)
{
	const char *nl = strchr(p, '\n');
	return nl ? nl + 1 : NULL;
}

void fmt_kib(char *dst, size_t n, uint64_t kib)
{
	double v = (double)kib;

	if (kib < 1024)
		snprintf(dst, n, "%uK", (unsigned)kib);
	else if (kib < 1024 * 1024)
		snprintf(dst, n, "%.0fM", v / 1024);
	else if (kib < 10ull * 1024 * 1024 * 1024)
		snprintf(dst, n, "%.1fG", v / (1024 * 1024));
	else
		snprintf(dst, n, "%.1fT", v / (1024.0 * 1024 * 1024));
}

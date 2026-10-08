#ifndef EDGETOP_UTIL_H
#define EDGETOP_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

double mono_now(void);
int open_ro(const char *path);
/* One pread at offset 0; a short read is treated as EOF (true for seq_file and sysfs). */
ssize_t read_fd(int fd, char *buf, size_t cap);
ssize_t read_path(const char *path, char *buf, size_t cap);

const char *parse_u64(const char *p, uint64_t *out);
const char *parse_i64(const char *p, int64_t *out);
const char *parse_dec(const char *p, double *out);
const char *next_line(const char *p);

/* Formats a KiB count as "512K", "170M", "98.3G". */
void fmt_kib(char *dst, size_t n, uint64_t kib);

#endif

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sample.h"
#include "util.h"

#define ZONE_PERIOD 2.0  /* ACPI _TMP evaluation, ~7 us per zone */
#define NVME_PERIOD 10.0 /* each read issues an NVMe admin command (~0.7 ms) */

static int read_mc(int fd)
{
	char buf[32];
	int64_t v;

	if (read_fd(fd, buf, sizeof buf) <= 0 || !parse_i64(buf, &v))
		return TEMP_NONE;
	return (int)v;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

void thermal_init(struct thermal *th)
{
	DIR *d;
	struct dirent *e;
	int ids[MAX_ZONES], n = 0;
	char path[300], buf[32];

	memset(th, 0, sizeof *th);
	th->nvme_fd = -1;
	th->nvme_last = TEMP_NONE;
	th->nvme_t = -1e9;
	th->zone_t = -1e9;

	if ((d = opendir("/sys/class/thermal"))) {
		while ((e = readdir(d)) && n < MAX_ZONES)
			if (strncmp(e->d_name, "thermal_zone", 12) == 0)
				ids[n++] = atoi(e->d_name + 12);
		closedir(d);
	}
	qsort(ids, (size_t)n, sizeof ids[0], cmp_int);
	for (int i = 0; i < n; i++) {
		snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", ids[i]);
		int fd = open_ro(path);
		if (fd >= 0)
			th->zone_fd[th->nzones++] = fd;
	}

	if ((d = opendir("/sys/class/hwmon"))) {
		while ((e = readdir(d)) && th->nvme_fd < 0) {
			if (e->d_name[0] == '.')
				continue;
			snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
			if (read_path(path, buf, sizeof buf) > 0 && strcmp(buf, "nvme\n") == 0) {
				snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp1_input", e->d_name);
				th->nvme_fd = open_ro(path);
			}
		}
		closedir(d);
	}
}

void thermal_read(struct thermal *th, struct sample *s)
{
	s->nzones = th->nzones;
	if (s->t - th->zone_t >= ZONE_PERIOD) {
		for (int i = 0; i < th->nzones; i++)
			th->zone_last[i] = read_mc(th->zone_fd[i]);
		th->zone_t = s->t;
	}
	memcpy(s->zone_mc, th->zone_last, sizeof th->zone_last);
	if (th->nvme_fd >= 0 && s->t - th->nvme_t >= NVME_PERIOD) {
		th->nvme_last = read_mc(th->nvme_fd);
		th->nvme_t = s->t;
	}
	s->nvme_mc = th->nvme_last;
}

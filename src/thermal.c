#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sample.h"
#include "util.h"

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

static int has(const char *s, const char *word)
{
	return strstr(s, word) != NULL;
}

/*
 * Names the zone from its ACPI path when the firmware uses the DGX Spark scheme
 * (TSOC, TS<cluster>E/P, TGPU, TUNC); otherwise from the zone type.
 */
static void classify(int id, struct zone_info *z)
{
	char path[128], buf[128], *nl, *name;

	z->kind = Z_OTHER;
	snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/device/path", id);
	if (read_path(path, buf, sizeof buf) > 0) {
		if ((nl = strchr(buf, '\n')))
			*nl = '\0';
		name = strrchr(buf, '.');
		name = name ? name + 1 : buf;
		if (!strcmp(name, "TSOC")) {
			z->kind = Z_SOC;
			strcpy(z->label, "soc");
			return;
		}
		if (!strcmp(name, "TGPU")) {
			z->kind = Z_GPU;
			strcpy(z->label, "gpu");
			return;
		}
		if (!strcmp(name, "TUNC")) {
			z->kind = Z_UNCORE;
			strcpy(z->label, "uncore");
			return;
		}
		if (name[0] == 'T' && name[1] == 'S' && name[2] >= '0' && name[2] <= '9' &&
		    (name[3] == 'E' || name[3] == 'P') && !name[4]) {
			z->kind = name[3] == 'P' ? Z_PCORE : Z_ECORE;
			snprintf(z->label, sizeof z->label, "c%c%c", name[2], name[3] == 'P' ? 'p' : 'e');
			return;
		}
	}
	snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", id);
	if (read_path(path, buf, sizeof buf) > 0) {
		if ((nl = strchr(buf, '\n')))
			*nl = '\0';
		snprintf(z->label, sizeof z->label, "%.11s", buf);
		if (has(buf, "gpu"))
			z->kind = Z_GPU;
		else if (has(buf, "cpu") || has(buf, "pkg") || has(buf, "soc") || has(buf, "core"))
			z->kind = Z_CPUISH;
	} else {
		snprintf(z->label, sizeof z->label, "z%d", id);
	}
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
		if (fd < 0)
			continue;
		classify(ids[i], &th->zone[th->nzones]);
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
	for (int i = 0; i < th->nzones; i++)
		s->zone_mc[i] = read_mc(th->zone_fd[i]);
	if (th->nvme_fd >= 0 && s->t - th->nvme_t >= NVME_PERIOD) {
		th->nvme_last = read_mc(th->nvme_fd);
		th->nvme_t = s->t;
	}
	s->nvme_mc = th->nvme_last;
}

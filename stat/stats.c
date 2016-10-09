#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stats.h"

#include "../day2epoch.h"

/* fill day info */
static void
stat_fill_dayinfo(const char *fn, int day, struct stat_dayinfo *di)
{
	struct stat st;
	char path[PATH_MAX];

	di->day = day;

	di->start = day2epoch(day);

	snprintf(path, PATH_MAX, "/%s", fn);
	stat(path, &st); /* FIXME: ignore error? */
	di->end = di->start + st.st_size / sizeof(struct stat_info);
}

/* statistics files handling */
int
stat_data_open(struct stat_data *sd)
{
	DIR *dir;
	struct dirent *de;
	time_t tmmax = 0;

	memset(sd, 0, sizeof(struct stat_data));

	dir = opendir("/");
	if (!dir) {
		fprintf(stderr, "Can't list data dir\n");
		return 0;
	}
	while ((de = readdir(dir))) {
		char name[PATH_MAX], buf[PATH_MAX];
		struct stat_set *stmp;
		const char ext[] = ".dat";
		char *day_start;
		size_t len, extlen, i;
		int daytmp, setidx;
		struct stat_dayinfo *dtmp;

		len = strlen(de->d_name);
		extlen = strlen(ext);

		if (len < 12) { /* X.DDMMYY.dat = 12 symbols */
			continue;
		}

		if (strncmp(de->d_name + len - extlen, ext, extlen) != 0) {
			/* not .dat file */
			continue;
		}

		memcpy(buf, de->d_name, len - extlen);
		buf[len - extlen] = '\0';

		day_start = strrchr(buf, '.');
		if (!day_start) {
			/* no "." in file name */
			continue;
		}

		memcpy(name, buf, day_start - buf);
		name[day_start - buf] = '\0';

		day_start++;
		daytmp = atoi(day_start);
		if (daytmp <= 0) {
			continue;
		}

		/* search for existing set with the same name */
		setidx = -1;
		for (i=0; i<sd->nsets; i++) {
			if (strcmp(name, sd->sets[i].name) == 0) {
				setidx = i;
				break;
			}
		}

		/* if not found, append new one */
		if (setidx < 0) {
			stmp = realloc(sd->sets, sizeof(struct stat_set) * (sd->nsets + 1));
			if (!stmp) {
				free(sd->sets);
				goto fail_dir;
			}
			sd->sets = stmp;

			/* clear new set */
			memset(&sd->sets[sd->nsets], 0, sizeof(struct stat_set));
			strncpy(sd->sets[sd->nsets].name, name, PATH_MAX);
			setidx = sd->nsets;
			sd->nsets++;
		}

		/* add day to data set */
		dtmp = realloc(sd->sets[setidx].days, sizeof(struct stat_dayinfo) * (sd->sets[setidx].ndays + 1));
		if (!dtmp) {
			free(sd->sets[setidx].days);
			goto fail_dir;
		}
		sd->sets[setidx].days = dtmp;

		/* mimimum time will be start of set */
		stat_fill_dayinfo(de->d_name, daytmp, &sd->sets[setidx].days[sd->sets[setidx].ndays]);
		if (sd->start == 0) {
			sd->start = sd->sets[setidx].days[sd->sets[setidx].ndays].start;
		} else if (sd->sets[setidx].days[sd->sets[setidx].ndays].start < sd->start) {
			sd->start = sd->sets[setidx].days[sd->sets[setidx].ndays].start;
		}

		if (sd->sets[setidx].days[sd->sets[setidx].ndays].end > tmmax) {
			tmmax = sd->sets[setidx].days[sd->sets[setidx].ndays].end;
		}

		sd->sets[setidx].ndays++;
	}
	closedir(dir);

	sd->nrec = tmmax - sd->start;

	return 1;

fail_dir:
	closedir(dir);
	return 0;
}

void
stat_data_close(struct stat_data *sd)
{
	size_t i;

	for (i=0; i<sd->nsets; i++) {
		if (sd->sets[i].days) {
			free(sd->sets[i].days);
			sd->sets[i].days = NULL;
		}
	}
	free(sd->sets);
	if (sd->f) {
		fclose(sd->f);
		sd->f = NULL;
	}
	sd->sets = NULL;
}

static void
stat_data_fetch(struct stat_data *sd, struct stat_dayinfo *dinfo, struct stat_info *info)
{
	int dayfound;
	size_t i, readres;
	char path[PATH_MAX];

	dayfound = 0;
	for (i=0; i<sd->sset->ndays; i++) {
		dinfo = &sd->sset->days[i];

		if ((sd->t > dinfo->start) && (sd->t < dinfo->end)) {
			/* found */
			dayfound = 1;
			break;
		}
	}

	/* not found */
	if (!dayfound) {
		/* zeros */
		goto empty;
	}

	sd->day = dinfo->day;
	if (!sd->f) {
		snprintf(path, PATH_MAX, "/%s.%06d.dat", sd->sset->name, sd->day);
		sd->f = fopen(path, "r");
		if (!sd->f) {
			goto empty;
		}

		fseek(sd->f, (sd->t - dinfo->start) * sizeof(struct stat_info), SEEK_SET);
	}

	readres = fread(info, 1, sizeof(struct stat_info), sd->f);
	if (readres != sizeof(struct stat_info)) {
		fclose(sd->f);
		sd->f = NULL;
		goto empty;
	}

	return;

empty:
	memset(info, 0, sizeof(struct stat_info));
}

int
stat_data_seek(struct stat_data *sd, char *mname, time_t seekto, struct stat_info *info)
{
	size_t i;
	struct stat_dayinfo *dinfo = NULL;

	/* search for module with name in *mname */
	sd->sset = NULL;
	for (i=0; i<sd->nsets; i++) {
		if (strcmp(sd->sets[i].name, mname) == 0) {
			sd->sset = &sd->sets[i];
			break;
		}
	}

	/* module not found */
	if (!sd->sset) {
		return 0;
	}

	/* search for day file */
	sd->t = seekto;

	stat_data_fetch(sd, dinfo, info);

	return 1;
}

int
stat_data_next(struct stat_data *sd, struct stat_info *info)
{
	size_t readres;
	struct stat_dayinfo *dinfo = NULL;

	sd->t++;

	if (!sd->f) {
		/* file not open */
		goto next_file;
	}

	if (feof(sd->f)) {
		fclose(sd->f);
		sd->f = NULL;
		goto next_file;
	}

	readres = fread(info, 1, sizeof(struct stat_info), sd->f);
	if (readres != sizeof(struct stat_info)) {
		fclose(sd->f);
		sd->f = NULL;
		goto empty;
	}
	return 1;

next_file:
	stat_data_fetch(sd, dinfo, info);

	return 1;

empty:
	memset(info, 0, sizeof(struct stat_info));

	return 1;
}


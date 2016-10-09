#ifndef stats_h_included
#define stats_h_included

#include <stdio.h>
#include <stdlib.h>

#include "../damper.h"

struct stat_dayinfo
{
	int day;            /* day (DDMMYY in file name) */
	time_t start, end;
};

struct stat_set
{
	char name[PATH_MAX];

	struct stat_dayinfo *days;
	size_t ndays;              /* number of days in set */
};

struct stat_data
{
	struct stat_set *sets;   /* data sets */
	size_t nsets;            /* number of sets */

	size_t nrec;             /* number of records in dataset */
	time_t start;            /* time of first day in statistics files */

	/* cursor */
	struct stat_set *sset;
	time_t t;                /* time */
	FILE *f;                 /* data file */
	int day;                 /* day */
};


int stat_data_open(struct stat_data *sd);
void stat_data_close(struct stat_data *sd);

int stat_data_seek(struct stat_data *sd, char *mname, time_t seekto, struct stat_info *info);
int stat_data_next(struct stat_data *sd, struct stat_info *info);

#endif


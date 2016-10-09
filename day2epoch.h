#ifndef day2epoch_h_included
#define day2epoch_h_included

static time_t
day2epoch(int day)
{
	struct tm tm;

	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_mday = day / 100 / 100;
	tm.tm_mon  = ((day / 100) % 100) - 1;
	tm.tm_year = (day % 100) + 100;

	return timegm(&tm);
}

#endif


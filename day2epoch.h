#ifndef day2epoch_h_included
#define day2epoch_h_included

/* hmm */
static pthread_mutex_t timegm_lock = PTHREAD_MUTEX_INITIALIZER;

static time_t
day2epoch(int day)
{
	struct tm tm;
	time_t res;

	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_mday = day / 100 / 100;
	tm.tm_mon  = ((day / 100) % 100) - 1;
	tm.tm_year = (day % 100) + 100;

	pthread_mutex_lock(&timegm_lock);
	res = timegm(&tm);
	pthread_mutex_unlock(&timegm_lock);

	return res;
}

#endif


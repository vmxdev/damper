/*
 * $ cc -Wall -pedantic damper.c modules.conf.c -o damper -lnetfilter_queue -pthread -lrt
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>

#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>

#include "damper.h"
#include "day2epoch.h"


#define BILLION ((uint64_t)1000000000)

#define KEEP_STAT 365    /* keep statistics about one year by default */
#define NFQ_DEFLEN 10000 /* internal queue length */

/* indicate termination by signal */
volatile sig_atomic_t damper_done = 0;

/* signal handler */
static void
on_term(int signum)
{
	damper_done = 1;
}

/* convert string with optional suffixes 'k', 'm' or 'g' (bits per second) to bytes per second */
static uint64_t
str2bps(const char *l)
{
	char unit;
	size_t len;
	int64_t res;
	uint64_t k;
	char lcopy[LINE_MAX];

	strncpy(lcopy, l, LINE_MAX);
	len = strlen(lcopy);
	if (len == 0) {
		return 0;
	}
	unit = tolower(lcopy[len - 1]);
	if (isdigit(unit)) {
		k = 1;
	} else if (unit == 'k') {
		k = 1000;
	} else if (unit == 'm') {
		k = 1000 * 1000;
	} else if (unit == 'g') {
		k = 1000 * 1000 * 1000;
	} else {
		/* unknown suffix */
		k = 0;
	}

	/* drop suffix */
	if (k != 1) {
		lcopy[len-1] = '\0';
	}

	res = strtoll(lcopy, NULL, 10);
	if ((res == INT64_MIN) || (res == INT64_MAX)) {
		k = 0;
	}

	return res * k / 8;
}

FILE *
fopen_or_create(const char *path)
{
	FILE *f;

	f = fopen(path, "r+");
	if (f) return f;
	f = fopen(path, "w+");
	if (f) return f;
	fprintf(stderr, "Can't open file '%s'\n", path);
	return NULL;
}

static void
stat_remove_old(struct userdata *u)
{
	DIR *dir;
	struct dirent *de;

	/* search for files by extension (.dat) and remove old */
	dir = opendir(u->statdir);
	if (!dir) {
		fprintf(stderr, "Can't list data dir %s\n", u->statdir);
		return;
	}

	while ((de = readdir(dir))) {
		char *ext, *day_start;
		char buf[PATH_MAX];
		const int extlen = 4;
		int day, days;
		size_t len;
		time_t t;

		len = strlen(de->d_name);
		if (len <= extlen) {
			/* file name too short */
			continue;
		}

		ext = de->d_name + (len - extlen);
		if (strstr(ext, ".dat") == NULL) {
			continue;
		}

		memcpy(buf, de->d_name, len - extlen);
		buf[len - extlen] = '\0';

		/* search for "." before extension */
		day_start = strrchr(buf, '.');
		if (!day_start) {
			continue;
		}
		day_start++;

		/* now day (DDMMYY in file name) is in buf */
		day = atoi(day_start);
		if (day <= 0) {
			continue;
		}

		/* calculate time_t for file */
		t = day2epoch(day);

		/* days between current timestamp and file */
		days = (u->curr_timestamp - t) / (60 * 60 * 24);
		if (days > u->keep_stat) {
			char path[PATH_MAX];

			snprintf(path, PATH_MAX, "%s/%s", u->statdir, de->d_name);
			fprintf(stderr, "Removing old statistics file '%s'\n", path);
			unlink(path);
		}
	}

	closedir(dir);
}

static void
stat_init(struct userdata *u)
{
	if (u->statdir[0] == '\0') {
		fprintf(stderr, "Directory for statistics is not set\n");
		goto fail;
	}

	u->curr_timestamp = time(NULL);
	u->old_timestamp = u->curr_timestamp;

	u->cday = 0;
	u->statf = NULL;
	u->daystart = 0;

	memset(&u->stat_info, 0, sizeof(u->stat_info));

	if (u->wchart) {
		size_t i;

		for (i=0; modules[i].name; i++) {
			modules[i].statf = NULL;
		}
	}

	return;
fail:
	u->stat = 0;
}

static void
stat_write(struct userdata *u)
{
	int day;
	struct tm *t;
	size_t i;

	/* get date in form DDMMYY */
	t = gmtime(&u->curr_timestamp);
	day = t->tm_mday * 100 * 100 + (t->tm_mon + 1) * 100 + (t->tm_year - 100);

	if (day != u->cday) {
		char path[PATH_MAX];

		/* day changed, close old file */
		if (u->statf) {
			fclose(u->statf);
			u->statf = NULL;
		}
		/* files in modules */
		if (u->wchart) {
			for (i=0; modules[i].name; i++) {
				if (modules[i].statf) {
					fclose(modules[i].statf);
					modules[i].statf = NULL;
				}
			}
		}

		/* and open new files */
		snprintf(path, PATH_MAX, "%s/dstat.%06d.dat", u->statdir, day);
		u->statf = fopen_or_create(path);
		if (!u->statf) {
			/* can't open file, stop collecting stats */
			u->stat = 0;
			return;
		}
		if (u->wchart) {
			for (i=0; modules[i].name; i++) {
				snprintf(path, PATH_MAX, "%s/%s.%06d.dat", u->statdir, modules[i].name, day);
				modules[i].statf = fopen_or_create(path);
				if (!modules[i].statf) {
					u->wchart = 0;
					return;
				}
			}
		}
		u->cday = day;

		/* calculate time_t when day start */
		t->tm_hour = t->tm_min = t->tm_sec = 0;
		u->daystart = timegm(t);

		/* delete old statistics files */
		stat_remove_old(u);
	}

	fseek(u->statf, (u->curr_timestamp - u->daystart) * sizeof(struct stat_info), SEEK_SET);
	fwrite(&u->stat_info, 1, sizeof(struct stat_info), u->statf); /* FIXME: check result? */

	memset(&u->stat_info, 0, sizeof(u->stat_info));

	/* write weights chart */
	if (u->wchart) {
		for (i=0; modules[i].name; i++) {
			double avg;

			avg = (modules[i].nw > DBL_EPSILON) ? (modules[i].stw / modules[i].nw) : 0.0f;

			fseek(modules[i].statf, (u->curr_timestamp - u->daystart) * sizeof(double), SEEK_SET);
			fwrite(&avg, 1, sizeof(double), modules[i].statf);

			modules[i].stw = modules[i].nw = 0.0f;
		}
	}

	u->old_timestamp = u->curr_timestamp;
}

static void *
stat_thread(void *arg)
{
	struct userdata *u = arg;
	struct timespec ts;

	while (!damper_done) {
		/* sleep for nearest second */
		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_sec++;
		ts.tv_nsec = 0;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

		pthread_mutex_lock(&u->lock);
		u->curr_timestamp++;

		if (u->stat) {
			stat_write(u);
		}

		pthread_mutex_unlock(&u->lock);
	}

	return NULL;
}

static int
config_read(struct userdata *u, char *confname)
{
	FILE *f;
	char line[LINE_MAX];

	f = fopen(confname, "r");
	if (!f) {
		fprintf(stderr, "Can't open file '%s'\n", confname);
		goto fail_open;
	}

	u->stat = 0;
	u->statdir[0] = '\0';

	u->keep_stat = 0;
	u->nfqlen = 0;

	u->wchart = 0;

	while (fgets(line, sizeof(line), f)) {
		char cmd[LINE_MAX], p1[LINE_MAX], p2[LINE_MAX];
		int scanres;

		scanres = sscanf(line, "%s %s %s", cmd, p1, p2);
		if ((cmd[0] == '#') || (scanres < 2)) {
			continue;
		}
		if (!strcmp(cmd, "queue")) {
			u->queue = atoi(p1);
		} else if (!strcmp(cmd, "limit")) {
			if (!strcmp(p1, "no")) {
				u->limit = UINT64_MAX;
			} else {
				u->limit = str2bps(p1);
			}
		} else if (!strcmp(cmd, "stat")) {
			if (!strcmp(p1, "yes")) {
				u->stat = 1;
			}
		} else if (!strcmp(cmd, "keepstat")) {
			u->keep_stat = atoi(p1);
			if (u->keep_stat <= 0) {
				fprintf(stderr, "Strange 'keepstat' value '%s', using %d instead", p1, KEEP_STAT);
				u->keep_stat = KEEP_STAT;
			}
		} else if (!strcmp(cmd, "nfqlen")) {
			u->nfqlen = atoi(p1);
			if (u->nfqlen <= 0) {
				fprintf(stderr, "Strange 'nfqlen' value '%s', using %d instead", p1, NFQ_DEFLEN);
				u->nfqlen = NFQ_DEFLEN;
			}
		} else if (!strcmp(cmd, "wchart")) {
			if (!strcmp(p1, "yes")) {
				u->wchart = 1;
			}
		} else if (!strcmp(cmd, "statdir")) {
			strncpy(u->statdir, p1, PATH_MAX);
		} else if (!strcmp(cmd, "packets")) {
			u->qlen = atoi(p1);
		} else {
			/* module parameters */
			size_t i;
			for (i=0; modules[i].name; i++) {
				if (!strcmp(cmd, modules[i].name) && modules[i].conf) {
					if (!strcmp(p1, "k")) {
						/* multiplicator */
						modules[i].k = atof(p2);
					} else {
						(modules[i].conf)(modules[i].mptr, p1, p2);
					}
					break;
				}
			}
		}
	}
	fclose(f);
	return 1;

fail_open:
	return 0;
}

static struct userdata *
userdata_init(char *confname)
{
	struct userdata *u;
	size_t i;

	u = malloc(sizeof(struct userdata));
	if (!u) {
		fprintf(stderr, "malloc(%lu) failed\n", (long)sizeof(struct userdata));
		goto fail_create;
	}

	/* init modules */
	for (i=0; modules[i].name; i++) {
		if (modules[i].init) {
			/* default multiplicator */
			modules[i].k = 1.0f;

			modules[i].mptr = (modules[i].init)(u, i);
		} else {
			modules[i].mptr = NULL;
		}
	}

	if (!config_read(u, confname)) {
		goto fail_conf;
	}

	if (u->limit == 0) {
		fprintf(stderr, "Something is wrong with limit, all traffic will be blocked\n");
	}

	if (u->nfqlen == 0) {
		u->nfqlen = NFQ_DEFLEN;
	}

	/* setup statistics */
	if (u->stat) {
		if (u->keep_stat == 0) {
			fprintf(stderr, "'keepstat' not set, statistics will be kept for %d days\n", KEEP_STAT);
			u->keep_stat = KEEP_STAT;
		}
		stat_init(u);
	}

	/* reserve memory for packets in queue */
	u->packets = malloc(u->qlen * sizeof(struct mpacket));
	if (!u->packets) {
		fprintf(stderr, "malloc(%lu) failed\n", (long)u->qlen * sizeof(struct mpacket));
		goto fail_packets;
	}

	/* create priority array - simplified implementation of priority queue */
	u->prioarray = malloc(u->qlen * sizeof(double));
	if (!u->prioarray) {
		fprintf(stderr, "malloc(%lu) failed\n", (long)u->qlen * sizeof(double));
		goto fail_prio_array;
	}
	/* fill priority array with minimal possible values */
	for (i=0; i<u->qlen; i++) {
		u->prioarray[i] = DBL_MIN;
	}

	/* init mutex */
	pthread_mutex_init(&u->lock, NULL);

	/* configuration done, notify modules */
	for (i=0; modules[i].name; i++) {
		if (modules[i].postconf) {
			int mres;

			mres = (modules[i].postconf)(modules[i].mptr);
			if (!mres) {
				/* disable module */
				fprintf(stderr, "Module '%s': initialization failed, disabling\n", modules[i].name);
				modules[i].enabled = 0;
			} else {
				modules[i].enabled = 1;
			}
		}
	}

	return u;

	free(u->prioarray);
fail_prio_array:
	free(u->packets);
fail_packets:
fail_conf:
	free(u);
fail_create:

	return NULL;
}

static void
userdata_destroy(struct userdata *u)
{
	size_t i;

	for (i=0; modules[i].name; i++) {
		if (modules[i].done) {
			(modules[i].done)(modules[i].mptr);
		}
		if (modules[i].statf) {
			fclose(modules[i].statf);
		}
	}

	pthread_mutex_destroy(&u->lock);

	if (u->stat) {
		fclose(u->statf);
	}
	free(u->prioarray);
	free(u->packets);
	free(u);
}

static void *
sender_thread(void *arg)
{

	struct userdata *u = arg;
	int vres;
	size_t i, idx = 0;
	double max = DBL_MIN;
	uint64_t limit;
	uint64_t sleep_ns;

	for (;;) {
		struct timespec ts;

		pthread_mutex_lock(&u->lock);
		limit = u->limit;

		if ((limit == 0) || (limit == UINT64_MAX)) {
			/* change limit to kbyte/sec, so will sleep about 0.1 sec */
			limit = 1000;
		}

		/* search for packet with maximum priority */
		max = DBL_MIN;
		for (i=0; i<u->qlen; i++) {
			if (max < u->prioarray[i]) {
				max = u->prioarray[i];
				idx = i;
			}
		}

		if (max != DBL_MIN) {
			/* accept (send) packet */
			vres = nfq_set_verdict(u->qh, u->packets[idx].id,
				NF_ACCEPT, u->packets[idx].size, u->packets[idx].packet);

			if (vres < 0) {
				fprintf(stderr, "nfq_set_verdict() failed, %s\n", strerror(errno));
			}

			/* mark packet buffer as empty */
			u->prioarray[idx] = DBL_MIN;

			/* update statistics */
			if (u->stat) {
				u->stat_info.packets_pass += 1;
				u->stat_info.octets_pass+= u->packets[idx].size;
			}
			sleep_ns = (u->packets[idx].size * BILLION) / limit;
		} else {
			/* no data to send, so just sleep for time required to transfer 100 bytes */
			sleep_ns = 100 * BILLION / limit;
		}

		if (sleep_ns > BILLION) {
			ts.tv_sec = sleep_ns / BILLION;
			ts.tv_nsec = sleep_ns % BILLION;
		} else {
			ts.tv_sec = 0;
			ts.tv_nsec = sleep_ns;
		}

		pthread_mutex_unlock(&u->lock);
		clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
	}

	return NULL;
}


static void
add_to_queue(struct userdata *u, char *packet, int id,
	int plen, double prio)
{
	size_t i, idx = 0;
	double min = DBL_MAX;

	/* search for packet with minimum priority */
	for (i=0; i<u->qlen; i++) {
		if (min > u->prioarray[i]) {
			min = u->prioarray[i];
			idx = i;
		}
	}

	/* and replace it with new packet */
	if (min < prio) {
		if (min != DBL_MIN) {
			int vres;

			/* drop packet */
			vres = nfq_set_verdict(u->qh, u->packets[idx].id, NF_DROP, 0, NULL);
			if (vres < 0) {
				fprintf(stderr, "nfq_set_verdict() failed, %s\n", strerror(errno));
			}

			if (u->stat) {
				/* and update statistics */
				u->stat_info.packets_drop += 1;
				u->stat_info.octets_drop += u->packets[idx].size;
			}
		}

		u->prioarray[idx] = prio;
		u->packets[idx].size = plen;
		u->packets[idx].id = id;
		memcpy(u->packets[idx].packet, packet, plen);
	}
}

static int
on_packet(struct nfq_q_handle *qh,
		struct nfgenmsg *nfmsg,
		struct nfq_data *nfad, void *data)
{
	int plen;
	int id;
	char *p;
	uint32_t mark;
	struct userdata *u;
	double weight = DBL_EPSILON;
	size_t i;
	int wchartstat;

	struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfad);
	if (ph) {
		id = ntohl(ph->packet_id);
	} else {
		return -1;
	}

	if ((plen = nfq_get_payload(nfad, (unsigned char **)&p)) < 0) {
		return -1;
	}

	u = data;

	/* there are two special cases:
	limit == 0 (traffic disabled) and limit == UINT64_MAX (no shaping performed) */
	pthread_mutex_lock(&u->lock);

	if (u->limit == 0) {
		/* drop packet */
		nfq_set_verdict(u->qh, id, NF_DROP, 0, NULL);
		/* and update statistics */
		if (u->stat) {
			u->stat_info.packets_drop += 1;
			u->stat_info.octets_drop += plen;
		}
	} else 	if (u->limit == UINT64_MAX) {
		/* accept packet */
		nfq_set_verdict(u->qh, id, NF_ACCEPT, plen, (unsigned char *)p);
		/* and update statistics */
		if (u->stat) {
			u->stat_info.packets_pass += 1;
			u->stat_info.octets_pass += plen;
		}
	}
	wchartstat = u->wchart;
	pthread_mutex_unlock(&u->lock);

	if ((u->limit == 0) || (u->limit == UINT64_MAX)) {
		return 1;
	}

	mark = nfq_get_nfmark(nfad);

	/* calculate weight for each enabled module */
	for (i=0; modules[i].name; i++) {
		if (modules[i].weight && modules[i].enabled) {
			double mweight = (modules[i].weight)(modules[i].mptr, p, plen, mark);

			if (mweight < 0.0) {
				weight = mweight;
				break;
			}
			mweight *= modules[i].k;

			if (wchartstat) {
				pthread_mutex_lock(&u->lock);
				modules[i].stw += mweight;
				modules[i].nw  += 1.0f;
				pthread_mutex_unlock(&u->lock);
			}

			weight += mweight;
		}
	}

	pthread_mutex_lock(&u->lock);
	if (weight < 0) {
		/* drop packet with with negative weight */
		nfq_set_verdict(u->qh, id, NF_DROP, 0, NULL);
		/* update statistics */
		if (u->stat) {
			u->stat_info.packets_drop += 1;
			u->stat_info.octets_drop += plen;
		}
	} else {
		/* add to queue with positive weight */
		add_to_queue(u, p, id, plen, weight);
	}
	pthread_mutex_unlock(&u->lock);

	return 1;
}

int
main(int argc, char *argv[])
{
	struct nfq_handle *h;
	int fd;
	char buf[0xffff];
	int r = EXIT_FAILURE;
	struct userdata *u;
	struct sigaction action;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s config.cfg\n", argv[0]);
		return EXIT_FAILURE;
	}

	u = userdata_init(argv[1]);
	if (!u) {
		return EXIT_FAILURE;
	}

	h = nfq_open();
	if (!h) {
		fprintf(stderr, "nfq_open() failed\n");
		return EXIT_FAILURE;
	}

	if (nfq_unbind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "nfq_unbind_pf() failed\n");
		goto fail_unbind;
	}

	if (nfq_bind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "nfq_bind_pf() failed\n");
		goto fail_bind;
	}

	u->qh = nfq_create_queue(h, u->queue, &on_packet, u);
	if (!u->qh) {
		fprintf(stderr, "nfq_create_queue() with queue %d failed\n", u->queue);
		goto fail_queue;
	}

	if (nfq_set_mode(u->qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "nfq_set_mode() failed\n");
		goto fail_mode;
	}

	if (nfq_set_queue_maxlen(u->qh, u->nfqlen) < 0) {
		fprintf(stderr, "nfq_set_queue_maxlen() failed with qlen=%d\n", u->nfqlen);
		goto fail_mode;
	}

	/* handle term and int signals */
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = on_term;
	sigaction(SIGTERM, &action, NULL);

	action.sa_handler = on_term;
	sigaction(SIGINT, &action, NULL);

	/* create sending thread */
	pthread_create(&u->sender_tid, NULL, &sender_thread, u);
	/* and thread for updating statistics */
	pthread_create(&u->stat_tid, NULL, &stat_thread, u);

	fd = nfq_fd(h);
	for (;;) {
		int rv;

		rv = recv(fd, buf, sizeof(buf), 0);
		if ((rv < 0) && (errno != EINTR)) {
			fprintf(stderr, "recv() on queue returned %d (%s)\n", rv, strerror(errno));
			fprintf(stderr, "Queue full? Current queue size %d, you can increase 'nfqlen' parameter in damper.conf\n",
				u->nfqlen);
			continue; /* don't stop after error */
		}

		if (damper_done) {
			break;
		}

		nfq_handle_packet(h, buf, rv);
	}

	pthread_join(u->stat_tid, NULL);
	/* FIXME: sender thread? */

	r = EXIT_SUCCESS;

fail_mode:
	nfq_destroy_queue(u->qh);

fail_queue:
fail_bind:
fail_unbind:
	nfq_close(h);

	userdata_destroy(u);

	return r;
}


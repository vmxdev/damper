/*
 * $ cc -Wall -pedantic damper.c modules.conf.c -o damper -lnetfilter_queue -pthread -lrt
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <ctype.h>

#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>

#include "damper.h"


#define BILLION ((uint64_t)1000000000)

#define KEEP_STAT 365 /* keep statistics about one year by default */

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

static void
wchart_init(struct userdata *u)
{
	char stat_path[PATH_MAX];
	size_t i;

	for (i=0; modules[i].name; i++) {
		snprintf(stat_path, PATH_MAX, "%s/%s.1.dat", u->statdir, modules[i].name);
		modules[i].st = fopen(stat_path, "r+");

		/* can't open, try to create */
		if (!modules[i].st) {
			modules[i].st = fopen(stat_path, "w+");
			if (!modules[i].st) {
				fprintf(stderr, "Can't open file '%s'\n", stat_path);
			}
		}
	}
}

static void
stat_init(struct userdata *u)
{
	char stat_path[PATH_MAX];

	if (u->statdir[0] == '\0') {
		fprintf(stderr, "Directory for statistics is not set\n");
		goto fail;
	}

	snprintf(stat_path, PATH_MAX, "%s/stat.dat", u->statdir);
	u->statf = fopen(stat_path, "r+");
	if (u->statf) {
		size_t s;

		s = fread(&u->stat_start, 1, sizeof(time_t), u->statf);
		if (s < sizeof(time_t)) {
			fprintf(stderr, "Incorrect statistics file '%s'\n", stat_path);
			goto fail_close;
		}
	} else {
		/* create new file */
		u->statf = fopen(stat_path, "w+");
		if (!u->statf) {
			fprintf(stderr, "Can't open file '%s'\n", stat_path);
			goto fail;
		}
		u->stat_start = time(NULL);
		if (u->stat_start == ((time_t) -1)) {
			fprintf(stderr, "time() failed\n");
			goto fail_close;
		}
		fwrite(&u->stat_start, 1, sizeof(time_t), u->statf);
	}
	memset(&u->stat_info, 0, sizeof(u->stat_info));

	u->curr_timestamp = time(NULL);
	u->old_timestamp = u->curr_timestamp;

	if (u->wchart) {
		wchart_init(u);
	}

	return;

fail_close:
	fclose(u->statf);
fail:
	u->stat = 0;
}

static void
stat_write(struct userdata *u)
{
	struct stat_info old_stat;
	long int off;
	size_t rr;

	off = (u->curr_timestamp - u->stat_start) * sizeof(struct stat_info) + sizeof(time_t);
	fseek(u->statf, off, SEEK_SET);
	rr = fread(&old_stat, 1, sizeof(struct stat_info), u->statf);
	if (rr == sizeof(struct stat_info)) {
		u->stat_info.packets_pass += old_stat.packets_pass;
		u->stat_info.octets_pass += old_stat.octets_pass;
		u->stat_info.packets_drop += old_stat.packets_drop;
		u->stat_info.octets_drop += old_stat.octets_drop;
	}

	fseek(u->statf, off, SEEK_SET);
	fwrite(&u->stat_info, 1, sizeof(struct stat_info), u->statf);
	memset(&u->stat_info, 0, sizeof(u->stat_info));

	u->old_timestamp = u->curr_timestamp;
}

static void
wchart_write(struct userdata *u, struct module_info *m)
{
	long int off;
	double avg;

	avg = (m->nw > DBL_EPSILON) ? (m->stw / m->nw) : 0.0f;

	off = (u->curr_timestamp - u->stat_start) * sizeof(double);
	fseek(m->st, off, SEEK_SET);

	fwrite(&avg, 1, sizeof(double), m->st);
	m->stw = m->nw = 0.0f;

	u->old_timestamp = u->curr_timestamp;
}

static void *
stat_thread(void *arg)
{
	struct userdata *u = arg;
	size_t i;
	struct timespec ts;

	for (;;) {
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

		if (u->wchart) {
			for (i=0; modules[i].name && modules[i].enabled; i++) {
				wchart_write(u, &modules[i]);
			}
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
	int rv;
	char buf[0xffff];
	int r = EXIT_FAILURE;
	struct userdata *u;

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

	if (nfq_set_queue_maxlen(u->qh, u->qlen * 2) < 0) { /* multiple by two just in case */
		fprintf(stderr, "nfq_set_queue_maxlen() failed with qlen=%lu\n", (long)u->qlen);
		goto fail_mode;
	}

	/* create sending thread */
	pthread_create(&u->sender_tid, NULL, &sender_thread, u);
	/* and thread for updating statistics */
	pthread_create(&u->stat_tid, NULL, &stat_thread, u);

	fd = nfq_fd(h);
	while ((rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
		nfq_handle_packet(h, buf, rv);
	}

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


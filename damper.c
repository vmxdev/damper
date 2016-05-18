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


/* convert string with optional suffixes 'k', 'm' or 'g' to bits per second */
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
stat_init(struct userdata *u)
{
	char stat_path[PATH_MAX];
	time_t t, tf;

	if (u->statdir[0] == '\0') {
		fprintf(stderr, "Directory for statistics is not set\n");
		goto fail;
	}

	t = time(NULL);
	if (t == ((time_t) -1)) {
		fprintf(stderr, "time() failed\n");
		goto fail;
	}

	snprintf(stat_path, PATH_MAX, "%s/stat.dat", u->statdir);
	u->statf = fopen(stat_path, "r+");
	if (u->statf) {
		size_t s;

		s = fread(&tf, 1, sizeof(time_t), u->statf);
		if (s < sizeof(time_t)) {
			fprintf(stderr, "Incorrect statistics file '%s'\n", stat_path);
			fclose(u->statf);
			goto fail;
		}
	} else {
		u->statf = fopen(stat_path, "w+");
		if (!u->statf) {
			fprintf(stderr, "Can't open file '%s'\n", stat_path);
			goto fail;
		}
		fwrite(&t, 1, sizeof(time_t), u->statf);
	}

	return;
	
fail:
	u->stat = 0;
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

	while (fgets(line, sizeof(line), f)) {
		char cmd[LINE_MAX], p1[LINE_MAX], p2[LINE_MAX];
		int scanres;

		scanres = sscanf(line, "%s %s %s", cmd, p1, p2);
		if ((cmd[0] == '#') || (scanres < 2)) {
			continue;
		}
		if (!strcmp(cmd, "queue")) {
			u->queue = atoi(p1);
		} else if (!strcmp(cmd, "iface")) {
			strncpy(u->interface, p1, IFNAMSIZ);
		} else if (!strcmp(cmd, "dscp")) {
			u->dscp = atoi(p1);
		} else if (!strcmp(cmd, "mark")) {
			u->mark = atoi(p1);
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
		} else if (!strcmp(cmd, "statdir")) {
			strncpy(u->statdir, p1, PATH_MAX);
		} else if (!strcmp(cmd, "packets")) {
			u->qlen = atoi(p1);
		} else {
			/* module parameters */
			size_t i;
			for (i=0; modules[i].name; i++) {
				if (!strcmp(cmd, modules[i].name) && modules[i].conf) {
					(modules[i].conf)(modules[i].mptr, p1, p2);
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
	int one = 1;
	struct ifreq ifr;
	size_t i;

	u = malloc(sizeof(struct userdata));
	if (!u) {
		fprintf(stderr, "malloc(%lu) failed\n", sizeof(struct userdata));
		goto fail_create;
	}

	/* init modules */
	for (i=0; modules[i].name; i++) {
		if (modules[i].init) {
			modules[i].mptr = (modules[i].init)(u);
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
		stat_init(u);
	}

	/* reserve memory for packets in queue */
	u->packets = malloc(u->qlen * sizeof(struct mpacket));
	if (!u->packets) {
		fprintf(stderr, "malloc(%lu) failed\n", u->qlen * sizeof(struct mpacket));
		goto fail_packets;
	}

	/* create priority array - simplified implementation of priority queue */
	u->prioarray = malloc(u->qlen * sizeof(double));
	if (!u->prioarray) {
		fprintf(stderr, "malloc(%lu) failed\n", u->qlen * sizeof(double));
		goto fail_prio_array;
	}
	/* fill priority array with minimal possible values */
	for (i=0; i<u->qlen; i++) {
		u->prioarray[i] = DBL_MIN;
	}

	/* create raw socket */
	u->socket = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	if (u->socket < 0) {
		fprintf(stderr, "socket(AF_INET, SOCK_RAW, IPPROTO_RAW) failed: %s\n", strerror(errno));
		goto fail_socket;
	}

	/* notify socket that we will pass our own IP headers */
	if (setsockopt(u->socket, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0) {
		fprintf(stderr, "setsockopt(IP_HDRINCL) on raw socket failed\n");
		goto fail_sockopt;
	}

	if (u->mark != 0) {
		/* set mark for outgoing packets */
		if (setsockopt (u->socket, SOL_SOCKET, SO_MARK, &u->mark, sizeof(u->mark)) < 0) {
			fprintf(stderr, "setsockopt(SO_MARK) on raw socket failed\n");
			goto fail_sockopt;
		}
	}

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof (ifr.ifr_name), "%s", u->interface);
	if (ioctl(u->socket, SIOCGIFINDEX, &ifr) < 0) {
		fprintf(stderr, "ioctl(SIOCGIFINDEX) on raw socket failed (interface %s)\n", u->interface);
		goto fail_sockopt;
	}
	if (setsockopt(u->socket, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr)) < 0) {
		fprintf(stderr, "setsockopt(SO_BINDTODEVICE) on raw socket failed (interface %s)\n", u->interface);
		goto fail_sockopt;
	}

	/* fill destinition address */
	u->daddr.sin_family = AF_INET;
	u->daddr.sin_port = 0; /* not needed in SOCK_RAW */
	inet_pton(AF_INET, "127.0.0.1", (struct in_addr *)&(u->daddr.sin_addr.s_addr));
	memset(u->daddr.sin_zero, 0, sizeof(u->daddr.sin_zero));

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

fail_sockopt:
	close(u->socket);
fail_socket:
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

	if (u->stat) {
		fclose(u->statf);
	}
	close(u->socket);
	free(u->prioarray);
	free(u->packets);
	free(u);
}

static void *
sender_thread(void *arg)
{
	struct userdata *u = arg;
	int sendres;
	size_t i, idx = 0;
	double max = DBL_MIN;
	uint64_t octets_allowed = 0, limit;
	struct timespec tprev, tcurr;
	uint64_t tprev_ns, tcurr_ns;

	clock_gettime(CLOCK_MONOTONIC, &tprev);
	tprev_ns = (uint64_t)tprev.tv_sec * 1e9 + tprev.tv_nsec;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &tcurr);
		tcurr_ns = (uint64_t)tcurr.tv_sec * 1e9 + tcurr.tv_nsec;

		pthread_mutex_lock(&u->lock);
		limit = u->limit;

		if (tcurr_ns == tprev_ns) {
			goto sleep_and_continue;
		}

		octets_allowed = ((tcurr_ns - tprev_ns) * limit) / 1e9;

		/* search for packet with maximum priority */
		max = DBL_MIN;
		for (i=0; i<u->qlen; i++) {
			if (max < u->prioarray[i]) {
				max = u->prioarray[i];
				idx = i;
			}
		}

		if (max == DBL_MIN) {
			goto sleep_and_continue;
		}

		if (octets_allowed >= u->packets[idx].size) {
			sendres = sendto(u->socket, u->packets[idx].packet, u->packets[idx].size, 0,
				(struct sockaddr *)&(u->daddr), (socklen_t)sizeof(u->daddr));

			if (sendres < 0) {
				fprintf(stderr, "sendto() failed, %s\n", strerror(errno));
			}

			u->prioarray[idx] = DBL_MIN;
			tprev_ns = tcurr_ns;
		} else {
			goto sleep_and_continue;
		}

		pthread_mutex_unlock(&u->lock);
		continue;

sleep_and_continue:
		pthread_mutex_unlock(&u->lock);

		/* time needed to transmit about 10 bytes */
		tcurr.tv_nsec += 10 * 1e9 / (limit + 1);
		if (tcurr.tv_nsec >= 1e9) {
			tcurr.tv_nsec -= 1e9;
			tcurr.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tcurr, NULL);
	}

	return NULL;
}


static void
add_to_queue(struct userdata *u, char *packet, int plen, double prio)
{
	size_t i, idx = 0;
	double min = DBL_MAX;

	pthread_mutex_lock(&u->lock);
	/* search for packet with minimum priority */
	for (i=0; i<u->qlen; i++) {
		if (min > u->prioarray[i]) {
			min = u->prioarray[i];
			idx = i;
		}
	}

	/* and replace it with new packet */
	if (min < prio) {
		u->prioarray[idx] = prio;
		u->packets[idx].size = plen;
		memcpy(u->packets[idx].packet, packet, plen);
	}
	pthread_mutex_unlock(&u->lock);
}

static int
on_packet(struct nfq_q_handle *qh,
		struct nfgenmsg *nfmsg,
		struct nfq_data *nfad, void *data)
{
	int verdict, vres, plen;
	int id;
	char *p;
	uint32_t mark;
	struct userdata *u;
	double weight = 1.0;
	size_t i;

	struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfad);
	if (ph) {
		id = ntohl(ph->packet_id);
	} else {
		return -1;
	}

	if ((plen = nfq_get_payload(nfad, &p)) < 0) {
		return -1;
	}

	u = data;

	mark = nfq_get_nfmark(nfad);

	/* calculate weight for each enabled module */
	for (i=0; modules[i].name; i++) {
		if (modules[i].weight && modules[i].enabled) {
			weight *= (modules[i].weight)(modules[i].mptr, p, plen, mark);
			if (weight < 0.0) break;
		}
	}

	if (weight < 0) {
		/* drop packets with with negative weight */
		/* statistics */
	} else {
		/* add to queue with positive weight */
		if (u->dscp != 0) {
			/* set DSCP */
			p[1] = (p[1] & 0x03) | (u->dscp << 2);
		}

		add_to_queue(u, p, plen, weight);
	}

	/* drop packet */
	verdict = NF_DROP;
	vres = nfq_set_verdict(qh, id, verdict, 0, NULL);

	return vres;
}

int
main(int argc, char *argv[])
{
	struct nfq_handle *h;
	struct nfq_q_handle *qh;
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

	pthread_create(&u->sender_tid, NULL, &sender_thread, u);

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

	qh = nfq_create_queue(h, u->queue, &on_packet, u); /* queue id */
	if (!qh) {
		fprintf(stderr, "nfq_create_queue() with queue %d failed\n", u->queue);
		goto fail_queue;
	}

	if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "nfq_set_mode() failed\n");
		goto fail_mode;
	}


	fd = nfq_fd(h);
	while ((rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
		nfq_handle_packet(h, buf, rv);
	}

	r = EXIT_SUCCESS;

fail_mode:
	nfq_destroy_queue(qh);

fail_queue:
fail_bind:
fail_unbind:
	nfq_close(h);

	userdata_destroy(u);

	return r;
}


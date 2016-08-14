#define TCP_PROTO_NUM 6
#define UDP_PROTO_NUM 17


struct entflow
{
	uint32_t saddr, daddr;
	uint16_t proto;
	uint16_t sport, dport;
	uint32_t stream_len;
	uint32_t map[256]; /* symbols map */
} __attribute__((packed));

struct entropy
{
	size_t module_number;

	struct entflow *recent_flows;
	int nflows;
	int currflow; /* pointer to current entry in flows circular buffer */

	int debug;
	pthread_t debug_tid;
	pthread_mutex_t lock;

	char *statdir;
	FILE *fdbg;
};

/* Shannon's entropy calculation */
static double
entropy_calc(struct entflow *e)
{
	double m;
	int i;

	m = DBL_EPSILON;
	for (i=0; i<256; i++) {
		double freq;

		if (e->map[i] == 0) continue;
		freq = (double)e->map[i] / e->stream_len;
		m += freq * log2(freq);
	}
	m = -m;

	return m;
}

void *
entropy_init(struct userdata *u, size_t n)
{
	struct entropy *data;

	data = malloc(sizeof(struct entropy));
	if (!data) {
		fprintf(stderr, "Module %s: malloc(%lu) failed\n",
			modules[n].name,
			sizeof(struct entropy));
		goto fail_alloc;
	}

	data->nflows = 0;
	data->currflow = 0;

	data->debug = 0;
	data->module_number = n;
	data->statdir = u->statdir;
	pthread_mutex_init(&data->lock, NULL);

	return data;

fail_alloc:
	return NULL;
}

void
entropy_conf(void *arg, char *param1, char *param2)
{
	struct entropy *data = arg;

	if (!strcmp(param1, "nrecent")) {
		data->nflows = atoi(param2);
	} else if (!strcmp(param1, "debug")) {
		data->debug = atoi(param2);
		if (data->debug <= 0) {
			fprintf(stderr, "Module %s: strange debug value %d\n",
				modules[data->module_number].name,
				data->debug);
			data->debug = 0;
		}
	} else {
		fprintf(stderr, "Module %s: unknown config parameter '%s'\n",
			modules[data->module_number].name, param1);
	}
}

void *
entropy_debug(void *arg)
{
	struct entropy *data = arg;
	int i;

	for (;;) {
		time_t t;
		struct tm* tm_info;
		char tbuf[100];

		sleep(data->debug);
		pthread_mutex_lock(&data->lock);

		time(&t);
		tm_info = localtime(&t);
		strftime(tbuf, sizeof(tbuf), "%Y:%m:%d %H:%M:%S", tm_info);

		fprintf(data->fdbg, "%s\n", tbuf);
		for (i=0; i<data->nflows; i++) {
			struct in_addr saddr, daddr;
			int proto, sport, dport;

			saddr.s_addr = data->recent_flows[i].saddr;
			daddr.s_addr = data->recent_flows[i].daddr;
			proto = data->recent_flows[i].proto;
			sport = data->recent_flows[i].sport;
			dport = data->recent_flows[i].dport;

			fprintf(data->fdbg, "%d: [prot: %3d %s:%d =>\t", i, proto, inet_ntoa(saddr), sport);
			fprintf(data->fdbg, "%s:%d] %f\n", inet_ntoa(daddr), dport, entropy_calc(&data->recent_flows[i]));
		}

		pthread_mutex_unlock(&data->lock);

		fprintf(data->fdbg, "\n\n");
		fflush(data->fdbg);
	}
	return NULL;
}

int
entropy_postconf(void *arg)
{
	struct entropy *data = arg;

	if (data->nflows < 1) {
		fprintf(stderr, "Module %s: incorrect value %d for number of recent flows\n",
			modules[data->module_number].name, data->nflows);
		goto fail;
	}

	/* create array of recent flows */
	data->recent_flows = malloc(data->nflows * sizeof(struct entflow));
	if (!data->recent_flows) {
		fprintf(stderr, "Module %s: malloc() failed for %d recent flows\n",
			modules[data->module_number].name, data->nflows);
		goto fail;
	}
	memset(data->recent_flows, 0, data->nflows * sizeof(struct entflow));

	if (data->debug) {
		char debugfile[PATH_MAX];

		snprintf(debugfile, PATH_MAX, "%s/entlog.txt", data->statdir);
		data->fdbg = fopen(debugfile, "a");
		if (!data->fdbg) {
			fprintf(stderr, "Module %s: can't open file '%s'\n",
				modules[data->module_number].name, debugfile);
			goto fail;
		}
		pthread_create(&data->debug_tid, NULL, &entropy_debug, data);
	}

	return 1;

fail:
	return 0;
}

void
entropy_free(void *arg)
{
	struct entropy *data = arg;

	if (data->debug) {
		fclose(data->fdbg);
	}
	free(data->recent_flows);
	free(data);
}

double
entropy_weight(void *arg, char *packet, int packetlen, int mark)
{
	double m;
	unsigned int i;
	uint32_t saddr, daddr;
	int proto, sport, dport;
	struct damper_ip_header *ip;
	int ip_hdrlen;
	char *payload;
	int found = 0;
	struct entropy *data = arg;

	ip = (struct damper_ip_header *)packet;
	saddr = ip->ip_src.s_addr;
	daddr = ip->ip_dst.s_addr;
	proto = ip->ip_p;

	ip_hdrlen = (ip->ip_vhl & 0x0f) * 4;
	if ((proto == TCP_PROTO_NUM) || (proto == UDP_PROTO_NUM)) {
		char *tcp_udp;

		tcp_udp = packet + ip_hdrlen;
		sport = ntohs(*((uint16_t *)(tcp_udp)));
		dport = ntohs(*((uint16_t *)(tcp_udp + sizeof(uint16_t))));

		if (proto == TCP_PROTO_NUM) {
			payload = tcp_udp + 20; /* incorrect, payload may include tcp options */
		} else {
			payload = tcp_udp + 8;
		}
	} else {
		sport = 0;
		dport = 0;
		payload = packet + ip_hdrlen;
	}

	if (data->debug) {
		pthread_mutex_lock(&data->lock);
	}

	for (i=0; i<data->nflows; i++) {
		if ((saddr == data->recent_flows[i].saddr) && (daddr == data->recent_flows[i].daddr)
			&& (proto == data->recent_flows[i].proto)
			&& (sport == data->recent_flows[i].sport) && (dport == data->recent_flows[i].dport)) {

			data->recent_flows[i].stream_len += packetlen - (payload - packet);
			found = 1;
			break;
		}
	}

	if (!found) {
		i = data->currflow;

		data->recent_flows[i].saddr = saddr;
		data->recent_flows[i].daddr = daddr;
		data->recent_flows[i].proto = proto;
		data->recent_flows[i].sport = sport;
		data->recent_flows[i].dport = dport;
		memset(data->recent_flows[i].map, 0, sizeof(uint32_t) * 256);
		data->recent_flows[i].stream_len = packetlen - (payload - packet);

		data->currflow++;
		if (data->currflow >= data->nflows) {
			data->currflow = 0;
		}
	}

	/* update symbols map */
	while ((payload - packet) < packetlen) {
		data->recent_flows[i].map[(unsigned char)(*payload)]++;
		payload++;
	}

	/* and calculate entropy */
	m = entropy_calc(&data->recent_flows[i]);

	if (data->debug) {
		pthread_mutex_unlock(&data->lock);
	}

	return m;
}



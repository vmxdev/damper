struct flow
{
	uint32_t saddr, daddr;
	uint64_t octets;
} __attribute__((packed));

struct inhibit_big_flows
{
	struct flow *recent_flows;
	int nflows;
	int currflow;
	uint64_t flow_octets;
	size_t module_number;

	int debug;
	pthread_t debug_tid;
	pthread_mutex_t lock;

	char *statdir;
	FILE *fdbg;
};

void *
inhibit_big_flows_init(struct userdata *u, size_t n)
{
	struct inhibit_big_flows *data;

	data = malloc(sizeof(struct inhibit_big_flows));
	if (!data) {
		fprintf(stderr, "Module %s: malloc(%lu) failed\n",
			modules[n].name,
			(long)sizeof(struct inhibit_big_flows));
		goto fail_alloc;
	}

	data->nflows = 0;
	data->currflow = 0;
	data->flow_octets = 0;

	data->debug = 0;
	data->module_number = n;
	data->statdir = u->statdir;
	pthread_mutex_init(&data->lock, NULL);

	return data;

fail_alloc:
	return NULL;
}

void
inhibit_big_flows_conf(void *arg, char *param1, char *param2)
{
	struct inhibit_big_flows *data = arg;

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
inhibit_big_flows_debug(void *arg)
{
	struct inhibit_big_flows *data = arg;
	int i;

	for (;;) {
		sleep(data->debug);
		pthread_mutex_lock(&data->lock);

		fprintf(data->fdbg, "total: %lu\n", (long)data->flow_octets);
		for (i=0; i<data->nflows; i++) {
			struct in_addr saddr, daddr;

			saddr.s_addr = data->recent_flows[i].saddr;
			daddr.s_addr = data->recent_flows[i].daddr;
			fprintf(data->fdbg, "%d: [%s => ", i, inet_ntoa(saddr));
			fprintf(data->fdbg, "%s] %lu\n", inet_ntoa(daddr), (long)data->recent_flows[i].octets);
		}

		pthread_mutex_unlock(&data->lock);

		fprintf(data->fdbg, "\n\n");
	}
	return NULL;
}

int
inhibit_big_flows_postconf(void *arg)
{
	struct inhibit_big_flows *data = arg;

	if (data->nflows < 1) {
		fprintf(stderr, "Module %s: incorrect value %d for number of recent flows\n",
			modules[data->module_number].name, data->nflows);
		goto fail;
	}

	/* create array of recent flows */
	data->recent_flows = malloc(data->nflows * sizeof(struct flow));
	if (!data->recent_flows) {
		fprintf(stderr, "Module %s: malloc() failed for %d recent flows\n",
			modules[data->module_number].name, data->nflows);
		goto fail;
	}
	memset(data->recent_flows, 0, data->nflows * sizeof(struct flow));

	if (data->debug) {
		char debugfile[PATH_MAX];

		snprintf(debugfile, PATH_MAX, "%s/ilog.txt", data->statdir);
		data->fdbg = fopen(debugfile, "a");
		if (!data->fdbg) {
			fprintf(stderr, "Module %s: can't open file '%s'\n",
				modules[data->module_number].name, debugfile);
			goto fail;
		}
		pthread_create(&data->debug_tid, NULL, &inhibit_big_flows_debug, data);
	}

	return 1;

fail:
	return 0;
}

void
inhibit_big_flows_free(void *arg)
{
	struct inhibit_big_flows *data = arg;

	if (data->debug) {
		fclose(data->fdbg);
	}
	free(data->recent_flows);
	free(data);
}

double
inhibit_big_flows_weight(void *arg, char *packet, int packetlen, int mark)
{
	double m;
	unsigned int i;
	uint32_t saddr, daddr;
	struct damper_ip_header *ip;
	int found = 0;
	struct inhibit_big_flows *data = arg;

	ip = (struct damper_ip_header *)packet;
	saddr = ip->ip_src.s_addr;
	daddr = ip->ip_dst.s_addr;

	if (data->debug) {
		pthread_mutex_lock(&data->lock);
	}

	for (i=0; i<data->nflows; i++) {
		if ((saddr == data->recent_flows[i].saddr) && (daddr == data->recent_flows[i].daddr)) {
			data->recent_flows[i].octets += packetlen;
			found = 1;
			break;
		}
	}

	if (!found) {
		i = data->currflow;

		data->flow_octets -= data->recent_flows[i].octets;

		data->recent_flows[i].saddr = saddr;
		data->recent_flows[i].daddr = daddr;
		data->recent_flows[i].octets = packetlen;

		data->currflow++;
		if (data->currflow >= data->nflows) {
			data->currflow = 0;
		}
	}

	data->flow_octets += packetlen;

	if (data->recent_flows[i].octets > 0) {
		m = (double)data->flow_octets / data->recent_flows[i].octets;
	} else {
		/* something greater than 0 */
		m = DBL_EPSILON;
	}

	if (data->debug) {
		pthread_mutex_unlock(&data->lock);
	}

	return m;
}



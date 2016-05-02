struct flow
{
	uint32_t saddr, daddr;
	uint64_t octets;
} __attribute__((packed));

struct inhibit_big_flows
{
	struct flow *rescent_flows;
	int nflows;
	int currflow;
	uint64_t flow_octets;

	int debug;
	pthread_t debug_tid;
	pthread_mutex_t lock;
};

void *
inhibit_big_flows_init(struct userdata *u)
{
	struct inhibit_big_flows *data;

	data = malloc(sizeof(struct inhibit_big_flows));
	if (!data) {
		fprintf(stderr, "malloc(%lu) failed\n", sizeof(struct inhibit_big_flows));
		goto fail_alloc;
	}

	data->nflows = 0;
	data->currflow = 0;
	data->flow_octets = 0;

	data->debug = 0;
	pthread_mutex_init(&data->lock, NULL);

	return data;

fail_alloc:
	return NULL;
}

void
inhibit_big_flows_conf(void *arg, char *param1, char *param2)
{
	struct inhibit_big_flows *data = arg;

	if (!strcmp(param1, "nrescent")) {
		data->nflows = atoi(param2);
	} else if (!strcmp(param1, "debug")) {
		data->debug = atoi(param2);
		if (data->debug <= 0) {
			fprintf(stderr, "Module inhibit_big_flows: strange debug value %d\n",
				data->debug);
			data->debug = 0;
		}
	} else {
		fprintf(stderr, "Module inhibit_big_flows: unknown config parameter '%s'\n", param1);
	}
}

void *
inhibit_big_flows_debug(void *arg)
{
	struct inhibit_big_flows *data = arg;
	FILE *f;
	int i;

	for (;;) {
		sleep(data->debug);
		f = fopen("ilog.txt", "a");
		if (!f) {
			continue;
		}

		pthread_mutex_lock(&data->lock);

		fprintf(f, "total: %lu\n", data->flow_octets);
		for (i=0; i<data->nflows; i++) {
			struct in_addr saddr, daddr;

			saddr.s_addr = data->rescent_flows[i].saddr;
			daddr.s_addr = data->rescent_flows[i].daddr;
			fprintf(f, "%d: [%s => ", i, inet_ntoa(saddr));
			fprintf(f, "%s] %lu\n", inet_ntoa(daddr), data->rescent_flows[i].octets);
		}

		pthread_mutex_unlock(&data->lock);

		fprintf(f, "\n\n");
		fclose(f);
	}
	return NULL;
}

int
inhibit_big_flows_postconf(void *arg)
{
	struct inhibit_big_flows *data = arg;

	if (data->nflows < 1) {
		fprintf(stderr, "Module 'inhibit_big_flows': incorrect value %d for number of rescent flows\n",
			data->nflows);
		goto fail;
	}

	/* create array of rescent flows */
	data->rescent_flows = malloc(data->nflows * sizeof(struct flow));
	if (!data->rescent_flows) {
		fprintf(stderr, "malloc() failed for %d rescent flows\n", data->nflows);
		goto fail;
	}
	memset(data->rescent_flows, 0, data->nflows * sizeof(struct flow));

	if (data->debug) {
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

	free(data->rescent_flows);
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
		if ((saddr == data->rescent_flows[i].saddr) && (daddr == data->rescent_flows[i].daddr)) {
			data->rescent_flows[i].octets += packetlen;
			found = 1;
			break;
		}
	}

	if (!found) {
		i = data->currflow;

		data->flow_octets -= data->rescent_flows[i].octets;

		data->rescent_flows[i].saddr = saddr;
		data->rescent_flows[i].daddr = daddr;
		data->rescent_flows[i].octets = packetlen;

		data->currflow++;
		if (data->currflow >= data->nflows) {
			data->currflow = 0;
		}
	}

	data->flow_octets += packetlen;

	m = (double)data->rescent_flows[i].octets / data->flow_octets;

	if (data->debug) {
		pthread_mutex_unlock(&data->lock);
	}

	return m;
}



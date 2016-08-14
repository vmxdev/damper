struct mod_random
{
	size_t module_number;
};

void *
random_init(struct userdata *u, size_t n)
{
	struct mod_random *data;

	data = malloc(sizeof(struct mod_random));
	if (!data) {
		fprintf(stderr, "Module %s: malloc(%lu) failed\n",
			modules[n].name, (long)sizeof(struct mod_random));
		goto fail_alloc;
	}

	data->module_number = n;

	return data;

fail_alloc:
	return NULL;
}

void
random_conf(void *arg, char *param1, char *param2)
{
}


int
random_postconf(void *arg)
{
	return 1;
}

void
random_free(void *arg)
{
	struct mod_random *data = arg;

	free(data);
}

double
random_weight(void *arg, char *packet, int packetlen, int mark)
{
	double m;

	m = (double)1.0 / (rand() + 1);

	return m;
}


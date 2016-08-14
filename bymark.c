struct mark_weight
{
	int mark;
	double w;
};

struct bymark
{
	size_t module_number;

	size_t nmarks;
	struct mark_weight *mw;
};

void *
bymark_init(struct userdata *u, size_t n)
{
	struct bymark *data;

	data = malloc(sizeof(struct bymark));
	if (!data) {
		fprintf(stderr, "Module %s: malloc(%lu) failed\n",
			modules[n].name, (long)sizeof(struct bymark));
		goto fail_alloc;
	}

	data->nmarks = 0;
	data->mw = NULL;
	data->module_number = n;

	return data;

fail_alloc:
	return NULL;
}

void
bymark_conf(void *arg, char *param1, char *param2)
{
	struct bymark *data = arg;
	int mark;
	double weight;
	struct mark_weight *tmp_mw;

	mark = strtol(param1, NULL, 10);
	if (errno) {
		fprintf(stderr, "Module %s: can't convert '%s' (mark) to integer \n",
			modules[data->module_number].name,
			param1);
		mark = 0;
	}

	weight = strtod(param2, NULL);
	if (errno) {
		fprintf(stderr, "Module %s: can't convert '%s' (mark weight) to double \n",
			modules[data->module_number].name,
			param2);
	}

	data->nmarks++;
	tmp_mw = realloc(data->mw, sizeof(struct mark_weight) * data->nmarks);
	if (!tmp_mw) {
		fprintf(stderr, "Module %s: realloc() failed\n", modules[data->module_number].name);
		free(data->mw);
		data->mw = NULL;
		data->nmarks = 0;
		return; /* FIXME: return error? */
	}

	data->mw = tmp_mw;
	data->mw[data->nmarks - 1].mark = mark;
	data->mw[data->nmarks - 1].w = weight;
}


int
bymark_postconf(void *arg)
{
	return 1;
}

void
bymark_free(void *arg)
{
	struct bymark *data = arg;

	free(data->mw);
	free(data);
}

double
bymark_weight(void *arg, char *packet, int packetlen, int mark)
{
	unsigned int i;
	struct bymark *data = arg;
	double m = DBL_EPSILON;

	for (i=0; i<data->nmarks; i++) {
		if (mark == data->mw[i].mark) {
			m = data->mw[i].w;
			break;
		}
	}

	return m;
}



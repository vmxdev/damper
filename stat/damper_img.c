/*
 $ cc -Wall -pedantic damper_img.c -o damper_img -lpng -pthread
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <png.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <dirent.h>

#include "image.h"
#include "stats.h"

#include "../damper.h"

static char statpath[PATH_MAX];

/* struct for passing parameters to scgi thread */
struct scgi_thread_arg
{
	int s; /* socket */
};

/* request */
struct request
{
	int w, h;          /* image width and height */
	time_t start, end; /* start and end time of chart */
	int pb;            /* packets or bytes, if zero display packets in chart */
};

/* response */
struct response
{
	time_t start, end;
	uint64_t max;       /* max data value (peak) in response */
	struct pngmembuf img;

	struct pngmembuf weights;
	size_t wn;
	char   *wnames;
	pixel_t *clrlegend;
};

/* passed and dropped octets (or packets) for one chart pixel */
struct pixel_info
{
	int passed;
	int dropped;
};


/* weight chart related stuff */
struct weight_chart
{
	bitmap_t rep;
	struct pngmembuf png;

	FILE **files;
	size_t nfiles;

	double *lines;
	double max;   /* max value in chart */

	char *mnames;
	pixel_t *mcolors;
};

/* background with grid */
static void
draw_bg(bitmap_t *bmp)
{
	size_t i;

	/* fill background */
	for (i=0; i<bmp->width; i++) {
		size_t j;

		for (j=0; j<bmp->height; j++) {
			pixel_t *pixel = bmp->pixels + bmp->width * j + i;
			pixel->red = pixel->green = pixel->blue = 255;
		}
	}

	/* grid lines*/
	for (i=0; i<bmp->width/10; i++) {
		if ((i % 5) == 0) {
			vert_line(bmp, i*10, 0, bmp->height,
				230, 230, 230);
		} else {
			vert_line(bmp, i*10, 0, bmp->height,
				240, 240, 240);
		}
	}
	for (i=0; i<bmp->height/10; i++) {
		if ((i % 5) == 0) {
			horiz_line(bmp, 0, bmp->width, i*10,
				230, 230, 230);
		} else {
			horiz_line(bmp, 0, bmp->width, i*10,
				240, 240, 240);
		}
	}
}

static uint32_t
octets_or_packets(struct request *p, struct stat_info *i, int pass)
{
	if (p->pb) {
		return pass ? i->octets_pass : i->octets_drop;
	} else {
		return pass ? i->packets_pass : i->packets_drop;
	}
}

/* get peak chart value */
static uint32_t
chart_get_peak(struct request *p, struct stat_data *sd)
{
	uint32_t peak = 0;
	int statret;
	struct stat_info info;           /* data cursor value */

	statret = stat_data_open(sd);
	if (!statret) {
		goto fail;
	}

	if (p->start == 0) {
		p->start = sd->start;
		p->end = p->start + sd->nrec;
	}

	statret = stat_data_seek(sd, "dstat", p->start, &info);
	if (!statret) {
		goto fail_statseek;
	}

	for (;;) {
		statret = stat_data_next(sd, &info);
		if (!statret) {
			break;
		}

		if (sd->t >= p->end) {
			break;
		}

		if (octets_or_packets(p, &info, 1) > peak) {
			peak = octets_or_packets(p, &info, 1);
		}
		if (octets_or_packets(p, &info, 0) > peak) {
			peak = octets_or_packets(p, &info, 0);
		}
	}

fail_statseek:
	stat_data_close(sd);

fail:
	return peak;
}

static void
chart_draw_row(bitmap_t * bmp, struct pixel_info *row, int line_prev, int h, int lines_per_row)
{
	int ih;
	int maxgreen = 150;
	int maxred = 150;

	/* make pixels a bit darker */
	if (lines_per_row > 1) {
		lines_per_row -= 1;
	}

	/* and display it */
	for (ih=0; ih<h; ih++) {
		int red_r = 0, green_r = 0, blue_r = 0;
		int red_g = 0, green_g = 0, blue_g = 0;
		int red, green, blue; /* result pixel colors */

		int pg = row[ih].passed;
		int pr = row[ih].dropped;
		int y = h - ih - 1;

		if ((pg == 0) && (pr == 0)) {
			break;
		}
		if (lines_per_row < 2) {
			if (pr) {
				/* has red color */
				red_r = maxred;
				green_r = blue_r = 0;
			}
			if (pg) {
				/* has green */
				green_g = maxgreen;
				red_g = blue_g = 0;
			}
		} else {
			double bright;

			if (pr) {
				bright = (double)(pr) / (double)lines_per_row;
				if (bright > 1.0) {
					bright = 1.0;
				}

				red_r = maxred + (255 - maxred) * (1.0 - bright);
				green_r = 200 * (1.0 - bright);
				blue_r = 200 * (1.0 - bright);
			}

			if (pg) {
				bright = (double)(pg) / (double)lines_per_row;
				if (bright > 1.0) {
					bright = 1.0;
				}

				red_g = 200 * (1.0 - bright);
				green_g = maxgreen + (255 - maxgreen) * (1.0 - bright);
				blue_g = 200 * (1.0 - bright);
			}
		}

		if (pr && (!pg)) {
			/* red only */
			red = red_r;
			green = green_r;
			blue = blue_r;
		} else if ((!pr) && pg) {
			/* green only */
			red = red_g;
			green = green_g;
			blue = blue_g;
		} else {
			/* red and green */
			red = (red_r + red_g) / 2;
			green = green_r / 2;
			blue = blue_r / 2;
		}

		put_pixel(bmp, line_prev, y, red, green, blue);
	}
}

static void
chart_plot(struct request *p, struct stat_data *sd, uint32_t peak, bitmap_t *bmp)
{
	int statret;
	struct stat_info info;
	int lines_per_row, line_prev = -1;
	struct pixel_info *row;

	statret = stat_data_open(sd);
	if (!statret) {
		return;
	}

	statret = stat_data_seek(sd, "dstat", p->start, &info);
	if (!statret) {
		goto fail_statseek;
	}

	row = calloc(sizeof(struct pixel_info), p->h);
	if (!row) {
		goto fail_row;
	}

	lines_per_row = (p->end - p->start) / p->w + 2;

	line_prev = 0;
	for (;;) {
		int h_pass, h_drop, line_start, line_end, i;

		statret = stat_data_next(sd, &info);
		if (!statret) {
			break;
		}

		if (sd->t >= p->end) {
			break;
		}

		h_pass = (uint64_t)octets_or_packets(p, &info, 1) * p->h / (peak + 1);
		h_drop = (uint64_t)octets_or_packets(p, &info, 0) * p->h / (peak + 1);

		line_start = (uint64_t)p->w * (sd->t - p->start) / (p->end - p->start);
		line_end   = (uint64_t)p->w * (sd->t - p->start + 1) / (p->end - p->start);

		if (line_end >= p->w) {
			line_end = p->w - 1;
		}

		for (i=line_start; i<=line_end; i++) {
			int ih;

			/* new line (well, row in fact) */
			if (line_prev != i) {
				if (line_prev > 0) {
					/* display row */
					chart_draw_row(bmp, row, line_prev, p->h, lines_per_row);
				}

				memset(row, 0, p->h * sizeof(struct pixel_info));
				line_prev = i;
			}

			/* fill row */
			for (ih=0; ih<h_pass; ih++) {
				row[ih].passed++;
			}
			for (ih=0; ih<h_drop; ih++) {
				row[ih].dropped++;
			}
		}
	}

	free(row);
fail_row:
fail_statseek:
	stat_data_close(sd);
}

/* draw chart */
static struct response *
build_chart(struct request *p)
{
	bitmap_t rep;
	struct pngmembuf mempng;         /* png in memory */
	struct response *r = NULL;       /* response */
	struct stat_data sd;             /* statistics data */
	uint32_t peak;                   /* maximum height */

	/* create an image */
	rep.width = p->w;
	rep.height = p->h;

	rep.pixels = calloc(sizeof(pixel_t), rep.width * rep.height);
	if (!rep.pixels) {
		goto fail_rep;
	}

	/* draw background */
	draw_bg(&rep);

	peak = chart_get_peak(p, &sd);
	if (peak > 0) {
		chart_plot(p, &sd, peak, &rep);
	}

	/* Write the image to memory */
	mempng.len = 0;
	mempng.ptr = NULL;
	mk_mempng(&rep, &mempng);

	r = malloc(sizeof(struct response));
	if (!r) {
		goto fail_resp;
	}

	/* base64 encoding */
	r->img.len = (mempng.len + 1)* 4 / 3 + 3;
	r->img.ptr = malloc(r->img.len);
	if (!r->img.ptr) {
		goto fail_r_img;
	}

	b64encode(mempng.ptr, r->img.ptr, mempng.len);
	r->start = p->start;
	r->end = p->end;

	r->max = peak;

	r->wn = 0;
	r->wnames = NULL;
	r->weights.len = 0;
	r->weights.ptr = NULL;

	/* free raw image */
	free(mempng.ptr);
	free(rep.pixels);

	return r;

fail_r_img:
	free(r);

fail_resp:
	free(rep.pixels);

fail_rep:

	return NULL;
}

/* parse script params */
void
parse_params(struct request *p, char *q)
{
	char *ptr = q;
	int last = 0;

	for (;;) {
		char *end;

		if (*ptr == '\0') {
			break;
		}
		end = strchr(ptr, '&');
		if (end == NULL) {
			end = strchr(ptr, '\0');
			last = 1;
		} else {
			*end = '\0';
			last = 0;
		}

		if ((end - ptr) < 3) {
			break;
		}

		if (memcmp(ptr, "w=", 2) == 0) {
			p->w = atoi(ptr + 2);
		} else if (memcmp(ptr, "h=", 2) == 0) {
			p->h = atoi(ptr + 2);
		} else if (memcmp(ptr, "start=", 6) == 0) {
			p->start = atol(ptr + 6);
		} else if (memcmp(ptr, "end=", 4) == 0) {
			p->end = atol(ptr + 4);
		} else if (memcmp(ptr, "pb=", 3) == 0) {
			p->pb = atoi(ptr + 3);
		}
		ptr = last ? end : end + 1;
	}
}

/* scgi working thread */
void *
scgi_thread(void *arg)
{
#define BUFSIZE 4096
	char buf[BUFSIZE];
	char key[BUFSIZE], val[BUFSIZE];
	char *start, *ptr;
	ssize_t rres;
	int rsize, i, stop_parse;
	struct scgi_thread_arg *scgi = arg;
	struct request req;
	struct response *resp;
	char *strresponse;

	/* FIXME: extremely simple (and incorrect) request handling */
	memset(buf, 0, BUFSIZE);
	rres = read(scgi->s, buf, sizeof(buf));
	if (rres < 0) {
		fprintf(stderr, "read() failed: %s\n", strerror(errno));
		goto fail;
	}
	start = memchr(buf, ':', rres);
	if (!start) {
		fprintf(stderr, "Can't find ':' in first %lu bytes of request\n", (long)rres);
		goto fail;
	}

	memcpy(key, buf, start - buf);
	key[start - buf] = '\0';
	rsize = atoi(key) + (start - buf) + 1;

	start++;
	ptr = start;
	stop_parse = 0;
	req.w = req.h = 0;
	req.pb  = 1; /* bytes */

	for (;;) {
		for (i=0; ; i++) {
			key[i] = *ptr;
			ptr++;
			if (key[i] == '\0') break;
			if (ptr >= (buf+rsize)) {
				stop_parse = 1;
				key[i+1] = '\0';
				break;
			}
		}
		for (i=0; ; i++) {
			val[i] = *ptr;
			ptr++;
			if (val[i] == '\0') break;
			if (ptr >= (buf+rsize)) {
				stop_parse = 2;
				break;
			}
		}
		if ((!stop_parse) || (stop_parse == 2)) {
			if (strcmp("QUERY_STRING", key) == 0) {
				parse_params(&req, val);
			}
		}
		if (stop_parse == 1) {
			/* last comma */
		}
		if (stop_parse) {
			break;
		}
	}

	if ((req.w < 100) || (req.w > 20000) || (req.h < 100) || (req.h > 20000)) {
		/* incorrect size */
	}

	resp = build_chart(&req);

	if (resp) {
		char strbuf[100];

		strresponse = malloc(resp->img.len + resp->weights.len + 1024*4); /* image + 4k for other text */
		strresponse[0] = '\0';
		strcat(strresponse, "Status: 200 OK\r\n");
		strcat(strresponse, "Content-Type: application/json\r\n");
		strcat(strresponse, "\r\n");
		strcat(strresponse, "{\n");
		strcat(strresponse, "\"status\": \"ok\",\n");

		snprintf(strbuf, sizeof(strbuf), "\"start\": \"%ld\",\n", (long)resp->start);
		strcat(strresponse, strbuf);
		snprintf(strbuf, sizeof(strbuf), "\"end\": \"%ld\",\n", (long)resp->end);
		strcat(strresponse, strbuf);
		snprintf(strbuf, sizeof(strbuf), "\"max\": \"%ld\",\n", (long)resp->max);
		strcat(strresponse, strbuf);

		strcat(strresponse, "\"img\": \"");
		strcat(strresponse, (char *)resp->img.ptr);
		strcat(strresponse, "\"\n");
		if (resp->wn) {
			strcat(strresponse, ",\n");
			strcat(strresponse, "\"weights\": \"");
			strcat(strresponse, (char *)resp->weights.ptr);
			strcat(strresponse, "\", \"legend\": [\n");

			for (i=0; i<resp->wn; i++) {
				char leg[PATH_MAX];
				snprintf(leg, PATH_MAX, "{ \"%s\": \"#%02X%02X%02X\" }",
					&resp->wnames[i * PATH_MAX],
					resp->clrlegend[i].red,
					resp->clrlegend[i].green,
					resp->clrlegend[i].blue);
				if (i != (resp->wn - 1)) strcat(leg, ", ");
				strcat(strresponse, leg);
			}
			strcat(strresponse, "]\n");

		}
		strcat(strresponse, "}\n");

		write(scgi->s, strresponse, strlen(strresponse));
		free(resp->img.ptr);
		if (resp->wn) {
			free(resp->weights.ptr);
			free(resp->wnames);
			free(resp->clrlegend);
		}
		free(resp);
		free(strresponse);
	} else {
		strresponse = malloc(1024*4);
		strresponse[0] = '\0';
		strcat(strresponse, "Status: 200 OK\r\n");
		strcat(strresponse, "Content-Type: application/json\r\n");
		strcat(strresponse, "\r\n");
		strcat(strresponse, "{\"status\": \"error\"}");

		write(scgi->s, strresponse, strlen(strresponse));
		free(strresponse);
	}

fail:
	close(scgi->s);
	free(scgi);
	return NULL;
#undef BUFSIZE
}

int
main(int argc, char *argv[])
{
	struct sockaddr_in name;
	in_addr_t saddr;
	int s;
	int ret = 1, on = 1;
	char addr_port[HOST_NAME_MAX];
	char addr[HOST_NAME_MAX];
	char *colon;
	int port;
	struct passwd *pswd;

	if (argc < 4) {
		fprintf(stderr, "Usage: %s [address:]port stat-directory user\n", argv[0]);
		fprintf(stderr, "For example: %s 127.0.0.1:9001 /var/lib/damper/ www-data\n", argv[0]);
		goto fail;
	}

	strncpy(addr_port, argv[1], HOST_NAME_MAX);
	if ((colon = strchr(addr_port, ':')) != NULL) {
		*colon = '\0';
		strcpy(addr, addr_port);
		port = atoi(colon + 1);
	} else {
		strcpy(addr, "127.0.0.1");
		port = atoi(addr_port);
	}

	strncpy(statpath, argv[2], PATH_MAX);

	/* get uid and gid */
	pswd = getpwnam(argv[3]);
	if (!pswd) {
		fprintf(stderr, "getpwnam() for user %s failed: %s\n", argv[3], strerror(errno));
		goto fail;
	}

	/* chroot */
	if (chroot(statpath) != 0) {
		fprintf(stderr, "chroot() to %s failed: %s\n", statpath, strerror(errno));
		goto fail;
	}

	/* and change gid and uid */
	if (setgid(pswd->pw_gid) < 0) {
		fprintf(stderr, "setgid() failed: %s\n", strerror(errno));
		goto fail;
	}
	if (setuid(pswd->pw_uid) < 0) {
		fprintf(stderr, "setuid() failed: %s\n", strerror(errno));
		goto fail;
	}


	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		fprintf(stderr, "socket() failed: %s\n", strerror(errno));
		goto fail;
	}

	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on)) != 0) {
		fprintf(stderr, "setsockopt() failed: %s\n", strerror(errno));
		goto fail_close;
	}

	memset(&name, 0, sizeof(name));
	name.sin_family = AF_INET;
	name.sin_port = htons(port);
	saddr = inet_addr(addr);
	if (saddr == -1) {
		fprintf(stderr, "Invalid address: '%s'\n", addr);
		goto fail_close;
	}
	name.sin_addr.s_addr = saddr;
	if (bind(s, (struct sockaddr *)&name, sizeof(name)) < 0) {
		fprintf(stderr, "bind() failed: %s\n", strerror(errno));
		goto fail_close;
	}
	if (listen(s, 5) < 0) {
		fprintf(stderr, "listen() failed: %s\n", strerror(errno));
		goto fail_close;
	}

	for (;;) {
		int client_sock;
		struct sockaddr_in remote_addr;
		int tres;
		socklen_t remote_addr_len = sizeof(remote_addr);
		pthread_t tid;
		struct scgi_thread_arg *scgi;

		client_sock = accept(s, (struct sockaddr *)&remote_addr, &remote_addr_len);
		if (client_sock < 0) {
			fprintf(stderr, "accept() failed: %s\n", strerror(errno));
			continue;
		}

		scgi = malloc(sizeof(struct scgi_thread_arg));
		if (!scgi) {
			fprintf(stderr, "malloc() failed: %s\n", strerror(errno));
			goto fail_close;
		}

		scgi->s = client_sock;

		tres = pthread_create(&tid, NULL, &scgi_thread, scgi);
		if (tres != 0) {
			fprintf(stderr, "pthread_create() failed: %s\n", strerror(errno));
			continue;
		}
		pthread_detach(tid);
	}
	ret = 0;

fail_close:
	close(s);
fail:
	return ret;
}


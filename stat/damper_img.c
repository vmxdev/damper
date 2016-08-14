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

#include "../damper.h"

static char statpath[PATH_MAX];

/* struct for passing parameters to scgi thread */
struct scgi_thread_arg
{
	int s; /* socket */
};

/* requested parameters */
struct request_params
{
	int w, h;          /* image width and height */
	time_t start, end; /* start and end time of chart */
	int pb;            /* packets or bytes, if zero display packets in chart */
	int apx;           /* approximation */
};

/* A coloured pixel */
typedef struct {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
} pixel_t;

/* A picture. */
typedef struct  {
	pixel_t *pixels;
	size_t width;
	size_t height;
} bitmap_t;

/* png file in memory */
struct membuf
{
	size_t len;
	unsigned char *ptr;
};

/* response */
struct response
{
	time_t start, end;
	uint64_t max;       /* max data value (peak) in response */
	struct membuf img;
};

/* Base64 translation table */
static const char cb64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* encode memory block to base64 */
static void
b64encode(unsigned char *in, unsigned char *out, int len)
{
	int idx, i;

#define OUT(SYM) *(out++) = SYM

	for (i=0; i<len; i+=3) {
		idx = (in[i] & 0xFC) >> 2;
		OUT(cb64[idx]);
		idx = (in[i] & 0x03) << 4;

		if (i + 1 < len) {
			idx |= (in[i + 1] & 0xF0) >> 4;
			OUT(cb64[idx]);
			idx = (in[i + 1] & 0x0F) << 2;
			if (i + 2 < len) {
				idx |= (in[i + 2] & 0xC0) >> 6;
				OUT(cb64[idx]);
				idx = in[i + 2] & 0x3F;
				OUT(cb64[idx]);
			} else {
				OUT(cb64[idx]);
				OUT('=');
			}
		} else {
			OUT(cb64[idx]);
			OUT('=');
			OUT('=');
		}
	}
	OUT('\0');

#undef OUT
}

/* png writing callback */
static void png_write_callback(png_structp png_ptr, png_bytep data, png_size_t length)
{
	struct membuf *m;
	m = png_get_io_ptr(png_ptr);
	/* FIXME: check return value */
	m->ptr = realloc(m->ptr, m->len + length);
	memcpy(m->ptr + m->len, data, length);
	m->len += length;
}

static int
write_png_to_mem(bitmap_t *bitmap, struct membuf *mem)
{
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;
	size_t x, y;
	png_byte **row_pointers = NULL;
	/* "status" contains the return value of this function. At first
	   it is set to a value which means 'failure'. When the routine
	   has finished its work, it is set to a value which means
	   'success'. */
	int status = -1;
	/* The following number is set by trial and error only. I cannot
	   see where it it is documented in the libpng manual.
	*/
	int pixel_size = 3;
	int depth = 8;

	png_ptr = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (png_ptr == NULL) {
		goto png_create_write_struct_failed;
	}

	info_ptr = png_create_info_struct (png_ptr);
	if (info_ptr == NULL) {
		goto png_create_info_struct_failed;
	}

	/* Set up error handling. */
	if (setjmp (png_jmpbuf (png_ptr))) {
		goto png_failure;
	}

	/* Set image attributes. */
	png_set_IHDR (png_ptr,
		info_ptr,
		bitmap->width,
		bitmap->height,
		depth,
		PNG_COLOR_TYPE_RGB,
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT);

	png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);

	/* Initialize rows of PNG. */
	row_pointers = png_malloc (png_ptr, bitmap->height * sizeof (png_byte *));
	for (y=0; y<bitmap->height; y++) {
		png_byte *row =
			png_malloc(png_ptr, sizeof (uint8_t) * bitmap->width * pixel_size);
		row_pointers[y] = row;
		for (x=0; x<bitmap->width; x++) {
			pixel_t *pixel = bitmap->pixels + bitmap->width * y + x;
			*row++ = pixel->red;
			*row++ = pixel->green;
			*row++ = pixel->blue;
		}
	}

	/* Write the image data to "fp". */
	png_set_rows(png_ptr, info_ptr, row_pointers);

	png_set_write_fn(png_ptr, mem, png_write_callback, NULL);
	png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

	/* The routine has successfully written the file, so we set
	"status" to a value which indicates success. */
	status = 0;

	for (y=0; y<bitmap->height; y++) {
		png_free(png_ptr, row_pointers[y]);
	}
	png_free(png_ptr, row_pointers);

png_failure:
png_create_info_struct_failed:
	png_destroy_write_struct (&png_ptr, &info_ptr);
png_create_write_struct_failed:

	return status;
}

/* draw pixel on bitmap */
static void
put_pixel(bitmap_t *bmp, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	pixel_t *pixel = bmp->pixels + bmp->width * y + x;
	pixel->red = r;
	pixel->green = g;
	pixel->blue = b;
}

/* horizontal line */
static void
horiz_line(bitmap_t *bmp, int x1, int x2, int y, uint8_t r, uint8_t g, uint8_t b)
{
	int i;

	for (i=x1; i<=x2; i++) {
		put_pixel(bmp, i, y, r, g, b);
	}
}

/* vertical line */
static void
vert_line(bitmap_t *bmp, int x, int y1, int y2, uint8_t r, uint8_t g, uint8_t b)
{
	int i;

	for (i=y1; i<y2; i++) {
		put_pixel(bmp, x, i, r, g, b);
	}
}

/* background with grid */
static void
draw_bg(bitmap_t *bmp)
{
	int i;

	/* fill background */
	for (i=0; i<bmp->width; i++) {
		int j;

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

/* calculate height of row */
static void
calc_line_h(struct stat_info *info, int *lines, size_t i, struct request_params *p, int n)
{
/* hmm */
#define SETMAX(x, y) x = (((x) > (y)) ? (x) : (y))
#define SETMIN(x, y) if (x != 0) { x = (((x) < (y)) ? (x) : (y)); } else { x = y; }
/* iterative mean, http://www.heikohoffmann.de/htmlthesis/node134.html */
#define SETAVG(x, y, t) x = (x + (((int64_t)y - x) / (t + 1)))

	if (p->pb) {
		/* display octets */
		switch (p->apx) {
			case 1: /* min */
				SETMIN (lines[i*2 + 0],  info->octets_pass);
				SETMIN (lines[i*2 + 1],  info->octets_drop);
				break;
			case 2: /* avg */
				SETAVG (lines[i*2 + 0],  info->octets_pass, n);
				SETAVG (lines[i*2 + 1],  info->octets_drop, n);
				break;
			case 0: /* max */
			default:
				SETMAX (lines[i*2 + 0],  info->octets_pass);
				SETMAX (lines[i*2 + 1],  info->octets_drop);
				break;
		}
	} else {
		/* or display packets */
		switch (p->apx) {
			case 1:
				SETMIN (lines[i*2 + 0],  info->packets_pass);
				SETMIN (lines[i*2 + 1],  info->packets_drop);
				break;
			case 2:
				SETAVG (lines[i*2 + 0],  info->packets_pass, n);
				SETAVG (lines[i*2 + 1],  info->packets_drop, n);
				break;
			case 0:
			default:
				SETMAX (lines[i*2 + 0],  info->packets_pass);
				SETMAX (lines[i*2 + 1],  info->packets_drop);
				break;
		}
	}
#undef SETAVG
#undef SETMIN
#undef SETMAX
}

/* draw chart */
static struct response *
build_chart(struct request_params *p)
{
	bitmap_t rep;
	FILE *f;       /* statistics file */
	time_t tstart; /* statistics start time */
	size_t s;
	int *lines, max_h;
	size_t i, n;   /* n - number of records in file */
	struct stat st;
	struct membuf mempng;  /* png in memory */
	struct response *r = NULL;
	char sp[PATH_MAX];

	lines = calloc(2 * sizeof(int), p->w);
	if (!lines) {
		goto fail;
	}

	snprintf(sp, PATH_MAX, "%s/stat.dat", statpath);
	f = fopen(sp, "r");
	if (!f) {
		fprintf(stderr, "Can't open stat file\n");
		goto fail_freelines;
	}

	/* get number of stat_info records */
	if (fstat(fileno(f), &st) != 0) {
		goto fail_close;
	}

	n = (st.st_size - sizeof(time_t)) / sizeof(struct stat_info);
	if (n < 1) {
		goto fail_close;
	}

	max_h = 0;

	/* read header */
	s = fread(&tstart, 1, sizeof(time_t), f);
	if (s != sizeof(time_t)) {
		goto fail_close;
	}

	/* create an image */
	rep.width = p->w;
	rep.height = p->h;

	rep.pixels = calloc(sizeof (pixel_t), rep.width * rep.height);
	if (!rep.pixels) {
		goto fail_close;
	}

	if (p->start == 0) {
		p->start = tstart;
		p->end = p->start + n;
	}

	/* prepare chart, get heights (passed and dropped) of each row */
	for (i=0; i<p->w; i++) {
		int64_t idx_start, idx_end, idx;
		struct stat_info info;

		idx_start = (double)p->start + (double)i * (p->end - p->start) / (double)p->w;
		idx_end   = (double)p->start + (double)(i + 1) * (p->end - p->start) / (double)p->w;

		for (idx=idx_start; idx<=idx_end; idx++) {
			if ((idx < tstart) || (idx >= (tstart + n))) {
				continue;
			}
			fseek(f, (idx - tstart)* sizeof(struct stat_info) + sizeof(time_t), SEEK_SET);
			s = fread(&info, 1, sizeof(struct stat_info), f);
			if (s != sizeof(struct stat_info)) {
				continue;
			}

			calc_line_h(&info, lines, i, p, idx - idx_start);
		}
		if (max_h < lines[i*2 + 0]) max_h = lines[i*2 + 0];
		if (max_h < lines[i*2 + 1]) max_h = lines[i*2 + 1];
	}

	draw_bg(&rep);

	/* draw chart */
	for (i=0; i<p->w; i++) {
		int line_h_p, line_h_d, j;

		if (max_h > 0) {
			line_h_p = p->h * lines[i * 2 + 0] / max_h;
			line_h_d = p->h * lines[i * 2 + 1] / max_h;
		} else {
			line_h_p = line_h_d = 0;
		}

		for (j=0; j<line_h_p; j++) {
			pixel_t *pixel = rep.pixels + p->w * (p->h - j - 1) + i;
			pixel->red = 0;
			pixel->green = 200;
			pixel->blue = 0;
		}

		for (j=0; j<line_h_d; j++) {
			pixel_t *pixel = rep.pixels + p->w * (p->h - j - 1) + i;
			pixel->red = 100;
			pixel->green = 50;
			pixel->blue = 0;
		}
	}


	/* Write the image to memory */
	mempng.len = 0;
	mempng.ptr = NULL;
	write_png_to_mem(&rep, &mempng);

	/* allocate response */
	r = malloc(sizeof(struct response));
	if (!r) {
		goto fail_resp;
	}

	/* base64 encoding */
	r->img.len = (mempng.len + 1)* 4 / 3 + 3;
	r->img.ptr = malloc(r->img.len);
	b64encode(mempng.ptr, r->img.ptr, mempng.len);
	r->start = p->start;
	r->end = p->end;

	r->max = max_h;

fail_resp:
	/* free raw image */
	free(mempng.ptr);

	free(rep.pixels);
fail_close:
	fclose(f);
fail_freelines:
	free(lines);
fail:
	return r;
}

/* parse script params */
void
parse_params(struct request_params *p, char *q)
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
		} else if (memcmp(ptr, "apx=", 4) == 0) {
			p->apx = atoi(ptr + 4);
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
	struct request_params params;
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
	params.w = params.h = 0;
	params.pb  = 1; /* bytes */
	params.apx = 0; /* max */

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
				parse_params(&params, val);
			}
		}
		if (stop_parse == 1) {
			/* last comma */
		}
		if (stop_parse) {
			break;
		}
	}

	if ((params.w < 100) || (params.w > 20000) || (params.h < 100) || (params.h > 20000)) {
		/* incorrect size */
	}

	resp = build_chart(&params);
	if (resp) {
		char strbuf[100];

		strresponse = malloc(resp->img.len + 1024*4); /* image + 4k for other text */
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
		strcat(strresponse, "}\n");

		write(scgi->s, strresponse, strlen(strresponse));
		free(resp->img.ptr);
		free(resp);
		free(strresponse);
	} else {
		strresponse = malloc(1024*4);
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
	char *addr = "127.0.0.1";
	int port;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s port stat-directory\n", argv[0]);
		fprintf(stderr, "For example: %s 9001 /var/lib/damper/\n", argv[0]);
		goto fail;
	}

	port = atoi(argv[1]);
	strncpy(statpath, argv[2], PATH_MAX);

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


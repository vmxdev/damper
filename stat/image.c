#include <errno.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "image.h"

/* calculate crc6 for string
   taken from http://electronix.ru/forum/lofiversion/index.php/t92226.html */
static unsigned char
crc6(unsigned char *data, unsigned char n)
{
	unsigned char i, j;
	unsigned char cs = 0, cst;

	for(i = 0; i < n; ++i) {
		cst = *(data + i);
		for(j = 0; j < 8; ++j) {
			cs >>= 1;
			if(((cs << 6) ^ (cst << 7)) & (1 << 7)) {
				cs ^= 0xC2;
			}
			cst >>= 1;
		}
	}
	return (cs >> 2);
}

/* generate unique color from string */
void
str2color(char *s, size_t n, pixel_t *p)
{
	unsigned char color;

	color = crc6((unsigned char *)s, n);
	p->red = (color & 0x30) << 2;
	p->green = (color & 0x0c) << 4;
	p->blue = (color & 0x03) << 6;
}

/* Base64 translation table */
static const char cb64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* encode memory block to base64 */
void
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
static void
png_write_callback(png_structp png_ptr, png_bytep data, png_size_t length)
{
	struct pngmembuf *m;

	m = png_get_io_ptr(png_ptr);
	/* FIXME: check return value */
	m->ptr = realloc(m->ptr, m->len + length);
	memcpy(m->ptr + m->len, data, length);
	m->len += length;
}

/* generate png in memory */
int
mk_mempng(bitmap_t *bitmap, struct pngmembuf *mem)
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

	/* FIXME: take compression level from user? */
	/*png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);*/

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
void
put_pixel(bitmap_t *bmp, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	pixel_t *pixel = bmp->pixels + bmp->width * y + x;
	pixel->red = r;
	pixel->green = g;
	pixel->blue = b;
}

/* get pixel color */
void
get_pixel(bitmap_t *bmp, int x, int y, pixel_t *p)
{
	pixel_t *pixel = bmp->pixels + bmp->width * y + x;
	*p = *pixel;
}

/* horizontal line */
void
horiz_line(bitmap_t *bmp, int x1, int x2, int y, uint8_t r, uint8_t g, uint8_t b)
{
	int i;

	for (i=x1; i<=x2; i++) {
		put_pixel(bmp, i, y, r, g, b);
	}
}

/* vertical line */
void
vert_line(bitmap_t *bmp, int x, int y1, int y2, uint8_t r, uint8_t g, uint8_t b)
{
	int i;

	for (i=y1; i<y2; i++) {
		put_pixel(bmp, x, i, r, g, b);
	}
}



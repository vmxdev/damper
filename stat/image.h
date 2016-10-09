#ifndef image_h_included
#define image_h_included

#include <stdint.h>

/* A coloured pixel */
typedef struct
{
	uint8_t red;
	uint8_t green;
	uint8_t blue;
} pixel_t;

/* A picture. */
typedef struct
{
	pixel_t *pixels;
	size_t width;
	size_t height;
} bitmap_t;

/* png file in memory */
struct pngmembuf
{
	size_t len;
	unsigned char *ptr;
};

/* generate unique color from string */
void str2color(char *s, size_t n, pixel_t *p);

/* encode memory block to base64 */
void b64encode(unsigned char *in, unsigned char *out, int len);

/* generate png in memory */
int mk_mempng(bitmap_t *bitmap, struct pngmembuf *mem);

/* draw pixel on bitmap */
void put_pixel(bitmap_t *bmp, int x, int y, uint8_t r, uint8_t g, uint8_t b);

/* get pixel color */
void get_pixel(bitmap_t *bmp, int x, int y, pixel_t *p);

/* horizontal line */
void horiz_line(bitmap_t *bmp, int x1, int x2, int y, uint8_t r, uint8_t g, uint8_t b);

/* vertical line */
void vert_line(bitmap_t *bmp, int x, int y1, int y2, uint8_t r, uint8_t g, uint8_t b);

#endif


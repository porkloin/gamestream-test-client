/* pattern.c draws what framestamp draws: frames hashed against framestamp's own (its GPU's, `framestamp --dump-frame N
 * --mode M --size 1920x1080`), and --check-picture reads a clean frame as clean and a broken one as broken. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_client.h"

static int failures;
#define CHECK(cond)                                                                \
	do {                                                                       \
		if (!(cond)) {                                                     \
			fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                \
		}                                                                  \
	} while (0)

#define W 1920
#define H 1080

static uint8_t rgb[W * H * 3];
static uint8_t luma[W * H];

static uint64_t draw(uint32_t frame, uint32_t mode)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint8_t *c = rgb + ((size_t)y * W + x) * 3;
			pattern_rgb(W, H, frame, mode, x, y, c);
			for (int i = 0; i < 3; i++)
				h = (h ^ c[i]) * 0x100000001b3ull;
		}
	return h;
}

/* Luma as an encoder makes it: BT.709, limited range. */
static void to_luma(void)
{
	for (size_t i = 0; i < (size_t)W * H; i++) {
		const uint8_t *c = rgb + i * 3;
		luma[i] = (uint8_t)(16 + (47 * c[0] + 157 * c[1] + 16 * c[2] + 128) / 256);
	}
}

/* A row source that hands out only the rows asked for, in a buffer of their own, as a GPU readback does: reading past
 * them would read the wrong picture. */
static uint8_t strip_buf[64 * W];

static const uint8_t *strip_rows(void *ctx, int y0, int h, int *stride)
{
	if (y0 < 0 || y0 + h > H || h > 64)
		return NULL;
	memset(strip_buf, 0, sizeof strip_buf);
	memcpy(strip_buf, luma + (size_t)y0 * W, (size_t)h * W);
	*stride = W;
	return strip_buf;
}

int main(void)
{
	static const struct {
		uint32_t frame, mode;
		uint64_t hash;
	} vectors[] = {
		{0, PATTERN_STATIC, 0x6e61a791df72274full},   {1, PATTERN_STATIC, 0x14cca2c04835188full},
		{330, PATTERN_STATIC, 0xc0d95a66433db4cfull}, {1234, PATTERN_STATIC, 0x8ab37e9fc80c790full},
		{0, PATTERN_REGION, 0x4b1fc5395acf8107ull},   {1, PATTERN_REGION, 0x9ad40ed0a27da5cbull},
		{330, PATTERN_REGION, 0x932360b1ef06b6fcull}, {1234, PATTERN_REGION, 0x54b48805bbd46666ull},
		{0, PATTERN_FULL, 0xcf0e60c667b105d6ull},     {1, PATTERN_FULL, 0xd396acb5dee1efc7ull},
		{330, PATTERN_FULL, 0x8b5cc90ee1ffae23ull},   {1234, PATTERN_FULL, 0x4ac759a21062c013ull},
	};
	for (size_t i = 0; i < sizeof vectors / sizeof *vectors; i++) {
		uint64_t h = draw(vectors[i].frame, vectors[i].mode);
		if (h != vectors[i].hash)
			fprintf(stderr, "frame %u mode %u: %016llx, framestamp drew %016llx\n", vectors[i].frame,
				vectors[i].mode, (unsigned long long)h, (unsigned long long)vectors[i].hash);
		CHECK(h == vectors[i].hash);
	}

	/* A clean frame: its stamp read, and every tile within a level or two (the luma rounding). */
	struct picture_result r;
	draw(330, PATTERN_FULL);
	to_luma();
	picture_check(luma, W, 8, W, H, &r);
	CHECK(r.readable && r.frame == 330 && r.mode == PATTERN_FULL && r.worst < 2);

	/* The same frame read a strip at a time: the same result. */
	struct picture_result rs;
	picture_check_rows(strip_rows, NULL, 8, W, H, &rs);
	CHECK(rs.readable && rs.frame == r.frame && rs.mode == r.mode && rs.worst == r.worst && rs.mean == r.mean);

	/* The frame before it, decoded as this one: the stamp says 330, the picture is 329's. A moving scene off by a frame
	 * is off by more than encoding. */
	draw(329, PATTERN_FULL);
	to_luma();
	for (int x = 0; x < 36 * 16; x++)
		for (int y = 0; y < 32; y++) {
			uint8_t c[3];
			pattern_rgb(W, H, 330, PATTERN_FULL, (uint32_t)x, (uint32_t)y, c);
			luma[y * W + x] = (uint8_t)(16 + (47 * c[0] + 157 * c[1] + 16 * c[2] + 128) / 256);
		}
	picture_check(luma, W, 8, W, H, &r);
	CHECK(r.readable && r.frame == 330 && r.worst > 20);

	/* A smear across the middle: corrupt. */
	draw(330, PATTERN_FULL);
	to_luma();
	for (int y = H / 3; y < H / 2; y++)
		memset(luma + (size_t)y * W, 128, W);
	picture_check(luma, W, 8, W, H, &r);
	CHECK(r.readable && r.worst > 20);
	picture_check_rows(strip_rows, NULL, 8, W, H, &rs);
	CHECK(rs.readable && rs.worst == r.worst);

	/* No stamp: unreadable. */
	memset(luma, 90, sizeof luma);
	picture_check(luma, W, 8, W, H, &r);
	CHECK(!r.readable);

	if (failures == 0)
		printf("pattern: ok\n");
	return failures != 0;
}

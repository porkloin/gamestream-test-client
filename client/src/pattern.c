/* framestamp's picture (github.com/porkloin/framestamp, its PATTERN.md and shaders/pattern.frag): the same function of
 * the pixel, the size, the frame number and the mode, in the same unsigned integer arithmetic, so the client can draw
 * any frame framestamp drew and compare it with what it decoded. Change one, change both; tests/test_pattern.c holds
 * framestamp's test vectors (hashes of frames its GPU drew).
 *
 * --check-picture: each decoded frame's stamp (its number and mode, top left) is read, then 84 tiles across the
 * picture are compared with the frame drawn again, in luma at 16 points each. The luma the encoder made from framestamp's
 * RGB is taken as BT.709 or BT.601 (whichever fits the frame better), scaled between the stamp's own black and white
 * cells, so limited or full range doesn't matter. A frame is corrupt when its worst tile is off by more than the
 * threshold on average: encoding loses a few levels, a picture built on the wrong reference far more. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_client.h"

#define OFF (1u << 24)
#define SCENE_FRAMES 600u
#define TILES_X 12
#define TILES_Y 7

static uint32_t hash(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
	return hash(a ^ hash(b ^ hash(c)));
}

static uint32_t noise(uint32_t u, uint32_t v, uint32_t l, uint32_t seed, uint32_t ch)
{
	uint32_t lf = l * 16u;
	uint32_t i = u / lf, j = v / lf, fx = u % lf, fy = v % lf;
	uint32_t s = seed * 3u + ch;
	uint32_t v00 = hash3(i, j, s) & 255u;
	uint32_t v10 = hash3(i + 1u, j, s) & 255u;
	uint32_t v01 = hash3(i, j + 1u, s) & 255u;
	uint32_t v11 = hash3(i + 1u, j + 1u, s) & 255u;
	uint32_t top = v00 * (lf - fx) + v10 * fx;
	uint32_t bot = v01 * (lf - fx) + v11 * fx;
	return (top * (lf - fy) + bot * fy) / (lf * lf);
}

static void camera(uint32_t t, int32_t *cx, int32_t *cy)
{
	static const int32_t len[5] = {240, 60, 120, 120, 60};
	static const int32_t vx[5] = {24, 384, 48, 0, 24}, vy[5] = {4, 0, -8, 0, 4};
	int32_t start = 0;
	*cx = *cy = 0;
	for (int k = 0; k < 5; k++) {
		int32_t d = (int32_t)t - start;
		d = d < 0 ? 0 : d > len[k] ? len[k] : d;
		*cx += vx[k] * d;
		*cy += vy[k] * d;
		start += len[k];
	}
}

static uint32_t tri(uint32_t t, uint32_t per)
{
	int32_t a = (int32_t)((t + per) % (2u * per)) - (int32_t)per;
	return (uint32_t)(a < 0 ? -a : a);
}

static void bytes3(uint32_t h, uint8_t c[3])
{
	c[0] = h & 255u;
	c[1] = (h >> 8) & 255u;
	c[2] = (h >> 16) & 255u;
}

bool pattern_rgb(uint32_t width, uint32_t height, uint32_t frame, uint32_t mode, uint32_t x, uint32_t y, uint8_t c[3])
{
	bool particle = false;
	uint32_t s = height / 270u > 1u ? height / 270u : 1u;
	uint32_t wv = width / s, hv = height / s;
	uint32_t w16 = wv * 16u, h16 = hv * 16u;
	uint32_t X = x * 16u / s, Y = y * 16u / s;

	uint32_t scene = mode == PATTERN_FULL ? frame / SCENE_FRAMES : 0u;
	uint32_t t = mode == PATTERN_STATIC ? 0u : frame % SCENE_FRAMES;
	uint32_t sb = hash(scene + 1u);
	int32_t camx = 0, camy = 0;
	if (mode == PATTERN_FULL)
		camera(t, &camx, &camy);
	uint32_t cx = (uint32_t)camx, cy = (uint32_t)(camy + 16384);

	uint32_t u = X + cx + OFF, v = Y + cy + OFF;
	for (uint32_t ch = 0; ch < 3; ch++)
		c[ch] = (uint8_t)((3u * noise(u, v, 32u, sb, ch) + noise(u, v, 8u, sb + 1u, ch)) / 4u);

	uint32_t u1 = X + 2u * cx + OFF;
	uint32_t horizon = h16 * 5u / 8u + noise(u1, OFF, 64u, sb + 2u, 0u) * (h16 / 4u) / 255u;
	if (Y > horizon)
		for (uint32_t ch = 0; ch < 3; ch++)
			c[ch] = (uint8_t)(noise(u1, v, 16u, sb + 3u, ch) * 3u / 4u);

	uint32_t sprites = mode == PATTERN_FULL ? 6u : mode == PATTERN_REGION ? 1u : 0u;
	for (uint32_t k = 0; k < sprites; k++) {
		uint32_t hk = hash3(k, 77u, sb);
		int32_t sz = (int32_t)((24u + (hk & 31u)) * 16u);
		uint32_t speed = 16u + ((hk >> 5) & 63u);
		uint32_t span = w16 + 2u * (uint32_t)sz;
		int32_t px = (int32_t)(((hk >> 11) % span + speed * t) % span) - sz;
		uint32_t per = 90u + ((hk >> 24) & 63u);
		int32_t py = (int32_t)(h16 / 8u + (hk >> 16) % (h16 / 2u) + tri(t, per) * (h16 / 8u) / per);
		int32_t dx = (int32_t)X - px, dy = (int32_t)Y - py;
		if (dx >= 0 && dx < sz && dy >= 0 && dy < sz)
			bytes3(((((uint32_t)dx >> 7) + ((uint32_t)dy >> 7)) & 1u) == 0u ? hash3(k, 1u, sb) : hash3(k, 2u, sb), c);
	}

	if (mode == PATTERN_FULL && t >= 300u && t < 420u && X >= w16 * 5u / 8u && X < w16 * 7u / 8u && Y >= h16 / 8u &&
	    Y < h16 * 3u / 8u) {
		bytes3(hash3(X >> 5, Y >> 5, frame ^ sb), c);
		particle = true;
	}

	if (Y >= h16 - 384u) {
		c[0] = c[1] = c[2] = 40;
		if (X >= 128u && X < w16 / 4u && Y >= h16 - 256u && Y < h16 - 128u) {
			c[0] = 200;
			c[1] = c[2] = 40;
		}
	}

	uint32_t xc = x / s / 4u, yc = y / s / 4u;
	if (yc < 2u && xc < 36u) {
		uint32_t bit = xc == 0u ? 1u : xc == 1u ? 0u : xc < 34u ? (frame >> (33u - xc)) & 1u : (mode >> (35u - xc)) & 1u;
		c[0] = c[1] = c[2] = (bit ^ yc) == 1u ? 255 : 0;
	}
	return particle && !(Y >= h16 - 384u);
}

/* A luma sample, 8-bit (a 16-bit plane keeps its bits high, as P010 does). */
static int sample(const uint8_t *plane, int stride, int depth, int x, int y)
{
	const uint8_t *row = plane + (size_t)y * stride;
	return depth > 8 ? ((const uint16_t *)row)[x] >> 8 : row[x];
}

/* --picture-dump: the first few corrupt frames' luma, as decoded and as expected, side by side in a PGM. */
static const char *dump_dir;
static int dumped;

void picture_set_dump(const char *dir)
{
	dump_dir = dir;
}

void picture_dump(const uint8_t *plane, int stride, int depth, int width, int height, const struct picture_result *r)
{
	if (dump_dir == NULL || dumped >= 3 || !r->readable)
		return;
	char path[4096];
	snprintf(path, sizeof path, "%s/picture-%u.pgm", dump_dir, r->frame);
	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return;
	dumped++;
	/* Left: decoded. Right: what framestamp drew, as BT.709 luma, full range. */
	fprintf(f, "P5\n%d %d\n255\n", 2 * width, height);
	uint8_t *row = malloc((size_t)width * 2);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint8_t c[3];
			row[x] = (uint8_t)sample(plane, stride, depth, x, y);
			pattern_rgb((uint32_t)width, (uint32_t)height, r->frame, r->mode, (uint32_t)x, (uint32_t)y, c);
			row[width + x] = (uint8_t)((54 * c[0] + 183 * c[1] + 19 * c[2]) / 256);
		}
		fwrite(row, 1, (size_t)width * 2, f);
	}
	free(row);
	fclose(f);
}

void picture_check(const uint8_t *plane, int stride, int depth, int width, int height, struct picture_result *out)
{
	memset(out, 0, sizeof *out);
	uint32_t s = height / 270 > 1 ? (uint32_t)height / 270u : 1u, cell = 4u * s;
	if ((uint32_t)width < 36u * cell || (uint32_t)height < 2u * cell)
		return;
	/* The stamp: each cell's two rows are a bit and its inverse, so the brighter one says which. */
	uint32_t frame = 0, mode = 0;
	long black = 0, white = 0;
	for (uint32_t xc = 0; xc < 36u; xc++) {
		int x = (int)(xc * cell + cell / 2), a = sample(plane, stride, depth, x, (int)(cell / 2)),
		    b = sample(plane, stride, depth, x, (int)(cell + cell / 2));
		/* Too little between them to tell: not a stamp (or a picture too broken to read). */
		if (abs(a - b) < 48)
			return;
		int bit = a > b;
		if ((xc == 0 && !bit) || (xc == 1 && bit))
			return;
		if (xc >= 2 && xc < 34)
			frame = frame << 1 | (uint32_t)bit;
		else if (xc >= 34)
			mode = mode << 1 | (uint32_t)bit;
		white += bit ? a : b;
		black += bit ? b : a;
	}
	if (mode > PATTERN_FULL)
		return;
	out->readable = true;
	out->frame = frame;
	out->mode = mode;
	double lo = black / 36.0, hi = white / 36.0;

	/* The tiles, against the frame drawn again: each one's mean error under either matrix. */
	/* Tiles over the particles are left out: noise new every frame doesn't survive encoding, by design. */
	double worst[2] = {0, 0}, total[2] = {0, 0};
	int tiles = 0;
	for (int ty = 0; ty < TILES_Y; ty++)
		for (int tx = 0; tx < TILES_X; tx++) {
			int x0 = width * (2 * tx + 1) / (2 * TILES_X) - 8, y0 = height * (2 * ty + 1) / (2 * TILES_Y) - 8;
			double err[2] = {0, 0};
			bool particles = false;
			for (int dy = 0; dy < 16 && !particles; dy += 4)
				for (int dx = 0; dx < 16; dx += 4) {
					int x = x0 + dx, y = y0 + dy;
					uint8_t c[3];
					if (pattern_rgb((uint32_t)width, (uint32_t)height, frame, mode, (uint32_t)x, (uint32_t)y, c)) {
						particles = true;
						break;
					}
					double got = sample(plane, stride, depth, x, y);
					double y709 = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
					double y601 = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
					double e709 = got - (lo + (hi - lo) * y709 / 255.0), e601 = got - (lo + (hi - lo) * y601 / 255.0);
					err[0] += e709 < 0 ? -e709 : e709;
					err[1] += e601 < 0 ? -e601 : e601;
				}
			if (particles)
				continue;
			tiles++;
			for (int m = 0; m < 2; m++) {
				err[m] /= 16;
				total[m] += err[m];
				if (err[m] > worst[m])
					worst[m] = err[m];
			}
		}
	int m = total[1] < total[0];
	out->worst = worst[m];
	out->mean = tiles > 0 ? total[m] / tiles : 0;
}

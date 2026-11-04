/* Tests for click.c, with the moonlight-common-c calls it makes stubbed. */
#include <stdio.h>
#include <string.h>

#include "test_client.h"

uint64_t LiGetMicroseconds(void)
{
	return 0;
}
int LiSendMousePositionEvent(short x, short y, short w, short h)
{
	return 0;
}
int LiSendMouseButtonEvent(char action, int button)
{
	return 0;
}

static int failures;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__, #cond);                               \
			failures++;                                                                                    \
		}                                                                                                          \
	} while (0)

static void test_flash_after_click(void)
{
	click_configure(500);
	struct click_stats s;
	uint64_t t0 = 10000000;
	click_on_luma(0.06, t0 - 1000, 0, 0);  /* dark before the click */
	click_sent(t0);
	click_on_luma(0.06, t0 + 16000, 0, 0); /* still dark */
	click_on_luma(0.92, t0 + 40000, 0, 0); /* the flash */
	click_on_luma(0.92, t0 + 56000, 0, 0); /* still bright: not another flash */
	click_on_luma(0.06, t0 + 72000, 0, 0);
	click_stats(&s);
	CHECK(s.clicks == 1 && s.flashes == 1 && s.missed == 0 && s.samples == 1);
	CHECK(s.decoded_ms[0] == 40.0);

	/* A bright picture with no click waiting isn't a sample. */
	click_on_luma(0.92, t0 + 200000, 0, 0);
	click_on_luma(0.06, t0 + 216000, 0, 0);
	click_stats(&s);
	CHECK(s.samples == 1);
}

static void test_missed(void)
{
	click_configure(500);
	struct click_stats s;
	uint64_t t0 = 50000000;
	click_sent(t0);
	click_sent(t0 + 500000);              /* the first never flashed */
	click_on_luma(0.06, t0 + 2000000, 0, 0); /* nor the second, within a second */
	click_stats(&s);
	CHECK(s.clicks == 2 && s.missed == 2 && s.samples == 0);
}

static void test_luma(void)
{
	static uint8_t plane8[64 * 64];
	memset(plane8, 235, sizeof(plane8));
	CHECK(click_luma(plane8, 64, 64, 64, 8) > 0.9);
	static uint16_t plane10[64 * 64];
	for (size_t i = 0; i < 64 * 64; i++)
		plane10[i] = 64;
	double l = click_luma((const uint8_t *)plane10, 64 * 2, 64, 64, 10);
	CHECK(l > 0.05 && l < 0.07);
	CHECK(click_luma(plane8, 16, 16, 16, 8) == 0); /* smaller than the patch */
}

/* The app's side of the id: the cell under the pointer, from its position over a window that may be scaled. */
static int cell(double x, double y, double width, double height)
{
	return (int)(y * CLICK_ROWS / height) * CLICK_COLS + (int)(x * CLICK_COLS / width);
}

static int flash_id, flash_frame;
static void on_flash(int id, int frame_number, uint64_t sent, uint64_t decoded, uint64_t received)
{
	(void)sent, (void)decoded, (void)received;
	flash_id = id;
	flash_frame = frame_number;
}

static void test_ids(void)
{
	CHECK(click_id(1) == 1 && click_id(CLICK_COLS * CLICK_ROWS) == 0 && click_id(CLICK_COLS * CLICK_ROWS + 5) == 5);
	const int sizes[][2] = {{1920, 1080}, {1280, 720}, {3840, 2160}, {800, 600}};
	for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
		for (int id = 0; id < CLICK_COLS * CLICK_ROWS; id += 37) {
			short x, y;
			click_position(id, sizes[s][0], sizes[s][1], &x, &y);
			CHECK(cell(x, y, sizes[s][0], sizes[s][1]) == id);
			/* Scaled to a window of another size, it lands in the same cell. */
			CHECK(cell(x * 1.5, y * 1.5, sizes[s][0] * 1.5, sizes[s][1] * 1.5) == id);
		}
	click_configure(100);
	click_on_flash = on_flash;
	uint64_t t0 = 1000000;
	click_on_luma(0.06, t0 - 1000, 0, 41);
	click_sent(t0);
	click_on_luma(0.92, t0 + 40000, 0, 44);
	CHECK(flash_id == 1 && flash_frame == 44);
	click_on_flash = NULL;
}

int main(void)
{
	test_ids();
	test_flash_after_click();
	test_missed();
	test_luma();
	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}

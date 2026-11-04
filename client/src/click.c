/* Click-to-flash latency: click every N ms, and time how long until the stream shows a flash.
 * The workload (clickflash, or anything that turns the middle of the picture bright on a click)
 * turns the picture white for a frame; the decoder hands each decoded picture's brightness here.
 * Times are microseconds on CLOCK_MONOTONIC_RAW, the clock clickflash logs with. */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_client.h"

/* Brightness (0 to 1) above which a picture counts as the flash; clickflash's black and white
 * decode to about 0.06 and 0.92. */
#define BRIGHT 0.5
/* A click with no flash within this long counts as missed. */
#define MISS_AFTER_US 1000000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int every_ms;
static uint64_t pending_us;  /* when the click waiting for its flash was sent, 0 if none */
static bool bright;          /* the last picture was bright */
static int clicks, flashes, missed;
static double *decoded_ms, *received_ms;
static size_t samples, samples_cap;
/* CLOCK_MONOTONIC_RAW minus LiGetMicroseconds, to place moonlight-common-c's times. */
static int64_t li_offset_us;

uint64_t click_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

void click_configure(int ms)
{
	pthread_mutex_lock(&lock);
	every_ms = ms;
	pending_us = 0;
	bright = false;
	clicks = flashes = missed = 0;
	samples = 0;
	li_offset_us = (int64_t)click_now_us() - (int64_t)LiGetMicroseconds();
	pthread_mutex_unlock(&lock);
}

bool click_enabled(void)
{
	return every_ms > 0;
}

/* A click was just sent at `now`. */
void (*click_on_flash)(int id, int frame_number, uint64_t sent_us, uint64_t decoded_us, uint64_t received_us);

int click_id(int n)
{
	return n % (CLICK_COLS * CLICK_ROWS);
}

void click_position(int id, int width, int height, short *x, short *y)
{
	int col = id % CLICK_COLS, row = id / CLICK_COLS % CLICK_ROWS;
	*x = (short)((2 * col + 1) * width / (2 * CLICK_COLS));
	*y = (short)((2 * row + 1) * height / (2 * CLICK_ROWS));
}

void click_sent(uint64_t now)
{
	pthread_mutex_lock(&lock);
	clicks++;
	if (pending_us != 0)
		missed++;
	pending_us = now;
	pthread_mutex_unlock(&lock);
}

static void add_sample(double decoded, double received)
{
	if (samples == samples_cap) {
		size_t cap = samples_cap ? samples_cap * 2 : 256;
		double *d = realloc(decoded_ms, cap * sizeof(double));
		double *r = d ? realloc(received_ms, cap * sizeof(double)) : NULL;
		if (d)
			decoded_ms = d;
		if (r == NULL)
			return;
		received_ms = r;
		samples_cap = cap;
	}
	decoded_ms[samples] = decoded;
	received_ms[samples] = received;
	samples++;
}

void click_on_luma(double luma, uint64_t now, uint64_t received_li_us, int frame_number)
{
	pthread_mutex_lock(&lock);
	bool was = bright;
	bright = luma > BRIGHT;
	if (pending_us != 0 && now - pending_us > MISS_AFTER_US) {
		missed++;
		pending_us = 0;
	}
	/* The flash is the first bright picture after a click. */
	if (bright && !was && pending_us != 0) {
		uint64_t received = (uint64_t)((int64_t)received_li_us + li_offset_us);
		add_sample((now - pending_us) / 1e3, received > pending_us ? (received - pending_us) / 1e3 : 0);
		if (click_on_flash != NULL)
			click_on_flash(click_id(clicks), frame_number, pending_us, now, received_li_us);
		flashes++;
		pending_us = 0;
	}
	pthread_mutex_unlock(&lock);
}

double click_luma(const uint8_t *plane, int stride, int width, int height, int depth)
{
	/* The mean of a 32x32 patch in the middle of the picture. */
	int size = 32, x0 = width / 2 - size / 2, y0 = height / 2 - size / 2;
	if (x0 < 0 || y0 < 0)
		return 0;
	double sum = 0;
	for (int y = y0; y < y0 + size; y++) {
		const uint8_t *row = plane + (size_t)y * stride;
		for (int x = x0; x < x0 + size; x++)
			sum += depth > 8 ? ((const uint16_t *)row)[x] : row[x];
	}
	return sum / (size * size) / ((1 << depth) - 1);
}

static pthread_t thread;
static bool running;
static int ref_width, ref_height;

static void sleep_us(uint64_t us)
{
	struct timespec ts = {.tv_sec = us / 1000000, .tv_nsec = (long)(us % 1000000) * 1000};
	nanosleep(&ts, NULL);
}

static void *click_loop(void *arg)
{
	(void)arg;
	/* Wait for the picture to settle, then put the pointer in the middle of the stream. */
	sleep_us(2000000);
	LiSendMousePositionEvent((short)(ref_width / 2), (short)(ref_height / 2), (short)ref_width, (short)ref_height);
	sleep_us(100000);
	uint64_t next = click_now_us();
	while (running) {
		pthread_mutex_lock(&lock);
		int id = click_id(clicks + 1);
		pthread_mutex_unlock(&lock);
		short x, y;
		click_position(id, ref_width, ref_height, &x, &y);
		LiSendMousePositionEvent(x, y, (short)ref_width, (short)ref_height);
		click_sent(click_now_us());
		LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
		sleep_us(20000);
		LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
		/* A little randomness, so clicks don't lock to the frame rate. */
		next += (uint64_t)every_ms * 1000 + (uint64_t)(rand() % 16667);
		uint64_t now = click_now_us();
		while (running && now < next) {
			sleep_us(next - now < 50000 ? next - now : 50000);
			now = click_now_us();
		}
	}
	return NULL;
}

void click_start(int width, int height)
{
	if (!click_enabled())
		return;
	ref_width = width;
	ref_height = height;
	running = true;
	if (pthread_create(&thread, NULL, click_loop, NULL) != 0)
		running = false;
}

void click_stop(void)
{
	if (!running)
		return;
	running = false;
	pthread_join(thread, NULL);
}

static int compare(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double percentile(const double *sorted, size_t n, int p)
{
	return sorted[(n - 1) * (size_t)p / 100];
}

void click_stats(struct click_stats *out)
{
	pthread_mutex_lock(&lock);
	memset(out, 0, sizeof(*out));
	out->clicks = clicks;
	out->flashes = flashes;
	/* A click still waiting at the end isn't counted as missed. */
	out->missed = missed;
	out->samples = samples;
	out->decoded_ms = decoded_ms;
	out->received_ms = received_ms;
	if (samples > 0) {
		double *sorted = malloc(samples * sizeof(double));
		if (sorted != NULL) {
			memcpy(sorted, decoded_ms, samples * sizeof(double));
			qsort(sorted, samples, sizeof(double), compare);
			double sum = 0;
			for (size_t i = 0; i < samples; i++)
				sum += sorted[i];
			out->mean_ms = sum / samples;
			out->p50_ms = percentile(sorted, samples, 50);
			out->p95_ms = percentile(sorted, samples, 95);
			out->p99_ms = percentile(sorted, samples, 99);
			out->max_ms = sorted[samples - 1];
			free(sorted);
		}
	}
	pthread_mutex_unlock(&lock);
}

void click_summary(FILE *out, const struct click_stats *s)
{
	fprintf(out, "click clicks=%d flashes=%d missed=%d", s->clicks, s->flashes, s->missed);
	if (s->samples > 0)
		fprintf(out, " ms mean=%.1f p50=%.1f p95=%.1f p99=%.1f max=%.1f", s->mean_ms, s->p50_ms, s->p95_ms,
			s->p99_ms, s->max_ms);
	fputc('\n', out);
}

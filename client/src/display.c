/* What a display would show: each decoded frame's time replayed against a refresh clock, through moonlight-qt's
 * two ways of presenting (app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp):
 *
 * - vsync (moonlight-qt's default: V-Sync on, frame pacing off): a decoded frame goes on screen at the next refresh;
 *   when frames pile up, the render queue drops all but the newest, or keeps up to 2 while it has recently run dry.
 * - paced (frame pacing on): at each refresh the pacer takes the oldest decoded frame, waiting up to the refresh
 *   less 3 ms for one, and it goes on screen at the refresh after; the pacing queue keeps 1, or 3 while it has
 *   recently held one or none and the stream runs at the refresh rate or faster.
 *
 * Both queues hold at most 3. Rendering itself takes no time here. The first refresh is half a refresh after the
 * first frame: neither the best nor the worst phase, and the stream's clock drifts against the display's anyway. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "test_client.h"

#define MAX_QUEUED 3
#define TIMER_SLACK_US 3000

static uint64_t *ready_us;
static int *ready_frame;
static size_t ready_n, ready_cap;

void display_frame(int frame_number, uint64_t decoded_us)
{
	if (ready_n == ready_cap) {
		ready_cap = ready_cap ? ready_cap * 2 : 4096;
		ready_us = realloc(ready_us, ready_cap * sizeof *ready_us);
		ready_frame = realloc(ready_frame, ready_cap * sizeof *ready_frame);
	}
	ready_frame[ready_n] = frame_number;
	ready_us[ready_n++] = decoded_us;
}

static int compare(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

/* A queue of frame indices, oldest first. */
struct queue {
	size_t items[MAX_QUEUED];
	int len;
};

static void push(struct queue *q, size_t frame, size_t *dropped)
{
	if (q->len == MAX_QUEUED) {
		memmove(q->items, q->items + 1, (MAX_QUEUED - 1) * sizeof *q->items);
		q->len--;
		(*dropped)++;
	}
	q->items[q->len++] = frame;
}

static size_t pop(struct queue *q)
{
	size_t frame = q->items[0];
	memmove(q->items, q->items + 1, (size_t)(q->len - 1) * sizeof *q->items);
	q->len--;
	return frame;
}

/* The queue lengths of the last `cap` refreshes or frames, for the lenient drop targets. */
struct history {
	int *len;
	int cap, n, next;
};

static bool history_any(const struct history *h, int at_most)
{
	for (int i = 0; i < h->n; i++)
		if (h->len[i] <= at_most)
			return true;
	return false;
}

static void history_add(struct history *h, int len)
{
	if (h->cap == 0)
		return;
	h->len[h->next] = len;
	h->next = (h->next + 1) % h->cap;
	if (h->n < h->cap)
		h->n++;
}

void display_simulate(const uint64_t *ready, size_t n, int fps, int hz, bool paced, struct display_stats *out,
		      struct display_shown *each)
{
	memset(out, 0, sizeof *out);
	for (size_t i = 0; each != NULL && i < n; i++)
		each[i] = (struct display_shown){.wait_us = -1};
	out->hz = hz;
	if (n == 0 || fps <= 0 || hz <= 0)
		return;
	double period = 1e6 / hz;
	/* A frame held on screen for more refreshes than the stream's rate needs is a judder. */
	int expected_hold = (int)ceil((double)hz / fps - 1e-9);
	struct history history = {.cap = paced ? hz / 2 : fps / 2};
	history.len = calloc(history.cap > 0 ? (size_t)history.cap : 1, sizeof *history.len);
	double *latency = malloc(n * sizeof *latency);
	double start = (double)ready[0] + period / 2;
	struct queue q = {0};
	size_t next = 0, shown = 0;
	long last_shown = -1;
	for (long k = 0; next < n || q.len > 0; k++) {
		double t = start + k * period;
		while (next < n && (double)ready[next] <= t)
			push(&q, next++, &out->dropped);
		long on_screen = -1;
		size_t frame = 0;
		if (paced) {
			int target = fps >= hz && history_any(&history, 1) ? 3 : 1;
			history_add(&history, q.len);
			while (q.len > target) {
				pop(&q);
				out->dropped++;
			}
			if (q.len == 0 && next < n && (double)ready[next] <= t + period - TIMER_SLACK_US)
				push(&q, next++, &out->dropped);
			if (q.len > 0) {
				frame = pop(&q);
				on_screen = k + 1;
			}
		} else if (q.len > 0) {
			frame = pop(&q);
			on_screen = k;
			int target = history_any(&history, 0) ? 2 : 0;
			history_add(&history, q.len);
			while (q.len > target) {
				pop(&q);
				out->dropped++;
			}
		}
		if (on_screen < 0)
			continue;
		double wait_us = start + on_screen * period - (double)ready[frame];
		latency[shown++] = wait_us / 1000.0;
		bool judder = last_shown >= 0 && on_screen - last_shown > expected_hold;
		if (judder)
			out->judder++;
		if (each != NULL)
			each[frame] = (struct display_shown){.wait_us = (int64_t)llround(wait_us), .judder = judder};
		last_shown = on_screen;
	}
	out->shown = shown;
	if (shown > 0) {
		qsort(latency, shown, sizeof *latency, compare);
		out->latency_p50_ms = latency[(shown - 1) / 2];
		out->latency_p99_ms = latency[(shown - 1) * 99 / 100];
	}
	free(latency);
	free(history.len);
}

void display_stats(struct display_stats *vsync, struct display_stats *paced, int fps, int hz)
{
	struct display_shown *v = calloc(ready_n ? ready_n : 1, sizeof *v), *p = calloc(ready_n ? ready_n : 1, sizeof *p);
	display_simulate(ready_us, ready_n, fps, hz, false, vsync, v);
	display_simulate(ready_us, ready_n, fps, hz, true, paced, p);
	/* Each frame's fate, for the --stats stream, then the totals; nothing when no frame was decoded. */
	for (size_t i = 0; i < ready_n; i++)
		record_display(ready_frame[i], ready_us[i], &v[i], &p[i]);
	if (ready_n > 0)
		record_display_summary(fps, vsync, paced);
	free(v);
	free(p);
}

void display_summary(FILE *out, const struct display_stats *vsync, const struct display_stats *paced)
{
	fprintf(out,
		"display hz=%d vsync: shown=%zu dropped=%zu judder=%zu latency_ms p50=%.1f p99=%.1f; "
		"paced: shown=%zu dropped=%zu judder=%zu latency_ms p50=%.1f p99=%.1f\n",
		vsync->hz, vsync->shown, vsync->dropped, vsync->judder, vsync->latency_p50_ms, vsync->latency_p99_ms,
		paced->shown, paced->dropped, paced->judder, paced->latency_p50_ms, paced->latency_p99_ms);
}

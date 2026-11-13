/* display.c: frames replayed against a refresh clock judder and drop where moonlight-qt's presenting would. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "test_client.h"

static int failures;
#define CHECK(cond)                                                                \
	do {                                                                       \
		if (!(cond)) {                                                     \
			fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                \
		}                                                                  \
	} while (0)

#define N 240

/* Frame i decoded at i frame intervals at `fps`, plus `offset_us` (and `late_us` more for frame `late`). */
static void frames(uint64_t *ready, int fps, int late, uint64_t late_us)
{
	for (int i = 0; i < N; i++)
		ready[i] = 1000000 + (uint64_t)floor(i * 1e6 / fps) + (i == late ? late_us : 0);
}

int main(void)
{
	uint64_t ready[N];
	struct display_stats s;

	/* On time, at the refresh rate: every frame shows, none held twice, half a refresh after it's decoded (the
	 * clock's phase). With pacing, a refresh later. */
	frames(ready, 120, -1, 0);
	display_simulate(ready, N, 120, 120, false, &s);
	CHECK(s.shown == N && s.dropped == 0 && s.judder == 0);
	CHECK(fabs(s.latency_p50_ms - 500.0 / 120) < 0.1);
	display_simulate(ready, N, 120, 120, true, &s);
	CHECK(s.shown == N && s.dropped == 0 && s.judder == 0);
	CHECK(fabs(s.latency_p50_ms - 1500.0 / 120) < 0.1);

	/* One frame 6 ms late, past its refresh: V-Sync holds the frame before it a second refresh, then catches up by
	 * dropping one. The pacer waits for it within the refresh (up to the refresh less 3 ms). */
	frames(ready, 120, 100, 6000);
	display_simulate(ready, N, 120, 120, false, &s);
	CHECK(s.judder == 1 && s.dropped == 1 && s.shown == N - 1);
	display_simulate(ready, N, 120, 120, true, &s);
	CHECK(s.judder == 0 && s.dropped == 0 && s.shown == N);

	/* Faster refresh than the stream (120 fps on 144 Hz, 60 fps on 120 Hz): frames held one or two refreshes as
	 * the rates need, which isn't judder. */
	frames(ready, 120, -1, 0);
	display_simulate(ready, N, 120, 144, false, &s);
	CHECK(s.judder == 0 && s.dropped == 0 && s.shown == N);
	frames(ready, 60, -1, 0);
	display_simulate(ready, N, 60, 120, false, &s);
	CHECK(s.judder == 0 && s.dropped == 0);

	/* A 100 ms freeze, then the frames held up arrive at once: a judder, and the queue drops the backlog. */
	frames(ready, 120, -1, 0);
	for (int i = 100; i < 112; i++)
		ready[i] = ready[112];
	display_simulate(ready, N, 120, 120, false, &s);
	CHECK(s.judder >= 1 && s.dropped >= 9);
	display_simulate(ready, N, 120, 120, true, &s);
	CHECK(s.judder >= 1 && s.dropped >= 9);

	if (failures == 0)
		printf("display: ok\n");
	return failures != 0;
}

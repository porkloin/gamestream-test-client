/* clock.c: NTP timestamps round-trip, and the client measures a clock-serve on this machine as about the same clock. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_client.h"

static int failures;
#define CHECK(cond)                                                                \
	do {                                                                       \
		if (!(cond)) {                                                     \
			fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                \
		}                                                                  \
	} while (0)

int main(void)
{
	uint8_t ts[8];
	int64_t t = 1793728374923981;
	clock_put_timestamp(ts, t);
	int64_t back = clock_get_timestamp(ts);
	CHECK(back - t <= 1 && t - back <= 1);

	pid_t server = fork();
	if (server == 0) {
		char *args[] = {"clock-serve", "--port", "51923", NULL};
		_exit(clock_serve_main(3, args));
	}
	usleep(200 * 1000);
	struct clock_result r;
	CHECK(clock_measure("127.0.0.1:51923", 8, &r));
	CHECK(r.samples == 8);
	/* The same clock: no offset beyond the exchange's own uncertainty, which is small over loopback. */
	CHECK(r.offset_us >= -r.uncertainty_us - 50 && r.offset_us <= r.uncertainty_us + 50);
	CHECK(r.uncertainty_us < 2000);
	CHECK(!clock_measure("127.0.0.1:51924", 2, &r));
	kill(server, SIGTERM);
	waitpid(server, NULL, 0);
	printf("clock offset %lld us +/- %lld\n", (long long)r.offset_us, (long long)r.uncertainty_us);
	return failures != 0;
}

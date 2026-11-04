/* Tests for loss.c, with the moonlight-common-c symbols it uses stubbed. */
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "test_client.h"

/* moonlight-common-c state the injector reads (Limelight-internal.h). */
uint32_t EncryptionFeaturesEnabled;
uint16_t VideoPortNumber;

static uint64_t clock_us;
uint64_t LiGetMicroseconds(void)
{
	return clock_us;
}

static uint8_t received[2048];
static ssize_t received_len;

ssize_t __real_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
	memcpy(buf, received, (size_t)received_len < len ? (size_t)received_len : (size_t)len);
	return received_len;
}

ssize_t __real_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
	return (ssize_t)len;
}

static uint32_t passed_frame;
static bool passed_ltr, frame_passed;
void __real_connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR)
{
	passed_frame = frameIndex;
	passed_ltr = frameIsLTR;
	frame_passed = true;
}

#include "../src/loss.c"
#include "../src/queue.c"

static int failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__, #cond);                               \
			failures++;                                                                                    \
		}                                                                                                          \
	} while (0)

#define RTP_PLAIN 12
#define RTP_EXTENDED 16
#define RTP_EXTENSION_FLAG 0x10
#define PACKET_SIZE (RTP_EXTENDED + 16 + 40)

static void write_le32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

/* A video packet as recvfrom hands it to moonlight-common-c: RTP header, NV_VIDEO_PACKET,
 * and a frame header in the payload. */
static size_t make_packet(uint8_t *buf, uint32_t frame_index, int rtp_len)
{
	memset(buf, 0, PACKET_SIZE);
	buf[0] = 0x80 | (rtp_len == RTP_EXTENDED ? RTP_EXTENSION_FLAG : 0);
	write_le32(buf + rtp_len + 0, 0);
	write_le32(buf + rtp_len + 4, frame_index);
	buf[rtp_len + 16 + 3] = 1;
	return PACKET_SIZE;
}

static void test_frame_of(void)
{
	uint8_t buf[PACKET_SIZE];
	CHECK(frame_of(buf, make_packet(buf, 1234, RTP_PLAIN)) == 1234);
	CHECK(frame_of(buf, make_packet(buf, 1234, RTP_EXTENDED)) == 1234);

	EncryptionFeaturesEnabled = SS_ENC_VIDEO;
	memset(buf, 0, sizeof(buf));
	write_le32(buf + 12, 77);
	CHECK(frame_of(buf, 16) == 77);
	CHECK(frame_of(buf, 15) == UINT32_MAX);
	EncryptionFeaturesEnabled = 0;

	CHECK(frame_of(buf, RTP_PLAIN + 7) == UINT32_MAX);
}

static void test_frame_run(void)
{
	struct loss_config config = {.seed = 1, .frame_every = 300, .frame_run = 20, .frame_warmup = 120};
	loss_configure(&config);
	uint8_t buf[PACKET_SIZE];
	int dropped = 0;
	for (int i = 0; i < 500; i++) {
		bool got = should_drop(buf, make_packet(buf, 5000 + (uint32_t)i, RTP_PLAIN));
		bool expected = (i >= 120 && i < 140) || (i >= 420 && i < 440);
		CHECK(got == expected);
		dropped += got;
	}
	CHECK(dropped == 40);
	CHECK(dropped_frames == 40);
}

static void test_burst(void)
{
	struct loss_config config = {.seed = 1, .burst_packets = 3, .burst_every_ms = 100};
	loss_configure(&config);
	uint8_t buf[PACKET_SIZE];
	size_t len = make_packet(buf, 10, RTP_PLAIN);

	clock_us = 1000;
	CHECK(!should_drop(buf, len));
	clock_us = 101000;
	for (int i = 0; i < 3; i++)
		CHECK(should_drop(buf, len));
	CHECK(!should_drop(buf, len));
	clock_us = 201000;
	for (int i = 0; i < 3; i++)
		CHECK(should_drop(buf, len));
	CHECK(!should_drop(buf, len));
	clock_us = 201000 + 50000;
	CHECK(!should_drop(buf, len));
}

static int drops(uint64_t seed)
{
	struct loss_config config = {.seed = seed, .random_percent = 10};
	loss_configure(&config);
	uint8_t buf[PACKET_SIZE];
	size_t len = make_packet(buf, 1, RTP_PLAIN);
	int total = 0;
	for (int i = 0; i < 20000; i++)
		total += should_drop(buf, len);
	return total;
}

static void test_seed_is_reproducible(void)
{
	int dropped = drops(7);
	CHECK(dropped == drops(7));
	CHECK(dropped > 1500 && dropped < 2500);
}

/* Gilbert-Elliott: the loss rate and the mean run length come out as configured, and the
 * same seed loses the same packets. */
static void bursty(uint64_t seed, int *lost, int *runs, uint64_t *hash)
{
	struct loss_config config = {.seed = seed, .bursty_percent = 5, .bursty_length = 8};
	loss_configure(&config);
	uint8_t buf[PACKET_SIZE];
	size_t len = make_packet(buf, 1, RTP_PLAIN);
	bool prev = false;
	*lost = *runs = 0;
	*hash = 0;
	for (int i = 0; i < 200000; i++) {
		bool d = should_drop(buf, len);
		*lost += d;
		*runs += d && !prev;
		*hash = *hash * 31 + d;
		prev = d;
	}
}

static void test_bursty(void)
{
	int lost, runs, lost2, runs2;
	uint64_t hash, hash2;
	bursty(3, &lost, &runs, &hash);
	double rate = lost / 200000.0, mean_run = (double)lost / runs;
	CHECK(rate > 0.04 && rate < 0.06);
	CHECK(mean_run > 7 && mean_run < 9);
	bursty(3, &lost2, &runs2, &hash2);
	CHECK(hash == hash2 && lost == lost2);
	bursty(4, &lost2, &runs2, &hash2);
	CHECK(hash != hash2);
}

/* Loss applies to the video socket and to nothing else. */
static void test_video_socket(void)
{
	struct loss_config config = {.seed = 1, .random_percent = 100};
	loss_configure(&config);
	VideoPortNumber = 47988;
	struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(47988)};
	__wrap_sendto(7, NULL, 0, 0, (const struct sockaddr *)&to, sizeof(to));

	uint8_t buf[PACKET_SIZE];
	received_len = (ssize_t)make_packet(buf, 1, RTP_PLAIN);
	memcpy(received, buf, (size_t)received_len);
	CHECK(__wrap_recvfrom(7, received, sizeof(received), 0, NULL, NULL) == 0);
	CHECK(dropped_packets == 1);

	received_len = (ssize_t)make_packet(buf, 1, RTP_PLAIN);
	memcpy(received, buf, (size_t)received_len);
	CHECK(__wrap_recvfrom(9, received, sizeof(received), 0, NULL, NULL) == received_len);
	CHECK(dropped_packets == 1);
	VideoPortNumber = 0;
}

/* Delay queue: packets carry their index in byte 0. */
static void add_at(uint64_t now, uint8_t index, size_t len)
{
	uint8_t pkt[2000] = {index};
	queue_add(pkt, len, now);
}

/* Everything delivered up to `until`, as indexes, in delivery order. */
static int drain(uint64_t until, uint8_t *out, int max)
{
	uint8_t buf[2000];
	int n = 0;
	for (uint64_t t = 0; t <= until && n < max; t += 100)
		while (n < max && queue_take(buf, sizeof(buf), t) > 0)
			out[n++] = buf[0];
	return n;
}

static void test_delay(void)
{
	struct loss_config config = {.seed = 1, .delay_ms = 40};
	loss_configure(&config);
	CHECK(queue_enabled());
	uint8_t buf[2000];
	add_at(1000, 7, 100);
	CHECK(queue_take(buf, sizeof(buf), 40999) == 0);
	CHECK(queue_take(buf, sizeof(buf), 41000) == 100 && buf[0] == 7);
	CHECK(queue_next_due() == UINT64_MAX);
}

static void test_jitter_keeps_order(void)
{
	struct loss_config config = {.seed = 1, .delay_ms = 20, .jitter_ms = 15};
	loss_configure(&config);
	for (int i = 0; i < 200; i++)
		add_at((uint64_t)i * 100, (uint8_t)i, 100);
	uint8_t got[256];
	int n = drain(200000, got, 256);
	CHECK(n == 200);
	for (int i = 1; i < n; i++)
		CHECK(got[i] == (uint8_t)(got[i - 1] + 1));
}

static void test_reorder(void)
{
	struct loss_config config = {.seed = 1, .reorder_percent = 10, .reorder_ms = 5};
	loss_configure(&config);
	for (int i = 0; i < 200; i++)
		add_at((uint64_t)i * 1000, (uint8_t)i, 100);
	uint8_t got[256];
	int n = drain(300000, got, 256), inversions = 0;
	for (int i = 1; i < n; i++)
		inversions += got[i] < got[i - 1];
	uint64_t dup, reord;
	queue_counts(&dup, &reord);
	CHECK(n == 200);
	CHECK(reord > 5 && reord < 40);
	CHECK(inversions > 0);
}

static void test_duplicate(void)
{
	struct loss_config config = {.seed = 1, .duplicate_percent = 50};
	loss_configure(&config);
	for (int i = 0; i < 200; i++)
		add_at((uint64_t)i * 1000, (uint8_t)i, 100);
	uint8_t got[512];
	int n = drain(300000, got, 512);
	uint64_t dup, reord;
	queue_counts(&dup, &reord);
	CHECK(n == 200 + (int)dup);
	CHECK(dup > 70 && dup < 130);
}

/* 1250-byte packets on a 1000 kb/s link take 10 ms each; with a 50 ms buffer the seventh
 * packet sent at once finds it full. */
static void test_rate(void)
{
	struct loss_config config = {.seed = 1, .rate_kbps = 1000, .rate_queue_ms = 50};
	loss_configure(&config);
	uint8_t pkt[1250] = {0};
	int accepted = 0;
	for (int i = 0; i < 10; i++)
		accepted += queue_add(pkt, sizeof(pkt), 0);
	CHECK(accepted == 6);
	CHECK(queue_next_due() == 10000);
	uint8_t got[16];
	CHECK(drain(59900, got, 16) == 5);
	CHECK(drain(60000, got, 16) == 1);
}

static void test_ack_wrap(void)
{
	struct loss_config config = {.seed = 1, .strip_ack = true};
	loss_configure(&config);
	__wrap_connectionReceivedCompleteFrame(42, true);
	CHECK(ack_frames == 1);
	CHECK(frame_passed && passed_frame == 42 && !passed_ltr);

	frame_passed = false;
	__wrap_connectionReceivedCompleteFrame(43, false);
	CHECK(ack_frames == 1);
	CHECK(frame_passed && passed_frame == 43 && !passed_ltr);

	config.strip_ack = false;
	loss_configure(&config);
	frame_passed = false;
	__wrap_connectionReceivedCompleteFrame(44, true);
	CHECK(ack_frames == 1);
	CHECK(frame_passed && passed_frame == 44 && passed_ltr);
}

int main(void)
{
	test_frame_of();
	test_frame_run();
	test_burst();
	test_seed_is_reproducible();
	test_bursty();
	test_video_socket();
	test_ack_wrap();
	test_delay();
	test_jitter_keeps_order();
	test_reorder();
	test_duplicate();
	test_rate();
	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}

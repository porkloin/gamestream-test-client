/* Seeded loss on the video socket, and the host's frame acknowledgement requests.
 * moonlight-common-c's calls to recvfrom, sendto and connectionReceivedCompleteFrame are
 * linked to the wrappers below (-Wl,--wrap), so a dropped packet looks to it exactly like
 * one lost on the network, the same seed drops the same packets in every run, and the
 * acknowledgement request is seen whatever the transport. */
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "test_client.h"

/* moonlight-common-c globals (Limelight-internal.h). */
extern uint16_t VideoPortNumber;
extern uint32_t EncryptionFeaturesEnabled;
#define SS_ENC_VIDEO 0x02

ssize_t __real_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
ssize_t __real_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
void __real_connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR);

static struct loss_config cfg;
static int video_fd = -1;
static uint64_t rng;
static uint64_t seen_packets, dropped_packets, dropped_frames, ack_frames;
static uint32_t first_frame = UINT32_MAX, dropping_frame = UINT32_MAX;
static uint64_t next_burst_us;
static int burst_left;
/* Gilbert-Elliott: chance per packet of entering the losing state, and of leaving it. */
static double bursty_enter, bursty_leave;
static bool bursty_losing;

void (*loss_on_drop)(uint32_t frame_number);

static uint64_t next_random(void)
{
	/* xorshift64* */
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 0x2545F4914F6CDD1DULL;
}

void loss_configure(const struct loss_config *config)
{
	cfg = *config;
	queue_configure(config);
	rng = cfg.seed ? cfg.seed : 1;
	seen_packets = dropped_packets = dropped_frames = ack_frames = 0;
	first_frame = dropping_frame = UINT32_MAX;
	next_burst_us = 0;
	burst_left = 0;
	/* The losing state lasts 1/leave packets on average, and the share of time spent in it,
	 * enter / (enter + leave), is the loss rate. */
	bursty_losing = false;
	bursty_enter = bursty_leave = 0;
	if (cfg.bursty_percent > 0 && cfg.bursty_length >= 1) {
		double f = cfg.bursty_percent / 100.0;
		bursty_leave = 1.0 / cfg.bursty_length;
		bursty_enter = bursty_leave * f / (1.0 - f);
	}
}

/* Uniform in [0, 1). */
static double next_uniform(void)
{
	return (next_random() >> 11) * 0x1p-53;
}

static uint16_t port_of(const struct sockaddr *addr)
{
	if (addr == NULL)
		return 0;
	if (addr->sa_family == AF_INET)
		return ntohs(((const struct sockaddr_in *)addr)->sin_port);
	if (addr->sa_family == AF_INET6)
		return ntohs(((const struct sockaddr_in6 *)addr)->sin6_port);
	return 0;
}

ssize_t __wrap_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
	/* The video socket is the one that pings the host's video port. */
	if (video_fd < 0 && VideoPortNumber != 0 && port_of(to) == VideoPortNumber)
		video_fd = s;
	return __real_sendto(s, buf, len, flags, to, tolen);
}

static uint32_t read_le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* RTP header: 12 bytes, 16 with the extension header. */
static int rtp_header_len(const uint8_t *p)
{
	return (p[0] & 0x10) ? 16 : 12;
}

/* The frame a video packet belongs to, or UINT32_MAX if it can't be read. */
static uint32_t frame_of(const uint8_t *packet, ssize_t len)
{
	if (EncryptionFeaturesEnabled & SS_ENC_VIDEO) {
		/* ENC_VIDEO_HEADER: iv[12], frameNumber (LE), tag[16]. */
		return len >= 16 ? read_le32(packet + 12) : UINT32_MAX;
	}
	/* Then NV_VIDEO_PACKET with streamPacketIndex and frameIndex (LE). */
	int rtp = rtp_header_len(packet);
	return len >= rtp + 8 ? read_le32(packet + rtp + 4) : UINT32_MAX;
}

static bool should_drop(const uint8_t *packet, ssize_t len)
{
	if (cfg.frame_every > 0) {
		uint32_t frame = frame_of(packet, len);
		if (frame != UINT32_MAX) {
			if (first_frame == UINT32_MAX)
				first_frame = frame;
			uint32_t n = frame - first_frame;
			int run = cfg.frame_run > 0 ? cfg.frame_run : 1;
			if (n >= (uint32_t)cfg.frame_warmup && (n - cfg.frame_warmup) % cfg.frame_every < (uint32_t)run) {
				if (dropping_frame != frame) {
					dropping_frame = frame;
					dropped_frames++;
					if (loss_on_drop != NULL)
						loss_on_drop(frame);
				}
				return true;
			}
		}
	}
	if (cfg.burst_every_ms > 0) {
		uint64_t now = LiGetMicroseconds();
		if (next_burst_us == 0)
			next_burst_us = now + (uint64_t)cfg.burst_every_ms * 1000;
		if (burst_left == 0 && now >= next_burst_us) {
			burst_left = cfg.burst_packets;
			next_burst_us = now + (uint64_t)cfg.burst_every_ms * 1000;
		}
		if (burst_left > 0) {
			burst_left--;
			return true;
		}
	}
	if (bursty_leave > 0) {
		double u = next_uniform();
		bursty_losing = bursty_losing ? u >= bursty_leave : u < bursty_enter;
		if (bursty_losing)
			return true;
	}
	if (cfg.random_percent > 0)
		return next_uniform() * 100.0 < cfg.random_percent;
	return false;
}

/* With the delay queue on: deliver what's due, otherwise wait for the socket or the next delivery
 * time, up to the 100 ms moonlight-common-c waits itself (0 then means "nothing yet"). */
static ssize_t recv_queued(int s, uint8_t *buf, size_t len, struct sockaddr *from, socklen_t *fromlen)
{
	for (;;) {
		uint64_t now = LiGetMicroseconds();
		size_t got = queue_take(buf, len, now);
		if (got > 0)
			return (ssize_t)got;
		uint64_t due = queue_next_due();
		int timeout_ms = 100;
		if (due != UINT64_MAX && (due - now + 999) / 1000 < (uint64_t)timeout_ms)
			timeout_ms = (int)((due - now + 999) / 1000);
		struct pollfd pfd = {.fd = s, .events = POLLIN};
		int ready = poll(&pfd, 1, timeout_ms);
		if (ready < 0 && errno != EINTR)
			return -1;
		if (ready <= 0) {
			if (queue_next_due() <= LiGetMicroseconds())
				continue;
			return 0;
		}
		ssize_t n = __real_recvfrom(s, buf, len, MSG_DONTWAIT, from, fromlen);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			continue;
		if (n <= 0)
			return n;
		seen_packets++;
		if (should_drop(buf, n) || !queue_add(buf, (size_t)n, LiGetMicroseconds()))
			dropped_packets++;
	}
}

ssize_t __wrap_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
	if (s == video_fd && queue_enabled())
		return recv_queued(s, buf, len, from, fromlen);
	ssize_t n = __real_recvfrom(s, buf, len, flags, from, fromlen);
	if (n <= 0 || s != video_fd)
		return n;
	seen_packets++;
	if (!should_drop(buf, n))
		return n;
	dropped_packets++;
	/* moonlight-common-c treats 0 as "nothing yet" and reads again. */
	return 0;
}

void loss_stats(struct loss_stats *out)
{
	out->video_packets = seen_packets;
	out->dropped_packets = dropped_packets;
	out->dropped_frames = dropped_frames;
	queue_counts(&out->duplicated_packets, &out->reordered_packets);
	out->ack_frames = ack_frames;
	out->seed = cfg.seed;
}

/* The host flags the frames it wants acknowledged. Clearing the flag is what a client that
 * never acknowledges looks like to the host, and it works however the video is carried. */
void __wrap_connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR)
{
	if (frameIsLTR)
		ack_frames++;
	__real_connectionReceivedCompleteFrame(frameIndex, frameIsLTR && !cfg.strip_ack);
}

void loss_summary(FILE *out, const struct loss_stats *s)
{
	fprintf(out, "loss video_packets=%llu dropped_packets=%llu dropped_frames=%llu seed=%llu\n",
		(unsigned long long)s->video_packets, (unsigned long long)s->dropped_packets,
		(unsigned long long)s->dropped_frames, (unsigned long long)s->seed);
	if (queue_enabled())
		fprintf(out, "queue duplicated_packets=%llu reordered_packets=%llu\n",
			(unsigned long long)s->duplicated_packets, (unsigned long long)s->reordered_packets);
	fprintf(out, "ack_requested_frames=%llu\n", (unsigned long long)s->ack_frames);
}

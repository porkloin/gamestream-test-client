/* A delay queue for the video socket: latency, jitter, reordering, duplicates and a bandwidth
 * cap. Packets that survive the loss model wait here until their delivery time; the recvfrom
 * wrapper in loss.c hands them to moonlight-common-c when it comes. Times are microseconds on
 * LiGetMicroseconds' clock. */
#include <stdlib.h>
#include <string.h>

#include "test_client.h"

/* More packets than this waiting (~1 s at 100 Mbps) and new ones are dropped. */
#define MAX_HELD 16384

struct held {
	uint64_t due, seq;
	size_t len;
	uint8_t *data;
};

static struct loss_config qcfg;
static struct held *heap;
static size_t held, heap_cap;
static uint64_t qrng, next_seq, last_due, link_free;
static uint64_t duplicated, reordered;

static uint64_t queue_random(void)
{
	qrng ^= qrng >> 12;
	qrng ^= qrng << 25;
	qrng ^= qrng >> 27;
	return qrng * 0x2545F4914F6CDD1DULL;
}

static double queue_uniform(void)
{
	return (queue_random() >> 11) * 0x1p-53;
}

void queue_configure(const struct loss_config *config)
{
	for (size_t i = 0; i < held; i++)
		free(heap[i].data);
	held = 0;
	qcfg = *config;
	/* Its own stream of numbers, so turning these on doesn't change which packets the loss
	 * model drops. */
	qrng = (config->seed ? config->seed : 1) ^ 0x9E3779B97F4A7C15ULL;
	next_seq = last_due = link_free = 0;
	duplicated = reordered = 0;
}

bool queue_enabled(void)
{
	return qcfg.delay_ms > 0 || qcfg.jitter_ms > 0 || qcfg.reorder_percent > 0 || qcfg.duplicate_percent > 0 ||
	       qcfg.rate_kbps > 0;
}

static bool earlier(const struct held *a, const struct held *b)
{
	return a->due < b->due || (a->due == b->due && a->seq < b->seq);
}

static bool push(const uint8_t *data, size_t len, uint64_t due)
{
	if (held == MAX_HELD)
		return false;
	if (held == heap_cap) {
		size_t cap = heap_cap ? heap_cap * 2 : 256;
		struct held *grown = realloc(heap, cap * sizeof(*heap));
		if (grown == NULL)
			return false;
		heap = grown;
		heap_cap = cap;
	}
	uint8_t *copy = malloc(len);
	if (copy == NULL)
		return false;
	memcpy(copy, data, len);
	size_t i = held++;
	heap[i] = (struct held){.due = due, .seq = next_seq++, .len = len, .data = copy};
	while (i > 0 && earlier(&heap[i], &heap[(i - 1) / 2])) {
		struct held t = heap[i];
		heap[i] = heap[(i - 1) / 2];
		heap[(i - 1) / 2] = t;
		i = (i - 1) / 2;
	}
	return true;
}

static struct held pop(void)
{
	struct held top = heap[0];
	heap[0] = heap[--held];
	size_t i = 0;
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, m = i;
		if (l < held && earlier(&heap[l], &heap[m]))
			m = l;
		if (r < held && earlier(&heap[r], &heap[m]))
			m = r;
		if (m == i)
			break;
		struct held t = heap[i];
		heap[i] = heap[m];
		heap[m] = t;
		i = m;
	}
	return top;
}

bool queue_add(const uint8_t *data, size_t len, uint64_t now)
{
	uint64_t t = now;
	if (qcfg.rate_kbps > 0) {
		/* A link that sends KBPS: each packet leaves after the ones ahead of it, and one that
		 * would wait longer than the link's buffer is dropped, as a full router queue does. */
		uint64_t send_us = (uint64_t)len * 8 * 1000 / (uint64_t)qcfg.rate_kbps;
		uint64_t buffer_us = (uint64_t)(qcfg.rate_queue_ms > 0 ? qcfg.rate_queue_ms : 50) * 1000;
		if (link_free < now)
			link_free = now;
		if (link_free - now > buffer_us)
			return false;
		link_free += send_us;
		t = link_free;
	}
	t += (uint64_t)qcfg.delay_ms * 1000;
	if (qcfg.jitter_ms > 0) {
		int64_t j = (int64_t)qcfg.jitter_ms * 1000;
		int64_t offset = (int64_t)(queue_uniform() * (double)(2 * j + 1)) - j;
		t = offset < 0 && (uint64_t)-offset > t - now ? now : t + (uint64_t)offset;
	}
	/* Jitter changes the delay, not the order: a packet never overtakes one sent before it. */
	if (t < last_due)
		t = last_due;
	last_due = t;
	uint64_t due = t;
	if (qcfg.reorder_percent > 0 && queue_uniform() * 100.0 < qcfg.reorder_percent) {
		due += (uint64_t)qcfg.reorder_ms * 1000;
		reordered++;
	}
	if (!push(data, len, due))
		return false;
	if (qcfg.duplicate_percent > 0 && queue_uniform() * 100.0 < qcfg.duplicate_percent && push(data, len, due))
		duplicated++;
	return true;
}

uint64_t queue_next_due(void)
{
	return held ? heap[0].due : UINT64_MAX;
}

size_t queue_take(uint8_t *buf, size_t len, uint64_t now)
{
	if (held == 0 || heap[0].due > now)
		return 0;
	struct held p = pop();
	size_t n = p.len < len ? p.len : len;
	memcpy(buf, p.data, n);
	free(p.data);
	return n;
}

void queue_counts(uint64_t *duplicated_out, uint64_t *reordered_out)
{
	*duplicated_out = duplicated;
	*reordered_out = reordered;
}

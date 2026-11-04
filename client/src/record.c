/* One CSV row per frame handed to the decoder, and a summary of the run. Times are
 * moonlight-common-c's microsecond clock (LiGetMicroseconds). */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#include "test_client.h"

/* The last few recovery frame numbers, written by the depacketizer's thread and read by the
 * decoder's. A recovery reaches the decoder a frame or two after it's named. */
#define RECOVERY_SLOTS 16
static atomic_int recovery_frames[RECOVERY_SLOTS];
static atomic_uint recovery_next;

void record_recovery_frame(int frame_number)
{
	/* Stored plus one, so the zeroed slots match no frame. */
	atomic_store(&recovery_frames[atomic_fetch_add(&recovery_next, 1) % RECOVERY_SLOTS], frame_number + 1);
}

bool is_recovery_frame(int frame_number)
{
	for (int i = 0; i < RECOVERY_SLOTS; i++)
		if (atomic_load(&recovery_frames[i]) == frame_number + 1)
			return true;
	return false;
}

static FILE *csv;
static int frames, idr_frames, decode_errors, corrupt_frames, missing_frames;
static int last_frame = -1;
static uint64_t first_receive_us, last_receive_us;
static double *intervals_ms;
static size_t intervals, intervals_cap;
static double *decodes_ms;
static size_t decodes, decodes_cap;

static FILE *stats;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

/* One line of the --stats stream, whole and flushed, from any thread. */
static void stats_line(const char *fmt, ...)
{
	if (stats == NULL)
		return;
	va_list ap;
	va_start(ap, fmt);
	pthread_mutex_lock(&stats_lock);
	vfprintf(stats, fmt, ap);
	fflush(stats);
	pthread_mutex_unlock(&stats_lock);
	va_end(ap);
}

/* Bytes received per second, for the --stats stream: each aligned second's bytes, and the busiest sliding one-second
 * window so far (what a link sized for the bitrate has to carry). */
static int requested_kbps;
static uint64_t *win_us, *win_bytes;
static size_t win_n, win_cap, win_start;
static uint64_t win_total, peak_window, second_start_us, second_bytes;
static int second_frames;

void record_set_bitrate(int kbps)
{
	requested_kbps = kbps;
}

/* The host's clock less this machine's, from --clock; 0 without it. */
static int64_t clock_offset_us;

void record_set_clock_offset(int64_t offset_us)
{
	clock_offset_us = offset_us;
}

/* The wall clock the --stats and --csv times are on: the host's with --clock, else this machine's. */
static int64_t wall_now_us(void)
{
	return clock_realtime_us() + clock_offset_us;
}

void record_clock(const char *when, const char *server, const struct clock_result *r)
{
	if (r == NULL) {
		stats_line("{\"type\":\"clock\",\"when\":\"%s\",\"server\":\"%s\",\"ok\":false}\n", when, server);
		return;
	}
	stats_line("{\"type\":\"clock\",\"when\":\"%s\",\"server\":\"%s\",\"ok\":true,\"offset_us\":%lld,"
		   "\"uncertainty_us\":%lld,\"samples\":%d,\"applied_offset_us\":%lld}\n",
		   when, server, (long long)r->offset_us, (long long)r->uncertainty_us, r->samples,
		   (long long)clock_offset_us);
}

void record_drop(uint32_t frame_number)
{
	stats_line("{\"type\":\"drop\",\"frame_number\":%u,\"unix_us\":%lld}\n", frame_number,
		   (long long)wall_now_us());
}

static int64_t wall_offset_us(void);

void record_click(int id, int frame_number, uint64_t sent_us, uint64_t decoded_us, uint64_t received_us)
{
	int64_t wall = wall_now_us() - (int64_t)click_now_us();
	/* The flash's arrival is on moonlight-common-c's clock, converted like the frame's own record of it. */
	int64_t li_wall = wall_offset_us();
	stats_line("{\"type\":\"click\",\"click\":%d,\"flash_frame\":%d,\"sent_unix_us\":%lld,\"decoded_unix_us\":%lld,"
		   "\"received_unix_us\":%lld}\n",
		   id, frame_number, (long long)((int64_t)sent_us + wall), (long long)((int64_t)decoded_us + wall),
		   (long long)(received_us > 0 ? (int64_t)received_us + li_wall : 0));
}

bool record_open(const char *path, const char *stats_path)
{
	if (stats_path != NULL) {
		stats = fopen(stats_path, "w");
		if (stats == NULL)
			return false;
	}
	if (path != NULL) {
		csv = fopen(path, "w");
		if (csv == NULL)
			return false;
		fprintf(csv, "frame_number,frame_type,bytes,host_latency_ms,receive_us,enqueue_us,"
			     "decode_start_us,decode_end_us,rtp_timestamp,decode_status,width,height,recovery,"
			     "receive_unix_us\n");
	}
	return true;
}

/* The wall clock less moonlight-common-c's clock, now: the frame times are on the latter, and the wall
 * clock lines them up with the host's record of the same frames. Taken per frame so drift doesn't add up. */
static int64_t wall_offset_us(void)
{
	return wall_now_us() - (int64_t)LiGetMicroseconds();
}

void record_frame(const struct frame_row *r)
{
	if (csv != NULL)
		fprintf(csv, "%d,%d,%d,%.1f,%llu,%llu,%llu,%llu,%u,%d,%d,%d,%d,%lld\n", r->frame_number,
			r->frame_type, r->bytes, r->host_latency_ms, (unsigned long long)r->receive_us,
			(unsigned long long)r->enqueue_us, (unsigned long long)r->decode_start_us,
			(unsigned long long)r->decode_end_us, r->rtp_timestamp, r->decode_status,
			r->width, r->height, r->recovery,
			(long long)((int64_t)r->receive_us + wall_offset_us()));
	int64_t wall = wall_offset_us();
	if (stats != NULL) {
		stats_line(
			"{\"type\":\"frame\",\"frame_number\":%d,\"frame_type\":%d,\"bytes\":%d,"
			"\"receive_unix_us\":%lld,\"enqueue_unix_us\":%lld,\"decode_start_unix_us\":%lld,"
			"\"decode_end_unix_us\":%lld,\"decode_status\":%d,\"recovery\":%s}\n",
			r->frame_number, r->frame_type, r->bytes, (long long)((int64_t)r->receive_us + wall),
			(long long)((int64_t)r->enqueue_us + wall), (long long)((int64_t)r->decode_start_us + wall),
			(long long)((int64_t)r->decode_end_us + wall), r->decode_status, r->recovery ? "true" : "false");
	}
	if (stats != NULL) {
		/* Seconds counted from the first frame's arrival; a second is written when a frame arrives after it. */
		if (second_start_us == 0)
			second_start_us = r->receive_us;
		while (r->receive_us >= second_start_us + 1000000) {
			second_start_us += 1000000;
			stats_line("{\"type\":\"second\",\"bytes\":%llu,\"frames\":%d,\"peak_window_bytes\":%llu,"
				   "\"requested_kbps\":%d,\"unix_us\":%lld}\n",
				   (unsigned long long)second_bytes, second_frames, (unsigned long long)peak_window,
				   requested_kbps, (long long)((int64_t)second_start_us + wall));
			second_bytes = 0;
			second_frames = 0;
		}
		second_bytes += r->bytes;
		second_frames++;
		if (win_n == win_cap) {
			win_cap = win_cap ? win_cap * 2 : 1024;
			win_us = realloc(win_us, win_cap * sizeof *win_us);
			win_bytes = realloc(win_bytes, win_cap * sizeof *win_bytes);
		}
		win_us[win_n] = r->receive_us;
		win_bytes[win_n++] = r->bytes;
		win_total += r->bytes;
		while (r->receive_us - win_us[win_start] > 1000000)
			win_total -= win_bytes[win_start++];
		/* Only full windows: the first second has no busiest second yet. */
		if (r->receive_us - win_us[0] >= 1000000 && win_total > peak_window)
			peak_window = win_total;
	}
	frames++;
	if (r->frame_type == FRAME_TYPE_IDR)
		idr_frames++;
	if (r->decode_status < 0)
		decode_errors++;
	else if (r->decode_status > 0)
		corrupt_frames++;
	if (last_frame >= 0 && r->frame_number > last_frame + 1) {
		missing_frames += r->frame_number - last_frame - 1;
		/* A freeze: frames never reached the decoder, and the picture stood still from the last one before
		 * them until this one. */
		stats_line("{\"type\":\"freeze\",\"after_frame\":%d,\"frame_number\":%d,\"missing\":%d,"
			   "\"duration_us\":%llu,\"recovered_by\":\"%s\",\"unix_us\":%lld}\n",
			   last_frame, r->frame_number, r->frame_number - last_frame - 1,
			   (unsigned long long)(r->receive_us - last_receive_us),
			   r->recovery ? "recovery_frame" : r->frame_type == FRAME_TYPE_IDR ? "keyframe" : "neither",
			   (long long)((int64_t)r->receive_us + wall));
	}
	if (last_receive_us != 0) {
		if (intervals == intervals_cap) {
			intervals_cap = intervals_cap ? intervals_cap * 2 : 4096;
			intervals_ms = realloc(intervals_ms, intervals_cap * sizeof(double));
		}
		intervals_ms[intervals++] = (r->receive_us - last_receive_us) / 1000.0;
	} else {
		first_receive_us = r->receive_us;
	}
	last_receive_us = r->receive_us;
	last_frame = r->frame_number;
	if (r->decode_end_us > r->decode_start_us) {
		if (decodes == decodes_cap) {
			decodes_cap = decodes_cap ? decodes_cap * 2 : 4096;
			decodes_ms = realloc(decodes_ms, decodes_cap * sizeof(double));
		}
		decodes_ms[decodes++] = (r->decode_end_us - r->decode_start_us) / 1000.0;
	}
}

static int compare(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

void record_stats(struct record_stats *out, int fps, uint64_t end_us)
{
	/* From the first frame to the end of the stream, so frames that stop coming count. */
	if (end_us < last_receive_us)
		end_us = last_receive_us;
	double seconds = frames > 0 ? (end_us - first_receive_us) / 1e6 : 0;
	/* A stall: no frame received for more than three frame intervals. */
	double stall_ms = 3000.0 / fps;
	out->frames = frames;
	out->idr = idr_frames;
	out->missing = missing_frames;
	out->decode_errors = decode_errors;
	out->corrupt = corrupt_frames;
	out->seconds = seconds;
	out->fps = seconds > 0 ? frames / seconds : 0;
	out->stalled_ms = 0;
	out->stalls = 0;
	for (size_t i = 0; i < intervals; i++)
		if (intervals_ms[i] > stall_ms) {
			out->stalls++;
			out->stalled_ms += intervals_ms[i];
		}
	/* Silence from the last frame to the end of the stream is a stall too. */
	double tail_ms = frames > 0 ? (end_us - last_receive_us) / 1e3 : 0;
	if (tail_ms > stall_ms) {
		out->stalls++;
		out->stalled_ms += tail_ms;
	}
	out->intervals = intervals;
	if (intervals > 0) {
		qsort(intervals_ms, intervals, sizeof(double), compare);
		out->p50_ms = intervals_ms[(intervals - 1) / 2];
		out->p95_ms = intervals_ms[(intervals - 1) * 95 / 100];
		out->p99_ms = intervals_ms[(intervals - 1) * 99 / 100];
		out->max_ms = intervals_ms[intervals - 1];
	}
	out->decodes = decodes;
	if (decodes > 0) {
		qsort(decodes_ms, decodes, sizeof(double), compare);
		out->decode_p50_ms = decodes_ms[(decodes - 1) / 2];
		out->decode_p95_ms = decodes_ms[(decodes - 1) * 95 / 100];
		out->decode_p99_ms = decodes_ms[(decodes - 1) * 99 / 100];
		out->decode_max_ms = decodes_ms[decodes - 1];
	}
}

void record_summary(FILE *out, const struct record_stats *s)
{
	fprintf(out, "frames=%d seconds=%.1f fps=%.1f idr=%d missing=%d decode_errors=%d corrupt=%d "
		     "stalls=%d stalled_ms=%.0f\n",
		s->frames, s->seconds, s->fps, s->idr, s->missing, s->decode_errors, s->corrupt, s->stalls,
		s->stalled_ms);
	if (s->intervals > 0)
		fprintf(out, "interval_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f\n", s->p50_ms, s->p95_ms,
			s->p99_ms, s->max_ms);
	if (s->decodes > 0)
		fprintf(out, "decode_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f\n", s->decode_p50_ms, s->decode_p95_ms,
			s->decode_p99_ms, s->decode_max_ms);
}

void record_close(void)
{
	if (csv != NULL)
		fclose(csv);
	csv = NULL;
	if (stats != NULL)
		fclose(stats);
	stats = NULL;
}

#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include <Limelight.h>

/* Loss injection on the video socket (loss.c). */
struct loss_config {
	uint64_t seed;
	double random_percent; /* drop each video packet with this probability */
	int frame_every;       /* drop every packet of every Nth frame, 0 = off */
	int frame_run;         /* ... and of the frames after it, this many in all */
	int frame_warmup;      /* frames before the first frame drop */
	int burst_packets;     /* drop this many consecutive packets ... */
	int burst_every_ms;    /* ... every this many ms, 0 = off */
	double bursty_percent; /* Gilbert-Elliott loss: this share of packets lost ... */
	double bursty_length;  /* ... in runs of this many packets on average, 0 = off */
	bool strip_ack;        /* clear the host's acknowledgement requests */
	/* Delay queue (queue.c). */
	int delay_ms;             /* deliver every packet this much later ... */
	int jitter_ms;            /* ... give or take up to this much, keeping order */
	double reorder_percent;   /* hold this share of packets ... */
	int reorder_ms;           /* ... this much longer, so later ones overtake them */
	double duplicate_percent; /* deliver this share of packets twice */
	int rate_kbps;            /* a link this fast, 0 = off ... */
	int rate_queue_ms;        /* ... that drops packets once this much is waiting (50) */
};
void loss_configure(const struct loss_config *config);
/* Called (on the network thread) when the loss model starts dropping a frame, with its frame index. */
extern void (*loss_on_drop)(uint32_t frame_number);

void queue_configure(const struct loss_config *config);
bool queue_enabled(void);
/* Queue a packet that arrived at `now`; false if it was dropped (link buffer full). */
bool queue_add(const uint8_t *data, size_t len, uint64_t now);
/* The next delivery time, or UINT64_MAX when nothing is waiting. */
uint64_t queue_next_due(void);
/* Copy the next packet due by `now` into buf; 0 if none is. */
size_t queue_take(uint8_t *buf, size_t len, uint64_t now);
void queue_counts(uint64_t *duplicated, uint64_t *reordered);

/* Counters of a finished run. */
struct loss_stats {
	uint64_t video_packets, dropped_packets, dropped_frames;
	uint64_t duplicated_packets, reordered_packets;
	uint64_t ack_frames;
	uint64_t seed;
};
void loss_stats(struct loss_stats *out);
void loss_summary(FILE *out, const struct loss_stats *stats);

/* Per-frame record (record.c). */
struct frame_row {
	int frame_number;
	int frame_type;
	int bytes;
	double host_latency_ms;
	uint64_t receive_us;
	uint64_t enqueue_us;
	uint64_t decode_start_us;
	uint64_t decode_end_us;
	uint32_t rtp_timestamp;
	int decode_status; /* 0 ok, else the libavcodec error */
	int width, height;
	bool recovery; /* the P-frame that recovers from a reference frame invalidation */
	int queued; /* frames waiting behind it for the decoder when it went to it (moonlight-common-c's queue holds 15) */
};
/* csv: the per-frame CSV; stats: the same frames as JSON lines, flushed as written, times by the wall clock.
 * Either may be NULL. */
bool record_open(const char *csv, const char *stats);
/* The host's clock from here (clock.c): NTP to SERVER ("host[:port]", port 123 by default), the fastest of `tries`
 * exchanges. offset_us is the server's clock less this machine's, true to within uncertainty_us. */
struct clock_result {
	int64_t offset_us, uncertainty_us;
	int samples;
};
bool clock_measure(const char *server, int tries, struct clock_result *out);
int clock_serve_main(int argc, char **argv);
int64_t clock_realtime_us(void);
void clock_put_timestamp(uint8_t *p, int64_t unix_us);
int64_t clock_get_timestamp(const uint8_t *p);
/* From now on the --stats and --csv wall clock times are on the host's clock: this one plus offset_us. */
void record_set_clock_offset(int64_t offset_us);
/* A clock line in the --stats stream: `when` start or end, the result (NULL when the measurement failed). */
void record_clock(const char *when, const char *server, const struct clock_result *r);
/* The bitrate the stream asked for, for the --stats stream's `second` lines. */
void record_set_bitrate(int kbps);
/* Events for the --stats stream; any thread. */
/* The --stats stream's first line: the client's commit. */
void record_version(const char *version);
void record_drop(uint32_t frame_number);
void record_click(int id, int frame_number, uint64_t sent_us, uint64_t decoded_us, uint64_t received_us);
void record_frame(const struct frame_row *row);
/* A keyframe request the client sent, and why (main.c's causes). */
void record_idr_request(const char *cause);
/* Frame number of a P-frame the host sent to recover from an invalidation; the depacketizer
 * names it before the frame reaches the decoder. */
void record_recovery_frame(int frame_number);
bool is_recovery_frame(int frame_number);

/* Summary of a finished run that ended at end_us; the stall threshold is three frame intervals
 * at fps, and a silence from the last frame to the end counts as one. */
struct record_stats {
	int frames, idr, missing, decode_errors, corrupt, stalls;
	double seconds, fps, stalled_ms;
	size_t intervals;
	double p50_ms, p95_ms, p99_ms, max_ms;
	/* Time to decode a frame, from handing it to the decoder to the picture being ready. */
	size_t decodes;
	double decode_p50_ms, decode_p95_ms, decode_p99_ms, decode_max_ms;
	/* The most frames waiting for the decoder behind one going to it. */
	int queued_max;
};
void record_stats(struct record_stats *out, int fps, uint64_t end_us);
void record_summary(FILE *out, const struct record_stats *stats);
void record_close(void);

/* What a display would show (display.c): each decoded frame's time against a refresh clock, through moonlight-qt's
 * presenting with V-Sync and with its frame pacer. */
struct display_stats {
	int hz;
	size_t shown, dropped;
	/* Frames held on screen for more refreshes than the stream's rate needs. */
	size_t judder;
	/* Decoded to on screen. */
	double latency_p50_ms, latency_p99_ms;
};
/* One frame's fate on the display: how long after it was decoded it went on screen (-1: dropped, never shown), and
 * whether it came a refresh or more later than the stream's rate needs (the frame before it held too long). */
struct display_shown {
	int64_t wait_us;
	bool judder;
};
void display_frame(int frame_number, uint64_t decoded_us);
/* `each`, when not NULL, gets each of the `n` frames' fate. */
void display_simulate(const uint64_t *ready, size_t n, int fps, int hz, bool paced, struct display_stats *out,
		      struct display_shown *each);
void display_stats(struct display_stats *vsync, struct display_stats *paced, int fps, int hz);
void display_summary(FILE *out, const struct display_stats *vsync, const struct display_stats *paced);
/* The display model's lines on the --stats stream, written once the stream ends (display.c). */
void record_display(int frame_number, uint64_t decoded_us, const struct display_shown *vsync,
		    const struct display_shown *paced);
void record_display_summary(int fps, const struct display_stats *vsync, const struct display_stats *paced);

/* framestamp's picture (pattern.c): what it draws at a pixel of frame `frame` in `mode` at width x height, and
 * --check-picture's comparison of a decoded luma plane with it. */
enum { PATTERN_STATIC = 0, PATTERN_REGION = 1, PATTERN_FULL = 2 };
/* Returns whether the pixel is a particle: noise new every frame, which the check leaves out. */
bool pattern_rgb(uint32_t width, uint32_t height, uint32_t frame, uint32_t mode, uint32_t x, uint32_t y, uint8_t c[3]);
struct picture_result {
	/* The stamp read: false when the picture has none (another app, or too broken to read). */
	bool readable;
	uint32_t frame, mode;
	/* Luma levels off, on average over a tile: the worst tile's and the mean over the tiles. */
	double worst, mean;
};
/* plane: luma, `depth` bits (a 16-bit plane keeps its bits high). */
void picture_check(const uint8_t *plane, int stride, int depth, int width, int height, struct picture_result *out);
/* The same, reading only the rows it needs from `rows`: rows y0 to y0 + h - 1, as a pointer to row y0 and the stride
 * (valid until the next call), or NULL when they can't be read. For a frame on the GPU, read back a strip at a time. */
typedef const uint8_t *(*picture_rows)(void *ctx, int y0, int h, int *stride);
void picture_check_rows(picture_rows rows, void *ctx, int depth, int width, int height, struct picture_result *out);
/* --check-picture: compare each decoded frame with framestamp's (decode.c), at most `threshold` levels off a tile. */
void decode_set_picture_check(double threshold);
/* --picture-dump DIR: the first 3 corrupt frames, decoded and expected luma side by side, as DIR/picture-N.pgm. */
void picture_set_dump(const char *dir);
/* Whether a corrupt frame now would be dumped (worth reading back whole). */
bool picture_dumping(void);
void picture_dump(const uint8_t *plane, int stride, int depth, int width, int height, const struct picture_result *r);
/* A checked frame (record.c): the --stats line, and the counts for the summary. */
void record_picture(int frame_number, bool recovery, bool keyframe, const struct picture_result *r, bool corrupt,
		    double check_ms);
struct picture_stats {
	int checked, unreadable, corrupt;
	/* The longest run of corrupt frames, and the longest that began at a recovery frame or keyframe: a frame the
	 * stream should be whole again from. */
	int corrupt_run_max, recovery_corrupt_max;
	/* Frames whose stamp repeated the frame before's. */
	int repeats;
	double worst_p99, check_ms_p99;
};
void picture_stats(struct picture_stats *out);

/* Click-to-flash latency (click.c). */
struct click_stats {
	int clicks, flashes, missed;
	size_t samples;
	const double *decoded_ms, *received_ms; /* click to flash decoded / received, per flash */
	double mean_ms, p50_ms, p95_ms, p99_ms, max_ms; /* of decoded_ms */
};
uint64_t click_now_us(void);
/* Click every `ms` (0 = off). */
void click_configure(int ms);
bool click_enabled(void);
void click_start(int width, int height);
void click_stop(void);
void click_sent(uint64_t now);
/* Each click's id travels with it in the pointer's position: before clicking, the pointer goes to the middle of cell
 * `id` of a CLICK_COLS × CLICK_ROWS grid over the stream, so the app can read the id back (cell = row × CLICK_COLS +
 * column, from the pointer's position over its window) and a lost input doesn't shift the ids after it. Ids count
 * clicks from 1, modulo the grid. */
#define CLICK_COLS 64
#define CLICK_ROWS 32
int click_id(int n);
void click_position(int id, int width, int height, short *x, short *y);
/* A decoded picture: its middle's brightness (0 to 1), when it was decoded, when it was received
 * (moonlight-common-c's clock), and its frame number. */
void click_on_luma(double luma, uint64_t now, uint64_t received_li_us, int frame_number);
/* Called for each flash timed: the click's id, the frame the flash was decoded from, when the click was sent and the
 * flash decoded (click_now_us's clock), and when the flash's first packet arrived (moonlight-common-c's clock, as
 * the frame's own record has it). */
extern void (*click_on_flash)(int id, int frame_number, uint64_t sent_us, uint64_t decoded_us, uint64_t received_us);
/* Brightness of the middle of a luma plane with `depth` bits per sample. */
double click_luma(const uint8_t *plane, int stride, int width, int height, int depth);
void click_stats(struct click_stats *out);
void click_summary(FILE *out, const struct click_stats *stats);

/* Software decode through libavcodec (decode.c). */
extern DECODER_RENDERER_CALLBACKS test_decoder;
/* Skip the picture decode: for streams the CPU can't decode in real time, where only
 * moonlight-common-c's view of the stream (loss, recovery, timing) is measured. */
void decode_set_enabled(bool enabled);
/* Write every frame as received (Annex B, or AV1 OBUs) to PATH. */
bool decode_set_dump(const char *path);
/* Decode on this VAAPI render node (e.g. /dev/dri/renderD128) instead of in software. */
void decode_set_hw(const char *device);
const char *decode_hw_device(void);
/* Make every frame take at least this long to decode, like a slow decoder. */
void decode_set_delay_ms(int ms);

/* A GPU's render node, PCI ids, name, VAAPI driver and what it decodes (probe.c). */
enum { DECODES_H264 = 1, DECODES_HEVC = 2, DECODES_HEVC10 = 4, DECODES_AV1 = 8, DECODES_AV1_10 = 16 };
struct gpu {
	char device[32];   /* /dev/dri/renderD128 */
	char pci[16];      /* 1002:73bf */
	char name[128];    /* AMD Radeon RX 6800 XT */
	char va_driver[96]; /* radeonsi Mesa 26.2.6, or "" without VAAPI */
	long vram_mb;      /* 0 when the driver doesn't say */
	unsigned decodes;  /* DECODES_* */
};
/* The render nodes under SYSFS/class/drm, by name; VAAPI is asked about the ones this process can open. */
int probe_gpus(const char *sysfs, struct gpu *out, int max);
/* The GPU to decode on: one that decodes all of NEEDS, another than AVOID_PCI if there is one, then the
 * most video memory. NULL if none can. */
const struct gpu *probe_pick_decoder(const struct gpu *gpus, int n, unsigned needs, const char *avoid_pci);
void probe_write_gpu(FILE *f, const struct gpu *g);
void json_string(FILE *f, const char *s);
/* `probe [--sysfs DIR] [--os-release FILE]`: the machine as one JSON object on stdout. */
int probe_main(int argc, char **argv);

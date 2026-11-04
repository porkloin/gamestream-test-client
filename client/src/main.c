/* gamestream-test-client: a headless Moonlight client. Pairs, lists apps, and streams an app
 * for a fixed time with optional seeded loss, writing one CSV row per frame and a summary on
 * stdout. */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/version.h>

#include <client.h>
#include <errors.h>

#include "test_client.h"

#define starts_with(s, p) (strncmp((s), (p), sizeof(p) - 1) == 0)

static atomic_bool terminated;
static atomic_int terminate_error;
static atomic_int idr_requests, rfi_requests, recovery_requests;
static bool verbose;

static void on_terminated(int error)
{
	terminate_error = error;
	terminated = true;
}

static void on_log(const char *format, ...)
{
	if (starts_with(format, "IDR frame request sent"))
		idr_requests++;
	else if (starts_with(format, "Invalidate reference frame request sent"))
		rfi_requests++;
	else if (starts_with(format, "Next post-invalidation frame is:")) {
		/* "%d (%s-frame)": the frame number, and "P" for a recovery frame or "I" for a
		 * keyframe the host sent instead. */
		va_list args;
		va_start(args, format);
		int frame_number = va_arg(args, int);
		const char *kind = va_arg(args, const char *);
		va_end(args);
		if (kind != NULL && kind[0] == 'P') {
			recovery_requests++;
			record_recovery_frame(frame_number);
		}
	}
	if (!verbose)
		return;
	va_list args;
	va_start(args, format);
	fprintf(stderr, "[%.3f] ", LiGetMicroseconds() / 1e6);
	vfprintf(stderr, format, args);
	va_end(args);
}

static void on_stage_failed(int stage, int error)
{
	fprintf(stderr, "stage %s failed: %d\n", LiGetStageName(stage), error);
}

static void on_signal(int sig)
{
	terminated = true;
}

static void usage(int code)
{
	fprintf(stderr,
		"usage: gamestream-test-client [--host H] [--port PORT] [--keys DIR] [-v] COMMAND\n"
		"  --host H, -H              host to reach (127.0.0.1)\n"
		"  --port PORT               control port (47989)\n"
		"  --keys DIR                keys directory ($HOME/.config/gamestream-test-client)\n"
		"  -v, --verbose             the protocol library's log on stderr\n"
		"  -h, --help                this text, exiting 0\n"
		"  pair PIN                  pair with PIN (enter it on the host)\n"
		"  list                      list the host's apps\n"
		"  clock-serve [--port N]    answer NTP with this machine's clock (123), for --clock\n"
		"  probe [--sysfs DIR] [--os-release FILE]\n"
		"                            describe this machine (hardware, OS, link, GPUs and what\n"
		"                            each decodes) as JSON, without a host\n"
		"  stream APP [options]      stream APP (its name, or the id list prints)\n"
		"                            and print a summary\n"
		"    --codec h264|hevc|av1   (hevc)\n"
		"    --hdr                   request HDR (10-bit HEVC or AV1)\n"
		"    --decode full|hw|none   decode every picture in software, on a VAAPI device, or\n"
		"                            skip it (full)\n"
		"    --decode-device PATH    the VAAPI render node for --decode hw (default: the GPU\n"
		"                            that decodes the codec, see --avoid-gpu, with the most\n"
		"                            video memory)\n"
		"    --avoid-gpu VENDOR:DEVICE  decode on another GPU than this one when one can\n"
		"                            (the host's encoder, over loopback)\n"
		"    --decode-delay MS       take at least MS to decode each frame, like a slow decoder\n"
		"    --dump PATH             write the received bitstream to PATH\n"
		"    --mode WxHxFPS          (1920x1080x60)\n"
		"    --bitrate KBPS          (20000)\n"
		"    --packet-size BYTES     video payload per packet the host sends (1392)\n"
		"    --seconds N             (20)\n"
		"    --csv PATH              per-frame record\n"
		"    --stats PATH            per-frame JSON lines as frames decode, times by the wall clock\n"
		"    --clock HOST[:PORT]     measure the host's clock by NTP before and after the stream\n"
		"                            (see clock-serve), and give --stats and --csv times on it\n"
		"    --json PATH             write the summary as one JSON object\n"
		"    --expect EXPR           fail with 3 unless a summary value holds:\n"
		"                            NAME OP NUMBER, OP one of <= >= == < >\n"
		"    --no-rfi                don't offer reference frame invalidation\n"
		"    --no-ack                don't acknowledge frames the host asks about\n"
		"    --quit                  quit the app on the host afterwards\n"
		"    --seed N                loss seed (1)\n"
		"    --loss-random PERCENT   drop each video packet with this probability\n"
		"    --loss-frame-every N    drop every packet of every Nth frame\n"
		"    --loss-frame-run N      ... and of the N-1 frames after it (1)\n"
		"    --loss-warmup N         frames before the first frame drop (120)\n"
		"    --loss-burst N:MS       drop N consecutive packets every MS ms\n"
		"    --loss-bursty PCT:LEN   lose PCT%% of packets in runs of LEN on average\n"
		"                            (Gilbert-Elliott, like Wi-Fi)\n"
		"    --delay MS[:JITTER]     deliver video packets MS later, give or take JITTER\n"
		"                            (in order)\n"
		"    --reorder PCT:MS        hold PCT%% of packets MS longer, so later ones overtake\n"
		"    --duplicate PCT         deliver PCT%% of packets twice\n"
		"    --rate KBPS[:QUEUE_MS]  a link of KBPS that drops packets once QUEUE_MS of\n"
		"                            them are waiting (50)\n"
		"    --click-every MS        click in the middle of the stream every MS (plus up to\n"
		"                            a frame at random) and time each until the picture\n"
		"                            flashes bright, e.g. with clickflash as the app\n"
		"exit: 0 ok, 1 couldn't stream or stopped early, 2 usage, 3 an expectation failed\n");
	exit(code);
}

/* The app to stream, by name or by the id `list` prints. */
static int find_app(PSERVER_DATA server, const char *name)
{
	char *end;
	long id = strtol(name, &end, 10);
	bool by_id = *name != '\0' && *end == '\0';
	PAPP_LIST list;
	if (gs_applist(server, &list) != GS_OK)
		return -1;
	for (; list != NULL; list = list->next)
		if (strcmp(list->name, name) == 0 || (by_id && list->id == id))
			return list->id;
	return -1;
}

/* An expectation on one summary value, e.g. "decode_errors==0". */
enum { OP_LE, OP_GE, OP_EQ, OP_LT, OP_GT };

#define MAX_EXPECT 16

struct expectation {
	char expr[64];
	char name[40];
	int op;
	double value;
};

static bool parse_expectation(const char *expr, struct expectation *out)
{
	static const struct { const char *text; int op; } ops[] = {
		{ "<=", OP_LE }, { ">=", OP_GE }, { "==", OP_EQ }, { "<", OP_LT },
		{ ">", OP_GT },
		{ NULL }
	};
	for (int i = 0; ops[i].text != NULL; i++) {
		const char *at = strstr(expr, ops[i].text);
		if (at == NULL)
			continue;
		size_t name_len = (size_t)(at - expr);
		if (name_len == 0 || name_len >= sizeof(out->name) || strlen(expr) >= sizeof(out->expr))
			return false;
		memcpy(out->name, expr, name_len);
		out->name[name_len] = '\0';
		out->op = ops[i].op;
		const char *number = at + strlen(ops[i].text);
		char *end;
		out->value = strtod(number, &end);
		snprintf(out->expr, sizeof(out->expr), "%s", expr);
		return end != number && *end == '\0';
	}
	return false;
}

static bool holds(int op, double got, double want)
{
	switch (op) {
	case OP_LE: return got <= want;
	case OP_GE: return got >= want;
	case OP_EQ: return got == want;
	case OP_GT: return got > want;
	default: return got < want;
	}
}

/* A summary value, by the name --json writes and --expect takes. Values that need frame
 * intervals are null when the run recorded none. */
struct named {
	const char *name;
	double value;
	bool available;
};

static int check_expectations(const struct expectation *expect, size_t count, const struct named *v, size_t n)
{
	int failures = 0;
	for (size_t i = 0; i < count; i++) {
		const struct named *found = NULL;
		for (size_t j = 0; j < n; j++)
			if (strcmp(v[j].name, expect[i].name) == 0) {
				found = &v[j];
				break;
			}
		if (found == NULL) {
			fprintf(stderr, "expectation failed: %s (no such summary field)\n", expect[i].expr);
			failures++;
		} else if (!found->available) {
			fprintf(stderr, "expectation failed: %s (no value: video is encrypted)\n", expect[i].expr);
			failures++;
		} else if (!holds(expect[i].op, found->value, expect[i].value)) {
			fprintf(stderr, "expectation failed: %s (got %g)\n", expect[i].expr, found->value);
			failures++;
		}
	}
	return failures;
}

static void write_array(FILE *f, const char *name, const double *v, size_t n)
{
	fprintf(f, ",\"%s\":[", name);
	for (size_t i = 0; i < n; i++)
		fprintf(f, "%s%.3f", i ? "," : "", v[i]);
	fputc(']', f);
}

/* The summary values, then with clicks every sample (for plotting the distribution). */
static bool write_json(const char *path, const struct named *v, size_t n, bool early, int terminate_error,
		       const struct click_stats *clk, const char *decode_how, const struct gpu *decoder)
{
	FILE *f = fopen(path, "w");
	if (f == NULL) {
		perror(path);
		return false;
	}
	fputc('{', f);
	for (size_t i = 0; i < n; i++) {
		fprintf(f, "%s\"%s\":", i ? "," : "", v[i].name);
		if (!v[i].available)
			fputs("null", f);
		else if (v[i].value == (double)(long long)v[i].value)
			fprintf(f, "%lld", (long long)v[i].value);
		else
			fprintf(f, "%.2f", v[i].value);
	}
	if (early)
		fprintf(f, ",\"terminated_error\":%d", terminate_error);
	/* How the pictures were decoded: on which GPU and driver, or in software. */
	fprintf(f, ",\"decoder\":{\"how\":\"%s\"", decode_how);
	if (decoder != NULL) {
		fputs(",\"gpu\":", f);
		probe_write_gpu(f, decoder);
	} else if (strcmp(decode_how, "software") == 0) {
		fputs(",\"driver\":\"libavcodec " AV_STRINGIFY(LIBAVCODEC_VERSION) "\"", f);
	}
	fputc('}', f);
	if (clk != NULL) {
		write_array(f, "click_ms", clk->decoded_ms, clk->samples);
		write_array(f, "click_received_ms", clk->received_ms, clk->samples);
	}
	fputs("}\n", f);
	return fclose(f) == 0;
}

static int stream(PSERVER_DATA server, int argc, char **argv)
{
	enum { CODEC = 256, HDR, DECODE, DECODE_DEVICE, AVOID_GPU, DUMP, MODE, BITRATE, SECONDS, CSV, STATS, CLOCK, JSON, EXPECT, NO_RFI, NO_ACK, QUIT, SEED, RANDOM, FRAME_EVERY, FRAME_RUN, WARMUP, BURST, BURSTY, PACKET_SIZE, DECODE_DELAY, DELAY, REORDER, DUPLICATE, RATE, CLICK_EVERY };
	static const struct option options[] = {
		{"codec", required_argument, NULL, CODEC},
		{"hdr", no_argument, NULL, HDR},
		{"decode", required_argument, NULL, DECODE},
		{"decode-device", required_argument, NULL, DECODE_DEVICE},
		{"avoid-gpu", required_argument, NULL, AVOID_GPU},
		{"dump", required_argument, NULL, DUMP},
		{"mode", required_argument, NULL, MODE},
		{"bitrate", required_argument, NULL, BITRATE},
		{"seconds", required_argument, NULL, SECONDS},
		{"csv", required_argument, NULL, CSV},
		{"stats", required_argument, NULL, STATS},
		{"clock", required_argument, NULL, CLOCK},
		{"json", required_argument, NULL, JSON},
		{"expect", required_argument, NULL, EXPECT},
		{"no-rfi", no_argument, NULL, NO_RFI},
		{"no-ack", no_argument, NULL, NO_ACK},
		{"quit", no_argument, NULL, QUIT},
		{"seed", required_argument, NULL, SEED},
		{"loss-random", required_argument, NULL, RANDOM},
		{"loss-frame-every", required_argument, NULL, FRAME_EVERY},
		{"loss-frame-run", required_argument, NULL, FRAME_RUN},
		{"loss-warmup", required_argument, NULL, WARMUP},
		{"loss-burst", required_argument, NULL, BURST},
		{"loss-bursty", required_argument, NULL, BURSTY},
		{"packet-size", required_argument, NULL, PACKET_SIZE},
		{"decode-delay", required_argument, NULL, DECODE_DELAY},
		{"delay", required_argument, NULL, DELAY},
		{"reorder", required_argument, NULL, REORDER},
		{"duplicate", required_argument, NULL, DUPLICATE},
		{"rate", required_argument, NULL, RATE},
		{"click-every", required_argument, NULL, CLICK_EVERY},
		{0},
	};
	const char *codec = "hevc", *csv = NULL, *stats = NULL, *clock_server = NULL, *json = NULL;
	struct expectation expect[MAX_EXPECT];
	size_t expectations = 0;
	int width = 1920, height = 1080, fps = 60, bitrate = 20000, seconds = 20, packet_size = 1392;
	bool rfi = true, quit = false, hdr = false;
	struct loss_config loss = {.seed = 1, .frame_warmup = 120};
	bool decode_none = false, decode_hw = false;
	const char *decode_device = NULL, *avoid_gpu = NULL;
	int click_every = 0;

	/* 0, not 1: glibc only fully resets getopt (including the "+" ordering) at 0. */
	optind = 0;
	int opt;
	while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
		switch (opt) {
		case CODEC: codec = optarg; break;
		case HDR: hdr = true; break;
		case DECODE:
			if (strcmp(optarg, "none") == 0)
				decode_set_enabled(false), decode_none = true;
			else if (strcmp(optarg, "hw") == 0)
				decode_hw = true;
			else if (strcmp(optarg, "full") != 0)
				usage(2);
			break;
		case DECODE_DEVICE: decode_device = optarg; break;
		case AVOID_GPU: avoid_gpu = optarg; break;
		case DUMP:
			if (!decode_set_dump(optarg)) {
				perror(optarg);
				return 1;
			}
			break;
		case MODE:
			if (sscanf(optarg, "%dx%dx%d", &width, &height, &fps) != 3)
				usage(2);
			break;
		case BITRATE: bitrate = atoi(optarg); break;
		case SECONDS: seconds = atoi(optarg); break;
		case CSV: csv = optarg; break;
		case STATS: stats = optarg; break;
		case CLOCK: clock_server = optarg; break;
		case JSON: json = optarg; break;
		case EXPECT:
			if (expectations == MAX_EXPECT) {
				fprintf(stderr, "too many --expect expressions\n");
				return 2;
			}
			if (!parse_expectation(optarg, &expect[expectations])) {
				fprintf(stderr, "bad --expect: %s\n", optarg);
				return 2;
			}
			expectations++;
			break;
		case NO_RFI: rfi = false; break;
		case NO_ACK: loss.strip_ack = true; break;
		case QUIT: quit = true; break;
		case SEED: loss.seed = strtoull(optarg, NULL, 10); break;
		case RANDOM: loss.random_percent = atof(optarg); break;
		case FRAME_EVERY: loss.frame_every = atoi(optarg); break;
		case FRAME_RUN: loss.frame_run = atoi(optarg); break;
		case WARMUP: loss.frame_warmup = atoi(optarg); break;
		case BURST:
			if (sscanf(optarg, "%d:%d", &loss.burst_packets, &loss.burst_every_ms) != 2)
				usage(2);
			break;
		case BURSTY:
			if (sscanf(optarg, "%lf:%lf", &loss.bursty_percent, &loss.bursty_length) != 2 ||
			    loss.bursty_percent <= 0 || loss.bursty_percent >= 100 || loss.bursty_length < 1) {
				fprintf(stderr, "bad --loss-bursty: %s (PERCENT:LENGTH, 0 < PERCENT < 100, LENGTH >= 1)\n", optarg);
				return 2;
			}
			break;
		case PACKET_SIZE:
			packet_size = atoi(optarg);
			if (packet_size < 256 || packet_size > 1392) {
				fprintf(stderr, "bad --packet-size: %s (256 to 1392)\n", optarg);
				return 2;
			}
			break;
		case DECODE_DELAY: decode_set_delay_ms(atoi(optarg)); break;
		case DELAY: {
			int n = sscanf(optarg, "%d:%d", &loss.delay_ms, &loss.jitter_ms);
			if (n < 1 || loss.delay_ms < 0 || loss.jitter_ms < 0 || loss.delay_ms > 5000 || loss.jitter_ms > 5000) {
				fprintf(stderr, "bad --delay: %s (MS or MS:JITTER, up to 5000)\n", optarg);
				return 2;
			}
			break;
		}
		case REORDER:
			if (sscanf(optarg, "%lf:%d", &loss.reorder_percent, &loss.reorder_ms) != 2 ||
			    loss.reorder_percent <= 0 || loss.reorder_percent > 100 || loss.reorder_ms < 1 ||
			    loss.reorder_ms > 5000) {
				fprintf(stderr, "bad --reorder: %s (PERCENT:MS)\n", optarg);
				return 2;
			}
			break;
		case DUPLICATE:
			loss.duplicate_percent = atof(optarg);
			if (loss.duplicate_percent <= 0 || loss.duplicate_percent > 100) {
				fprintf(stderr, "bad --duplicate: %s (PERCENT)\n", optarg);
				return 2;
			}
			break;
		case CLICK_EVERY:
			click_every = atoi(optarg);
			if (click_every < 100) {
				fprintf(stderr, "bad --click-every: %s (at least 100 ms)\n", optarg);
				return 2;
			}
			break;
		case RATE: {
			int n = sscanf(optarg, "%d:%d", &loss.rate_kbps, &loss.rate_queue_ms);
			if (n < 1 || loss.rate_kbps < 1 || loss.rate_queue_ms < 0) {
				fprintf(stderr, "bad --rate: %s (KBPS or KBPS:QUEUE_MS)\n", optarg);
				return 2;
			}
			break;
		}
		default: usage(2);
		}
	}
	if (optind >= argc)
		usage(2);
	if (click_every > 0 && decode_none) {
		fprintf(stderr, "--click-every needs the pictures decoded (not --decode none)\n");
		return 2;
	}
	click_configure(click_every);
	/* The VAAPI device: the one asked for, or the best that decodes this codec; none, software. */
	struct gpu gpus[16];
	const struct gpu *decoder = NULL;
	if (decode_hw) {
		int n = probe_gpus("/sys", gpus, 16);
		if (decode_device != NULL) {
			for (int i = 0; i < n; i++)
				if (strcmp(gpus[i].device, decode_device) == 0)
					decoder = &gpus[i];
		} else {
			unsigned needs = strcmp(codec, "h264") == 0 ? DECODES_H264
					 : strcmp(codec, "av1") == 0 ? (hdr ? DECODES_AV1_10 : DECODES_AV1)
					 : hdr ? DECODES_HEVC10 : DECODES_HEVC;
			decoder = probe_pick_decoder(gpus, n, needs, avoid_gpu);
			if (decoder == NULL) {
				fprintf(stderr, "no VAAPI device decodes %s%s here; decoding in software\n", codec, hdr ? " 10-bit" : "");
				decode_hw = false;
			}
		}
		if (decode_hw)
			decode_set_hw(decoder != NULL ? decoder->device : decode_device);
	}
	const char *decode_how = decode_none ? "none" : decode_hw ? "vaapi" : "software";
	const char *app = argv[optind];

	int app_id = find_app(server, app);
	if (app_id < 0) {
		fprintf(stderr, "no app named \"%s\" (try: list)\n", app);
		return 1;
	}

	STREAM_CONFIGURATION config;
	LiInitializeStreamConfiguration(&config);
	config.width = width;
	config.height = height;
	config.fps = fps;
	config.bitrate = bitrate;
	config.packetSize = packet_size;
	config.streamingRemotely = STREAM_CFG_LOCAL;
	config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
	/* Moonlight's default: audio encrypted, video only when the host requires it. */
	config.encryptionFlags = ENCFLG_AUDIO;
	if (strcmp(codec, "h264") == 0)
		config.supportedVideoFormats = VIDEO_FORMAT_H264;
	else if (strcmp(codec, "hevc") == 0)
		config.supportedVideoFormats = VIDEO_FORMAT_H265;
	else if (strcmp(codec, "av1") == 0)
		config.supportedVideoFormats = VIDEO_FORMAT_AV1_MAIN8;
	else
		usage(2);
	/* A 10-bit format is what asks the host for HDR at launch. */
	if (hdr) {
		if (config.supportedVideoFormats == VIDEO_FORMAT_H265)
			config.supportedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
		else if (config.supportedVideoFormats == VIDEO_FORMAT_AV1_MAIN8)
			config.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN10;
		else
			usage(2);
	}

	if (!record_open(csv, stats)) {
		perror(csv != NULL ? csv : stats);
		return 1;
	}
	loss_configure(&loss);
	record_set_bitrate(bitrate);
	if (stats != NULL) {
		loss_on_drop = record_drop;
		click_on_flash = record_click;
	}
	struct clock_result clock_start = {0}, clock_end = {0};
	bool clock_ok = false;
	if (clock_server != NULL) {
		clock_ok = clock_measure(clock_server, 16, &clock_start);
		if (clock_ok)
			record_set_clock_offset(clock_start.offset_us);
		else
			fprintf(stderr, "no NTP answer from %s: times stay on this machine's clock\n", clock_server);
		record_clock("start", clock_server, clock_ok ? &clock_start : NULL);
	}
	test_decoder.capabilities = rfi ? CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC |
						 CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
						 CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1
				       : 0;

	int ret = gs_start_app(server, &config, app_id, false, false, 0);
	if (ret != GS_OK) {
		fprintf(stderr, "launch failed (%d): %s\n", ret, gs_error ? gs_error : "");
		return 1;
	}

	CONNECTION_LISTENER_CALLBACKS listener;
	LiInitializeConnectionCallbacks(&listener);
	listener.connectionTerminated = on_terminated;
	listener.logMessage = on_log;
	listener.stageFailed = on_stage_failed;

	if (LiStartConnection(&server->serverInfo, &config, &listener, &test_decoder, NULL, NULL, 0, NULL, 0) != 0) {
		fprintf(stderr, "connection failed\n");
		return 1;
	}
	click_start(width, height);
	for (int i = 0; i < seconds * 10 && !terminated; i++)
		usleep(100 * 1000);
	uint64_t end_us = LiGetMicroseconds();
	bool early = terminated;
	click_stop();
	LiStopConnection();
	/* Again after the stream: how far the clocks drifted apart during it. */
	bool clock_end_ok = clock_server != NULL && clock_measure(clock_server, 16, &clock_end);
	if (clock_server != NULL)
		record_clock("end", clock_server, clock_end_ok ? &clock_end : NULL);
	if (quit)
		gs_quit_app(server);

	printf("stream app=\"%s\" codec=%s hdr=%d mode=%dx%dx%d bitrate=%d rfi=%d decode=%s\n", app, codec, hdr, width,
	       height, fps, bitrate, rfi, decode_hw ? decode_hw_device() : decode_how);
	struct record_stats rec;
	struct loss_stats loss_run;
	struct click_stats clk;
	record_stats(&rec, fps, end_us);
	loss_stats(&loss_run);
	click_stats(&clk);
	bool intervals = rec.intervals > 0, clicking = click_enabled(), click_samples = clk.samples > 0;
	const struct named values[] = {
		{"frames", rec.frames, true},
		{"seconds", rec.seconds, true},
		{"fps", rec.fps, true},
		{"idr", rec.idr, true},
		{"missing", rec.missing, true},
		{"decode_errors", rec.decode_errors, true},
		{"corrupt", rec.corrupt, true},
		{"stalls", rec.stalls, true},
		{"stalled_ms", rec.stalled_ms, true},
		{"interval_ms_p50", rec.p50_ms, intervals},
		{"interval_ms_p95", rec.p95_ms, intervals},
		{"interval_ms_p99", rec.p99_ms, intervals},
		{"interval_ms_max", rec.max_ms, intervals},
		{"decode_hw", decode_hw ? 1 : 0, true},
		{"decode_ms_p50", rec.decode_p50_ms, rec.decodes > 0 && !decode_none},
		{"decode_ms_p95", rec.decode_p95_ms, rec.decodes > 0 && !decode_none},
		{"decode_ms_p99", rec.decode_p99_ms, rec.decodes > 0 && !decode_none},
		{"decode_ms_max", rec.decode_max_ms, rec.decodes > 0 && !decode_none},
		{"video_packets", (double)loss_run.video_packets, true},
		{"dropped_packets", (double)loss_run.dropped_packets, true},
		{"dropped_frames", (double)loss_run.dropped_frames, true},
		{"duplicated_packets", (double)loss_run.duplicated_packets, true},
		{"reordered_packets", (double)loss_run.reordered_packets, true},
		{"seed", (double)loss_run.seed, true},
		{"recovery_frames", (double)recovery_requests, true},
		{"ack_requested_frames", (double)loss_run.ack_frames, true},
		{"idr_requests", (double)idr_requests, true},
		{"rfi_requests", (double)rfi_requests, true},
		{"clicks", clk.clicks, clicking},
		{"click_flashes", clk.flashes, clicking},
		{"click_missed", clk.missed, clicking},
		{"click_ms_mean", clk.mean_ms, click_samples},
		{"click_ms_p50", clk.p50_ms, click_samples},
		{"click_ms_p95", clk.p95_ms, click_samples},
		{"click_ms_p99", clk.p99_ms, click_samples},
		{"click_ms_max", clk.max_ms, click_samples},
	};
	size_t values_n = sizeof values / sizeof *values;
	record_summary(stdout, &rec);
	loss_summary(stdout, &loss_run);
	printf("requests idr=%d rfi=%d recovery=%d\n", (int)idr_requests, (int)rfi_requests,
		(int)recovery_requests);
	if (clicking)
		click_summary(stdout, &clk);
	if (clock_ok && clock_end_ok)
		printf("clock server=%s offset_us=%lld uncertainty_us=%lld end_offset_us=%lld end_uncertainty_us=%lld\n",
		       clock_server, (long long)clock_start.offset_us, (long long)clock_start.uncertainty_us,
		       (long long)clock_end.offset_us, (long long)clock_end.uncertainty_us);
	else if (clock_ok)
		printf("clock server=%s offset_us=%lld uncertainty_us=%lld\n", clock_server,
		       (long long)clock_start.offset_us, (long long)clock_start.uncertainty_us);
	if (early)
		printf("terminated error=%d\n", (int)terminate_error);
	record_close();
	if (json != NULL && !write_json(json, values, values_n, early, (int)terminate_error, clicking ? &clk : NULL, decode_how,
			     decoder))
		return 1;
	if (check_expectations(expect, expectations, values, values_n) > 0)
		return 3;
	return early ? 1 : 0;
}

static void make_keys_dir(const char *keys)
{
	char path[4096];
	snprintf(path, sizeof(path), "%s", keys);
	for (char *at = path + 1; *at != '\0'; at++) {
		if (*at != '/')
			continue;
		*at = '\0';
		if (mkdir(path, 0700) != 0 && errno != EEXIST) {
			perror(path);
			exit(1);
		}
		*at = '/';
	}
	if (mkdir(path, 0700) != 0 && errno != EEXIST) {
		perror(path);
		exit(1);
	}
}

/* libgamestream falls back to the fixed id 0123456789ABCDEF, which Moonlight clients share
 * and Moonshine keys pairing on, so give this client an id of its own. The keys directory is
 * this client's to create: the protocol library makes it later, when it writes certificates. */
static void ensure_unique_id(const char *keys)
{
	char path[4096];
	snprintf(path, sizeof(path), "%s/uniqueid.dat", keys);
	if (access(path, F_OK) == 0)
		return;
	FILE *random = fopen("/dev/urandom", "r"), *file = fopen(path, "w");
	if (random == NULL || file == NULL) {
		perror(path);
		exit(1);
	}
	for (int i = 0; i < 16; i++)
		fputc("0123456789ABCDEF"[fgetc(random) & 15], file);
	fclose(random);
	fclose(file);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{"host", required_argument, NULL, 'H'},
		{"port", required_argument, NULL, 'p'},
		{"keys", required_argument, NULL, 'k'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{0},
	};
	char *host = "127.0.0.1";
	unsigned short port = 47989;
	const char *keys = NULL;
	int opt;
	/* Stop at the command; its own options are parsed after it. */
	while ((opt = getopt_long(argc, argv, "+hH:p:k:v", options, NULL)) != -1) {
		switch (opt) {
		case 'h': usage(0); break;
		case 'H': host = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'k': keys = optarg; break;
		case 'v': verbose = true; break;
		default: usage(2);
		}
	}
	if (optind >= argc)
		usage(2);
	char *command = argv[optind];
	argc -= optind;
	argv += optind;
	if (strcmp(command, "probe") == 0)
		return probe_main(argc, argv);
	if (strcmp(command, "clock-serve") == 0)
		return clock_serve_main(argc, argv);

	if (keys == NULL) {
		static char path[4096];
		snprintf(path, sizeof(path), "%s/.config/gamestream-test-client", getenv("HOME") ? getenv("HOME") : ".");
		keys = path;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	make_keys_dir(keys);
	ensure_unique_id(keys);

	SERVER_DATA server;
	int ret = gs_init(&server, host, port, keys, verbose ? 2 : 0, true);
	if (ret != GS_OK) {
		fprintf(stderr, "can't reach %s:%u (%d): %s\n", host, port, ret, gs_error ? gs_error : "");
		return 1;
	}

	if (strcmp(command, "pair") == 0) {
		if (argc < 2)
			usage(2);
		if (gs_pair(&server, argv[1]) != GS_OK) {
			fprintf(stderr, "pairing failed: %s\n", gs_error);
			return 1;
		}
		printf("paired\n");
		return 0;
	}
	if (!server.paired) {
		fprintf(stderr, "not paired with %s (run: pair PIN)\n", host);
		return 1;
	}
	if (strcmp(command, "list") == 0) {
		PAPP_LIST list;
		if (gs_applist(&server, &list) != GS_OK) {
			fprintf(stderr, "app list failed: %s\n", gs_error);
			return 1;
		}
		for (; list != NULL; list = list->next)
			printf("%d\t%s\n", list->id, list->name);
		return 0;
	}
	if (strcmp(command, "stream") == 0)
		return stream(&server, argc, argv);
	usage(2);
	return 2;
}

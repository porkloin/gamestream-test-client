/* Decode with libavcodec, in software as Moonlight's FFmpeg software path does, or on a VAAPI
 * device as its hardware path does: a frame that fails to decode asks the host for a keyframe
 * (DR_NEED_IDR). A hardware frame counts as decoded once the device has finished it
 * (vaSyncSurface), as a client needs before it can show it; the click detector reads back only
 * the patch it looks at. */
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <va/va.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_client.h"

static AVCodecContext *ctx;
static AVPacket *packet;
static AVFrame *frame;
static uint8_t *buffer;
static size_t buffer_size;
static bool enabled = true;
static uint64_t delay_us;
static FILE *dump;
static const char *hw_device; /* a VAAPI render node, or NULL for software */
static AVBufferRef *hw_ctx;
static VAImage patch = {.image_id = VA_INVALID_ID}; /* the click detector's 32x32 patch, read back */
static VAImage whole = {.image_id = VA_INVALID_ID}; /* --check-picture's whole frame, read back */
static double picture_threshold; /* --check-picture: 0 off */
static int picture_width, picture_height; /* the stream's size, which framestamp drew at */

void decode_set_hw(const char *device)
{
	hw_device = device;
}

const char *decode_hw_device(void)
{
	return hw_device;
}

static enum AVPixelFormat pick_vaapi(AVCodecContext *c, const enum AVPixelFormat *formats)
{
	(void)c;
	for (; *formats != AV_PIX_FMT_NONE; formats++)
		if (*formats == AV_PIX_FMT_VAAPI)
			return *formats;
	fprintf(stderr, "%s can't decode this stream\n", hw_device);
	return AV_PIX_FMT_NONE;
}

/* The decoder for ID: on a VAAPI device, one with VAAPI hwaccel, since libavcodec's default can be a
 * software-only library (AV1's is libdav1d, which ignores hw_device_ctx and decodes on the CPU). */
static const AVCodec *find_decoder(enum AVCodecID id)
{
	if (hw_device == NULL)
		return avcodec_find_decoder(id);
	void *it = NULL;
	for (const AVCodec *c; (c = av_codec_iterate(&it)) != NULL;) {
		if (c->id != id || !av_codec_is_decoder(c))
			continue;
		for (int i = 0;; i++) {
			const AVCodecHWConfig *hw = avcodec_get_hw_config(c, i);
			if (hw == NULL)
				break;
			if (hw->device_type == AV_HWDEVICE_TYPE_VAAPI && (hw->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
				return c;
		}
	}
	return NULL;
}

static VADisplay va_display(void)
{
	return ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)hw_ctx->data)->hwctx)->display;
}

/* Wait for the device to finish decoding a frame. */
static void hw_sync(const AVFrame *f)
{
	vaSyncSurface(va_display(), (VASurfaceID)(uintptr_t)f->data[3]);
}

/* The brightness of the 32x32 patch in the middle of a hardware frame, read back with vaGetImage
 * (which untiles it), or -1. Copying the whole frame back would cost milliseconds at 4K. */
static double hw_patch_luma(const AVFrame *f)
{
	VADisplay display = va_display();
	enum AVPixelFormat sw = ((AVHWFramesContext *)f->hw_frames_ctx->data)->sw_format;
	unsigned fourcc = sw == AV_PIX_FMT_P010 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
	if (patch.image_id == VA_INVALID_ID || patch.format.fourcc != fourcc) {
		if (patch.image_id != VA_INVALID_ID)
			vaDestroyImage(display, patch.image_id);
		patch.image_id = VA_INVALID_ID;
		VAImageFormat format = {.fourcc = fourcc, .byte_order = VA_LSB_FIRST,
					.bits_per_pixel = fourcc == VA_FOURCC_P010 ? 24 : 12};
		if (vaCreateImage(display, &format, 32, 32, &patch) != VA_STATUS_SUCCESS) {
			patch.image_id = VA_INVALID_ID;
			return -1;
		}
	}
	int x = (f->width / 2 - 16) & ~1, y = (f->height / 2 - 16) & ~1;
	if (x < 0 || y < 0 ||
	    vaGetImage(display, (VASurfaceID)(uintptr_t)f->data[3], x, y, 32, 32, patch.image_id) != VA_STATUS_SUCCESS)
		return -1;
	void *data;
	if (vaMapBuffer(display, patch.buf, &data) != VA_STATUS_SUCCESS)
		return -1;
	/* P010 keeps its 10 bits high in 16, so it reads as 16-bit. */
	double luma = click_luma((const uint8_t *)data + patch.offsets[0], (int)patch.pitches[0], 32, 32,
				 fourcc == VA_FOURCC_P010 ? 16 : 8);
	vaUnmapBuffer(display, patch.buf);
	return luma;
}

void decode_set_picture_check(double threshold, int width, int height)
{
	picture_threshold = threshold;
	picture_width = width;
	picture_height = height;
}

/* A VAAPI frame's luma, read back for --check-picture. */
struct va_rows {
	VADisplay display;
	VASurfaceID surface;
	unsigned fourcc;
	int width, height, bytes; /* bytes a sample */
	VAImage *image;
	uint8_t *copy;
	size_t copy_size;
};

static VAImage strip = {.image_id = VA_INVALID_ID}; /* --check-picture's strip, reused frame to frame */

/* (Re)create `img` as a fourcc image of at least width x height. */
static bool va_image(VADisplay display, VAImage *img, unsigned fourcc, int width, int height)
{
	if (img->image_id != VA_INVALID_ID && img->format.fourcc == fourcc && img->width >= width &&
	    img->height >= height)
		return true;
	if (img->image_id != VA_INVALID_ID)
		vaDestroyImage(display, img->image_id);
	img->image_id = VA_INVALID_ID;
	VAImageFormat format = {.fourcc = fourcc, .byte_order = VA_LSB_FIRST,
				.bits_per_pixel = fourcc == VA_FOURCC_P010 ? 24 : 12};
	if (vaCreateImage(display, &format, width, height, img) != VA_STATUS_SUCCESS) {
		img->image_id = VA_INVALID_ID;
		return false;
	}
	return true;
}

/* Rows y0 to y0 + h - 1 of the surface's luma, copied out of a strip read back from the GPU (NV12 and P010 strips
 * start on an even row and are an even number of rows). */
static const uint8_t *va_rows(void *ctx, int y0, int h, int *stride)
{
	struct va_rows *v = ctx;
	int y = y0 & ~1, rows = (y0 + h - y + 1) & ~1;
	if (y < 0 || y + rows > v->height)
		return NULL;
	/* One image for every strip: at least 64 rows, so the stamp's strip (two cells, 64 rows at 4K) fits too. */
	if (!va_image(v->display, v->image, v->fourcc, v->width, rows > 64 ? rows : 64))
		return NULL;
	if (vaGetImage(v->display, v->surface, 0, y, v->width, rows, v->image->image_id) != VA_STATUS_SUCCESS)
		return NULL;
	void *data;
	if (vaMapBuffer(v->display, v->image->buf, &data) != VA_STATUS_SUCCESS)
		return NULL;
	size_t line = (size_t)v->width * v->bytes, need = line * rows;
	if (need > v->copy_size) {
		v->copy = realloc(v->copy, need);
		v->copy_size = need;
	}
	const uint8_t *luma = (const uint8_t *)data + v->image->offsets[0];
	for (int r = 0; r < rows; r++)
		memcpy(v->copy + line * r, luma + (size_t)v->image->pitches[0] * r, line);
	vaUnmapBuffer(v->display, v->image->buf);
	*stride = (int)line;
	return v->copy + line * (y0 - y);
}

/* --check-picture on a decoded frame: its luma plane in memory, or for a VAAPI frame the strips the check reads, read
 * back from the GPU. A corrupt frame with --picture-dump is read back whole, to be written out. */
static void check_picture(const AVFrame *f, struct picture_result *r)
{
	memset(r, 0, sizeof *r);
	/* framestamp drew at the stream's size; a frame decoded larger (padding rows an AV1 encoder left in) is checked in
	 * its top left, and the size it came at recorded. One decoded smaller can't hold the picture. */
	int width = picture_width > 0 ? picture_width : f->width, height = picture_height > 0 ? picture_height : f->height;
	bool wrong = f->width != width || f->height != height;
	if (f->width < width || f->height < height) {
		r->decoded_width = f->width, r->decoded_height = f->height;
		return;
	}
	if (f->format != AV_PIX_FMT_VAAPI) {
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(f->format);
		int depth = desc ? desc->comp[0].depth : 8;
		picture_check(f->data[0], f->linesize[0], depth, width, height, r);
		if (r->readable && r->worst > picture_threshold)
			picture_dump(f->data[0], f->linesize[0], depth, width, height, r);
		if (wrong)
			r->decoded_width = f->width, r->decoded_height = f->height;
		return;
	}
	enum AVPixelFormat sw = ((AVHWFramesContext *)f->hw_frames_ctx->data)->sw_format;
	static uint8_t *copy;
	static size_t copy_size;
	struct va_rows v = {
		.display = va_display(),
		.surface = (VASurfaceID)(uintptr_t)f->data[3],
		.fourcc = sw == AV_PIX_FMT_P010 ? VA_FOURCC_P010 : VA_FOURCC_NV12,
		.width = f->width,
		.height = f->height,
		.bytes = sw == AV_PIX_FMT_P010 ? 2 : 1,
		.image = &strip,
		.copy = copy,
		.copy_size = copy_size,
	};
	int depth = v.bytes == 2 ? 16 : 8;
	picture_check_rows(va_rows, &v, depth, width, height, r);
	copy = v.copy, copy_size = v.copy_size;
	if (wrong)
		r->decoded_width = f->width, r->decoded_height = f->height;
	if (!(r->readable && r->worst > picture_threshold) || !picture_dumping())
		return;
	if (!va_image(v.display, &whole, v.fourcc, f->width, f->height) ||
	    vaGetImage(v.display, v.surface, 0, 0, f->width, f->height, whole.image_id) != VA_STATUS_SUCCESS)
		return;
	void *data;
	if (vaMapBuffer(v.display, whole.buf, &data) != VA_STATUS_SUCCESS)
		return;
	picture_dump((const uint8_t *)data + whole.offsets[0], (int)whole.pitches[0], depth, width, height, r);
	vaUnmapBuffer(v.display, whole.buf);
}

/* --check-picture runs off the decoder's thread: the decoder hands each decoded frame (a reference to it, so its
 * surface stays alive) to a worker that reads it back and checks it. The decoder waits for nothing; when the worker is
 * behind and the queue full, the frame is skipped and counted instead. The queue's frames are surfaces held out of
 * the decoder's pool, so the pool gets that many more (setup). */
#define PICTURE_QUEUE 3

struct picture_job {
	AVFrame *frame;
	int frame_number;
	bool recovery, keyframe;
};

static struct {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t ready;
	struct picture_job jobs[PICTURE_QUEUE];
	int head, count;
	bool running, stopping;
} worker = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};

static void *picture_worker(void *arg)
{
	pthread_mutex_lock(&worker.lock);
	for (;;) {
		while (worker.count == 0 && !worker.stopping)
			pthread_cond_wait(&worker.ready, &worker.lock);
		/* Stopping finishes what's queued, so the summary counts every frame handed over. */
		if (worker.count == 0)
			break;
		struct picture_job job = worker.jobs[worker.head];
		pthread_mutex_unlock(&worker.lock);
		struct picture_result r;
		uint64_t start = LiGetMicroseconds();
		check_picture(job.frame, &r);
		record_picture(job.frame_number, job.recovery, job.keyframe, &r, r.readable && r.worst > picture_threshold,
			       (LiGetMicroseconds() - start) / 1000.0);
		av_frame_free(&job.frame);
		pthread_mutex_lock(&worker.lock);
		worker.head = (worker.head + 1) % PICTURE_QUEUE;
		worker.count--;
	}
	pthread_mutex_unlock(&worker.lock);
	return NULL;
}

/* Hand a decoded frame to the worker, or count it skipped when the queue is full. */
static void queue_picture(const AVFrame *f, int frame_number, bool recovery, bool keyframe)
{
	pthread_mutex_lock(&worker.lock);
	if (!worker.running) {
		worker.running = pthread_create(&worker.thread, NULL, picture_worker, NULL) == 0;
		worker.stopping = false;
	}
	AVFrame *ref = worker.count < PICTURE_QUEUE ? av_frame_clone(f) : NULL;
	if (ref == NULL) {
		pthread_mutex_unlock(&worker.lock);
		record_picture_skipped();
		return;
	}
	worker.jobs[(worker.head + worker.count) % PICTURE_QUEUE] =
		(struct picture_job){.frame = ref, .frame_number = frame_number, .recovery = recovery, .keyframe = keyframe};
	worker.count++;
	pthread_cond_signal(&worker.ready);
	pthread_mutex_unlock(&worker.lock);
}

/* Let the worker finish the queue, and stop it. */
static void stop_picture_worker(void)
{
	pthread_mutex_lock(&worker.lock);
	bool running = worker.running;
	worker.stopping = true;
	pthread_cond_signal(&worker.ready);
	pthread_mutex_unlock(&worker.lock);
	if (running)
		pthread_join(worker.thread, NULL);
	worker.running = false;
}

bool decode_set_dump(const char *path)
{
	dump = fopen(path, "wb");
	return dump != NULL;
}

void decode_set_delay_ms(int ms)
{
	delay_us = ms > 0 ? (uint64_t)ms * 1000 : 0;
}

/* Hold the decoder thread until the frame has taken delay_us since start. */
static void hold(uint64_t start)
{
	uint64_t now = LiGetMicroseconds();
	if (delay_us > 0 && now < start + delay_us)
		usleep((useconds_t)(start + delay_us - now));
}

void decode_set_enabled(bool on)
{
	enabled = on;
}

static int setup(int video_format, int width, int height, int fps, void *context, int flags)
{
	if (!enabled) {
		fprintf(stderr, "decoder off %dx%d@%d\n", width, height, fps);
		return 0;
	}
	enum AVCodecID id;
	if (video_format & VIDEO_FORMAT_MASK_H264)
		id = AV_CODEC_ID_H264;
	else if (video_format & VIDEO_FORMAT_MASK_H265)
		id = AV_CODEC_ID_HEVC;
	else if (video_format & VIDEO_FORMAT_MASK_AV1)
		id = AV_CODEC_ID_AV1;
	else
		return -1;
	const AVCodec *codec = find_decoder(id);
	if (codec == NULL) {
		fprintf(stderr, "no libavcodec decoder%s for format 0x%x\n", hw_device ? " with VAAPI" : "", video_format);
		return -1;
	}
	ctx = avcodec_alloc_context3(codec);
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx->flags2 |= AV_CODEC_FLAG2_FAST;
	if (hw_device != NULL) {
		int err = av_hwdevice_ctx_create(&hw_ctx, AV_HWDEVICE_TYPE_VAAPI, hw_device, NULL, 0);
		if (err < 0) {
			fprintf(stderr, "can't open VAAPI device %s: %s\n", hw_device, av_err2str(err));
			return -1;
		}
		ctx->hw_device_ctx = av_buffer_ref(hw_ctx);
		ctx->get_format = pick_vaapi;
		/* Surfaces for the frames waiting to be checked, besides the decoder's own. */
		if (picture_threshold > 0)
			ctx->extra_hw_frames = PICTURE_QUEUE + 1;
	} else {
		ctx->thread_type = FF_THREAD_SLICE;
		ctx->thread_count = 4;
	}
	ctx->width = width;
	ctx->height = height;
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		fprintf(stderr, "avcodec_open2 failed\n");
		return -1;
	}
	packet = av_packet_alloc();
	frame = av_frame_alloc();
	fprintf(stderr, "decoder %s%s%s %dx%d@%d\n", codec->name, hw_device ? " vaapi " : "", hw_device ? hw_device : "",
		width, height, fps);
	return 0;
}

static void cleanup(void)
{
	stop_picture_worker();
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&ctx);
	if (hw_ctx != NULL && patch.image_id != VA_INVALID_ID)
		vaDestroyImage(va_display(), patch.image_id);
	patch.image_id = VA_INVALID_ID;
	if (hw_ctx != NULL && whole.image_id != VA_INVALID_ID)
		vaDestroyImage(va_display(), whole.image_id);
	whole.image_id = VA_INVALID_ID;
	if (hw_ctx != NULL && strip.image_id != VA_INVALID_ID)
		vaDestroyImage(va_display(), strip.image_id);
	strip.image_id = VA_INVALID_ID;
	av_buffer_unref(&hw_ctx);
	if (dump != NULL)
		fclose(dump);
	dump = NULL;
	free(buffer);
	buffer = NULL;
	buffer_size = 0;
}

static int submit(PDECODE_UNIT unit)
{
	if (dump != NULL)
		for (PLENTRY entry = unit->bufferList; entry != NULL; entry = entry->next)
			fwrite(entry->data, 1, entry->length, dump);
	struct frame_row row = {
		.frame_number = unit->frameNumber,
		.frame_type = unit->frameType,
		.bytes = unit->fullLength,
		.host_latency_ms = unit->frameHostProcessingLatency / 10.0,
		.receive_us = unit->receiveTimeUs,
		.enqueue_us = unit->enqueueTimeUs,
		.rtp_timestamp = unit->rtpTimestamp,
		.decode_start_us = LiGetMicroseconds(),
		.recovery = is_recovery_frame(unit->frameNumber),
		.queued = LiGetPendingVideoFrames(),
	};
	if (!enabled) {
		hold(row.decode_start_us);
		row.decode_end_us = LiGetMicroseconds();
		record_frame(&row);
		return DR_OK;
	}
	size_t needed = unit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE;
	if (needed > buffer_size) {
		buffer = realloc(buffer, needed);
		buffer_size = needed;
	}
	size_t offset = 0;
	for (PLENTRY entry = unit->bufferList; entry != NULL; entry = entry->next) {
		memcpy(buffer + offset, entry->data, entry->length);
		offset += entry->length;
	}
	memset(buffer + offset, 0, AV_INPUT_BUFFER_PADDING_SIZE);

	packet->data = buffer;
	packet->size = (int)offset;
	int err = avcodec_send_packet(ctx, packet);
	if (err == 0) {
		err = avcodec_receive_frame(ctx, frame);
		if (err == 0) {
			row.width = frame->width;
			row.height = frame->height;
			/* A frame decoded from a missing reference still decodes; libavcodec
			 * flags it corrupt. */
			if (frame->flags & AV_FRAME_FLAG_CORRUPT)
				err = 1;
			if (frame->format == AV_PIX_FMT_VAAPI)
				hw_sync(frame);
			if (click_enabled()) {
				uint64_t decoded_us = click_now_us();
				double luma;
				if (frame->format == AV_PIX_FMT_VAAPI) {
					luma = hw_patch_luma(frame);
				} else {
					const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
					int depth = desc ? desc->comp[0].depth : 8;
					luma = click_luma(frame->data[0], frame->linesize[0], frame->width, frame->height, depth);
				}
				if (luma >= 0)
					click_on_luma(luma, decoded_us, row.receive_us, row.frame_number);
			}
		} else if (err == AVERROR(EAGAIN)) {
			err = 0;
		}
	}
	hold(row.decode_start_us);
	row.decode_end_us = LiGetMicroseconds();
	row.decode_status = err;
	/* Checked on the worker's thread, after the decode's own time is taken. */
	if (picture_threshold > 0 && err == 0 && row.width > 0)
		queue_picture(frame, row.frame_number, row.recovery, row.frame_type == FRAME_TYPE_IDR);
	record_frame(&row);
	return err < 0 ? DR_NEED_IDR : DR_OK;
}

DECODER_RENDERER_CALLBACKS test_decoder = {
	.setup = setup,
	.cleanup = cleanup,
	.submitDecodeUnit = submit,
};

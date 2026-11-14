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
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&ctx);
	if (hw_ctx != NULL && patch.image_id != VA_INVALID_ID)
		vaDestroyImage(va_display(), patch.image_id);
	patch.image_id = VA_INVALID_ID;
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
	record_frame(&row);
	return err < 0 ? DR_NEED_IDR : DR_OK;
}

DECODER_RENDERER_CALLBACKS test_decoder = {
	.setup = setup,
	.cleanup = cleanup,
	.submitDecodeUnit = submit,
};

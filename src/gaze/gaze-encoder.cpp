/******************************************************************************
	Copyright (C) 2016-2024 DistroAV <contact@distroav.org>

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, see <https://www.gnu.org/licenses/>.
******************************************************************************/

#include "gaze-encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <obs-module.h>
#include <plugin-support.h>
#include <cstring>

// Encoder names for each type and codec
struct encoder_info {
	gaze_encoder_type_t type;
	const char *hevc_name;
	const char *h264_name;
};

static const encoder_info encoder_list[] = {
	{GAZE_ENCODER_NVENC, "hevc_nvenc", "h264_nvenc"},
	{GAZE_ENCODER_AMF, "hevc_amf", "h264_amf"},
	{GAZE_ENCODER_QSV, "hevc_qsv", "h264_qsv"},
	{GAZE_ENCODER_SOFTWARE, "libx265", "libx264"},
};

static const char *get_encoder_name(gaze_codec_t codec, gaze_encoder_type_t type)
{
	for (const auto &info : encoder_list) {
		if (info.type == type) {
			return (codec == GAZE_CODEC_HEVC) ? info.hevc_name
							  : info.h264_name;
		}
	}
	return nullptr;
}

const char *gaze_encoder_type_name(gaze_encoder_type_t type)
{
	switch (type) {
	case GAZE_ENCODER_AUTO:
		return "Auto";
	case GAZE_ENCODER_NVENC:
		return "NVENC";
	case GAZE_ENCODER_AMF:
		return "AMF";
	case GAZE_ENCODER_QSV:
		return "QSV";
	case GAZE_ENCODER_SOFTWARE:
		return "Software";
	default:
		return "Unknown";
	}
}

const char *gaze_codec_name(gaze_codec_t codec)
{
	switch (codec) {
	case GAZE_CODEC_HEVC:
		return "HEVC";
	case GAZE_CODEC_H264:
		return "H.264";
	default:
		return "Unknown";
	}
}

bool gaze_encoder_is_available(gaze_codec_t codec, gaze_encoder_type_t type)
{
	const char *name = get_encoder_name(codec, type);
	if (!name)
		return false;

	const AVCodec *enc = avcodec_find_encoder_by_name(name);
	return enc != nullptr;
}

static bool configure_nvenc(AVCodecContext *ctx, gaze_codec_t codec)
{
	(void)codec;

	// Ultra low latency preset
	av_opt_set(ctx->priv_data, "preset", "p1", 0);
	av_opt_set(ctx->priv_data, "tune", "ull", 0);
	av_opt_set(ctx->priv_data, "rc", "cbr", 0);
	av_opt_set_int(ctx->priv_data, "zerolatency", 1, 0);
	av_opt_set_int(ctx->priv_data, "delay", 0, 0);

	// Disable B-frames for low latency
	ctx->max_b_frames = 0;

	// Use BGRA input directly - NVENC handles color conversion on GPU
	// This eliminates CPU-based sws_scale which is a major bottleneck
	ctx->pix_fmt = AV_PIX_FMT_BGRA;

	return true;
}

static bool configure_amf(AVCodecContext *ctx, gaze_codec_t codec)
{
	(void)codec;

	// Ultra low latency settings
	av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
	av_opt_set(ctx->priv_data, "quality", "speed", 0);
	av_opt_set(ctx->priv_data, "rc", "cbr", 0);

	ctx->max_b_frames = 0;

	return true;
}

static bool configure_qsv(AVCodecContext *ctx, gaze_codec_t codec)
{
	(void)codec;

	// Low latency settings
	av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
	av_opt_set_int(ctx->priv_data, "low_delay_brc", 1, 0);
	av_opt_set_int(ctx->priv_data, "async_depth", 1, 0);

	ctx->max_b_frames = 0;

	return true;
}

static bool configure_software(AVCodecContext *ctx, gaze_codec_t codec)
{
	// Ultra low latency software encoding
	av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
	av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);

	if (codec == GAZE_CODEC_H264) {
		// x264 specific
		av_opt_set(ctx->priv_data, "profile", "baseline", 0);
	}

	ctx->max_b_frames = 0;

	return true;
}

static bool try_init_encoder(gaze_encoder_t *encoder, gaze_encoder_type_t type)
{
	const char *enc_name = get_encoder_name(encoder->codec, type);
	if (!enc_name) {
		return false;
	}

	const AVCodec *codec = avcodec_find_encoder_by_name(enc_name);
	if (!codec) {
		obs_log(LOG_DEBUG, "[gaze-encoder] Encoder not found: %s",
			enc_name);
		return false;
	}

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		obs_log(LOG_ERROR, "[gaze-encoder] Failed to allocate context");
		return false;
	}

	// Basic configuration
	ctx->width = encoder->width;
	ctx->height = encoder->height;
	ctx->time_base = {(int)encoder->fps_den, (int)encoder->fps_num};
	ctx->framerate = {(int)encoder->fps_num, (int)encoder->fps_den};
	ctx->pix_fmt = AV_PIX_FMT_NV12; // Default, may be overridden by encoder config
	ctx->bit_rate = (int64_t)encoder->bitrate_kbps * 1000;
	ctx->rc_max_rate = ctx->bit_rate;
	ctx->rc_buffer_size = (int)ctx->bit_rate;
	ctx->gop_size =
		encoder->keyframe_interval > 0
			? encoder->keyframe_interval
			: (encoder->fps_num / encoder->fps_den) * 2; // 2 seconds
	ctx->thread_count = 0; // Auto

	// Apply encoder-specific configuration
	bool configured = false;
	switch (type) {
	case GAZE_ENCODER_NVENC:
		configured = configure_nvenc(ctx, encoder->codec);
		break;
	case GAZE_ENCODER_AMF:
		configured = configure_amf(ctx, encoder->codec);
		break;
	case GAZE_ENCODER_QSV:
		configured = configure_qsv(ctx, encoder->codec);
		break;
	case GAZE_ENCODER_SOFTWARE:
		configured = configure_software(ctx, encoder->codec);
		break;
	default:
		break;
	}

	if (!configured) {
		avcodec_free_context(&ctx);
		return false;
	}

	// Try to open the encoder
	int ret = avcodec_open2(ctx, codec, nullptr);
	if (ret < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, err, sizeof(err));
		obs_log(LOG_DEBUG, "[gaze-encoder] Failed to open %s: %s",
			enc_name, err);
		avcodec_free_context(&ctx);
		return false;
	}

	encoder->codec_ctx = ctx;
	encoder->actual_encoder = type;

	obs_log(LOG_INFO, "[gaze-encoder] Initialized %s (%s)",
		gaze_encoder_type_name(type), enc_name);

	return true;
}

bool gaze_encoder_init(gaze_encoder_t *encoder, gaze_codec_t codec,
		       gaze_encoder_type_t type, uint32_t width,
		       uint32_t height, uint32_t bitrate_kbps,
		       uint32_t fps_num, uint32_t fps_den,
		       uint32_t keyframe_interval)
{
	if (!encoder || width == 0 || height == 0)
		return false;

	memset(encoder, 0, sizeof(*encoder));

	encoder->codec = codec;
	encoder->encoder_type = type;
	encoder->width = width;
	encoder->height = height;
	encoder->bitrate_kbps = bitrate_kbps;
	encoder->fps_num = fps_num > 0 ? fps_num : 30;
	encoder->fps_den = fps_den > 0 ? fps_den : 1;
	encoder->keyframe_interval =
		keyframe_interval > 0 ? keyframe_interval
				      : GAZE_DEFAULT_KEYFRAME_INTERVAL;

	// Try encoders in priority order
	bool success = false;

	if (type == GAZE_ENCODER_AUTO) {
		// Try hardware encoders first, then software
		static const gaze_encoder_type_t priority[] = {
			GAZE_ENCODER_NVENC, GAZE_ENCODER_AMF, GAZE_ENCODER_QSV,
			GAZE_ENCODER_SOFTWARE};

		for (auto t : priority) {
			if (try_init_encoder(encoder, t)) {
				success = true;
				break;
			}
		}
	} else {
		// Try specific encoder, fallback to software if not available
		if (try_init_encoder(encoder, type)) {
			success = true;
		} else if (type != GAZE_ENCODER_SOFTWARE) {
			obs_log(LOG_WARNING,
				"[gaze-encoder] %s not available, falling back to software",
				gaze_encoder_type_name(type));
			success = try_init_encoder(encoder,
						   GAZE_ENCODER_SOFTWARE);
		}
	}

	if (!success) {
		obs_log(LOG_ERROR, "[gaze-encoder] No encoder available");
		return false;
	}

	// Allocate frame with the pixel format the encoder expects
	encoder->frame = av_frame_alloc();
	if (!encoder->frame) {
		obs_log(LOG_ERROR, "[gaze-encoder] Failed to allocate frame");
		gaze_encoder_destroy(encoder);
		return false;
	}

	encoder->frame->format = encoder->codec_ctx->pix_fmt;
	encoder->frame->width = width;
	encoder->frame->height = height;

	if (av_frame_get_buffer(encoder->frame, 32) < 0) {
		obs_log(LOG_ERROR,
			"[gaze-encoder] Failed to allocate frame buffer");
		gaze_encoder_destroy(encoder);
		return false;
	}

	// Allocate packet
	encoder->packet = av_packet_alloc();
	if (!encoder->packet) {
		obs_log(LOG_ERROR, "[gaze-encoder] Failed to allocate packet");
		gaze_encoder_destroy(encoder);
		return false;
	}

	// Initialize color converter only if needed (not for NVENC with BGRA input)
	if (encoder->codec_ctx->pix_fmt != AV_PIX_FMT_BGRA) {
		encoder->sws_ctx = sws_getContext(
			width, height, AV_PIX_FMT_BGRA, width, height,
			encoder->codec_ctx->pix_fmt,
			SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

		if (!encoder->sws_ctx) {
			obs_log(LOG_ERROR,
				"[gaze-encoder] Failed to create color converter");
			gaze_encoder_destroy(encoder);
			return false;
		}
	} else {
		encoder->sws_ctx = nullptr;
		obs_log(LOG_INFO,
			"[gaze-encoder] Using direct BGRA input - no color conversion needed");
	}

	encoder->initialized = true;

	const char *pix_fmt_name =
		encoder->codec_ctx->pix_fmt == AV_PIX_FMT_BGRA ? "BGRA (direct)"
							       : "NV12";
	obs_log(LOG_INFO,
		"[gaze-encoder] Ready: %s %s %ux%u @ %u/%u fps, %u kbps, %s",
		gaze_codec_name(codec),
		gaze_encoder_type_name(encoder->actual_encoder), width, height,
		fps_num, fps_den, bitrate_kbps, pix_fmt_name);

	return true;
}

bool gaze_encoder_encode(gaze_encoder_t *encoder, const uint8_t *bgra_data,
			 uint32_t linesize, uint8_t **encoded_data,
			 size_t *encoded_size, bool *is_keyframe)
{
	if (!encoder || !encoder->initialized || !bgra_data)
		return false;

	*encoded_data = nullptr;
	*encoded_size = 0;
	*is_keyframe = false;

	// Make frame writable
	if (av_frame_make_writable(encoder->frame) < 0) {
		obs_log(LOG_ERROR,
			"[gaze-encoder] Failed to make frame writable");
		return false;
	}

	// Copy/convert frame data
	if (encoder->sws_ctx) {
		// Convert BGRA to NV12 (for non-NVENC encoders)
		const uint8_t *src_data[1] = {bgra_data};
		int src_linesize[1] = {(int)linesize};

		sws_scale(encoder->sws_ctx, src_data, src_linesize, 0,
			  encoder->height, encoder->frame->data,
			  encoder->frame->linesize);
	} else {
		// Direct BGRA copy for NVENC (no color conversion needed)
		// NVENC handles BGRA -> YUV conversion internally on GPU
		if ((int)linesize == encoder->frame->linesize[0]) {
			// Same linesize, single memcpy
			memcpy(encoder->frame->data[0], bgra_data,
			       linesize * encoder->height);
		} else {
			// Different linesize, copy line by line
			uint32_t copy_width =
				encoder->width * 4; // 4 bytes per pixel (BGRA)
			for (uint32_t y = 0; y < encoder->height; y++) {
				memcpy(encoder->frame->data[0] +
					       y * encoder->frame->linesize[0],
				       bgra_data + y * linesize, copy_width);
			}
		}
	}

	// Set PTS
	encoder->frame->pts = encoder->frame_count++;

	// Force keyframe if requested
	if (encoder->keyframe_pending) {
		encoder->frame->pict_type = AV_PICTURE_TYPE_I;
		encoder->frame->flags |= AV_FRAME_FLAG_KEY;
		encoder->keyframe_pending = false;
	} else {
		encoder->frame->pict_type = AV_PICTURE_TYPE_NONE;
		encoder->frame->flags &= ~AV_FRAME_FLAG_KEY;
	}

	// Send frame to encoder
	int ret = avcodec_send_frame(encoder->codec_ctx, encoder->frame);
	if (ret < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, err, sizeof(err));
		obs_log(LOG_ERROR, "[gaze-encoder] Send frame failed: %s", err);
		return false;
	}

	// Receive encoded packet
	ret = avcodec_receive_packet(encoder->codec_ctx, encoder->packet);
	if (ret == AVERROR(EAGAIN)) {
		// Need more input
		return true;
	} else if (ret < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, err, sizeof(err));
		obs_log(LOG_ERROR, "[gaze-encoder] Receive packet failed: %s",
			err);
		return false;
	}

	*encoded_data = encoder->packet->data;
	*encoded_size = encoder->packet->size;
	*is_keyframe = (encoder->packet->flags & AV_PKT_FLAG_KEY) != 0;

	return true;
}

void gaze_encoder_request_keyframe(gaze_encoder_t *encoder)
{
	if (encoder)
		encoder->keyframe_pending = true;
}

bool gaze_encoder_set_bitrate(gaze_encoder_t *encoder, uint32_t bitrate_kbps)
{
	if (!encoder || !encoder->initialized)
		return false;

	// Note: Dynamic bitrate change may not be supported by all encoders
	encoder->codec_ctx->bit_rate = (int64_t)bitrate_kbps * 1000;
	encoder->codec_ctx->rc_max_rate = encoder->codec_ctx->bit_rate;
	encoder->bitrate_kbps = bitrate_kbps;

	return true;
}

gaze_encoder_type_t gaze_encoder_get_type(const gaze_encoder_t *encoder)
{
	if (!encoder)
		return GAZE_ENCODER_AUTO;
	return encoder->actual_encoder;
}

void gaze_encoder_destroy(gaze_encoder_t *encoder)
{
	if (!encoder)
		return;

	if (encoder->sws_ctx) {
		sws_freeContext(encoder->sws_ctx);
		encoder->sws_ctx = nullptr;
	}

	if (encoder->packet) {
		av_packet_free(&encoder->packet);
	}

	if (encoder->frame) {
		av_frame_free(&encoder->frame);
	}

	if (encoder->codec_ctx) {
		avcodec_free_context(&encoder->codec_ctx);
	}

	if (encoder->nv12_buffer) {
		av_free(encoder->nv12_buffer);
		encoder->nv12_buffer = nullptr;
	}

	encoder->initialized = false;
}

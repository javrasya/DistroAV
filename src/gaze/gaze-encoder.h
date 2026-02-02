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

#pragma once

#include "gaze-protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for FFmpeg types
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/**
 * Gaze Stream Encoder
 *
 * FFmpeg-based hardware encoder with automatic fallback.
 * Supports NVENC, AMF, QSV, and software encoding.
 *
 * Input: BGRA frame data
 * Output: Encoded H.264 or HEVC NAL units
 */
typedef struct gaze_encoder {
	// FFmpeg contexts
	struct AVCodecContext *codec_ctx;
	struct AVFrame *frame;
	struct AVPacket *packet;
	struct SwsContext *sws_ctx;

	// Configuration
	gaze_codec_t codec;
	gaze_encoder_type_t encoder_type;
	gaze_encoder_type_t actual_encoder; // What we actually got after fallback

	uint32_t width;
	uint32_t height;
	uint32_t bitrate_kbps;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t keyframe_interval;

	// NV12 conversion buffer
	uint8_t *nv12_buffer;
	size_t nv12_buffer_size;

	// State
	bool initialized;
	bool keyframe_pending;
	uint64_t frame_count;
} gaze_encoder_t;

/**
 * Initialize the encoder.
 *
 * @param encoder The encoder instance to initialize
 * @param codec The video codec (HEVC or H.264)
 * @param type The preferred encoder type (Auto for best available)
 * @param width Frame width
 * @param height Frame height
 * @param bitrate_kbps Target bitrate in kbps
 * @param fps_num Frame rate numerator
 * @param fps_den Frame rate denominator
 * @param keyframe_interval Keyframe interval in frames (0 for default)
 * @return true on success, false on failure
 */
bool gaze_encoder_init(gaze_encoder_t *encoder, gaze_codec_t codec,
		       gaze_encoder_type_t type, uint32_t width,
		       uint32_t height, uint32_t bitrate_kbps,
		       uint32_t fps_num, uint32_t fps_den,
		       uint32_t keyframe_interval);

/**
 * Encode a BGRA frame.
 *
 * @param encoder The encoder instance
 * @param bgra_data Pointer to BGRA frame data
 * @param linesize Bytes per line (stride)
 * @param encoded_data Output: pointer to encoded data (valid until next call)
 * @param encoded_size Output: size of encoded data
 * @param is_keyframe Output: true if this is a keyframe
 * @return true on success, false on failure
 */
bool gaze_encoder_encode(gaze_encoder_t *encoder, const uint8_t *bgra_data,
			 uint32_t linesize, uint8_t **encoded_data,
			 size_t *encoded_size, bool *is_keyframe);

/**
 * Request that the next frame be encoded as a keyframe.
 *
 * @param encoder The encoder instance
 */
void gaze_encoder_request_keyframe(gaze_encoder_t *encoder);

/**
 * Update encoder bitrate.
 *
 * @param encoder The encoder instance
 * @param bitrate_kbps New target bitrate in kbps
 * @return true on success, false on failure
 */
bool gaze_encoder_set_bitrate(gaze_encoder_t *encoder, uint32_t bitrate_kbps);

/**
 * Get the actual encoder type being used (after fallback).
 *
 * @param encoder The encoder instance
 * @return The actual encoder type
 */
gaze_encoder_type_t gaze_encoder_get_type(const gaze_encoder_t *encoder);

/**
 * Get a human-readable name for the encoder type.
 *
 * @param type The encoder type
 * @return Static string with encoder name
 */
const char *gaze_encoder_type_name(gaze_encoder_type_t type);

/**
 * Get a human-readable name for the codec.
 *
 * @param codec The codec type
 * @return Static string with codec name
 */
const char *gaze_codec_name(gaze_codec_t codec);

/**
 * Destroy the encoder and free resources.
 *
 * @param encoder The encoder instance
 */
void gaze_encoder_destroy(gaze_encoder_t *encoder);

/**
 * Check if a specific encoder type is available on this system.
 *
 * @param codec The codec to check
 * @param type The encoder type to check
 * @return true if available, false otherwise
 */
bool gaze_encoder_is_available(gaze_codec_t codec, gaze_encoder_type_t type);

#ifdef __cplusplus
}
#endif

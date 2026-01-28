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

#include <obs-module.h>
#include <util/platform.h>

/**
 * Video resolution and frame rate converter for NDI streams.
 * Provides shared functionality for both NDI Filter and NDI Output.
 */

// Resolution conversion mode
enum ndi_resolution_mode {
	NDI_RESOLUTION_AUTO = 0,     // Use source resolution
	NDI_RESOLUTION_720P,          // 1280x720
	NDI_RESOLUTION_1080P,         // 1920x1080
	NDI_RESOLUTION_1440P,         // 2560x1440
	NDI_RESOLUTION_4K,            // 3840x2160
	NDI_RESOLUTION_CUSTOM         // User-specified custom resolution
};

// Frame rate mode
enum ndi_framerate_mode {
	NDI_FRAMERATE_AUTO = 0,      // Use source frame rate
	NDI_FRAMERATE_5,              // 5 fps
	NDI_FRAMERATE_10,             // 10 fps
	NDI_FRAMERATE_15,             // 15 fps
	NDI_FRAMERATE_24,             // 24 fps
	NDI_FRAMERATE_25,             // 25 fps
	NDI_FRAMERATE_30,             // 30 fps (30000/1000)
	NDI_FRAMERATE_2997,           // 29.97 fps (30000/1001)
	NDI_FRAMERATE_50,             // 50 fps
	NDI_FRAMERATE_60,             // 60 fps (60000/1000)
	NDI_FRAMERATE_5994,           // 59.94 fps (60000/1001)
	NDI_FRAMERATE_CUSTOM          // User-specified custom frame rate
};

// Scaling algorithm
enum ndi_scale_type {
	NDI_SCALE_FAST_BILINEAR = 0,  // Fastest, lower quality
	NDI_SCALE_BILINEAR,           // Fast, good quality
	NDI_SCALE_BICUBIC              // Balanced (default), best quality
};

// Crop type mode
enum ndi_crop_type {
	NDI_CROP_TYPE_PIXEL = 0,      // Absolute pixel coordinates
	NDI_CROP_TYPE_PERCENTAGE      // Percentage-based (0-100)
};

/**
 * Video converter state structure
 */
typedef struct {
	// Resolution settings
	bool enable_custom_resolution;
	enum ndi_resolution_mode resolution_mode;
	uint32_t custom_width;
	uint32_t custom_height;
	uint32_t target_width;
	uint32_t target_height;
	enum ndi_scale_type scale_type;

	// Crop settings
	bool enable_crop;
	enum ndi_crop_type crop_type;
	int32_t crop_left;
	int32_t crop_top;
	uint32_t crop_width;
	uint32_t crop_height;
	double crop_left_pct;
	double crop_top_pct;
	double crop_width_pct;
	double crop_height_pct;

	// Frame rate settings
	bool enable_custom_framerate;
	enum ndi_framerate_mode framerate_mode;
	uint32_t custom_fps_num;
	uint32_t custom_fps_den;
	uint32_t target_fps_num;
	uint32_t target_fps_den;

	// Frame rate conversion state
	int64_t accumulator_ns;
	int64_t target_frame_interval_ns;
	uint64_t last_frame_timestamp;

	// Source dimensions (for detecting changes)
	uint32_t source_width;
	uint32_t source_height;
	enum video_format source_format;

	// Cached crop values in source space (for GPU rendering - crop before scale)
	int32_t cached_crop_left;
	int32_t cached_crop_top;
	uint32_t cached_crop_width;
	uint32_t cached_crop_height;
	
	// Cached crop values in scaled space (for CPU - only when custom res disabled)
	int32_t cached_crop_scaled_left;
	int32_t cached_crop_scaled_top;
	uint32_t cached_crop_scaled_width;
	uint32_t cached_crop_scaled_height;
	
	bool crop_cache_valid;
} ndi_video_converter_t;

/**
 * Initialize a video converter instance.
 * @param converter Pointer to converter structure to initialize
 */
void ndi_converter_init(ndi_video_converter_t *converter);

/**
 * Update converter settings from OBS data.
 * @param converter The converter instance
 * @param settings OBS settings data
 */
void ndi_converter_update(ndi_video_converter_t *converter, obs_data_t *settings);

/**
 * Update cached crop values based on source and scaled dimensions.
 * Call this when source dimensions change or when first enabling crop.
 * @param converter The converter instance
 * @param source_width Original source width
 * @param source_height Original source height
 * @param scaled_width Width after scaling (if custom res enabled)
 * @param scaled_height Height after scaling (if custom res enabled)
 */
void ndi_converter_update_crop_cache(ndi_video_converter_t *converter, uint32_t source_width, uint32_t source_height,
				     uint32_t scaled_width, uint32_t scaled_height);

/**
 * Determine if a frame should be sent based on frame rate conversion.
 * Uses timestamp-based accumulator for downconversion (dropping frames).
 * Never duplicates frames - output FPS is capped at source FPS.
 * @param converter The converter instance
 * @param frame_timestamp Timestamp of current frame in nanoseconds
 * @param frames_to_send Output: 1 if frame should be sent, 0 if dropped
 * @return true if frame should be sent, false if frame should be dropped
 */
bool ndi_converter_should_send_frame(ndi_video_converter_t *converter, uint64_t frame_timestamp,
				     int *frames_to_send);

/**
 * Peek to check if a frame would be sent, WITHOUT mutating accumulator state.
 * Use this BEFORE GPU rendering to skip frames that would be dropped anyway.
 * @param converter The converter instance
 * @param frame_timestamp Timestamp of current frame in nanoseconds
 * @return true if frame would be sent, false if it would be dropped
 */
bool ndi_converter_should_render_frame_peek(ndi_video_converter_t *converter, uint64_t frame_timestamp);

/**
 * Destroy and free converter resources.
 * @param converter The converter instance
 */
void ndi_converter_destroy(ndi_video_converter_t *converter);

/**
 * Get resolution dimensions for a preset mode.
 * @param mode The resolution mode
 * @param width Output width pointer
 * @param height Output height pointer
 */
void ndi_converter_get_preset_resolution(enum ndi_resolution_mode mode, uint32_t *width, uint32_t *height);

/**
 * Get frame rate numerator/denominator for a preset mode.
 * @param mode The frame rate mode
 * @param fps_num Output FPS numerator pointer
 * @param fps_den Output FPS denominator pointer
 */
void ndi_converter_get_preset_framerate(enum ndi_framerate_mode mode, uint32_t *fps_num, uint32_t *fps_den);

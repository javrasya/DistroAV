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

#include "ndi-video-converter.h"
#include <util/bmem.h>
#include <cstring>

// Property names
#define PROP_ENABLE_CUSTOM_RES "enable_custom_resolution"
#define PROP_RESOLUTION_MODE "resolution_mode"
#define PROP_CUSTOM_WIDTH "custom_width"
#define PROP_CUSTOM_HEIGHT "custom_height"
#define PROP_SCALE_TYPE "scale_type"
#define PROP_ENABLE_CROP "enable_crop"
#define PROP_CROP_LEFT "crop_left"
#define PROP_CROP_TOP "crop_top"
#define PROP_CROP_WIDTH "crop_width"
#define PROP_CROP_HEIGHT "crop_height"
#define PROP_ENABLE_CUSTOM_FPS "enable_custom_framerate"
#define PROP_FRAMERATE_MODE "framerate_mode"
#define PROP_CUSTOM_FPS_NUM "custom_fps_num"
#define PROP_CUSTOM_FPS_DEN "custom_fps_den"

void ndi_converter_init(ndi_video_converter_t *converter)
{
	memset(converter, 0, sizeof(ndi_video_converter_t));

	// Set defaults
	converter->resolution_mode = NDI_RESOLUTION_AUTO;
	converter->scale_type = NDI_SCALE_BICUBIC;
	converter->framerate_mode = NDI_FRAMERATE_AUTO;
	converter->custom_width = 1920;
	converter->custom_height = 1080;
	converter->custom_fps_num = 30;
	converter->custom_fps_den = 1;
}

void ndi_converter_get_preset_resolution(enum ndi_resolution_mode mode, uint32_t *width, uint32_t *height)
{
	switch (mode) {
	case NDI_RESOLUTION_720P:
		*width = 1280;
		*height = 720;
		break;
	case NDI_RESOLUTION_1080P:
		*width = 1920;
		*height = 1080;
		break;
	case NDI_RESOLUTION_1440P:
		*width = 2560;
		*height = 1440;
		break;
	case NDI_RESOLUTION_4K:
		*width = 3840;
		*height = 2160;
		break;
	default:
		*width = 0;
		*height = 0;
		break;
	}
}

void ndi_converter_get_preset_framerate(enum ndi_framerate_mode mode, uint32_t *fps_num, uint32_t *fps_den)
{
	switch (mode) {
	case NDI_FRAMERATE_5:
		*fps_num = 5;
		*fps_den = 1;
		break;
	case NDI_FRAMERATE_10:
		*fps_num = 10;
		*fps_den = 1;
		break;
	case NDI_FRAMERATE_15:
		*fps_num = 15;
		*fps_den = 1;
		break;
	case NDI_FRAMERATE_24:
		*fps_num = 24;
		*fps_den = 1;
		break;
	case NDI_FRAMERATE_25:
		*fps_num = 25;
		*fps_den = 1;
		break;
	case NDI_FRAMERATE_30:
		*fps_num = 30;
		*fps_den = 1;
		break;
	case NDI_FRAMERATE_2997:
		*fps_num = 30000;
		*fps_den = 1001;
		break;
	case NDI_FRAMERATE_50:
		*fps_num = 50;
		*fps_den = 1;
		break;
	case NDI_FRAMERATE_60:
		*fps_num = 60;
		*fps_den = 1;
		break;
	case NDI_FRAMERATE_5994:
		*fps_num = 60000;
		*fps_den = 1001;
		break;
	default:
		*fps_num = 0;
		*fps_den = 0;
		break;
	}
}

void ndi_converter_update(ndi_video_converter_t *converter, obs_data_t *settings)
{
	// Resolution settings
	converter->enable_custom_resolution = obs_data_get_bool(settings, PROP_ENABLE_CUSTOM_RES);
	converter->resolution_mode = (enum ndi_resolution_mode)obs_data_get_int(settings, PROP_RESOLUTION_MODE);
	converter->custom_width = (uint32_t)obs_data_get_int(settings, PROP_CUSTOM_WIDTH);
	converter->custom_height = (uint32_t)obs_data_get_int(settings, PROP_CUSTOM_HEIGHT);
	converter->scale_type = (enum ndi_scale_type)obs_data_get_int(settings, PROP_SCALE_TYPE);

	// Validate custom resolution
	if (converter->custom_width < 128)
		converter->custom_width = 128;
	if (converter->custom_width > 7680)
		converter->custom_width = 7680;
	if (converter->custom_height < 72)
		converter->custom_height = 72;
	if (converter->custom_height > 4320)
		converter->custom_height = 4320;

	// Calculate target resolution
	if (converter->enable_custom_resolution) {
		if (converter->resolution_mode == NDI_RESOLUTION_CUSTOM) {
			converter->target_width = converter->custom_width;
			converter->target_height = converter->custom_height;
		} else {
			ndi_converter_get_preset_resolution(converter->resolution_mode, &converter->target_width,
							     &converter->target_height);
		}
	} else {
		converter->target_width = 0;
		converter->target_height = 0;
	}

	// Crop settings
	converter->enable_crop = obs_data_get_bool(settings, PROP_ENABLE_CROP);
	converter->crop_left = (int32_t)obs_data_get_int(settings, PROP_CROP_LEFT);
	converter->crop_top = (int32_t)obs_data_get_int(settings, PROP_CROP_TOP);
	converter->crop_width = (uint32_t)obs_data_get_int(settings, PROP_CROP_WIDTH);
	converter->crop_height = (uint32_t)obs_data_get_int(settings, PROP_CROP_HEIGHT);

	// Validate crop values (0 means use full dimensions, validated in render)
	if (converter->crop_left < 0)
		converter->crop_left = 0;
	if (converter->crop_top < 0)
		converter->crop_top = 0;
	// Allow 0 for width/height (means use full dimensions)

	// Frame rate settings
	converter->enable_custom_framerate = obs_data_get_bool(settings, PROP_ENABLE_CUSTOM_FPS);
	converter->framerate_mode = (enum ndi_framerate_mode)obs_data_get_int(settings, PROP_FRAMERATE_MODE);
	converter->custom_fps_num = (uint32_t)obs_data_get_int(settings, PROP_CUSTOM_FPS_NUM);
	converter->custom_fps_den = (uint32_t)obs_data_get_int(settings, PROP_CUSTOM_FPS_DEN);

	// Validate custom frame rate
	if (converter->custom_fps_num < 1)
		converter->custom_fps_num = 1;
	if (converter->custom_fps_den < 1)
		converter->custom_fps_den = 1;

	// Calculate target frame rate
	if (converter->enable_custom_framerate) {
		if (converter->framerate_mode == NDI_FRAMERATE_CUSTOM) {
			converter->target_fps_num = converter->custom_fps_num;
			converter->target_fps_den = converter->custom_fps_den;
		} else {
			ndi_converter_get_preset_framerate(converter->framerate_mode, &converter->target_fps_num,
							    &converter->target_fps_den);
		}

		// Calculate target frame interval in nanoseconds
		if (converter->target_fps_num > 0 && converter->target_fps_den > 0) {
			converter->target_frame_interval_ns =
				(int64_t)((1000000000.0 * converter->target_fps_den) / converter->target_fps_num);
		} else {
			converter->target_frame_interval_ns = 0;
		}

		// Reset accumulator when settings change
		converter->accumulator_ns = 0;
		converter->last_frame_timestamp = 0;
	} else {
		converter->target_fps_num = 0;
		converter->target_fps_den = 0;
		converter->target_frame_interval_ns = 0;
	}
}

bool ndi_converter_update_scaler(ndi_video_converter_t *converter, uint32_t source_width, uint32_t source_height,
				  enum video_format source_format)
{
	if (!converter->enable_custom_resolution || converter->target_width == 0 || converter->target_height == 0) {
		blog(LOG_DEBUG, "[ndi-converter] Scaling disabled or no target dimensions");
		return false;
	}

	// Check if we need to recreate the scaler
	bool need_recreate =
		!converter->scaler || converter->source_width != source_width ||
		converter->source_height != source_height || converter->source_format != source_format;

	if (!need_recreate) {
		blog(LOG_DEBUG, "[ndi-converter] Scaler already exists, reusing");
		return true;
	}

	blog(LOG_INFO, "[ndi-converter] Creating scaler: %dx%d -> %dx%d", source_width, source_height,
	     converter->target_width, converter->target_height);

	// Destroy old scaler
	if (converter->scaler) {
		video_scaler_destroy(converter->scaler);
		converter->scaler = nullptr;
	}

	// Create new scaler
	struct video_scale_info src_info = {};
	src_info.format = source_format;
	src_info.width = source_width;
	src_info.height = source_height;
	src_info.range = VIDEO_RANGE_DEFAULT;
	src_info.colorspace = VIDEO_CS_DEFAULT;

	struct video_scale_info dst_info = {};
	dst_info.format = VIDEO_FORMAT_BGRA; // NDI Filter uses BGRA
	dst_info.width = converter->target_width;
	dst_info.height = converter->target_height;
	dst_info.range = VIDEO_RANGE_DEFAULT;
	dst_info.colorspace = VIDEO_CS_DEFAULT;

	// Map our scale type to OBS scale type
	enum video_scale_type obs_scale_type;
	switch (converter->scale_type) {
	case NDI_SCALE_FAST_BILINEAR:
		obs_scale_type = VIDEO_SCALE_FAST_BILINEAR;
		break;
	case NDI_SCALE_BILINEAR:
		obs_scale_type = VIDEO_SCALE_BILINEAR;
		break;
	case NDI_SCALE_BICUBIC:
		obs_scale_type = VIDEO_SCALE_BICUBIC;
		break;
	default:
		obs_scale_type = VIDEO_SCALE_BICUBIC;
		break;
	}

	blog(LOG_DEBUG, "[ndi-converter] Creating video_scaler...");
	int result = video_scaler_create(&converter->scaler, &dst_info, &src_info, obs_scale_type);
	if (result != VIDEO_SCALER_SUCCESS) {
		blog(LOG_ERROR, "[ndi-converter] Failed to create video scaler: %d", result);
		converter->scaler = nullptr;
		return false;
	}
	blog(LOG_INFO, "[ndi-converter] Scaler created successfully");

	// Update source dimensions
	converter->source_width = source_width;
	converter->source_height = source_height;
	converter->source_format = source_format;

	// Allocate scaled buffer
	size_t required_size = converter->target_width * converter->target_height * 4; // BGRA = 4 bytes per pixel
	if (converter->scaled_buffer_size < required_size) {
		blog(LOG_DEBUG, "[ndi-converter] Allocating scaled buffer: %zu bytes", required_size);
		if (converter->scaled_buffer) {
			bfree(converter->scaled_buffer);
		}
		converter->scaled_buffer = (uint8_t *)bmalloc(required_size);
		converter->scaled_buffer_size = required_size;
	}

	blog(LOG_INFO, "[ndi-converter] Scaler setup complete");
	return true;
}

bool ndi_converter_scale_video(ndi_video_converter_t *converter, uint8_t *frame_in[], uint32_t linesize_in[],
			       uint32_t source_width, uint32_t source_height, enum video_format source_format,
			       uint8_t **frame_out, uint32_t *linesize_out)
{
	blog(LOG_DEBUG, "[ndi-converter] scale_video called: %dx%d", source_width, source_height);

	if (!ndi_converter_update_scaler(converter, source_width, source_height, source_format)) {
		blog(LOG_DEBUG, "[ndi-converter] update_scaler returned false");
		return false;
	}

	if (!converter->scaler || !converter->scaled_buffer) {
		blog(LOG_ERROR, "[ndi-converter] No scaler or buffer after update!");
		return false;
	}

	// Prepare output arrays for scaler
	uint8_t *output_planes[1] = {converter->scaled_buffer};
	uint32_t output_linesize[1] = {converter->target_width * 4}; // BGRA = 4 bytes per pixel

	blog(LOG_DEBUG, "[ndi-converter] Calling video_scaler_scale...");
	// Scale the video
	bool success = video_scaler_scale(converter->scaler, output_planes, output_linesize,
					  (const uint8_t *const *)frame_in, linesize_in);

	blog(LOG_DEBUG, "[ndi-converter] video_scaler_scale returned: %d", success);

	if (success) {
		*frame_out = converter->scaled_buffer;
		*linesize_out = converter->target_width * 4; // BGRA linesize
		blog(LOG_DEBUG, "[ndi-converter] Scaling successful");
		return true;
	}

	blog(LOG_WARNING, "[ndi-converter] Scaling failed");
	return false;
}

bool ndi_converter_should_send_frame(ndi_video_converter_t *converter, uint64_t frame_timestamp, int *frames_to_send)
{
	*frames_to_send = 0;

	if (!converter->enable_custom_framerate || converter->target_frame_interval_ns == 0)
		return true; // No FPS conversion, send all frames

	// Calculate elapsed time since last frame
	int64_t delta_ns = 0;
	if (converter->last_frame_timestamp > 0) {
		delta_ns = (int64_t)(frame_timestamp - converter->last_frame_timestamp);
	}

	// Add to accumulator
	converter->accumulator_ns += delta_ns;
	converter->last_frame_timestamp = frame_timestamp;

	// Determine how many frames to send
	int count = 0;
	while (converter->accumulator_ns >= converter->target_frame_interval_ns) {
		count++;
		converter->accumulator_ns -= converter->target_frame_interval_ns;
	}

	*frames_to_send = count;
	return count > 0;
}

void ndi_converter_destroy(ndi_video_converter_t *converter)
{
	if (converter->scaler) {
		video_scaler_destroy(converter->scaler);
		converter->scaler = nullptr;
	}

	if (converter->scaled_buffer) {
		bfree(converter->scaled_buffer);
		converter->scaled_buffer = nullptr;
		converter->scaled_buffer_size = 0;
	}

	memset(converter, 0, sizeof(ndi_video_converter_t));
}

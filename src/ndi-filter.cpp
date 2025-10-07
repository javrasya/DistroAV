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

#include "plugin-main.h"
#include "ndi-video-converter.h"

#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-frame.h>

#include <QDesktopServices>
#include <QUrl>

#define TEXFORMAT GS_BGRA
#define FLT_PROP_NAME "ndi_filter_ndiname"
#define FLT_PROP_GROUPS "ndi_filter_ndigroups"

typedef struct {
	obs_source_t *obs_source;

	NDIlib_send_instance_t ndi_sender;

	pthread_mutex_t ndi_sender_video_mutex;
	pthread_mutex_t ndi_sender_audio_mutex;

	obs_video_info ovi;
	obs_audio_info oai;

	uint32_t known_width;
	uint32_t known_height;

	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;
	uint8_t *video_data;
	uint32_t video_linesize;

	video_t *video_output;
	bool is_audioonly;

	uint8_t *audio_conv_buffer;
	size_t audio_conv_buffer_size;

	// Video converter for custom resolution/FPS
	ndi_video_converter_t converter;
} ndi_filter_t;

const char *ndi_filter_getname(void *)
{
	return obs_module_text("NDIPlugin.FilterName");
}

const char *ndi_audiofilter_getname(void *)
{
	return obs_module_text("NDIPlugin.AudioFilterName");
}

void ndi_filter_update(void *data, obs_data_t *settings);
void ndi_sender_destroy(ndi_filter_t *filter);
void ndi_sender_create(ndi_filter_t *filter, obs_data_t *settings);

obs_properties_t *ndi_filter_getproperties(void *)
{
	obs_log(LOG_DEBUG, "+ndi_filter_getproperties(...)");
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, FLT_PROP_NAME, obs_module_text("NDIPlugin.FilterProps.NDIName"),
				OBS_TEXT_DEFAULT);

	obs_properties_add_text(props, FLT_PROP_GROUPS, obs_module_text("NDIPlugin.FilterProps.NDIGroups"),
				OBS_TEXT_DEFAULT);

	// Custom Resolution Settings
	auto group_res = obs_properties_create();
	obs_properties_add_bool(group_res, "enable_custom_resolution", "Enable Custom Resolution");

	obs_property_t *res_mode = obs_properties_add_list(group_res, "resolution_mode", "Resolution Preset",
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(res_mode, "1280x720 (720p)", NDI_RESOLUTION_720P);
	obs_property_list_add_int(res_mode, "1920x1080 (1080p)", NDI_RESOLUTION_1080P);
	obs_property_list_add_int(res_mode, "2560x1440 (1440p)", NDI_RESOLUTION_1440P);
	obs_property_list_add_int(res_mode, "3840x2160 (4K)", NDI_RESOLUTION_4K);
	obs_property_list_add_int(res_mode, "Custom", NDI_RESOLUTION_CUSTOM);

	obs_properties_add_int(group_res, "custom_width", "Custom Width", 128, 7680, 1);
	obs_properties_add_int(group_res, "custom_height", "Custom Height", 72, 4320, 1);

	obs_property_t *scale_type = obs_properties_add_list(group_res, "scale_type", "Scaling Algorithm",
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(scale_type, "Fast Bilinear (Fastest)", NDI_SCALE_FAST_BILINEAR);
	obs_property_list_add_int(scale_type, "Bilinear (Good)", NDI_SCALE_BILINEAR);
	obs_property_list_add_int(scale_type, "Bicubic (Best)", NDI_SCALE_BICUBIC);

	obs_properties_add_group(props, "group_resolution", "Resolution Conversion", OBS_GROUP_NORMAL, group_res);

	// Crop Settings (applied AFTER scaling)
	auto group_crop = obs_properties_create();
	obs_properties_add_bool(group_crop, "enable_crop", "Enable Crop");
	obs_properties_add_text(
		group_crop, "crop_info",
		"Coordinates in source resolution space (auto-scaled if custom resolution enabled). 0 = full dimension",
		OBS_TEXT_INFO);
	obs_properties_add_int(group_crop, "crop_left", "Left (source coords)", 0, 7680, 1);
	obs_properties_add_int(group_crop, "crop_top", "Top (source coords)", 0, 4320, 1);
	obs_properties_add_int(group_crop, "crop_width", "Width (0 = full)", 0, 7680, 1);
	obs_properties_add_int(group_crop, "crop_height", "Height (0 = full)", 0, 4320, 1);

	obs_properties_add_group(props, "group_crop", "Crop Region", OBS_GROUP_NORMAL, group_crop);

	// Custom Frame Rate Settings
	auto group_fps = obs_properties_create();
	obs_properties_add_bool(group_fps, "enable_custom_framerate", "Enable Custom Frame Rate");

	obs_property_t *fps_mode = obs_properties_add_list(group_fps, "framerate_mode", "Frame Rate Preset",
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps_mode, "5 fps", NDI_FRAMERATE_5);
	obs_property_list_add_int(fps_mode, "10 fps", NDI_FRAMERATE_10);
	obs_property_list_add_int(fps_mode, "15 fps", NDI_FRAMERATE_15);
	obs_property_list_add_int(fps_mode, "24 fps", NDI_FRAMERATE_24);
	obs_property_list_add_int(fps_mode, "25 fps", NDI_FRAMERATE_25);
	obs_property_list_add_int(fps_mode, "29.97 fps (NTSC)", NDI_FRAMERATE_2997);
	obs_property_list_add_int(fps_mode, "30 fps", NDI_FRAMERATE_30);
	obs_property_list_add_int(fps_mode, "50 fps", NDI_FRAMERATE_50);
	obs_property_list_add_int(fps_mode, "59.94 fps (NTSC)", NDI_FRAMERATE_5994);
	obs_property_list_add_int(fps_mode, "60 fps", NDI_FRAMERATE_60);
	obs_property_list_add_int(fps_mode, "Custom", NDI_FRAMERATE_CUSTOM);

	obs_properties_add_int(group_fps, "custom_fps_num", "Custom FPS Numerator", 1, 240, 1);
	obs_properties_add_int(group_fps, "custom_fps_den", "Custom FPS Denominator", 1, 1001, 1);

	obs_properties_add_group(props, "group_framerate", "Frame Rate Conversion", OBS_GROUP_NORMAL, group_fps);

	obs_properties_add_button(props, "ndi_apply", obs_module_text("NDIPlugin.FilterProps.ApplySettings"),
				  [](obs_properties_t *, obs_property_t *, void *private_data) {
					  auto s = (ndi_filter_t *)private_data;
					  auto settings = obs_source_get_settings(s->obs_source);
					  ndi_filter_update(s, settings);
					  obs_data_release(settings);
					  return true;
				  });

	auto group_ndi = obs_properties_create();
	obs_properties_add_button(group_ndi, "ndi_website", NDI_OFFICIAL_WEB_URL,
				  [](obs_properties_t *, obs_property_t *, void *) {
					  QDesktopServices::openUrl(QUrl(rehostUrl(PLUGIN_REDIRECT_NDI_WEB_URL)));
					  return false;
				  });
	obs_properties_add_group(props, "ndi", "NDI®", OBS_GROUP_NORMAL, group_ndi);

	obs_log(LOG_DEBUG, "-ndi_filter_getproperties(...)");
	return props;
}

void ndi_filter_getdefaults(obs_data_t *defaults)
{
	obs_log(LOG_DEBUG, "+ndi_filter_getdefaults(...)");
	obs_data_set_default_string(defaults, FLT_PROP_NAME, obs_module_text("NDIPlugin.FilterProps.NDIName.Default"));
	obs_data_set_default_string(defaults, FLT_PROP_GROUPS, "");

	// Resolution defaults
	obs_data_set_default_bool(defaults, "enable_custom_resolution", false);
	obs_data_set_default_int(defaults, "resolution_mode", NDI_RESOLUTION_1080P);
	obs_data_set_default_int(defaults, "custom_width", 1920);
	obs_data_set_default_int(defaults, "custom_height", 1080);
	obs_data_set_default_int(defaults, "scale_type", NDI_SCALE_BICUBIC);

	// Crop defaults (applied AFTER scaling)
	obs_data_set_default_bool(defaults, "enable_crop", false);
	obs_data_set_default_int(defaults, "crop_left", 0);
	obs_data_set_default_int(defaults, "crop_top", 0);
	obs_data_set_default_int(defaults, "crop_width", 0);
	obs_data_set_default_int(defaults, "crop_height", 0);

	// Frame rate defaults
	obs_data_set_default_bool(defaults, "enable_custom_framerate", false);
	obs_data_set_default_int(defaults, "framerate_mode", NDI_FRAMERATE_30);
	obs_data_set_default_int(defaults, "custom_fps_num", 30);
	obs_data_set_default_int(defaults, "custom_fps_den", 1);

	obs_log(LOG_DEBUG, "-ndi_filter_getdefaults(...)");
}

bool is_filter_valid(ndi_filter_t *filter)
{
	obs_source_t *target = obs_filter_get_target(filter->obs_source);
	obs_source_t *parent = obs_filter_get_parent(filter->obs_source);
	if (!target || !parent) {
		return false;
	}

	uint32_t width = obs_source_get_width(filter->obs_source);
	uint32_t height = obs_source_get_height(filter->obs_source);

	// Valid if parent width/height are nonzero, source is enabled, and parent is active
	bool is_valid = (width != 0) && (height != 0) && obs_source_enabled(filter->obs_source) &&
			obs_source_active(parent);

	return is_valid;
}

void ndi_filter_raw_video(void *data, video_data *frame)
{
	auto f = (ndi_filter_t *)data;

	// Check frame rate limiting
	int frames_to_send = 1;
	if (f->converter.enable_custom_framerate && frame) {
		bool should_send = ndi_converter_should_send_frame(&f->converter, frame->timestamp, &frames_to_send);
		if (!should_send || frames_to_send == 0) {
			return; // Skip this frame
		}
	}

	// Determine frame rate metadata
	uint32_t ndi_fps_num = f->ovi.fps_num;
	uint32_t ndi_fps_den = f->ovi.fps_den;
	if (f->converter.enable_custom_framerate && f->converter.target_fps_num > 0 &&
	    f->converter.target_fps_den > 0) {
		ndi_fps_num = f->converter.target_fps_num;
		ndi_fps_den = f->converter.target_fps_den;
	}

	// Apply crop (AFTER scaling) if enabled
	uint32_t final_width = f->known_width;
	uint32_t final_height = f->known_height;
	uint8_t *final_data = frame->data[0];
	uint32_t final_linesize = frame->linesize[0];

	if (f->converter.enable_crop && frame && frame->data[0]) {
		// Get crop coordinates (assumed to be in source resolution space)
		int32_t crop_left = f->converter.crop_left;
		int32_t crop_top = f->converter.crop_top;
		uint32_t crop_width = f->converter.crop_width;
		uint32_t crop_height = f->converter.crop_height;

		// If custom resolution is enabled, normalize crop coordinates from source to scaled space
		if (f->converter.enable_custom_resolution && f->converter.target_width > 0 &&
		    f->converter.target_height > 0) {
			// Get source dimensions
			uint32_t source_width = obs_source_get_width(f->obs_source);
			uint32_t source_height = obs_source_get_height(f->obs_source);

			if (source_width > 0 && source_height > 0) {
				// Calculate scaling ratios
				float scale_x = (float)f->known_width / (float)source_width;
				float scale_y = (float)f->known_height / (float)source_height;

				// Scale crop coordinates proportionally
				crop_left = (int32_t)((float)crop_left * scale_x);
				crop_top = (int32_t)((float)crop_top * scale_y);
				crop_width = (uint32_t)((float)crop_width * scale_x);
				crop_height = (uint32_t)((float)crop_height * scale_y);

				obs_log(LOG_DEBUG,
					"[distroav] Crop normalized from source %ux%u to scaled %ux%u: (%d,%d,%u,%u) "
					"-> (%d,%d,%u,%u)",
					source_width, source_height, f->known_width, f->known_height,
					f->converter.crop_left, f->converter.crop_top, f->converter.crop_width,
					f->converter.crop_height, crop_left, crop_top, crop_width, crop_height);
			}
		}

		// 0 means use full dimension
		if (crop_width == 0)
			crop_width = f->known_width;
		if (crop_height == 0)
			crop_height = f->known_height;

		// Clamp to valid range
		if (crop_left < 0)
			crop_left = 0;
		if (crop_top < 0)
			crop_top = 0;
		// Don't reset to 0 - clamp to max valid position
		if ((uint32_t)crop_left >= f->known_width)
			crop_left = f->known_width - 1;
		if ((uint32_t)crop_top >= f->known_height)
			crop_top = f->known_height - 1;
		// Clamp dimensions to fit within frame
		if (crop_left + crop_width > f->known_width)
			crop_width = f->known_width - crop_left;
		if (crop_top + crop_height > f->known_height)
			crop_height = f->known_height - crop_top;

		obs_log(LOG_DEBUG, "[distroav] Crop applied: left=%d, top=%d, width=%u, height=%u", crop_left, crop_top,
			crop_width, crop_height);

		if (crop_width > 0 && crop_height > 0 && (crop_left > 0 || crop_top > 0 ||
							  crop_width < f->known_width ||
							  crop_height < f->known_height)) {
			// Offset pointer to crop region (BGRA = 4 bytes per pixel)
			final_data = frame->data[0] + (crop_top * final_linesize) + (crop_left * 4);
			final_width = crop_width;
			final_height = crop_height;
			// linesize stays the same (full row stride)
		}
	}

	// Send frame(s) - dimensions may be cropped from scaled frame
	for (int i = 0; i < frames_to_send; i++) {
		NDIlib_video_frame_v2_t video_frame = {0};

		if (frame && frame->data[0]) {
			video_frame.xres = final_width;
			video_frame.yres = final_height;
			video_frame.FourCC = NDIlib_FourCC_type_BGRA;
			video_frame.frame_rate_N = ndi_fps_num;
			video_frame.frame_rate_D = ndi_fps_den;
			video_frame.picture_aspect_ratio = 0;
			video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
			video_frame.timecode = NDIlib_send_timecode_synthesize;
			video_frame.p_data = final_data;
			video_frame.line_stride_in_bytes = final_linesize;
		}

		pthread_mutex_lock(&f->ndi_sender_video_mutex);
		ndiLib->send_send_video_v2(f->ndi_sender, &video_frame);
		pthread_mutex_unlock(&f->ndi_sender_video_mutex);
	}
}

void ndi_filter_render_video(void *data, gs_effect_t *)
{
	auto f = (ndi_filter_t *)data;
	obs_source_skip_video_filter(f->obs_source);

	obs_source_t *target = obs_filter_get_target(f->obs_source);
	obs_source_t *parent = obs_filter_get_parent(f->obs_source);

	if (!target || !parent) {
		return;
	}

	if (!is_filter_valid(f)) {
		// Send empty frame to indicate invalid filter
		NDIlib_video_frame_v2_t video_frame = {0};
		pthread_mutex_lock(&f->ndi_sender_video_mutex);
		ndiLib->send_send_video_v2(f->ndi_sender, &video_frame);
		pthread_mutex_unlock(&f->ndi_sender_video_mutex);
		return;
	}

	uint32_t width = obs_source_get_width(f->obs_source);
	uint32_t height = obs_source_get_height(f->obs_source);

	// Determine render dimensions (use custom if enabled and different)
	uint32_t render_width = width;
	uint32_t render_height = height;
	if (f->converter.enable_custom_resolution && f->converter.target_width > 0 && f->converter.target_height > 0) {
		render_width = f->converter.target_width;
		render_height = f->converter.target_height;
	}

	if (f->known_width != render_width || f->known_height != render_height) {
		gs_stagesurface_destroy(f->stagesurface);
		f->stagesurface = gs_stagesurface_create(render_width, render_height, TEXFORMAT);

		video_output_info vi = {0};
		vi.format = VIDEO_FORMAT_BGRA;
		vi.width = render_width;
		vi.height = render_height;
		vi.fps_den = f->ovi.fps_den;
		vi.fps_num = f->ovi.fps_num;
		vi.cache_size = 16;
		vi.colorspace = VIDEO_CS_DEFAULT;
		vi.range = VIDEO_RANGE_DEFAULT;
		vi.name = obs_source_get_name(f->obs_source);

		video_output_close(f->video_output);
		video_output_open(&f->video_output, &vi);
		video_output_connect(f->video_output, nullptr, ndi_filter_raw_video, f);

		f->known_width = render_width;
		f->known_height = render_height;
	}

	gs_texrender_reset(f->texrender);

	// Render at target resolution (GPU scaling happens here - this is the key!)
	if (gs_texrender_begin(f->texrender, render_width, render_height)) {
		vec4 background;
		vec4_zero(&background);

		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		// Ortho uses SOURCE dimensions - source fills render target, causing automatic scaling
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		if (target == parent) {
			obs_source_skip_video_filter(f->obs_source);
		} else {
			obs_source_video_render(target);
		}

		gs_blend_state_pop();
		gs_texrender_end(f->texrender);

		gs_stage_texture(f->stagesurface, gs_texrender_get_texture(f->texrender));
		if (gs_stagesurface_map(f->stagesurface, &f->video_data, &f->video_linesize)) {
			video_frame output_frame;
			if (video_output_lock_frame(f->video_output, &output_frame, 1, os_gettime_ns())) {
				uint32_t linesize = output_frame.linesize[0];
				for (uint32_t i = 0; i < render_height; ++i) {
					uint32_t dst_offset = linesize * i;
					uint32_t src_offset = f->video_linesize * i;
					memcpy(output_frame.data[0] + dst_offset, f->video_data + src_offset, linesize);
				}

				video_output_unlock_frame(f->video_output);
			}

			gs_stagesurface_unmap(f->stagesurface);
		}
	}
}

void ndi_sender_destroy(ndi_filter_t *filter)
{
	if (!filter || !filter->ndi_sender) {
		return;
	}

	if (!filter->is_audioonly) {
		pthread_mutex_lock(&filter->ndi_sender_video_mutex);
	}

	pthread_mutex_lock(&filter->ndi_sender_audio_mutex);
	ndiLib->send_destroy(filter->ndi_sender);
	filter->ndi_sender = nullptr;
	pthread_mutex_unlock(&filter->ndi_sender_audio_mutex);

	if (!filter->is_audioonly) {
		pthread_mutex_unlock(&filter->ndi_sender_video_mutex);
	}
}

void ndi_sender_create(ndi_filter_t *filter, obs_data_t *settings)
{
	if (!filter || !filter->obs_source) {
		return;
	}

	auto obs_source = filter->obs_source;
	if (!settings) {
		settings = obs_source_get_settings(obs_source);
	}

	NDIlib_send_create_t send_desc;
	send_desc.p_ndi_name = obs_data_get_string(settings, FLT_PROP_NAME);
	auto groups = obs_data_get_string(settings, FLT_PROP_GROUPS);
	if (groups && groups[0])
		send_desc.p_groups = groups;
	else
		send_desc.p_groups = nullptr;
	send_desc.clock_video = false;
	send_desc.clock_audio = false;

	if (!filter->is_audioonly) {
		pthread_mutex_lock(&filter->ndi_sender_video_mutex);
	}

	pthread_mutex_lock(&filter->ndi_sender_audio_mutex);
	ndiLib->send_destroy(filter->ndi_sender);
	filter->ndi_sender = ndiLib->send_create(&send_desc);
	pthread_mutex_unlock(&filter->ndi_sender_audio_mutex);

	if (!filter->is_audioonly) {
		pthread_mutex_unlock(&filter->ndi_sender_video_mutex);
	}
}

void ndi_filter_update(void *data, obs_data_t *settings)
{
	auto f = (ndi_filter_t *)data;
	auto obs_source = f->obs_source;
	auto name = obs_source_get_name(obs_source);
	obs_log(LOG_DEBUG, "+ndi_filter_update(name='%s')", name);

	ndi_sender_create(f, settings);

	// Update video converter settings
	ndi_converter_update(&f->converter, settings);

	auto groups = obs_data_get_string(settings, FLT_PROP_GROUPS);

	obs_log(LOG_INFO, "NDI Filter Updated: '%s'", name);
	obs_log(LOG_DEBUG, "-ndi_filter_update(name='%s', groups='%s')", name, groups);
}

void *ndi_filter_create(obs_data_t *settings, obs_source_t *obs_source)
{
	auto name = obs_data_get_string(settings, FLT_PROP_NAME);
	auto groups = obs_data_get_string(settings, FLT_PROP_GROUPS);
	obs_log(LOG_DEBUG, "+ndi_filter_create(name='%s', groups='%s')", name, groups);

	auto f = (ndi_filter_t *)bzalloc(sizeof(ndi_filter_t));
	f->obs_source = obs_source;
	f->texrender = gs_texrender_create(TEXFORMAT, GS_ZS_NONE);
	pthread_mutex_init(&f->ndi_sender_video_mutex, NULL);
	pthread_mutex_init(&f->ndi_sender_audio_mutex, NULL);
	obs_get_video_info(&f->ovi);
	obs_get_audio_info(&f->oai);

	// Initialize video converter
	ndi_converter_init(&f->converter);

	ndi_filter_update(f, settings);

	obs_log(LOG_INFO, "NDI Filter Created: '%s'", name);
	obs_log(LOG_DEBUG, "-ndi_filter_create(...)");

	return f;
}

void *ndi_filter_create_audioonly(obs_data_t *settings, obs_source_t *obs_source)
{
	auto name = obs_data_get_string(settings, FLT_PROP_NAME);
	auto groups = obs_data_get_string(settings, FLT_PROP_GROUPS);
	obs_log(LOG_DEBUG, "+ndi_filter_create_audioonly(name='%s', groups='%s')", name, groups);

	auto f = (ndi_filter_t *)bzalloc(sizeof(ndi_filter_t));
	f->is_audioonly = true;
	f->obs_source = obs_source;
	pthread_mutex_init(&f->ndi_sender_audio_mutex, NULL);
	obs_get_audio_info(&f->oai);

	ndi_filter_update(f, settings);

	obs_log(LOG_INFO, "NDI Audio-Only Filter Created: '%s'", name);
	obs_log(LOG_DEBUG, "-ndi_filter_create_audioonly(...)");

	return f;
}

void ndi_filter_destroy(void *data)
{
	auto f = (ndi_filter_t *)data;
	auto name = obs_source_get_name(f->obs_source);
	obs_log(LOG_DEBUG, "+ndi_filter_destroy('%s'...)", name);

	video_output_close(f->video_output);

	pthread_mutex_lock(&f->ndi_sender_video_mutex);
	pthread_mutex_lock(&f->ndi_sender_audio_mutex);
	ndiLib->send_destroy(f->ndi_sender);
	pthread_mutex_unlock(&f->ndi_sender_audio_mutex);
	pthread_mutex_unlock(&f->ndi_sender_video_mutex);

	gs_stagesurface_unmap(f->stagesurface);
	gs_stagesurface_destroy(f->stagesurface);
	gs_texrender_destroy(f->texrender);

	if (f->audio_conv_buffer) {
		obs_log(LOG_DEBUG, "ndi_filter_destroy: freeing %zu bytes", f->audio_conv_buffer_size);
		bfree(f->audio_conv_buffer);
		f->audio_conv_buffer = nullptr;
	}

	// Destroy video converter (minimal - only used for FPS conversion state)
	ndi_converter_destroy(&f->converter);

	bfree(f);

	obs_log(LOG_INFO, "NDI Filter Destroyed: '%s'", name);
	obs_log(LOG_DEBUG, "-ndi_filter_destroy('%s'...)", name);
}

void ndi_filter_destroy_audioonly(void *data)
{
	auto f = (ndi_filter_t *)data;
	auto name = obs_source_get_name(f->obs_source);
	obs_log(LOG_DEBUG, "+ndi_filter_destroy_audioonly('%s'...)", name);

	pthread_mutex_lock(&f->ndi_sender_audio_mutex);
	ndiLib->send_destroy(f->ndi_sender);
	pthread_mutex_unlock(&f->ndi_sender_audio_mutex);

	if (f->audio_conv_buffer) {
		bfree(f->audio_conv_buffer);
		f->audio_conv_buffer = nullptr;
	}

	bfree(f);

	obs_log(LOG_INFO, "NDI Audio-Only Filter Destroyed: '%s'", name);
	obs_log(LOG_DEBUG, "-ndi_filter_destroy_audioonly('%s'...)", name);
}

void ndi_filter_tick(void *data, float)
{
	auto f = (ndi_filter_t *)data;
	obs_get_video_info(&f->ovi);

	if (!is_filter_valid(f)) {
		return;
	} else if (!f->ndi_sender) {
		// If the sender is null then recreate it
		ndi_sender_create(f, nullptr);
	}
}

obs_audio_data *ndi_filter_asyncaudio(void *data, obs_audio_data *audio_data)
{
	// NOTE: The logic in this function should be similar to
	// ndi-output.cpp/ndi_output_raw_audio(...)
	auto f = (ndi_filter_t *)data;

	obs_get_audio_info(&f->oai);

	NDIlib_audio_frame_v3_t audio_frame = {0};
	audio_frame.sample_rate = f->oai.samples_per_sec;
	audio_frame.no_channels = f->oai.speakers;
	audio_frame.timecode = NDIlib_send_timecode_synthesize;
	audio_frame.no_samples = audio_data->frames;
	audio_frame.channel_stride_in_bytes =
		audio_frame.no_samples *
		4; // TODO: Check if this correct or should 4 be replaced by number of channels.
	// audio_frame.FourCC = NDIlib_FourCC_audio_type_FLTP;
	// audio_frame.p_data = p_frame;
	audio_frame.p_metadata = NULL; // No metadata support yet!

	const size_t data_size = audio_frame.no_channels * audio_frame.channel_stride_in_bytes;

	if (data_size > f->audio_conv_buffer_size) {
		obs_log(LOG_DEBUG, "ndi_filter_asyncaudio: growing audio_conv_buffer from %zu to %zu bytes",
			f->audio_conv_buffer_size, data_size);
		if (f->audio_conv_buffer) {
			obs_log(LOG_DEBUG, "ndi_filter_asyncaudio: freeing %zu bytes", f->audio_conv_buffer_size);
			bfree(f->audio_conv_buffer);
		}
		obs_log(LOG_DEBUG, "ndi_filter_asyncaudio: allocating %zu bytes", data_size);
		f->audio_conv_buffer = (uint8_t *)bmalloc(data_size);
		f->audio_conv_buffer_size = data_size;
	}

	for (int i = 0; i < audio_frame.no_channels; ++i) {
		memcpy(f->audio_conv_buffer + (i * audio_frame.channel_stride_in_bytes), audio_data->data[i],
		       audio_frame.channel_stride_in_bytes);
	}

	audio_frame.p_data = f->audio_conv_buffer;

	pthread_mutex_lock(&f->ndi_sender_audio_mutex);
	ndiLib->send_send_audio_v3(f->ndi_sender, &audio_frame);
	pthread_mutex_unlock(&f->ndi_sender_audio_mutex);

	return audio_data;
}

obs_source_info create_ndi_filter_info()
{
	obs_source_info ndi_filter_info = {};
	ndi_filter_info.id = "ndi_filter";
	ndi_filter_info.type = OBS_SOURCE_TYPE_FILTER;
	ndi_filter_info.output_flags = OBS_SOURCE_VIDEO;

	ndi_filter_info.get_name = ndi_filter_getname;
	ndi_filter_info.get_properties = ndi_filter_getproperties;
	ndi_filter_info.get_defaults = ndi_filter_getdefaults;

	ndi_filter_info.create = ndi_filter_create;
	ndi_filter_info.destroy = ndi_filter_destroy;
	ndi_filter_info.update = ndi_filter_update;

	ndi_filter_info.video_tick = ndi_filter_tick;
	ndi_filter_info.video_render = ndi_filter_render_video;

	// Audio is available only with async sources
	ndi_filter_info.filter_audio = ndi_filter_asyncaudio;

	return ndi_filter_info;
}

obs_source_info create_ndi_audiofilter_info()
{
	obs_source_info ndi_filter_info = {};
	ndi_filter_info.id = "ndi_audiofilter";
	ndi_filter_info.type = OBS_SOURCE_TYPE_FILTER;
	ndi_filter_info.output_flags = OBS_SOURCE_AUDIO;

	ndi_filter_info.get_name = ndi_audiofilter_getname;
	ndi_filter_info.get_properties = ndi_filter_getproperties;
	ndi_filter_info.get_defaults = ndi_filter_getdefaults;

	ndi_filter_info.create = ndi_filter_create_audioonly;
	ndi_filter_info.update = ndi_filter_update;
	ndi_filter_info.destroy = ndi_filter_destroy_audioonly;

	ndi_filter_info.filter_audio = ndi_filter_asyncaudio;

	return ndi_filter_info;
}

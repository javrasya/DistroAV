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
#define MAX_SMART_AIM_OUTPUTS 4

// Forward declaration for video_output callback
struct smart_aim_output;
struct ndi_smart_aim_filter;

// Per-output state with video_output for decoupled NDI sending
typedef struct smart_aim_output {
	bool enabled;
	char ndi_name[256];
	char ndi_groups[256];

	NDIlib_send_instance_t ndi_sender;
	pthread_mutex_t sender_mutex;

	ndi_video_converter_t converter;

	// Each output has its own texrender and stagesurface
	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;
	uint8_t *video_data;
	uint32_t video_linesize;

	// video_output for decoupled NDI sending (key fix for latency)
	video_t *video_output;

	// Back-reference to parent filter for ovi access in callback
	struct ndi_smart_aim_filter *parent;
	int output_index;

	uint32_t known_width;
	uint32_t known_height;
} smart_aim_output_t;

typedef struct ndi_smart_aim_filter {
	obs_source_t *obs_source;

	// Shared: render source once per frame
	gs_texrender_t *shared_texrender;
	uint32_t shared_width;
	uint32_t shared_height;

	smart_aim_output_t outputs[MAX_SMART_AIM_OUTPUTS];
	obs_video_info ovi;
	bool stop_when_inactive;
} ndi_smart_aim_filter_t;

static void ndi_smart_aim_filter_update(void *data, obs_data_t *settings);

static void build_prop_name(char *buf, size_t size, int index, const char *suffix)
{
	snprintf(buf, size, "output_%d_%s", index + 1, suffix);
}

// video_output callback - handles NDI sending decoupled from GPU render
static void smart_aim_output_raw_video(void *data, video_data *frame)
{
	auto out = (smart_aim_output_t *)data;
	if (!out || !out->enabled || !out->ndi_sender || !out->parent)
		return;

	ndi_smart_aim_filter_t *f = out->parent;

	// FPS gating
	int frames_to_send = 1;
	if (out->converter.enable_custom_framerate && frame) {
		bool should_send = ndi_converter_should_send_frame(&out->converter, frame->timestamp, &frames_to_send);
		if (!should_send || frames_to_send == 0)
			return;
	}

	// Determine frame rate metadata
	uint32_t ndi_fps_num = f->ovi.fps_num;
	uint32_t ndi_fps_den = f->ovi.fps_den;
	if (out->converter.enable_custom_framerate && out->converter.target_fps_num > 0 &&
	    out->converter.target_fps_den > 0) {
		ndi_fps_num = out->converter.target_fps_num;
		ndi_fps_den = out->converter.target_fps_den;
	}

	// Apply CPU crop only when custom resolution is disabled
	uint32_t final_width = out->known_width;
	uint32_t final_height = out->known_height;
	uint8_t *final_data = frame->data[0];
	uint32_t final_linesize = frame->linesize[0];

	if (out->converter.enable_crop && !out->converter.enable_custom_resolution && out->converter.crop_cache_valid &&
	    frame && frame->data[0]) {
		int32_t crop_left = out->converter.cached_crop_scaled_left;
		int32_t crop_top = out->converter.cached_crop_scaled_top;
		uint32_t crop_width = out->converter.cached_crop_scaled_width;
		uint32_t crop_height = out->converter.cached_crop_scaled_height;

		if (crop_width > 0 && crop_height > 0 &&
		    (crop_left > 0 || crop_top > 0 || crop_width < out->known_width ||
		     crop_height < out->known_height)) {
			final_data = frame->data[0] + (crop_top * final_linesize) + (crop_left * 4);
			final_width = crop_width;
			final_height = crop_height;
		}
	}

	// Build and send NDI frame
	NDIlib_video_frame_v2_t video_frame = {0};
	if (frame && frame->data[0]) {
		video_frame.xres = final_width;
		video_frame.yres = final_height;
		video_frame.FourCC = NDIlib_FourCC_type_BGRA;
		video_frame.frame_rate_N = ndi_fps_num;
		video_frame.frame_rate_D = ndi_fps_den;
		video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
		video_frame.timecode = NDIlib_send_timecode_synthesize;
		video_frame.p_data = final_data;
		video_frame.line_stride_in_bytes = final_linesize;
	}

	pthread_mutex_lock(&out->sender_mutex);
	// Double-check after acquiring lock (sender could have been destroyed)
	if (out->ndi_sender) {
		ndiLib->send_send_video_async_v2(out->ndi_sender, &video_frame);
	}
	pthread_mutex_unlock(&out->sender_mutex);
}

static const char *ndi_smart_aim_filter_getname(void *)
{
	return obs_module_text("NDIPlugin.SmartAimFilterName");
}

static bool is_output_valid(ndi_smart_aim_filter_t *f, smart_aim_output_t *out)
{
	if (!out->enabled || !out->ndi_sender)
		return false;

	obs_source_t *target = obs_filter_get_target(f->obs_source);
	obs_source_t *parent = obs_filter_get_parent(f->obs_source);
	if (!target || !parent)
		return false;

	uint32_t width = obs_source_get_width(f->obs_source);
	uint32_t height = obs_source_get_height(f->obs_source);
	if (width == 0 || height == 0 || !obs_source_enabled(f->obs_source))
		return false;

	if (!f->stop_when_inactive)
		return true;

	return obs_source_active(parent);
}

// Helper to create/recreate video_output for an output
static void output_recreate_video_output(smart_aim_output_t *out, uint32_t width, uint32_t height, obs_video_info *ovi)
{
	if (out->video_output) {
		video_output_stop(out->video_output);
		video_output_close(out->video_output);
		out->video_output = nullptr;
	}

	video_output_info vi = {0};
	vi.format = VIDEO_FORMAT_BGRA;
	vi.width = width;
	vi.height = height;
	vi.fps_den = ovi->fps_den;
	vi.fps_num = ovi->fps_num;
	vi.cache_size = 2; // Low latency buffer
	vi.colorspace = VIDEO_CS_DEFAULT;
	vi.range = VIDEO_RANGE_DEFAULT;
	vi.name = out->ndi_name;

	video_output_open(&out->video_output, &vi);
	video_output_connect(out->video_output, nullptr, smart_aim_output_raw_video, out);
}

// Process a single output - uses video_output for decoupled NDI sending
static void process_single_output(ndi_smart_aim_filter_t *f, smart_aim_output_t *out, obs_source_t *target,
				  obs_source_t *parent, uint32_t width, uint32_t height, uint64_t timestamp)
{
	// Determine render dimensions
	uint32_t render_width = width;
	uint32_t render_height = height;
	if (out->converter.enable_custom_resolution && out->converter.target_width > 0 &&
	    out->converter.target_height > 0) {
		render_width = out->converter.target_width;
		render_height = out->converter.target_height;
	}

	// Recreate stagesurface and video_output if dimensions changed
	if (out->known_width != render_width || out->known_height != render_height) {
		gs_stagesurface_destroy(out->stagesurface);
		out->stagesurface = gs_stagesurface_create(render_width, render_height, TEXFORMAT);

		// Recreate video_output with new dimensions
		output_recreate_video_output(out, render_width, render_height, &f->ovi);

		out->known_width = render_width;
		out->known_height = render_height;
		ndi_converter_update_crop_cache(&out->converter, width, height, render_width, render_height);
	} else if (out->converter.enable_crop && !out->converter.crop_cache_valid) {
		ndi_converter_update_crop_cache(&out->converter, width, height, render_width, render_height);
	}

	// Ensure video_output exists
	if (!out->video_output) {
		output_recreate_video_output(out, render_width, render_height, &f->ovi);
	}

	gs_texrender_reset(out->texrender);

	if (!gs_texrender_begin(out->texrender, render_width, render_height))
		return;

	vec4 bg;
	vec4_zero(&bg);
	gs_clear(GS_CLEAR_COLOR, &bg, 0.0f, 0);

	// Calculate ortho projection (handles crop)
	float ortho_left = 0.0f;
	float ortho_right = (float)width;
	float ortho_top = 0.0f;
	float ortho_bottom = (float)height;

	if (out->converter.enable_crop && out->converter.enable_custom_resolution && out->converter.crop_cache_valid) {
		ortho_left = (float)out->converter.cached_crop_left;
		ortho_right = (float)(out->converter.cached_crop_left + out->converter.cached_crop_width);
		ortho_top = (float)out->converter.cached_crop_top;
		ortho_bottom = (float)(out->converter.cached_crop_top + out->converter.cached_crop_height);
	}

	gs_ortho(ortho_left, ortho_right, ortho_top, ortho_bottom, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	// Render source directly
	if (target == parent) {
		obs_source_skip_video_filter(f->obs_source);
	} else {
		obs_source_video_render(target);
	}

	gs_blend_state_pop();
	gs_texrender_end(out->texrender);

	// Stage texture
	gs_stage_texture(out->stagesurface, gs_texrender_get_texture(out->texrender));

	if (!gs_stagesurface_map(out->stagesurface, &out->video_data, &out->video_linesize))
		return;

	// Copy to video_output buffer (decoupled from NDI sending)
	video_frame output_frame;
	if (video_output_lock_frame(out->video_output, &output_frame, 1, timestamp)) {
		uint32_t linesize = output_frame.linesize[0];
		// Optimize: if linesizes match, copy entire frame at once
		if (linesize == out->video_linesize) {
			memcpy(output_frame.data[0], out->video_data, linesize * render_height);
		} else {
			// Fallback: copy line by line if linesizes differ
			for (uint32_t i = 0; i < render_height; ++i) {
				uint32_t dst_offset = linesize * i;
				uint32_t src_offset = out->video_linesize * i;
				memcpy(output_frame.data[0] + dst_offset, out->video_data + src_offset, linesize);
			}
		}
		video_output_unlock_frame(out->video_output);
	}

	gs_stagesurface_unmap(out->stagesurface);
}

static void ndi_smart_aim_filter_render_video(void *data, gs_effect_t *)
{
	auto f = (ndi_smart_aim_filter_t *)data;
	obs_source_skip_video_filter(f->obs_source);

	obs_source_t *target = obs_filter_get_target(f->obs_source);
	obs_source_t *parent = obs_filter_get_parent(f->obs_source);
	if (!target || !parent)
		return;

	uint32_t width = obs_source_get_width(f->obs_source);
	uint32_t height = obs_source_get_height(f->obs_source);
	if (width == 0 || height == 0)
		return;

	uint64_t timestamp = os_gettime_ns();

	// Process each output independently (like having N separate filters)
	for (int i = 0; i < MAX_SMART_AIM_OUTPUTS; i++) {
		smart_aim_output_t *out = &f->outputs[i];

		if (!is_output_valid(f, out))
			continue;

		// Check for receivers under lock to avoid race with update thread
		pthread_mutex_lock(&out->sender_mutex);
		bool has_connections = out->ndi_sender &&
				       ndiLib->send_get_no_connections(out->ndi_sender, 0) > 0;
		pthread_mutex_unlock(&out->sender_mutex);

		if (!has_connections)
			continue;

		// FPS gating - skip GPU work for frames that will be dropped
		if (out->converter.enable_custom_framerate &&
		    !ndi_converter_should_render_frame_peek(&out->converter, timestamp))
			continue;

		// Process this output (renders source, stages, copies to video_output)
		process_single_output(f, out, target, parent, width, height, timestamp);
	}
}

static void output_destroy_sender(smart_aim_output_t *out)
{
	// FIRST: Stop video_output to ensure callback is not running
	// This MUST happen before touching the sender to avoid race condition
	if (out->video_output) {
		video_output_stop(out->video_output);
		video_output_close(out->video_output);
		out->video_output = nullptr;
	}

	// Reset dimensions to force video_output recreation if re-enabled
	out->known_width = 0;
	out->known_height = 0;

	// NOW safe to destroy sender - callback is guaranteed stopped
	if (!out->ndi_sender)
		return;
	pthread_mutex_lock(&out->sender_mutex);
	ndiLib->send_destroy(out->ndi_sender);
	out->ndi_sender = nullptr;
	pthread_mutex_unlock(&out->sender_mutex);
}

static void output_create_sender(smart_aim_output_t *out)
{
	if (!out->ndi_name[0])
		return;

	NDIlib_send_create_t desc;
	desc.p_ndi_name = out->ndi_name;
	desc.p_groups = out->ndi_groups[0] ? out->ndi_groups : nullptr;
	desc.clock_video = false;
	desc.clock_audio = false;

	pthread_mutex_lock(&out->sender_mutex);
	if (out->ndi_sender)
		ndiLib->send_destroy(out->ndi_sender);
	out->ndi_sender = ndiLib->send_create(&desc);
	pthread_mutex_unlock(&out->sender_mutex);
}

static void add_output_properties(obs_properties_t *props, int index)
{
	char prop_name[128];
	char label[64];

	obs_properties_t *group = obs_properties_create();

	build_prop_name(prop_name, sizeof(prop_name), index, "enabled");
	snprintf(label, sizeof(label), "Enable Output %d", index + 1);
	obs_properties_add_bool(group, prop_name, label);

	build_prop_name(prop_name, sizeof(prop_name), index, "ndi_name");
	obs_properties_add_text(group, prop_name, obs_module_text("NDIPlugin.FilterProps.NDIName"), OBS_TEXT_DEFAULT);

	build_prop_name(prop_name, sizeof(prop_name), index, "ndi_groups");
	obs_properties_add_text(group, prop_name, obs_module_text("NDIPlugin.FilterProps.NDIGroups"), OBS_TEXT_DEFAULT);

	// Resolution
	obs_properties_t *group_res = obs_properties_create();
	build_prop_name(prop_name, sizeof(prop_name), index, "enable_custom_resolution");
	obs_properties_add_bool(group_res, prop_name, "Enable Custom Resolution");

	build_prop_name(prop_name, sizeof(prop_name), index, "resolution_mode");
	auto res_mode =
		obs_properties_add_list(group_res, prop_name, "Resolution Preset", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(res_mode, "720p", NDI_RESOLUTION_720P);
	obs_property_list_add_int(res_mode, "1080p", NDI_RESOLUTION_1080P);
	obs_property_list_add_int(res_mode, "1440p", NDI_RESOLUTION_1440P);
	obs_property_list_add_int(res_mode, "4K", NDI_RESOLUTION_4K);
	obs_property_list_add_int(res_mode, "Custom", NDI_RESOLUTION_CUSTOM);

	build_prop_name(prop_name, sizeof(prop_name), index, "custom_width");
	obs_properties_add_int(group_res, prop_name, "Width", 128, 7680, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "custom_height");
	obs_properties_add_int(group_res, prop_name, "Height", 72, 4320, 1);

	char grp[64];
	snprintf(grp, sizeof(grp), "output_%d_res", index + 1);
	obs_properties_add_group(group, grp, "Resolution", OBS_GROUP_NORMAL, group_res);

	// Crop
	obs_properties_t *group_crop = obs_properties_create();
	build_prop_name(prop_name, sizeof(prop_name), index, "enable_crop");
	obs_properties_add_bool(group_crop, prop_name, "Enable Crop");

	build_prop_name(prop_name, sizeof(prop_name), index, "crop_left");
	obs_properties_add_int(group_crop, prop_name, "Left", 0, 7680, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_top");
	obs_properties_add_int(group_crop, prop_name, "Top", 0, 4320, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_width");
	obs_properties_add_int(group_crop, prop_name, "Width (0=full)", 0, 7680, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_height");
	obs_properties_add_int(group_crop, prop_name, "Height (0=full)", 0, 4320, 1);

	snprintf(grp, sizeof(grp), "output_%d_crop", index + 1);
	obs_properties_add_group(group, grp, "Crop", OBS_GROUP_NORMAL, group_crop);

	// FPS
	obs_properties_t *group_fps = obs_properties_create();
	build_prop_name(prop_name, sizeof(prop_name), index, "enable_custom_framerate");
	obs_properties_add_bool(group_fps, prop_name, "Enable Custom FPS");

	build_prop_name(prop_name, sizeof(prop_name), index, "framerate_mode");
	auto fps_mode =
		obs_properties_add_list(group_fps, prop_name, "FPS Preset", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps_mode, "5", NDI_FRAMERATE_5);
	obs_property_list_add_int(fps_mode, "10", NDI_FRAMERATE_10);
	obs_property_list_add_int(fps_mode, "15", NDI_FRAMERATE_15);
	obs_property_list_add_int(fps_mode, "30", NDI_FRAMERATE_30);
	obs_property_list_add_int(fps_mode, "60", NDI_FRAMERATE_60);
	obs_property_list_add_int(fps_mode, "Custom", NDI_FRAMERATE_CUSTOM);

	build_prop_name(prop_name, sizeof(prop_name), index, "custom_fps");
	obs_properties_add_int(group_fps, prop_name, "Custom FPS", 1, 240, 1);

	snprintf(grp, sizeof(grp), "output_%d_fps", index + 1);
	obs_properties_add_group(group, grp, "Frame Rate", OBS_GROUP_NORMAL, group_fps);

	snprintf(grp, sizeof(grp), "output_%d", index + 1);
	snprintf(label, sizeof(label), "Output %d", index + 1);
	obs_properties_add_group(props, grp, label, OBS_GROUP_NORMAL, group);
}

static obs_properties_t *ndi_smart_aim_filter_getproperties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_bool(props, "stop_when_inactive", "Stop when inactive");

	for (int i = 0; i < MAX_SMART_AIM_OUTPUTS; i++)
		add_output_properties(props, i);

	obs_properties_add_button(props, "apply", obs_module_text("NDIPlugin.FilterProps.ApplySettings"),
				  [](obs_properties_t *, obs_property_t *, void *data) {
					  auto f = (ndi_smart_aim_filter_t *)data;
					  auto s = obs_source_get_settings(f->obs_source);
					  ndi_smart_aim_filter_update(f, s);
					  obs_data_release(s);
					  return true;
				  });

	return props;
}

static void set_output_defaults(obs_data_t *d, int i)
{
	char p[128];
	build_prop_name(p, sizeof(p), i, "enabled");
	obs_data_set_default_bool(d, p, false);

	build_prop_name(p, sizeof(p), i, "ndi_name");
	char name[64];
	snprintf(name, sizeof(name), "Smart Zenith %d", i + 1);
	obs_data_set_default_string(d, p, name);

	build_prop_name(p, sizeof(p), i, "resolution_mode");
	obs_data_set_default_int(d, p, NDI_RESOLUTION_1080P);
	build_prop_name(p, sizeof(p), i, "custom_width");
	obs_data_set_default_int(d, p, 1920);
	build_prop_name(p, sizeof(p), i, "custom_height");
	obs_data_set_default_int(d, p, 1080);

	build_prop_name(p, sizeof(p), i, "framerate_mode");
	obs_data_set_default_int(d, p, NDI_FRAMERATE_30);

	build_prop_name(p, sizeof(p), i, "custom_fps");
	obs_data_set_default_int(d, p, 30);
}

static void ndi_smart_aim_filter_getdefaults(obs_data_t *d)
{
	obs_data_set_default_bool(d, "stop_when_inactive", true);
	for (int i = 0; i < MAX_SMART_AIM_OUTPUTS; i++)
		set_output_defaults(d, i);
}

static void update_output(smart_aim_output_t *out, obs_data_t *s, int i)
{
	char p[128];

	// FIRST: Stop video_output to ensure callback is not running
	// This MUST happen before modifying any state the callback uses
	if (out->video_output) {
		video_output_stop(out->video_output);
		video_output_close(out->video_output);
		out->video_output = nullptr;
	}

	// Reset dimensions to force video_output recreation
	out->known_width = 0;
	out->known_height = 0;

	// NOW safe to update settings - callback is guaranteed stopped
	build_prop_name(p, sizeof(p), i, "enabled");
	out->enabled = obs_data_get_bool(s, p);

	build_prop_name(p, sizeof(p), i, "ndi_name");
	strncpy(out->ndi_name, obs_data_get_string(s, p), sizeof(out->ndi_name) - 1);

	build_prop_name(p, sizeof(p), i, "ndi_groups");
	strncpy(out->ndi_groups, obs_data_get_string(s, p), sizeof(out->ndi_groups) - 1);

	// Build converter settings
	obs_data_t *cs = obs_data_create();

	build_prop_name(p, sizeof(p), i, "enable_custom_resolution");
	obs_data_set_bool(cs, "enable_custom_resolution", obs_data_get_bool(s, p));
	build_prop_name(p, sizeof(p), i, "resolution_mode");
	obs_data_set_int(cs, "resolution_mode", obs_data_get_int(s, p));
	build_prop_name(p, sizeof(p), i, "custom_width");
	obs_data_set_int(cs, "custom_width", obs_data_get_int(s, p));
	build_prop_name(p, sizeof(p), i, "custom_height");
	obs_data_set_int(cs, "custom_height", obs_data_get_int(s, p));

	build_prop_name(p, sizeof(p), i, "enable_crop");
	obs_data_set_bool(cs, "enable_crop", obs_data_get_bool(s, p));
	build_prop_name(p, sizeof(p), i, "crop_left");
	obs_data_set_int(cs, "crop_left", obs_data_get_int(s, p));
	build_prop_name(p, sizeof(p), i, "crop_top");
	obs_data_set_int(cs, "crop_top", obs_data_get_int(s, p));
	build_prop_name(p, sizeof(p), i, "crop_width");
	obs_data_set_int(cs, "crop_width", obs_data_get_int(s, p));
	build_prop_name(p, sizeof(p), i, "crop_height");
	obs_data_set_int(cs, "crop_height", obs_data_get_int(s, p));

	build_prop_name(p, sizeof(p), i, "enable_custom_framerate");
	obs_data_set_bool(cs, "enable_custom_framerate", obs_data_get_bool(s, p));
	build_prop_name(p, sizeof(p), i, "framerate_mode");
	obs_data_set_int(cs, "framerate_mode", obs_data_get_int(s, p));
	build_prop_name(p, sizeof(p), i, "custom_fps");
	obs_data_set_int(cs, "custom_fps_num", obs_data_get_int(s, p));
	obs_data_set_int(cs, "custom_fps_den", 1);

	ndi_converter_update(&out->converter, cs);
	obs_data_release(cs);

	if (out->enabled)
		output_create_sender(out);
	else
		output_destroy_sender(out);
}

static void ndi_smart_aim_filter_update(void *data, obs_data_t *s)
{
	auto f = (ndi_smart_aim_filter_t *)data;
	f->stop_when_inactive = obs_data_get_bool(s, "stop_when_inactive");

	for (int i = 0; i < MAX_SMART_AIM_OUTPUTS; i++)
		update_output(&f->outputs[i], s, i);
}

static void *ndi_smart_aim_filter_create(obs_data_t *s, obs_source_t *src)
{
	auto f = (ndi_smart_aim_filter_t *)bzalloc(sizeof(ndi_smart_aim_filter_t));
	f->obs_source = src;
	f->shared_texrender = gs_texrender_create(TEXFORMAT, GS_ZS_NONE);
	obs_get_video_info(&f->ovi);

	for (int i = 0; i < MAX_SMART_AIM_OUTPUTS; i++) {
		smart_aim_output_t *out = &f->outputs[i];
		pthread_mutex_init(&out->sender_mutex, NULL);
		out->texrender = gs_texrender_create(TEXFORMAT, GS_ZS_NONE);
		ndi_converter_init(&out->converter);
		// Back-references for video_output callback
		out->parent = f;
		out->output_index = i;
	}

	ndi_smart_aim_filter_update(f, s);
	return f;
}

static void ndi_smart_aim_filter_destroy(void *data)
{
	auto f = (ndi_smart_aim_filter_t *)data;

	for (int i = 0; i < MAX_SMART_AIM_OUTPUTS; i++) {
		smart_aim_output_t *out = &f->outputs[i];

		// output_destroy_sender handles video_output cleanup safely
		output_destroy_sender(out);

		gs_stagesurface_unmap(out->stagesurface);
		gs_stagesurface_destroy(out->stagesurface);
		gs_texrender_destroy(out->texrender);
		ndi_converter_destroy(&out->converter);
	}

	gs_texrender_destroy(f->shared_texrender);
	bfree(f);
}

static void ndi_smart_aim_filter_tick(void *data, float)
{
	auto f = (ndi_smart_aim_filter_t *)data;
	obs_get_video_info(&f->ovi);
}

obs_source_info create_ndi_smart_aim_filter_info()
{
	obs_source_info info = {};
	info.id = "ndi_smart_aim_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = ndi_smart_aim_filter_getname;
	info.get_properties = ndi_smart_aim_filter_getproperties;
	info.get_defaults = ndi_smart_aim_filter_getdefaults;
	info.create = ndi_smart_aim_filter_create;
	info.destroy = ndi_smart_aim_filter_destroy;
	info.update = ndi_smart_aim_filter_update;
	info.video_tick = ndi_smart_aim_filter_tick;
	info.video_render = ndi_smart_aim_filter_render_video;
	return info;
}

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

#include "gaze-filter.h"
#include "plugin-main.h"

#include <util/platform.h>
#include <media-io/video-frame.h>

#include <cstring>
#include <chrono>

#define TEXFORMAT GS_BGRA

// Property name helpers
static void build_prop_name(char *buf, size_t size, int index, const char *suffix)
{
	snprintf(buf, size, "gaze_output_%d_%s", index + 1, suffix);
}

// Forward declarations
static void gaze_filter_update(void *data, obs_data_t *settings);
static bool output_init_components(gaze_output_t *out, gaze_filter_t *f);

//
// video_output callback - handles encoding and sending decoupled from GPU render
//
static void gaze_output_raw_video(void *data, struct video_data *frame)
{
	auto out = (gaze_output_t *)data;
	if (!out || !out->enabled || !out->parent || !out->components_initialized)
		return;

	// Reduce logging overhead - only log every 300 frames (5 seconds at 60fps)
	static int raw_video_count = 0;
	raw_video_count++;

	gaze_filter_t *f = out->parent;

	// FPS gating
	int frames_to_send = 1;
	if (out->converter.enable_custom_framerate && frame) {
		bool should_send = ndi_converter_should_send_frame(
			&out->converter, frame->timestamp, &frames_to_send);
		if (!should_send || frames_to_send == 0)
			return;
	}

	// Note: CPU crop path was removed - it caused crashes because the encoder's
	// sws_ctx dimensions wouldn't match the cropped dimensions.
	// Crop is now always applied via GPU ortho projection in process_single_output().
	// For crop to work properly, users should enable "Custom Resolution" which
	// triggers the GPU crop path.
	uint8_t *final_data = frame->data[0];
	uint32_t final_linesize = frame->linesize[0];

	// Encode frame
	uint8_t *encoded_data = nullptr;
	size_t encoded_size = 0;
	bool is_keyframe = false;

	pthread_mutex_lock(&out->output_mutex);

	// Request keyframe if needed (e.g., at startup)
	if (out->keyframe_requested) {
		gaze_encoder_request_keyframe(&out->encoder);
		out->keyframe_requested = false;
		obs_log(LOG_INFO, "[gaze-filter] Output %d: Requesting keyframe", out->output_index + 1);
	}

	if (!gaze_encoder_encode(&out->encoder, final_data, final_linesize,
				 &encoded_data, &encoded_size, &is_keyframe)) {
		pthread_mutex_unlock(&out->output_mutex);
		return;
	}

	if (encoded_size == 0) {
		// Encoder needs more frames
		pthread_mutex_unlock(&out->output_mutex);
		return;
	}

	// Keyframes logged at DEBUG level to reduce overhead
	if (is_keyframe) {
		obs_log(LOG_DEBUG, "[gaze-filter] Keyframe at frame %u",
			gaze_packetizer_get_frame_index(&out->packetizer));
	}

	// Packetize
	gaze_packet_t *packets = nullptr;
	size_t packet_count = 0;
	// Use wall clock time for latency measurement (not OBS relative timestamp)
	auto now = std::chrono::system_clock::now();
	uint32_t capture_ts_ms = (uint32_t)(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			now.time_since_epoch())
			.count());

	if (!gaze_packetizer_packetize(&out->packetizer, encoded_data,
				       encoded_size, is_keyframe, capture_ts_ms,
				       &packets, &packet_count)) {
		pthread_mutex_unlock(&out->output_mutex);
		return;
	}

	// Send packets
	if (packet_count > 0) {
		gaze_sender_send_packets(&out->sender, packets, packet_count);
	}

	pthread_mutex_unlock(&out->output_mutex);
}

//
// Filter name
//
static const char *gaze_filter_getname(void *)
{
	return obs_module_text("GazePlugin.FilterName");
}

//
// Check if output is valid for processing
//
static bool is_output_valid(gaze_filter_t *f, gaze_output_t *out)
{
	if (!out->enabled || !out->components_initialized)
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

//
// Recreate video_output for an output
//
static void output_recreate_video_output(gaze_output_t *out, uint32_t width,
					 uint32_t height, obs_video_info *ovi)
{
	if (out->video_output) {
		video_output_stop(out->video_output);
		video_output_close(out->video_output);
		out->video_output = nullptr;
	}

	struct video_output_info vi = {};
	vi.format = VIDEO_FORMAT_BGRA;
	vi.width = width;
	vi.height = height;
	vi.fps_den = ovi->fps_den;
	vi.fps_num = ovi->fps_num;
	vi.cache_size = 2;
	vi.colorspace = VIDEO_CS_DEFAULT;
	vi.range = VIDEO_RANGE_DEFAULT;
	vi.name = out->stream_name;

	video_output_open(&out->video_output, &vi);
	video_output_connect(out->video_output, nullptr, gaze_output_raw_video,
			     out);
}

//
// Process a single output
//
static void process_single_output(gaze_filter_t *f, gaze_output_t *out,
				  obs_source_t *target, obs_source_t *parent,
				  uint32_t width, uint32_t height,
				  uint64_t timestamp)
{
	// Determine render dimensions
	uint32_t render_width = width;
	uint32_t render_height = height;

	// First, ensure crop cache is valid if crop is enabled
	if (out->converter.enable_crop && !out->converter.crop_cache_valid) {
		// Use source dimensions for initial crop cache calculation
		ndi_converter_update_crop_cache(&out->converter, width, height,
						width, height);
	}

	if (out->converter.enable_custom_resolution &&
	    out->converter.target_width > 0 &&
	    out->converter.target_height > 0) {
		// Custom resolution: render at target size, crop applied via ortho
		render_width = out->converter.target_width;
		render_height = out->converter.target_height;
	} else if (out->converter.enable_crop &&
		   out->converter.crop_cache_valid &&
		   out->converter.cached_crop_width > 0 &&
		   out->converter.cached_crop_height > 0) {
		// Crop without custom resolution: render at crop size
		render_width = out->converter.cached_crop_width;
		render_height = out->converter.cached_crop_height;
	}

	// Recreate resources if dimensions changed
	if (out->known_width != render_width ||
	    out->known_height != render_height) {
		// Recreate stagesurface
		gs_stagesurface_destroy(out->stagesurface);
		out->stagesurface =
			gs_stagesurface_create(render_width, render_height,
					       TEXFORMAT);

		// Recreate video_output
		output_recreate_video_output(out, render_width, render_height,
					     &f->ovi);

		// Recreate encoder
		pthread_mutex_lock(&out->output_mutex);
		gaze_encoder_destroy(&out->encoder);

		uint32_t fps_num = f->ovi.fps_num;
		uint32_t fps_den = f->ovi.fps_den;
		if (out->converter.enable_custom_framerate &&
		    out->converter.target_fps_num > 0) {
			fps_num = out->converter.target_fps_num;
			fps_den = out->converter.target_fps_den;
		}

		gaze_encoder_init(&out->encoder, f->codec, f->encoder_type,
				  render_width, render_height, f->bitrate_kbps,
				  fps_num, fps_den, GAZE_DEFAULT_KEYFRAME_INTERVAL);
		// Request keyframe after encoder (re)creation
		out->keyframe_requested = true;
		pthread_mutex_unlock(&out->output_mutex);

		// Update discovery
		gaze_discovery_update(&out->discovery, render_width,
				      render_height, fps_num / fps_den);

		out->known_width = render_width;
		out->known_height = render_height;
		ndi_converter_update_crop_cache(&out->converter, width, height,
						render_width, render_height);
	} else if (out->converter.enable_crop &&
		   !out->converter.crop_cache_valid) {
		ndi_converter_update_crop_cache(&out->converter, width, height,
						render_width, render_height);
	}

	// Ensure video_output exists
	if (!out->video_output) {
		output_recreate_video_output(out, render_width, render_height,
					     &f->ovi);
	}

	gs_texrender_reset(out->texrender);

	if (!gs_texrender_begin(out->texrender, render_width, render_height))
		return;

	struct vec4 bg;
	vec4_zero(&bg);
	gs_clear(GS_CLEAR_COLOR, &bg, 0.0f, 0);

	// Calculate ortho projection (handles crop)
	float ortho_left = 0.0f;
	float ortho_right = (float)width;
	float ortho_top = 0.0f;
	float ortho_bottom = (float)height;

	// Apply crop via ortho projection whenever crop is enabled
	if (out->converter.enable_crop && out->converter.crop_cache_valid) {
		ortho_left = (float)out->converter.cached_crop_left;
		ortho_right = (float)(out->converter.cached_crop_left +
				      out->converter.cached_crop_width);
		ortho_top = (float)out->converter.cached_crop_top;
		ortho_bottom = (float)(out->converter.cached_crop_top +
				       out->converter.cached_crop_height);
	}

	gs_ortho(ortho_left, ortho_right, ortho_top, ortho_bottom, -100.0f,
		 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	// Render source
	if (target == parent) {
		obs_source_skip_video_filter(f->obs_source);
	} else {
		obs_source_video_render(target);
	}

	gs_blend_state_pop();
	gs_texrender_end(out->texrender);

	// Stage texture
	gs_stage_texture(out->stagesurface,
			 gs_texrender_get_texture(out->texrender));

	if (!gs_stagesurface_map(out->stagesurface, &out->video_data,
				 &out->video_linesize))
		return;

	// Encode and send directly (like NDI filter does)
	// This bypasses video_output for lower latency and simpler timing
	uint8_t *encoded_data = nullptr;
	size_t encoded_size = 0;
	bool is_keyframe = false;

	pthread_mutex_lock(&out->output_mutex);

	// Request keyframe if needed
	if (out->keyframe_requested) {
		gaze_encoder_request_keyframe(&out->encoder);
		out->keyframe_requested = false;
	}

	if (gaze_encoder_encode(&out->encoder, out->video_data,
				out->video_linesize, &encoded_data,
				&encoded_size, &is_keyframe) &&
	    encoded_size > 0) {
		// Packetize and send
		gaze_packet_t *packets = nullptr;
		size_t packet_count = 0;

		// Wall clock timestamp for latency measurement
		auto now = std::chrono::system_clock::now();
		uint32_t capture_ts_ms = (uint32_t)(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				now.time_since_epoch())
				.count());

		if (gaze_packetizer_packetize(&out->packetizer, encoded_data,
					      encoded_size, is_keyframe,
					      capture_ts_ms, &packets,
					      &packet_count)) {
			if (packet_count > 0) {
				gaze_sender_send_packets(&out->sender, packets,
							 packet_count);
			}
		}
	}

	pthread_mutex_unlock(&out->output_mutex);

	gs_stagesurface_unmap(out->stagesurface);
}

//
// Video render callback
//
static void gaze_filter_render_video(void *data, gs_effect_t *)
{
	auto f = (gaze_filter_t *)data;
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

	// Debug: log render callback timing and detect mode changes
	static uint64_t last_log_time = 0;
	static uint64_t last_render_time = 0;
	static int render_count = 0;
	static int skip_count = 0;
	static int rapid_count = 0;  // Renders within 1ms of each other

	render_count++;

	// Studio mode deduplication: OBS creates two filter instances (preview + program)
	// and calls render on both within ~1ms. Skip the second call entirely.
	bool is_rapid_call = last_render_time > 0 && (timestamp - last_render_time) < 1000000;
	last_render_time = timestamp;

	if (is_rapid_call) {
		rapid_count++;
		skip_count++;
		// Skip entirely - don't even iterate outputs
		goto log_stats;
	}

	// Track which checks fail
	static int valid_fail = 0, receiver_fail = 0, fps_fail = 0, processed = 0;

	// Process each output independently
	for (int i = 0; i < MAX_GAZE_OUTPUTS; i++) {
		gaze_output_t *out = &f->outputs[i];

		// Lazy initialization: only initialize on first non-rapid render
		// This prevents duplicate sockets/mDNS in studio mode (two filter instances)
		if (out->pending_init) {
			out->pending_init = false;
			if (!output_init_components(out, f)) {
				obs_log(LOG_ERROR,
					"[gaze-filter] Failed lazy init for output %d",
					i + 1);
			}
		}

		if (!is_output_valid(f, out)) {
			valid_fail++;
			continue;
		}

		// Check for receivers
		if (!gaze_sender_has_receivers(&out->sender)) {
			receiver_fail++;
			continue;
		}

		// FPS gating - skip GPU work for frames that will be dropped
		if (out->converter.enable_custom_framerate &&
		    !ndi_converter_should_render_frame_peek(&out->converter,
							    timestamp)) {
			fps_fail++;
			continue;
		}

		process_single_output(f, out, target, parent, width, height,
				      timestamp);
	}

log_stats:
	// Log stats every 2 seconds
	if (timestamp - last_log_time > 2000000000ULL) {
		obs_log(LOG_INFO,
			"[gaze-filter] Render: %d calls, %d rapid, %d skipped | fails: valid=%d recv=%d fps=%d",
			render_count, rapid_count, skip_count,
			valid_fail, receiver_fail, fps_fail);
		render_count = 0;
		rapid_count = 0;
		skip_count = 0;
		valid_fail = 0;
		receiver_fail = 0;
		fps_fail = 0;
		last_log_time = timestamp;
	}
}

//
// Destroy output components
//
static void output_destroy_components(gaze_output_t *out)
{
	// Stop video_output first
	if (out->video_output) {
		video_output_stop(out->video_output);
		video_output_close(out->video_output);
		out->video_output = nullptr;
	}

	out->known_width = 0;
	out->known_height = 0;

	pthread_mutex_lock(&out->output_mutex);

	gaze_discovery_unregister(&out->discovery);
	gaze_discovery_destroy(&out->discovery);

	gaze_sender_destroy(&out->sender);
	gaze_packetizer_destroy(&out->packetizer);
	gaze_encoder_destroy(&out->encoder);

	out->components_initialized = false;

	pthread_mutex_unlock(&out->output_mutex);
}

//
// Initialize output components
//
static bool output_init_components(gaze_output_t *out, gaze_filter_t *f)
{
	pthread_mutex_lock(&out->output_mutex);

	// Initialize sender
	if (!gaze_sender_init(&out->sender, out->target_host, out->base_port,
			      out->multicast)) {
		obs_log(LOG_ERROR,
			"[gaze-filter] Failed to init sender for output %d",
			out->output_index + 1);
		pthread_mutex_unlock(&out->output_mutex);
		return false;
	}

	// For unicast, assume receiver is present
	if (!out->multicast) {
		gaze_sender_assume_receiver(&out->sender);
	}

	// Initialize packetizer
	bool fec_enabled = f->fec_percent > 0;
	if (!gaze_packetizer_init(&out->packetizer, fec_enabled, f->fec_percent,
				  f->codec)) {
		obs_log(LOG_ERROR,
			"[gaze-filter] Failed to init packetizer for output %d",
			out->output_index + 1);
		gaze_sender_destroy(&out->sender);
		pthread_mutex_unlock(&out->output_mutex);
		return false;
	}

	// Encoder will be initialized when we know the dimensions
	// (in process_single_output)

	// Initialize discovery
	gaze_discovery_init(&out->discovery);

	// Request a keyframe at startup so new receivers can decode immediately
	out->keyframe_requested = true;

	out->components_initialized = true;

	pthread_mutex_unlock(&out->output_mutex);

	obs_log(LOG_INFO,
		"[gaze-filter] Output %d initialized: %s -> %s:%d (%s)",
		out->output_index + 1, out->stream_name,
		out->multicast ? "multicast" : out->target_host, out->base_port,
		gaze_codec_name(f->codec));

	return true;
}

//
// Add output properties to UI
//
static void add_output_properties(obs_properties_t *props, int index)
{
	char prop_name[128];
	char label[64];

	obs_properties_t *group = obs_properties_create();

	build_prop_name(prop_name, sizeof(prop_name), index, "enabled");
	snprintf(label, sizeof(label), "Enable Output %d", index + 1);
	obs_properties_add_bool(group, prop_name, label);

	build_prop_name(prop_name, sizeof(prop_name), index, "stream_name");
	obs_properties_add_text(group, prop_name,
				obs_module_text("GazePlugin.StreamName"),
				OBS_TEXT_DEFAULT);

	build_prop_name(prop_name, sizeof(prop_name), index, "multicast");
	obs_properties_add_bool(group, prop_name,
				obs_module_text("GazePlugin.Multicast"));

	build_prop_name(prop_name, sizeof(prop_name), index, "target_host");
	obs_properties_add_text(group, prop_name,
				obs_module_text("GazePlugin.TargetHost"),
				OBS_TEXT_DEFAULT);

	build_prop_name(prop_name, sizeof(prop_name), index, "base_port");
	obs_properties_add_int(group, prop_name,
			       obs_module_text("GazePlugin.BasePort"), 1024,
			       65535, 1);

	// Resolution
	obs_properties_t *group_res = obs_properties_create();
	build_prop_name(prop_name, sizeof(prop_name), index,
			"enable_custom_resolution");
	obs_properties_add_bool(group_res, prop_name, "Enable Custom Resolution");

	build_prop_name(prop_name, sizeof(prop_name), index, "resolution_mode");
	auto res_mode = obs_properties_add_list(group_res, prop_name,
						"Resolution Preset",
						OBS_COMBO_TYPE_LIST,
						OBS_COMBO_FORMAT_INT);
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
	snprintf(grp, sizeof(grp), "gaze_output_%d_res", index + 1);
	obs_properties_add_group(group, grp, "Resolution", OBS_GROUP_NORMAL,
				 group_res);

	// Crop
	obs_properties_t *group_crop = obs_properties_create();
	build_prop_name(prop_name, sizeof(prop_name), index, "enable_crop");
	obs_properties_add_bool(group_crop, prop_name, "Enable Crop");

	build_prop_name(prop_name, sizeof(prop_name), index, "crop_left");
	obs_properties_add_int(group_crop, prop_name, "Left", 0, 7680, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_top");
	obs_properties_add_int(group_crop, prop_name, "Top", 0, 4320, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_width");
	obs_properties_add_int(group_crop, prop_name, "Width (0=full)", 0, 7680,
			       1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_height");
	obs_properties_add_int(group_crop, prop_name, "Height (0=full)", 0,
			       4320, 1);

	snprintf(grp, sizeof(grp), "gaze_output_%d_crop", index + 1);
	obs_properties_add_group(group, grp, "Crop", OBS_GROUP_NORMAL,
				 group_crop);

	// FPS
	obs_properties_t *group_fps = obs_properties_create();
	build_prop_name(prop_name, sizeof(prop_name), index,
			"enable_custom_framerate");
	obs_properties_add_bool(group_fps, prop_name, "Enable Custom FPS");

	build_prop_name(prop_name, sizeof(prop_name), index, "framerate_mode");
	auto fps_mode = obs_properties_add_list(group_fps, prop_name,
						"FPS Preset", OBS_COMBO_TYPE_LIST,
						OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps_mode, "5", NDI_FRAMERATE_5);
	obs_property_list_add_int(fps_mode, "10", NDI_FRAMERATE_10);
	obs_property_list_add_int(fps_mode, "15", NDI_FRAMERATE_15);
	obs_property_list_add_int(fps_mode, "30", NDI_FRAMERATE_30);
	obs_property_list_add_int(fps_mode, "60", NDI_FRAMERATE_60);
	obs_property_list_add_int(fps_mode, "Custom", NDI_FRAMERATE_CUSTOM);

	build_prop_name(prop_name, sizeof(prop_name), index, "custom_fps");
	obs_properties_add_int(group_fps, prop_name, "Custom FPS", 1, 240, 1);

	snprintf(grp, sizeof(grp), "gaze_output_%d_fps", index + 1);
	obs_properties_add_group(group, grp, "Frame Rate", OBS_GROUP_NORMAL,
				 group_fps);

	snprintf(grp, sizeof(grp), "gaze_output_%d", index + 1);
	snprintf(label, sizeof(label), "Output %d", index + 1);
	obs_properties_add_group(props, grp, label, OBS_GROUP_NORMAL, group);
}

//
// Get properties
//
static obs_properties_t *gaze_filter_getproperties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	// Global settings
	auto codec_list = obs_properties_add_list(
		props, "gaze_codec", obs_module_text("GazePlugin.Codec"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(codec_list, "HEVC (Recommended)",
				  GAZE_CODEC_HEVC);
	obs_property_list_add_int(codec_list, "H.264", GAZE_CODEC_H264);

	auto encoder_list = obs_properties_add_list(
		props, "gaze_encoder_type",
		obs_module_text("GazePlugin.Encoder"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(encoder_list, "Auto (Recommended)",
				  GAZE_ENCODER_AUTO);
	obs_property_list_add_int(encoder_list, "NVIDIA NVENC",
				  GAZE_ENCODER_NVENC);
	obs_property_list_add_int(encoder_list, "AMD AMF", GAZE_ENCODER_AMF);
	obs_property_list_add_int(encoder_list, "Intel QSV", GAZE_ENCODER_QSV);
	obs_property_list_add_int(encoder_list, "Software (CPU)",
				  GAZE_ENCODER_SOFTWARE);

	obs_properties_add_int(props, "gaze_bitrate",
			       obs_module_text("GazePlugin.Bitrate"),
			       GAZE_MIN_BITRATE_KBPS, GAZE_MAX_BITRATE_KBPS,
			       1000);

	obs_properties_add_int(props, "gaze_fec_percent",
			       obs_module_text("GazePlugin.FECPercent"), 0,
			       GAZE_FEC_MAX_PERCENT, 5);

	obs_properties_add_bool(props, "stop_when_inactive",
				obs_module_text("GazePlugin.StopWhenInactive"));

	// Per-output settings
	for (int i = 0; i < MAX_GAZE_OUTPUTS; i++)
		add_output_properties(props, i);

	// Apply button
	obs_properties_add_button(
		props, "apply", obs_module_text("GazePlugin.ApplySettings"),
		[](obs_properties_t *, obs_property_t *, void *data) {
			auto f = (gaze_filter_t *)data;
			auto s = obs_source_get_settings(f->obs_source);
			gaze_filter_update(f, s);
			obs_data_release(s);
			return true;
		});

	return props;
}

//
// Set output defaults
//
static void set_output_defaults(obs_data_t *d, int i)
{
	char p[128];

	build_prop_name(p, sizeof(p), i, "enabled");
	obs_data_set_default_bool(d, p, false);

	build_prop_name(p, sizeof(p), i, "stream_name");
	char name[64];
	snprintf(name, sizeof(name), "Gaze Stream %d", i + 1);
	obs_data_set_default_string(d, p, name);

	build_prop_name(p, sizeof(p), i, "multicast");
	obs_data_set_default_bool(d, p, false);

	build_prop_name(p, sizeof(p), i, "target_host");
	obs_data_set_default_string(d, p, "");

	build_prop_name(p, sizeof(p), i, "base_port");
	obs_data_set_default_int(d, p, GAZE_DEFAULT_RTP_PORT + i * 2);

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

//
// Get defaults
//
static void gaze_filter_getdefaults(obs_data_t *d)
{
	obs_data_set_default_int(d, "gaze_codec", GAZE_CODEC_HEVC);
	obs_data_set_default_int(d, "gaze_encoder_type", GAZE_ENCODER_AUTO);
	obs_data_set_default_int(d, "gaze_bitrate", GAZE_DEFAULT_BITRATE_KBPS);
	obs_data_set_default_int(d, "gaze_fec_percent", GAZE_FEC_DEFAULT_PERCENT);
	obs_data_set_default_bool(d, "stop_when_inactive", true);

	for (int i = 0; i < MAX_GAZE_OUTPUTS; i++)
		set_output_defaults(d, i);
}

//
// Update output from settings
//
static void update_output(gaze_output_t *out, obs_data_t *s, int i,
			  gaze_filter_t *f)
{
	char p[128];

	// Stop video_output first
	if (out->video_output) {
		video_output_stop(out->video_output);
		video_output_close(out->video_output);
		out->video_output = nullptr;
	}

	out->known_width = 0;
	out->known_height = 0;

	// Destroy existing components
	output_destroy_components(out);

	// Read settings
	build_prop_name(p, sizeof(p), i, "enabled");
	out->enabled = obs_data_get_bool(s, p);

	build_prop_name(p, sizeof(p), i, "stream_name");
	strncpy(out->stream_name, obs_data_get_string(s, p),
		sizeof(out->stream_name) - 1);

	build_prop_name(p, sizeof(p), i, "multicast");
	out->multicast = obs_data_get_bool(s, p);

	build_prop_name(p, sizeof(p), i, "target_host");
	strncpy(out->target_host, obs_data_get_string(s, p),
		sizeof(out->target_host) - 1);

	build_prop_name(p, sizeof(p), i, "base_port");
	out->base_port = (uint16_t)obs_data_get_int(s, p);

	// Build converter settings
	obs_data_t *cs = obs_data_create();

	build_prop_name(p, sizeof(p), i, "enable_custom_resolution");
	obs_data_set_bool(cs, "enable_custom_resolution",
			  obs_data_get_bool(s, p));
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
	obs_data_set_bool(cs, "enable_custom_framerate",
			  obs_data_get_bool(s, p));
	build_prop_name(p, sizeof(p), i, "framerate_mode");
	obs_data_set_int(cs, "framerate_mode", obs_data_get_int(s, p));
	build_prop_name(p, sizeof(p), i, "custom_fps");
	obs_data_set_int(cs, "custom_fps_num", obs_data_get_int(s, p));
	obs_data_set_int(cs, "custom_fps_den", 1);

	ndi_converter_update(&out->converter, cs);
	obs_data_release(cs);

	// Mark for lazy initialization (deferred until first non-rapid render)
	// This prevents duplicate sockets/mDNS when OBS creates two filter instances in studio mode
	out->pending_init = false;
	if (out->enabled) {
		if (!out->multicast && out->target_host[0] == '\0') {
			obs_log(LOG_WARNING,
				"[gaze-filter] Output %d: No target host for unicast",
				i + 1);
			out->enabled = false;
		} else {
			out->pending_init = true;
		}
	}
}

//
// Update filter
//
static void gaze_filter_update(void *data, obs_data_t *s)
{
	auto f = (gaze_filter_t *)data;

	f->codec = (gaze_codec_t)obs_data_get_int(s, "gaze_codec");
	f->encoder_type =
		(gaze_encoder_type_t)obs_data_get_int(s, "gaze_encoder_type");
	f->bitrate_kbps = (uint32_t)obs_data_get_int(s, "gaze_bitrate");
	f->fec_percent = (uint8_t)obs_data_get_int(s, "gaze_fec_percent");
	f->stop_when_inactive = obs_data_get_bool(s, "stop_when_inactive");

	for (int i = 0; i < MAX_GAZE_OUTPUTS; i++)
		update_output(&f->outputs[i], s, i, f);
}

//
// Create filter
//
static void *gaze_filter_create(obs_data_t *s, obs_source_t *src)
{
	// Initialize platform networking
	gaze_sender_init_platform();

	auto f = (gaze_filter_t *)bzalloc(sizeof(gaze_filter_t));
	f->obs_source = src;
	obs_get_video_info(&f->ovi);

	for (int i = 0; i < MAX_GAZE_OUTPUTS; i++) {
		gaze_output_t *out = &f->outputs[i];
		pthread_mutex_init(&out->output_mutex, nullptr);
		out->texrender = gs_texrender_create(TEXFORMAT, GS_ZS_NONE);
		ndi_converter_init(&out->converter);
		out->parent = f;
		out->output_index = i;
	}

	gaze_filter_update(f, s);
	return f;
}

//
// Destroy filter
//
static void gaze_filter_destroy(void *data)
{
	auto f = (gaze_filter_t *)data;

	for (int i = 0; i < MAX_GAZE_OUTPUTS; i++) {
		gaze_output_t *out = &f->outputs[i];

		output_destroy_components(out);

		gs_stagesurface_unmap(out->stagesurface);
		gs_stagesurface_destroy(out->stagesurface);
		gs_texrender_destroy(out->texrender);
		ndi_converter_destroy(&out->converter);
		pthread_mutex_destroy(&out->output_mutex);
	}

	bfree(f);
}

//
// Tick
//
static void gaze_filter_tick(void *data, float)
{
	auto f = (gaze_filter_t *)data;
	obs_get_video_info(&f->ovi);
}

//
// Create filter info
//
struct obs_source_info create_gaze_filter_info()
{
	struct obs_source_info info = {};
	info.id = "gaze_stream_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = gaze_filter_getname;
	info.get_properties = gaze_filter_getproperties;
	info.get_defaults = gaze_filter_getdefaults;
	info.create = gaze_filter_create;
	info.destroy = gaze_filter_destroy;
	info.update = gaze_filter_update;
	info.video_tick = gaze_filter_tick;
	info.video_render = gaze_filter_render_video;
	return info;
}

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

//
// Calculate crop top-left coordinates from reference point settings
// Converts reference-based positioning to traditional top-left coordinates
//
static void calculate_crop_topleft(gaze_output_t *out, uint32_t source_w, uint32_t source_h,
				   int32_t *out_left, int32_t *out_top,
				   uint32_t *out_width, uint32_t *out_height)
{
	// Get crop dimensions (convert percentage if needed)
	uint32_t w, h;
	int32_t ref_x, ref_y;

	if (out->crop_type == GAZE_CROP_TYPE_PERCENTAGE) {
		w = (uint32_t)(out->crop_w_pct / 100.0 * source_w);
		h = (uint32_t)(out->crop_h_pct / 100.0 * source_h);
		ref_x = (int32_t)(out->crop_ref_x_pct / 100.0 * source_w);
		ref_y = (int32_t)(out->crop_ref_y_pct / 100.0 * source_h);
	} else {
		w = out->crop_w;
		h = out->crop_h;
		ref_x = out->crop_ref_x;
		ref_y = out->crop_ref_y;
	}

	// Handle zero width/height as full source dimension
	if (w == 0)
		w = source_w;
	if (h == 0)
		h = source_h;

	// Convert reference point to top-left
	int32_t left = 0, top = 0;

	switch (out->crop_reference) {
	case GAZE_CROP_REF_TOP_LEFT:
		left = ref_x;
		top = ref_y;
		break;
	case GAZE_CROP_REF_TOP_CENTER:
		left = ref_x - (int32_t)(w / 2);
		top = ref_y;
		break;
	case GAZE_CROP_REF_TOP_RIGHT:
		left = ref_x - (int32_t)w;
		top = ref_y;
		break;
	case GAZE_CROP_REF_CENTER_LEFT:
		left = ref_x;
		top = ref_y - (int32_t)(h / 2);
		break;
	case GAZE_CROP_REF_CENTER:
		left = ref_x - (int32_t)(w / 2);
		top = ref_y - (int32_t)(h / 2);
		break;
	case GAZE_CROP_REF_CENTER_RIGHT:
		left = ref_x - (int32_t)w;
		top = ref_y - (int32_t)(h / 2);
		break;
	case GAZE_CROP_REF_BOTTOM_LEFT:
		left = ref_x;
		top = ref_y - (int32_t)h;
		break;
	case GAZE_CROP_REF_BOTTOM_CENTER:
		left = ref_x - (int32_t)(w / 2);
		top = ref_y - (int32_t)h;
		break;
	case GAZE_CROP_REF_BOTTOM_RIGHT:
		left = ref_x - (int32_t)w;
		top = ref_y - (int32_t)h;
		break;
	}

	// Clamp to valid range (ensure crop zone stays within source bounds)
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if ((uint32_t)left + w > source_w)
		left = (int32_t)(source_w - w);
	if ((uint32_t)top + h > source_h)
		top = (int32_t)(source_h - h);
	// Final safety clamp in case dimensions exceed source
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;

	*out_left = left;
	*out_top = top;
	*out_width = w;
	*out_height = h;
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

	gaze_filter_t *f = out->parent;

	// Convert OBS frame timestamp to wall clock for latency measurement
	// frame->timestamp is closer to actual capture time than current wall clock
	// Use 100ns units (10,000,000 per second) for NDI compatibility
	int64_t obs_ts_100ns = (int64_t)(frame->timestamp / 100);
	uint32_t capture_ts = (uint32_t)(obs_ts_100ns + f->timestamp_offset_100ns);

	// FPS gating is now done at the GPU render stage (gaze_filter_render_video)
	// Every frame that arrives here has already passed the FPS check

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

		// Cache keyframe for probe responses
		// Use known dimensions since video_data doesn't carry size info
		gaze_sender_cache_keyframe(&out->sender, encoded_data,
					   encoded_size, out->known_width,
					   out->known_height);
		out->probe_keyframe_cached = true;
		out->last_probe_refresh_ns = os_gettime_ns();
	}

	// Increment frame count for probe stats
	gaze_sender_increment_frame_count(&out->sender);

	// Packetize (using capture_ts from start of processing)
	gaze_packet_t *packets = nullptr;
	size_t packet_count = 0;

	if (!gaze_packetizer_packetize(&out->packetizer, encoded_data,
				       encoded_size, is_keyframe, capture_ts,
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
// Pipeline: Source → Pre-Crop Resolution → Crop → Post-Crop Resolution → Encode
//
static void process_single_output(gaze_filter_t *f, gaze_output_t *out,
				  obs_source_t *target, obs_source_t *parent,
				  uint32_t width, uint32_t height,
				  uint64_t timestamp)
{
	// Convert OBS timestamp to wall clock for latency measurement
	// OBS timestamp is closer to actual frame capture time than current wall clock
	// Use 100ns units (10,000,000 per second) for NDI compatibility
	int64_t obs_ts_100ns = (int64_t)(timestamp / 100);
	uint32_t capture_ts = (uint32_t)(obs_ts_100ns + f->timestamp_offset_100ns);

	// Step 1: Determine pre-crop dimensions
	// If pre-crop resolution is enabled, we first scale the source to that size
	uint32_t precrop_width = width;
	uint32_t precrop_height = height;
	bool use_precrop = out->converter.enable_precrop_resolution &&
			   out->converter.precrop_target_width > 0 &&
			   out->converter.precrop_target_height > 0;

	if (use_precrop) {
		precrop_width = out->converter.precrop_target_width;
		precrop_height = out->converter.precrop_target_height;
	}

	// Step 2: Calculate crop coordinates from reference point if crop is enabled
	// Crop is now applied to the pre-crop dimensions, not the original source
	if (out->converter.enable_crop) {
		// Calculate top-left coordinates from reference point settings
		// using pre-crop dimensions as the source
		int32_t crop_left, crop_top;
		uint32_t crop_w, crop_h;
		calculate_crop_topleft(out, precrop_width, precrop_height,
				       &crop_left, &crop_top, &crop_w, &crop_h);

		// Update converter's crop settings with calculated values
		out->converter.crop_left = crop_left;
		out->converter.crop_top = crop_top;
		out->converter.crop_width = crop_w;
		out->converter.crop_height = crop_h;

		// Update crop cache directly
		out->converter.cached_crop_left = crop_left;
		out->converter.cached_crop_top = crop_top;
		out->converter.cached_crop_width = crop_w;
		out->converter.cached_crop_height = crop_h;
		out->converter.crop_cache_valid = true;
	}

	// Step 3: Determine final render dimensions
	uint32_t render_width = precrop_width;
	uint32_t render_height = precrop_height;

	if (out->converter.enable_custom_resolution &&
	    out->converter.target_width > 0 &&
	    out->converter.target_height > 0) {
		// Post-crop resolution: render at target size
		render_width = out->converter.target_width;
		render_height = out->converter.target_height;
	} else if (out->converter.enable_crop &&
		   out->converter.crop_cache_valid &&
		   out->converter.cached_crop_width > 0 &&
		   out->converter.cached_crop_height > 0) {
		// Crop without post-crop resolution: render at crop size
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
				  render_width, render_height, out->bitrate_kbps,
				  fps_num, fps_den, GAZE_DEFAULT_KEYFRAME_INTERVAL);
		// Request keyframe after encoder (re)creation
		out->keyframe_requested = true;
		out->probe_keyframe_cached = false;  // Re-cache keyframe for new resolution

		// Update stream info for probe responses
		gaze_sender_set_stream_info(&out->sender, (uint16_t)render_width,
					    (uint16_t)render_height,
					    (uint16_t)fps_num, (uint16_t)fps_den);
		gaze_sender_set_active(&out->sender, true);
		pthread_mutex_unlock(&out->output_mutex);

		// Register or update discovery (on selected network interface)
		if (!out->discovery.registered) {
			gaze_discovery_register(&out->discovery, out->stream_name,
						out->rtp_port, f->codec,
						render_width, render_height,
						fps_num / fps_den,
						f->network_if_index,
						f->network_bind_addr);
		} else {
			gaze_discovery_update(&out->discovery, render_width,
					      render_height, fps_num / fps_den);
		}

		out->known_width = render_width;
		out->known_height = render_height;
	}

	// Register discovery if not yet registered (handles case where discovery
	// was re-initialized after dimensions were already known, e.g. network change)
	if (!out->discovery.registered && out->discovery.initialized) {
		uint32_t fps_num = f->ovi.fps_num;
		uint32_t fps_den = f->ovi.fps_den;
		if (out->converter.enable_custom_framerate &&
		    out->converter.target_fps_num > 0) {
			fps_num = out->converter.target_fps_num;
			fps_den = out->converter.target_fps_den;
		}
		gaze_discovery_register(&out->discovery, out->stream_name,
					out->rtp_port, f->codec,
					render_width, render_height,
					fps_num / fps_den,
					f->network_if_index,
					f->network_bind_addr);
	}

	// Ensure video_output exists
	if (!out->video_output) {
		output_recreate_video_output(out, render_width, render_height,
					     &f->ovi);
	}

	struct vec4 bg;
	vec4_zero(&bg);

	// === Pass 1: Pre-crop resize (source → precrop_target size) ===
	gs_texture_t *intermediate_texture = nullptr;

	if (use_precrop) {
		gs_texrender_reset(out->texrender_precrop);

		if (!gs_texrender_begin(out->texrender_precrop, precrop_width, precrop_height))
			return;

		gs_clear(GS_CLEAR_COLOR, &bg, 0.0f, 0);

		// Full source ortho (no crop yet, just scale)
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		// Render original source
		if (target == parent) {
			obs_source_skip_video_filter(f->obs_source);
		} else {
			obs_source_video_render(target);
		}

		gs_blend_state_pop();
		gs_texrender_end(out->texrender_precrop);

		// Use precrop texture as source for pass 2
		intermediate_texture = gs_texrender_get_texture(out->texrender_precrop);
	}

	// === Pass 2: Crop + Post-crop resize (from precrop/source → final size) ===
	gs_texrender_reset(out->texrender);

	if (!gs_texrender_begin(out->texrender, render_width, render_height))
		return;

	gs_clear(GS_CLEAR_COLOR, &bg, 0.0f, 0);

	// Calculate ortho projection (handles crop)
	// The source for pass 2 is either the precrop texture or the original source
	uint32_t source_for_crop_w = use_precrop ? precrop_width : width;
	uint32_t source_for_crop_h = use_precrop ? precrop_height : height;

	float ortho_left = 0.0f;
	float ortho_right = (float)source_for_crop_w;
	float ortho_top = 0.0f;
	float ortho_bottom = (float)source_for_crop_h;

	// Apply crop via ortho projection whenever crop is enabled
	if (out->converter.enable_crop && out->converter.crop_cache_valid) {
		ortho_left = (float)out->converter.cached_crop_left;
		ortho_right = (float)(out->converter.cached_crop_left +
				      out->converter.cached_crop_width);
		ortho_top = (float)out->converter.cached_crop_top;
		ortho_bottom = (float)(out->converter.cached_crop_top +
				       out->converter.cached_crop_height);
	}

	gs_ortho(ortho_left, ortho_right, ortho_top, ortho_bottom, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (intermediate_texture) {
		// Render from precrop texture
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(image, intermediate_texture);

		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		gs_draw_sprite(intermediate_texture, 0, precrop_width, precrop_height);

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	} else {
		// No pre-crop: render from original source directly
		if (target == parent) {
			obs_source_skip_video_filter(f->obs_source);
		} else {
			obs_source_video_render(target);
		}
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
		// Cache keyframe for probe responses (always update to keep preview fresh)
		if (is_keyframe) {
			gaze_sender_cache_keyframe(&out->sender, encoded_data,
						   encoded_size, render_width,
						   render_height);
			out->probe_keyframe_cached = true;
			out->last_probe_refresh_ns = os_gettime_ns();
		}

		// Increment frame count for probe stats
		gaze_sender_increment_frame_count(&out->sender);

		// Packetize and send (using capture_ts from start of processing)
		gaze_packet_t *packets = nullptr;
		size_t packet_count = 0;

		if (gaze_packetizer_packetize(&out->packetizer, encoded_data,
					      encoded_size, is_keyframe,
					      capture_ts, &packets,
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

		// Check for receivers - but process periodically to keep probe keyframe fresh
		bool has_receivers = gaze_sender_has_receivers(&out->sender);
		if (!has_receivers) {
			// Refresh probe keyframe every 2 seconds even without receivers
			bool needs_refresh = !out->probe_keyframe_cached ||
				(timestamp - out->last_probe_refresh_ns > 2000000000ULL);
			if (!needs_refresh) {
				receiver_fail++;
				continue;
			}
		}

		// FPS gating - skip GPU work for frames that will be dropped
		// Use the mutating version to track timing state at the beginning of pipeline
		// This is the ONLY FPS gating point - video_output callback does not filter
		if (out->converter.enable_custom_framerate) {
			int frames_to_send = 0;
			if (!ndi_converter_should_send_frame(&out->converter,
							     timestamp,
							     &frames_to_send) ||
			    frames_to_send == 0) {
				fps_fail++;
				continue;
			}
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
	out->probe_keyframe_cached = false;
	out->last_probe_refresh_ns = 0;

	pthread_mutex_lock(&out->output_mutex);

	gaze_discovery_unregister(&out->discovery);
	gaze_discovery_destroy(&out->discovery);

	// Mark stream as inactive before destroying
	gaze_sender_set_active(&out->sender, false);
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

	// Initialize sender with receiver-initiated subscription
	// Pass bind_addr for network interface binding
	if (!gaze_sender_init(&out->sender, out->rtp_port, f->network_bind_addr)) {
		obs_log(LOG_ERROR,
			"[gaze-filter] Failed to init sender for output %d",
			out->output_index + 1);
		pthread_mutex_unlock(&out->output_mutex);
		return false;
	}

	// Initialize packetizer
	bool fec_enabled = out->fec_percent > 0;
	if (!gaze_packetizer_init(&out->packetizer, fec_enabled, out->fec_percent,
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

	// Initialize discovery if not already initialized (may be preserved across updates)
	if (!out->discovery.initialized) {
		gaze_discovery_init(&out->discovery);
	}

	// Store service info for later registration when dimensions are known
	strncpy(out->discovery.service_name, out->stream_name,
		sizeof(out->discovery.service_name) - 1);
	out->discovery.port = out->rtp_port;
	out->discovery.codec = f->codec;
	out->discovery.if_index = f->network_if_index;
	out->discovery.ip_addr = f->network_bind_addr;

	// Set sender as active (stream is producing frames)
	gaze_sender_set_active(&out->sender, true);

	// Request a keyframe at startup so new receivers can decode immediately
	out->keyframe_requested = true;
	out->probe_keyframe_cached = false;  // Need to cache keyframe for probing

	out->components_initialized = true;

	pthread_mutex_unlock(&out->output_mutex);

	obs_log(LOG_INFO,
		"[gaze-filter] Output %d initialized: %s on port %d (%s)",
		out->output_index + 1, out->stream_name, out->rtp_port,
		gaze_codec_name(f->codec));

	return true;
}

//
// Helper to add resolution mode dropdown options
//
static void add_resolution_mode_options(obs_property_t *res_mode)
{
	obs_property_list_add_int(res_mode, "Auto (Source)", -1);
	obs_property_list_add_int(res_mode, "240x240", NDI_RESOLUTION_240_SQUARE);
	obs_property_list_add_int(res_mode, "320x320", NDI_RESOLUTION_320_SQUARE);
	obs_property_list_add_int(res_mode, "480x480", NDI_RESOLUTION_480_SQUARE);
	obs_property_list_add_int(res_mode, "640x640", NDI_RESOLUTION_640_SQUARE);
	obs_property_list_add_int(res_mode, "720p", NDI_RESOLUTION_720P);
	obs_property_list_add_int(res_mode, "1080p", NDI_RESOLUTION_1080P);
	obs_property_list_add_int(res_mode, "1440p", NDI_RESOLUTION_1440P);
	obs_property_list_add_int(res_mode, "4K", NDI_RESOLUTION_4K);
	obs_property_list_add_int(res_mode, "Custom", NDI_RESOLUTION_CUSTOM);
}

//
// Add output properties to UI
// Order: Enable/Name → Frame Rate → Pre-Crop Resolution → Crop → Post-Crop Resolution
//
static void add_output_properties(obs_properties_t *props, int index)
{
	char prop_name[128];
	char label[64];
	char grp[64];

	obs_properties_t *group = obs_properties_create();

	// === Enable/Stream Name (always first) ===
	build_prop_name(prop_name, sizeof(prop_name), index, "enabled");
	snprintf(label, sizeof(label), "Enable Output %d", index + 1);
	obs_properties_add_bool(group, prop_name, label);

	build_prop_name(prop_name, sizeof(prop_name), index, "stream_name");
	obs_properties_add_text(group, prop_name,
				obs_module_text("GazePlugin.StreamName"),
				OBS_TEXT_DEFAULT);

	// === Quality preset group (per-output override) ===
	obs_properties_t *group_quality = obs_properties_create();

	char quality_preset_name[128];
	build_prop_name(quality_preset_name, sizeof(quality_preset_name), index,
			"quality_preset");
	auto out_quality_list = obs_properties_add_list(
		group_quality, quality_preset_name,
		obs_module_text("GazePlugin.QualityPreset"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(out_quality_list, "Use Global", -1);
	obs_property_list_add_int(out_quality_list, "Ultra (25 Mbps)", 0);
	obs_property_list_add_int(out_quality_list, "High (15 Mbps)", 1);
	obs_property_list_add_int(out_quality_list, "Balanced (10 Mbps)", 2);
	obs_property_list_add_int(out_quality_list, "Low (5 Mbps)", 3);
	obs_property_list_add_int(out_quality_list, "Custom...", 4);

	// Custom bitrate (visible only when Custom selected)
	build_prop_name(prop_name, sizeof(prop_name), index, "bitrate");
	obs_properties_add_int(group_quality, prop_name,
			       obs_module_text("GazePlugin.Bitrate"),
			       GAZE_MIN_BITRATE_KBPS, GAZE_MAX_BITRATE_KBPS,
			       1000);

	// Custom FEC % (visible only when Custom selected)
	build_prop_name(prop_name, sizeof(prop_name), index, "fec_percent");
	obs_properties_add_int(group_quality, prop_name,
			       obs_module_text("GazePlugin.FECPercent"), 0,
			       GAZE_FEC_MAX_PERCENT, 5);

	snprintf(grp, sizeof(grp), "gaze_output_%d_quality", index + 1);
	obs_properties_add_group(group, grp,
				 obs_module_text("GazePlugin.QualityPreset"),
				 OBS_GROUP_NORMAL, group_quality);

	// Show/hide custom quality fields based on preset selection
	obs_property_set_modified_callback2(
		out_quality_list,
		[](void *priv, obs_properties_t *props, obs_property_t *,
		   obs_data_t *settings) {
			int idx = (int)(intptr_t)priv;
			char preset_prop[128], bitrate_prop[128], fec_prop[128];
			snprintf(preset_prop, sizeof(preset_prop),
				 "gaze_output_%d_quality_preset", idx + 1);
			snprintf(bitrate_prop, sizeof(bitrate_prop),
				 "gaze_output_%d_bitrate", idx + 1);
			snprintf(fec_prop, sizeof(fec_prop),
				 "gaze_output_%d_fec_percent", idx + 1);

			int preset = (int)obs_data_get_int(settings, preset_prop);
			bool is_custom = (preset == 4);

			obs_property_set_visible(
				obs_properties_get(props, bitrate_prop),
				is_custom);
			obs_property_set_visible(
				obs_properties_get(props, fec_prop), is_custom);
			return true;
		},
		(void *)(intptr_t)index);

	// === Frame Rate group (first in processing pipeline) ===
	obs_properties_t *group_fps = obs_properties_create();
	char fps_mode_name[128];
	build_prop_name(fps_mode_name, sizeof(fps_mode_name), index, "framerate_mode");
	auto fps_mode =
		obs_properties_add_list(group_fps, fps_mode_name, "Preset",
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps_mode, "Auto (Source)", -1);
	obs_property_list_add_int(fps_mode, "15 fps", NDI_FRAMERATE_15);
	obs_property_list_add_int(fps_mode, "30 fps", NDI_FRAMERATE_30);
	obs_property_list_add_int(fps_mode, "60 fps", NDI_FRAMERATE_60);
	obs_property_list_add_int(fps_mode, "Custom", NDI_FRAMERATE_CUSTOM);

	// Custom FPS field (only visible when Custom selected)
	build_prop_name(prop_name, sizeof(prop_name), index, "custom_fps");
	obs_properties_add_int(group_fps, prop_name, "FPS", 1, 240, 1);

	snprintf(grp, sizeof(grp), "gaze_output_%d_framerate", index + 1);
	obs_properties_add_group(group, grp, "Frame Rate",
				 OBS_GROUP_NORMAL, group_fps);

	// Show/hide custom FPS based on dropdown selection
	obs_property_set_modified_callback2(
		fps_mode,
		[](void *priv, obs_properties_t *props, obs_property_t *,
		   obs_data_t *settings) {
			int idx = (int)(intptr_t)priv;
			char mode_prop[128], fps_prop[128];
			snprintf(mode_prop, sizeof(mode_prop),
				 "gaze_output_%d_framerate_mode", idx + 1);
			snprintf(fps_prop, sizeof(fps_prop),
				 "gaze_output_%d_custom_fps", idx + 1);
			int mode = (int)obs_data_get_int(settings, mode_prop);
			bool is_custom = (mode == NDI_FRAMERATE_CUSTOM);
			obs_property_set_visible(obs_properties_get(props, fps_prop),
						 is_custom);
			return true;
		},
		(void *)(intptr_t)index);

	// === Pre-Crop Resolution group (scale source before cropping) ===
	obs_properties_t *group_precrop_res = obs_properties_create();
	char precrop_res_mode_name[128];
	build_prop_name(precrop_res_mode_name, sizeof(precrop_res_mode_name), index, "precrop_resolution_mode");
	auto precrop_res_mode =
		obs_properties_add_list(group_precrop_res, precrop_res_mode_name, "Preset",
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	add_resolution_mode_options(precrop_res_mode);

	// Custom resolution fields (only visible when Custom selected)
	build_prop_name(prop_name, sizeof(prop_name), index, "precrop_custom_width");
	obs_properties_add_int(group_precrop_res, prop_name, "Width", 128, 7680, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "precrop_custom_height");
	obs_properties_add_int(group_precrop_res, prop_name, "Height", 72, 4320, 1);

	snprintf(grp, sizeof(grp), "gaze_output_%d_precrop_resolution", index + 1);
	obs_properties_add_group(group, grp,
				 obs_module_text("GazePlugin.PreCropResolution"),
				 OBS_GROUP_NORMAL, group_precrop_res);

	// Show/hide pre-crop custom resolution fields based on dropdown selection
	obs_property_set_modified_callback2(
		precrop_res_mode,
		[](void *priv, obs_properties_t *props, obs_property_t *,
		   obs_data_t *settings) {
			int idx = (int)(intptr_t)priv;
			char mode_prop[128], w_prop[128], h_prop[128];
			snprintf(mode_prop, sizeof(mode_prop),
				 "gaze_output_%d_precrop_resolution_mode", idx + 1);
			snprintf(w_prop, sizeof(w_prop),
				 "gaze_output_%d_precrop_custom_width", idx + 1);
			snprintf(h_prop, sizeof(h_prop),
				 "gaze_output_%d_precrop_custom_height", idx + 1);
			int mode = (int)obs_data_get_int(settings, mode_prop);
			bool is_custom = (mode == NDI_RESOLUTION_CUSTOM);
			obs_property_set_visible(obs_properties_get(props, w_prop),
						 is_custom);
			obs_property_set_visible(obs_properties_get(props, h_prop),
						 is_custom);
			return true;
		},
		(void *)(intptr_t)index);

	// === Crop group ===
	obs_properties_t *group_crop = obs_properties_create();
	build_prop_name(prop_name, sizeof(prop_name), index, "enable_crop");
	obs_properties_add_bool(group_crop, prop_name,
				obs_module_text("GazePlugin.CropEnable"));

	// Type dropdown (Pixel / Percentage)
	char crop_type_name[128];
	build_prop_name(crop_type_name, sizeof(crop_type_name), index, "crop_type");
	auto crop_type_list = obs_properties_add_list(
		group_crop, crop_type_name,
		obs_module_text("GazePlugin.CropType"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(crop_type_list,
				  obs_module_text("GazePlugin.CropType.Pixel"),
				  GAZE_CROP_TYPE_PIXEL);
	obs_property_list_add_int(crop_type_list,
				  obs_module_text("GazePlugin.CropType.Percentage"),
				  GAZE_CROP_TYPE_PERCENTAGE);

	// Reference point dropdown (9 options)
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_reference");
	auto ref_list = obs_properties_add_list(
		group_crop, prop_name,
		obs_module_text("GazePlugin.CropReference"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ref_list,
				  obs_module_text("GazePlugin.CropRef.TopLeft"),
				  GAZE_CROP_REF_TOP_LEFT);
	obs_property_list_add_int(ref_list,
				  obs_module_text("GazePlugin.CropRef.TopCenter"),
				  GAZE_CROP_REF_TOP_CENTER);
	obs_property_list_add_int(ref_list,
				  obs_module_text("GazePlugin.CropRef.TopRight"),
				  GAZE_CROP_REF_TOP_RIGHT);
	obs_property_list_add_int(ref_list,
				  obs_module_text("GazePlugin.CropRef.CenterLeft"),
				  GAZE_CROP_REF_CENTER_LEFT);
	obs_property_list_add_int(ref_list,
				  obs_module_text("GazePlugin.CropRef.Center"),
				  GAZE_CROP_REF_CENTER);
	obs_property_list_add_int(ref_list,
				  obs_module_text("GazePlugin.CropRef.CenterRight"),
				  GAZE_CROP_REF_CENTER_RIGHT);
	obs_property_list_add_int(ref_list,
				  obs_module_text("GazePlugin.CropRef.BottomLeft"),
				  GAZE_CROP_REF_BOTTOM_LEFT);
	obs_property_list_add_int(ref_list,
				  obs_module_text("GazePlugin.CropRef.BottomCenter"),
				  GAZE_CROP_REF_BOTTOM_CENTER);
	obs_property_list_add_int(ref_list,
				  obs_module_text("GazePlugin.CropRef.BottomRight"),
				  GAZE_CROP_REF_BOTTOM_RIGHT);

	// Pixel crop sub-group - NUMBER INPUTS
	obs_properties_t *pixel_crop = obs_properties_create();
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_ref_x");
	obs_properties_add_int(pixel_crop, prop_name,
			       obs_module_text("GazePlugin.CropRefX"),
			       0, 7680, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_ref_y");
	obs_properties_add_int(pixel_crop, prop_name,
			       obs_module_text("GazePlugin.CropRefY"),
			       0, 4320, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_w");
	obs_properties_add_int(pixel_crop, prop_name,
			       obs_module_text("GazePlugin.CropWidth"),
			       0, 7680, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_h");
	obs_properties_add_int(pixel_crop, prop_name,
			       obs_module_text("GazePlugin.CropHeight"),
			       0, 4320, 1);
	snprintf(grp, sizeof(grp), "gaze_output_%d_crop_pixel", index + 1);
	obs_properties_add_group(group_crop, grp,
				 obs_module_text("GazePlugin.CropPixelValues"),
				 OBS_GROUP_NORMAL, pixel_crop);

	// Percentage crop sub-group - SLIDERS (0-100%)
	obs_properties_t *pct_crop = obs_properties_create();
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_ref_x_pct");
	obs_properties_add_float_slider(pct_crop, prop_name,
					obs_module_text("GazePlugin.CropRefX"),
					0, 100, 0.1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_ref_y_pct");
	obs_properties_add_float_slider(pct_crop, prop_name,
					obs_module_text("GazePlugin.CropRefY"),
					0, 100, 0.1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_w_pct");
	obs_properties_add_float_slider(pct_crop, prop_name,
					obs_module_text("GazePlugin.CropWidth"),
					0, 100, 0.1);
	build_prop_name(prop_name, sizeof(prop_name), index, "crop_h_pct");
	obs_properties_add_float_slider(pct_crop, prop_name,
					obs_module_text("GazePlugin.CropHeight"),
					0, 100, 0.1);
	snprintf(grp, sizeof(grp), "gaze_output_%d_crop_pct", index + 1);
	obs_properties_add_group(group_crop, grp,
				 obs_module_text("GazePlugin.CropPctValues"),
				 OBS_GROUP_NORMAL, pct_crop);

	// Visibility callback - SHOW/HIDE based on Type selection
	obs_property_set_modified_callback2(
		crop_type_list,
		[](void *priv, obs_properties_t *props, obs_property_t *,
		   obs_data_t *settings) {
			int idx = (int)(intptr_t)priv;
			char type_prop[128], pixel_grp[64], pct_grp[64];
			snprintf(type_prop, sizeof(type_prop),
				 "gaze_output_%d_crop_type", idx + 1);
			snprintf(pixel_grp, sizeof(pixel_grp),
				 "gaze_output_%d_crop_pixel", idx + 1);
			snprintf(pct_grp, sizeof(pct_grp),
				 "gaze_output_%d_crop_pct", idx + 1);

			int type = (int)obs_data_get_int(settings, type_prop);
			bool is_pixel = (type == GAZE_CROP_TYPE_PIXEL);

			// Show pixel group when Pixel selected, hide percentage group
			obs_property_set_visible(
				obs_properties_get(props, pixel_grp), is_pixel);
			// Show percentage group when Percentage selected
			obs_property_set_visible(
				obs_properties_get(props, pct_grp), !is_pixel);

			return true;
		},
		(void *)(intptr_t)index);

	snprintf(grp, sizeof(grp), "gaze_output_%d_crop", index + 1);
	obs_properties_add_group(group, grp,
				 obs_module_text("GazePlugin.Crop"),
				 OBS_GROUP_NORMAL, group_crop);

	// === Post-Crop Resolution group (scale cropped result to final output) ===
	obs_properties_t *group_res = obs_properties_create();
	char res_mode_name[128];
	build_prop_name(res_mode_name, sizeof(res_mode_name), index, "resolution_mode");
	auto res_mode =
		obs_properties_add_list(group_res, res_mode_name, "Preset",
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	add_resolution_mode_options(res_mode);

	// Custom resolution fields (only visible when Custom selected)
	build_prop_name(prop_name, sizeof(prop_name), index, "custom_width");
	obs_properties_add_int(group_res, prop_name, "Width", 128, 7680, 1);
	build_prop_name(prop_name, sizeof(prop_name), index, "custom_height");
	obs_properties_add_int(group_res, prop_name, "Height", 72, 4320, 1);

	snprintf(grp, sizeof(grp), "gaze_output_%d_resolution", index + 1);
	obs_properties_add_group(group, grp,
				 obs_module_text("GazePlugin.PostCropResolution"),
				 OBS_GROUP_NORMAL, group_res);

	// Show/hide custom resolution fields based on dropdown selection
	obs_property_set_modified_callback2(
		res_mode,
		[](void *priv, obs_properties_t *props, obs_property_t *,
		   obs_data_t *settings) {
			int idx = (int)(intptr_t)priv;
			char mode_prop[128], w_prop[128], h_prop[128];
			snprintf(mode_prop, sizeof(mode_prop),
				 "gaze_output_%d_resolution_mode", idx + 1);
			snprintf(w_prop, sizeof(w_prop),
				 "gaze_output_%d_custom_width", idx + 1);
			snprintf(h_prop, sizeof(h_prop),
				 "gaze_output_%d_custom_height", idx + 1);
			int mode = (int)obs_data_get_int(settings, mode_prop);
			bool is_custom = (mode == NDI_RESOLUTION_CUSTOM);
			obs_property_set_visible(obs_properties_get(props, w_prop),
						 is_custom);
			obs_property_set_visible(obs_properties_get(props, h_prop),
						 is_custom);
			return true;
		},
		(void *)(intptr_t)index);

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

	// Quality preset (combines bitrate + FEC)
	auto quality_list = obs_properties_add_list(
		props, "gaze_quality_preset",
		obs_module_text("GazePlugin.QualityPreset"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(quality_list, "Ultra (25 Mbps) - Best quality",
				  0);
	obs_property_list_add_int(quality_list, "High (15 Mbps) - Great quality",
				  1);
	obs_property_list_add_int(quality_list,
				  "Balanced (10 Mbps) - WiFi friendly", 2);
	obs_property_list_add_int(quality_list,
				  "Low (5 Mbps) - Save bandwidth", 3);
	obs_property_list_add_int(quality_list, "Custom...", 4);

	// Custom bitrate/FEC (only shown when Custom is selected)
	auto bitrate_prop = obs_properties_add_int(
		props, "gaze_bitrate", obs_module_text("GazePlugin.Bitrate"),
		GAZE_MIN_BITRATE_KBPS, GAZE_MAX_BITRATE_KBPS, 1000);

	auto fec_prop = obs_properties_add_int(
		props, "gaze_fec_percent",
		obs_module_text("GazePlugin.FECPercent"), 0, GAZE_FEC_MAX_PERCENT,
		5);

	// Show/hide custom controls based on preset selection
	obs_property_set_modified_callback2(
		quality_list,
		[](void *, obs_properties_t *props, obs_property_t *,
		   obs_data_t *settings) {
			int preset = (int)obs_data_get_int(settings,
							   "gaze_quality_preset");
			bool is_custom = (preset == 4);
			obs_property_set_visible(
				obs_properties_get(props, "gaze_bitrate"),
				is_custom);
			obs_property_set_visible(
				obs_properties_get(props, "gaze_fec_percent"),
				is_custom);
			return true;
		},
		nullptr);

	// Set initial visibility
	obs_property_set_visible(bitrate_prop, false);
	obs_property_set_visible(fec_prop, false);

	obs_properties_add_bool(props, "stop_when_inactive",
				obs_module_text("GazePlugin.StopWhenInactive"));

	// Network interface selection
	auto net_list = obs_properties_add_list(
		props, "gaze_network_interface",
		obs_module_text("GazePlugin.NetworkInterface"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	// Add "Auto (Primary)" option first
	obs_property_list_add_string(net_list, "Auto (Primary Network)", "");

	// Enumerate network interfaces
	gaze_network_list_t net_interfaces;
	if (gaze_network_get_interfaces(&net_interfaces)) {
		for (int i = 0; i < net_interfaces.count; i++) {
			const gaze_network_interface_t *iface =
				&net_interfaces.interfaces[i];
			// Build display string: "Name (IP)"
			char display[128];
			if (iface->is_primary) {
				snprintf(display, sizeof(display),
					 "%s (%s) [Primary]", iface->name,
					 iface->ip);
			} else {
				snprintf(display, sizeof(display), "%s (%s)",
					 iface->name, iface->ip);
			}
			obs_property_list_add_string(net_list, display,
						     iface->ip);
		}
	}

	// Number of outputs selector
	auto output_count = obs_properties_add_list(
		props, "gaze_output_count",
		obs_module_text("GazePlugin.OutputCount"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int i = 1; i <= MAX_GAZE_OUTPUTS; i++) {
		char label[16];
		snprintf(label, sizeof(label), "%d", i);
		obs_property_list_add_int(output_count, label, i);
	}

	// Per-output settings
	for (int i = 0; i < MAX_GAZE_OUTPUTS; i++)
		add_output_properties(props, i);

	// Show/hide output groups based on output count
	obs_property_set_modified_callback2(
		output_count,
		[](void *, obs_properties_t *props, obs_property_t *,
		   obs_data_t *settings) {
			int count = (int)obs_data_get_int(settings, "gaze_output_count");
			for (int i = 0; i < MAX_GAZE_OUTPUTS; i++) {
				char grp_name[64];
				snprintf(grp_name, sizeof(grp_name),
					 "gaze_output_%d", i + 1);
				obs_property_set_visible(
					obs_properties_get(props, grp_name),
					i < count);
			}
			return true;
		},
		nullptr);

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

	// Per-output quality defaults
	build_prop_name(p, sizeof(p), i, "quality_preset");
	obs_data_set_default_int(d, p, -1); // Use Global by default
	build_prop_name(p, sizeof(p), i, "bitrate");
	obs_data_set_default_int(d, p, GAZE_PRESET_HIGH_BITRATE);
	build_prop_name(p, sizeof(p), i, "fec_percent");
	obs_data_set_default_int(d, p, GAZE_PRESET_HIGH_FEC);

	// Pre-crop resolution defaults
	build_prop_name(p, sizeof(p), i, "precrop_resolution_mode");
	obs_data_set_default_int(d, p, -1); // Auto (Source)
	build_prop_name(p, sizeof(p), i, "precrop_custom_width");
	obs_data_set_default_int(d, p, 1920);
	build_prop_name(p, sizeof(p), i, "precrop_custom_height");
	obs_data_set_default_int(d, p, 1080);

	// Post-crop resolution defaults
	build_prop_name(p, sizeof(p), i, "resolution_mode");
	obs_data_set_default_int(d, p, -1); // Auto (Source)
	build_prop_name(p, sizeof(p), i, "custom_width");
	obs_data_set_default_int(d, p, 1920);
	build_prop_name(p, sizeof(p), i, "custom_height");
	obs_data_set_default_int(d, p, 1080);

	// Crop defaults
	build_prop_name(p, sizeof(p), i, "enable_crop");
	obs_data_set_default_bool(d, p, false);
	build_prop_name(p, sizeof(p), i, "crop_type");
	obs_data_set_default_int(d, p, GAZE_CROP_TYPE_PIXEL);
	build_prop_name(p, sizeof(p), i, "crop_reference");
	obs_data_set_default_int(d, p, GAZE_CROP_REF_TOP_LEFT);
	// Pixel crop defaults
	build_prop_name(p, sizeof(p), i, "crop_ref_x");
	obs_data_set_default_int(d, p, 0);
	build_prop_name(p, sizeof(p), i, "crop_ref_y");
	obs_data_set_default_int(d, p, 0);
	build_prop_name(p, sizeof(p), i, "crop_w");
	obs_data_set_default_int(d, p, 0); // 0 = full width
	build_prop_name(p, sizeof(p), i, "crop_h");
	obs_data_set_default_int(d, p, 0); // 0 = full height
	// Percentage crop defaults
	build_prop_name(p, sizeof(p), i, "crop_ref_x_pct");
	obs_data_set_default_double(d, p, 50.0);
	build_prop_name(p, sizeof(p), i, "crop_ref_y_pct");
	obs_data_set_default_double(d, p, 50.0);
	build_prop_name(p, sizeof(p), i, "crop_w_pct");
	obs_data_set_default_double(d, p, 100.0);
	build_prop_name(p, sizeof(p), i, "crop_h_pct");
	obs_data_set_default_double(d, p, 100.0);

	build_prop_name(p, sizeof(p), i, "framerate_mode");
	obs_data_set_default_int(d, p, -1); // Auto (Source)
	build_prop_name(p, sizeof(p), i, "custom_fps");
	obs_data_set_default_int(d, p, 60);
}

//
// Get defaults
//
static void gaze_filter_getdefaults(obs_data_t *d)
{
	obs_data_set_default_int(d, "gaze_codec", GAZE_CODEC_HEVC);
	obs_data_set_default_int(d, "gaze_encoder_type", GAZE_ENCODER_AUTO);
	obs_data_set_default_int(d, "gaze_quality_preset", 1); // High (15 Mbps)
	obs_data_set_default_int(d, "gaze_bitrate", GAZE_PRESET_HIGH_BITRATE);
	obs_data_set_default_int(d, "gaze_fec_percent", GAZE_PRESET_HIGH_FEC);
	obs_data_set_default_bool(d, "stop_when_inactive", true);
	obs_data_set_default_string(d, "gaze_network_interface", ""); // Auto (Primary)
	obs_data_set_default_int(d, "gaze_output_count", 1); // Default to 1 output

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

	// Read enabled state FIRST to decide whether to preserve discovery
	build_prop_name(p, sizeof(p), i, "enabled");
	bool new_enabled = obs_data_get_bool(s, p);
	bool was_enabled = out->enabled;

	// Stop video_output first
	if (out->video_output) {
		video_output_stop(out->video_output);
		video_output_close(out->video_output);
		out->video_output = nullptr;
	}

	out->known_width = 0;
	out->known_height = 0;
	out->probe_keyframe_cached = false;
	out->last_probe_refresh_ns = 0;

	// Preserve discovery if output stays enabled AND network interface unchanged
	// This fixes the bug where enabling Stream 2 would destroy Stream 1's mDNS
	// But we must recreate discovery if the network interface changed
	bool network_changed = out->discovery.initialized &&
			       out->discovery.ip_addr != f->network_bind_addr;
	bool preserve_discovery = was_enabled && new_enabled &&
				  out->discovery.initialized && !network_changed;

	// Destroy existing components (but preserve discovery if staying enabled)
	if (!preserve_discovery) {
		output_destroy_components(out);
	} else {
		// Destroy everything EXCEPT discovery
		pthread_mutex_lock(&out->output_mutex);

		// Mark stream as inactive before destroying
		gaze_sender_set_active(&out->sender, false);
		gaze_sender_destroy(&out->sender);
		gaze_packetizer_destroy(&out->packetizer);
		gaze_encoder_destroy(&out->encoder);

		out->components_initialized = false;

		pthread_mutex_unlock(&out->output_mutex);
	}

	// Read settings (enabled already read above)
	out->enabled = new_enabled;

	build_prop_name(p, sizeof(p), i, "stream_name");
	strncpy(out->stream_name, obs_data_get_string(s, p),
		sizeof(out->stream_name) - 1);

	// Fixed port assignment: GAZE_BASE_PORT + (output_index * 2)
	out->rtp_port = GAZE_BASE_PORT + (i * 2);

	// Read per-output quality preset and resolve bitrate/FEC
	build_prop_name(p, sizeof(p), i, "quality_preset");
	out->quality_preset = (int)obs_data_get_int(s, p);

	if (out->quality_preset == -1) {
		// Use global settings
		out->bitrate_kbps = f->bitrate_kbps;
		out->fec_percent = f->fec_percent;
	} else {
		switch (out->quality_preset) {
		case 0: // Ultra
			out->bitrate_kbps = GAZE_PRESET_ULTRA_BITRATE;
			out->fec_percent = GAZE_PRESET_ULTRA_FEC;
			break;
		case 1: // High
			out->bitrate_kbps = GAZE_PRESET_HIGH_BITRATE;
			out->fec_percent = GAZE_PRESET_HIGH_FEC;
			break;
		case 2: // Balanced
			out->bitrate_kbps = GAZE_PRESET_BALANCED_BITRATE;
			out->fec_percent = GAZE_PRESET_BALANCED_FEC;
			break;
		case 3: // Low
			out->bitrate_kbps = GAZE_PRESET_LOW_BITRATE;
			out->fec_percent = GAZE_PRESET_LOW_FEC;
			break;
		case 4: // Custom
		default:
			build_prop_name(p, sizeof(p), i, "bitrate");
			out->bitrate_kbps = (uint32_t)obs_data_get_int(s, p);
			build_prop_name(p, sizeof(p), i, "fec_percent");
			out->fec_percent = (uint8_t)obs_data_get_int(s, p);
			break;
		}
	}

	// Build converter settings
	obs_data_t *cs = obs_data_create();

	// Pre-crop resolution: -1 = Auto (disabled), otherwise use the mode
	build_prop_name(p, sizeof(p), i, "precrop_resolution_mode");
	int precrop_res_mode = (int)obs_data_get_int(s, p);
	obs_data_set_bool(cs, "enable_precrop_resolution", precrop_res_mode != -1);
	obs_data_set_int(cs, "precrop_resolution_mode", precrop_res_mode);
	build_prop_name(p, sizeof(p), i, "precrop_custom_width");
	obs_data_set_int(cs, "precrop_custom_width", obs_data_get_int(s, p));
	build_prop_name(p, sizeof(p), i, "precrop_custom_height");
	obs_data_set_int(cs, "precrop_custom_height", obs_data_get_int(s, p));

	// Post-crop resolution: -1 = Auto (disabled), otherwise use the mode
	build_prop_name(p, sizeof(p), i, "resolution_mode");
	int res_mode = (int)obs_data_get_int(s, p);
	obs_data_set_bool(cs, "enable_custom_resolution", res_mode != -1);
	obs_data_set_int(cs, "resolution_mode", res_mode);
	build_prop_name(p, sizeof(p), i, "custom_width");
	obs_data_set_int(cs, "custom_width", obs_data_get_int(s, p));
	build_prop_name(p, sizeof(p), i, "custom_height");
	obs_data_set_int(cs, "custom_height", obs_data_get_int(s, p));

	// Read crop settings into gaze_output fields
	build_prop_name(p, sizeof(p), i, "enable_crop");
	bool enable_crop = obs_data_get_bool(s, p);
	obs_data_set_bool(cs, "enable_crop", enable_crop);

	build_prop_name(p, sizeof(p), i, "crop_type");
	out->crop_type = (enum gaze_crop_type)obs_data_get_int(s, p);
	build_prop_name(p, sizeof(p), i, "crop_reference");
	out->crop_reference = (enum gaze_crop_reference)obs_data_get_int(s, p);

	// Read pixel crop settings
	build_prop_name(p, sizeof(p), i, "crop_ref_x");
	out->crop_ref_x = (int32_t)obs_data_get_int(s, p);
	build_prop_name(p, sizeof(p), i, "crop_ref_y");
	out->crop_ref_y = (int32_t)obs_data_get_int(s, p);
	build_prop_name(p, sizeof(p), i, "crop_w");
	out->crop_w = (uint32_t)obs_data_get_int(s, p);
	build_prop_name(p, sizeof(p), i, "crop_h");
	out->crop_h = (uint32_t)obs_data_get_int(s, p);

	// Read percentage crop settings
	build_prop_name(p, sizeof(p), i, "crop_ref_x_pct");
	out->crop_ref_x_pct = obs_data_get_double(s, p);
	build_prop_name(p, sizeof(p), i, "crop_ref_y_pct");
	out->crop_ref_y_pct = obs_data_get_double(s, p);
	build_prop_name(p, sizeof(p), i, "crop_w_pct");
	out->crop_w_pct = obs_data_get_double(s, p);
	build_prop_name(p, sizeof(p), i, "crop_h_pct");
	out->crop_h_pct = obs_data_get_double(s, p);

	// Crop values are calculated dynamically in process_single_output
	// based on reference point. We just pass placeholder values here.
	obs_data_set_int(cs, "crop_left", 0);
	obs_data_set_int(cs, "crop_top", 0);
	obs_data_set_int(cs, "crop_width", 0);
	obs_data_set_int(cs, "crop_height", 0);

	// Frame rate: -1 = Auto (disabled), otherwise use the mode
	build_prop_name(p, sizeof(p), i, "framerate_mode");
	int fps_mode = (int)obs_data_get_int(s, p);
	obs_data_set_bool(cs, "enable_custom_framerate", fps_mode != -1);
	obs_data_set_int(cs, "framerate_mode", fps_mode);
	build_prop_name(p, sizeof(p), i, "custom_fps");
	obs_data_set_int(cs, "custom_fps_num", obs_data_get_int(s, p));
	obs_data_set_int(cs, "custom_fps_den", 1);

	ndi_converter_update(&out->converter, cs);
	obs_data_release(cs);

	// Mark for lazy initialization (deferred until first non-rapid render)
	// This prevents duplicate sockets/mDNS when OBS creates two filter instances in studio mode
	out->pending_init = false;
	if (out->enabled) {
		out->pending_init = true;
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

	// Apply quality preset (or custom values)
	int preset = (int)obs_data_get_int(s, "gaze_quality_preset");
	switch (preset) {
	case 0: // Ultra
		f->bitrate_kbps = GAZE_PRESET_ULTRA_BITRATE;
		f->fec_percent = GAZE_PRESET_ULTRA_FEC;
		break;
	case 1: // High
		f->bitrate_kbps = GAZE_PRESET_HIGH_BITRATE;
		f->fec_percent = GAZE_PRESET_HIGH_FEC;
		break;
	case 2: // Balanced
		f->bitrate_kbps = GAZE_PRESET_BALANCED_BITRATE;
		f->fec_percent = GAZE_PRESET_BALANCED_FEC;
		break;
	case 3: // Low
		f->bitrate_kbps = GAZE_PRESET_LOW_BITRATE;
		f->fec_percent = GAZE_PRESET_LOW_FEC;
		break;
	case 4: // Custom
	default:
		f->bitrate_kbps = (uint32_t)obs_data_get_int(s, "gaze_bitrate");
		f->fec_percent =
			(uint8_t)obs_data_get_int(s, "gaze_fec_percent");
		break;
	}

	f->stop_when_inactive = obs_data_get_bool(s, "stop_when_inactive");

	// Network interface selection
	const char *net_ip = obs_data_get_string(s, "gaze_network_interface");
	strncpy(f->network_interface_ip, net_ip ? net_ip : "",
		sizeof(f->network_interface_ip) - 1);

	// Resolve to bind_addr and if_index
	f->network_bind_addr = 0;  // Default: any interface
	f->network_if_index = 0;   // Default: all interfaces for mDNS

	if (net_ip && net_ip[0] != '\0') {
		// User selected a specific interface - bind and announce only on that interface
		gaze_network_list_t net_list;
		if (gaze_network_get_interfaces(&net_list)) {
			const gaze_network_interface_t *iface =
				gaze_network_find_by_ip(&net_list, net_ip);
			if (iface) {
				f->network_bind_addr = iface->ip_addr;
				f->network_if_index = iface->index;
				obs_log(LOG_INFO,
					"[gaze-filter] Using network interface: %s (%s), index=%u",
					iface->name, iface->ip, iface->index);
			}
		}
	} else {
		// Auto: bind to any (0), announce mDNS on all interfaces (0)
		// This matches the original behavior before network selection was added
		f->network_bind_addr = 0;
		f->network_if_index = 0;
		obs_log(LOG_INFO,
			"[gaze-filter] Using all network interfaces (Auto)");
	}

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

	// Calculate timestamp offset: wall_clock - obs_time
	// This allows converting OBS frame timestamps to wall clock for latency measurement
	// Use 100ns units (10,000,000 per second) for NDI compatibility
	auto wall_now = std::chrono::system_clock::now();
	int64_t wall_100ns =
		std::chrono::duration_cast<
			std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
			wall_now.time_since_epoch())
			.count();
	int64_t obs_100ns = (int64_t)(os_gettime_ns() / 100);
	f->timestamp_offset_100ns = wall_100ns - obs_100ns;

	for (int i = 0; i < MAX_GAZE_OUTPUTS; i++) {
		gaze_output_t *out = &f->outputs[i];
		pthread_mutex_init(&out->output_mutex, nullptr);
		out->texrender = gs_texrender_create(TEXFORMAT, GS_ZS_NONE);
		out->texrender_precrop = gs_texrender_create(TEXFORMAT, GS_ZS_NONE);
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
		gs_texrender_destroy(out->texrender_precrop);
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

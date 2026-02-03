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

#include "gaze/gaze-protocol.h"
#include "gaze/gaze-encoder.h"
#include "gaze/gaze-packetizer.h"
#include "gaze/gaze-sender.h"
#include "gaze/gaze-discovery.h"
#include "gaze/gaze-network.h"
#include "ndi-video-converter.h"

#include <obs-module.h>
#include <util/threading.h>

#define MAX_GAZE_OUTPUTS 4

/**
 * Crop type for Gaze filter.
 */
enum gaze_crop_type {
	GAZE_CROP_TYPE_PIXEL = 0,         // Absolute pixel coordinates
	GAZE_CROP_TYPE_PERCENTAGE         // Percentage-based (0-100)
};

/**
 * Reference point for crop positioning.
 * Defines the anchor point for Reference X/Y coordinates.
 */
enum gaze_crop_reference {
	GAZE_CROP_REF_TOP_LEFT = 0,       // Default (current behavior)
	GAZE_CROP_REF_TOP_CENTER,
	GAZE_CROP_REF_TOP_RIGHT,
	GAZE_CROP_REF_CENTER_LEFT,
	GAZE_CROP_REF_CENTER,
	GAZE_CROP_REF_CENTER_RIGHT,
	GAZE_CROP_REF_BOTTOM_LEFT,
	GAZE_CROP_REF_BOTTOM_CENTER,
	GAZE_CROP_REF_BOTTOM_RIGHT
};

/**
 * Per-output state for Gaze Stream filter
 */
typedef struct gaze_output {
	// Configuration
	bool enabled;
	char stream_name[256];
	uint16_t rtp_port;  // Fixed: GAZE_BASE_PORT + (output_index * 2)

	// Video converter (resolution/crop/fps)
	ndi_video_converter_t converter;

	// Gaze-specific crop settings (with reference point support)
	// These are processed before passing to converter
	enum gaze_crop_type crop_type;          // Pixel or Percentage
	enum gaze_crop_reference crop_reference; // Anchor point
	int32_t crop_ref_x;                     // Reference X (pixel)
	int32_t crop_ref_y;                     // Reference Y (pixel)
	double crop_ref_x_pct;                  // Reference X (percentage)
	double crop_ref_y_pct;                  // Reference Y (percentage)
	uint32_t crop_w;                        // Width (pixel)
	uint32_t crop_h;                        // Height (pixel)
	double crop_w_pct;                      // Width (percentage)
	double crop_h_pct;                      // Height (percentage)

	// GPU resources
	gs_texrender_t *texrender;          // Post-crop resize
	gs_texrender_t *texrender_precrop;  // Pre-crop resize (optional)
	gs_stagesurf_t *stagesurface;
	uint8_t *video_data;
	uint32_t video_linesize;

	// Decoupled output via video_output
	video_t *video_output;

	// Encoding/streaming components
	gaze_encoder_t encoder;
	gaze_packetizer_t packetizer;
	gaze_sender_t sender;
	gaze_discovery_t discovery;

	// State
	pthread_mutex_t output_mutex;
	uint32_t known_width;
	uint32_t known_height;
	bool components_initialized;
	bool pending_init;  // Lazy init: defer until first non-rapid render
	bool keyframe_requested;
	uint64_t last_render_frame;  // Dedup renders in studio mode (OBS frame counter)

	// Back-reference
	struct gaze_filter *parent;
	int output_index;
} gaze_output_t;

/**
 * Main Gaze Stream filter state
 */
typedef struct gaze_filter {
	obs_source_t *obs_source;

	// Global settings
	gaze_codec_t codec;
	gaze_encoder_type_t encoder_type;
	uint32_t bitrate_kbps;
	uint8_t fec_percent;
	bool stop_when_inactive;

	// Network interface selection
	char network_interface_ip[GAZE_INTERFACE_IP_LEN];  // Selected IP or empty for auto
	uint32_t network_bind_addr;    // IPv4 address in network byte order (0 = any)
	uint32_t network_if_index;     // Interface index for mDNS (0 = all)

	// Per-output state
	gaze_output_t outputs[MAX_GAZE_OUTPUTS];

	// OBS video info
	obs_video_info ovi;

	// Timestamp offset: wall_clock_ms - obs_time_ms at startup
	// Used to convert OBS frame timestamps to wall clock for latency measurement
	int64_t timestamp_offset_ms;
} gaze_filter_t;

/**
 * Create the Gaze Stream filter info structure.
 * Called from plugin-main.cpp to register the filter.
 */
struct obs_source_info create_gaze_filter_info(void);

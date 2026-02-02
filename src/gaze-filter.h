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
#include "ndi-video-converter.h"

#include <obs-module.h>
#include <util/threading.h>

#define MAX_GAZE_OUTPUTS 4

/**
 * Per-output state for Gaze Stream filter
 */
typedef struct gaze_output {
	// Configuration
	bool enabled;
	char stream_name[256];
	char target_host[256];
	uint16_t base_port;
	bool multicast;

	// Video converter (resolution/crop/fps)
	ndi_video_converter_t converter;

	// GPU resources
	gs_texrender_t *texrender;
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

	// Per-output state
	gaze_output_t outputs[MAX_GAZE_OUTPUTS];

	// OBS video info
	obs_video_info ovi;
} gaze_filter_t;

/**
 * Create the Gaze Stream filter info structure.
 * Called from plugin-main.cpp to register the filter.
 */
struct obs_source_info create_gaze_filter_info(void);

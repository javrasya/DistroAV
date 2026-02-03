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

/**
 * Gaze Stream Discovery (mDNS)
 *
 * Announces Gaze Stream services via mDNS/DNS-SD.
 * Allows receivers to discover streams automatically.
 *
 * Service type: _gazestream._udp
 *
 * TXT records:
 * - codec: hevc or h264
 * - width: video width
 * - height: video height
 * - fps: frame rate
 * - version: protocol version
 */

/**
 * Discovery state
 */
typedef struct gaze_discovery {
	// Platform-specific handle
	void *platform_handle;

	// Service info
	char service_name[256];
	uint16_t port;
	gaze_codec_t codec;
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	uint32_t if_index;  // Network interface index (0 = all)

	bool registered;
	bool initialized;
} gaze_discovery_t;

/**
 * Initialize discovery service.
 *
 * @param disc The discovery instance
 * @return true on success, false on failure
 */
bool gaze_discovery_init(gaze_discovery_t *disc);

/**
 * Register a Gaze Stream service.
 *
 * @param disc The discovery instance
 * @param name Service name (human-readable)
 * @param port RTP port
 * @param codec Video codec
 * @param width Video width
 * @param height Video height
 * @param fps Frame rate
 * @param if_index Network interface index (0 = all interfaces)
 * @return true on success, false on failure
 */
bool gaze_discovery_register(gaze_discovery_t *disc, const char *name,
			     uint16_t port, gaze_codec_t codec,
			     uint32_t width, uint32_t height, uint32_t fps,
			     uint32_t if_index);

/**
 * Update service info.
 *
 * @param disc The discovery instance
 * @param width New video width
 * @param height New video height
 * @param fps New frame rate
 * @return true on success, false on failure
 */
bool gaze_discovery_update(gaze_discovery_t *disc, uint32_t width,
			   uint32_t height, uint32_t fps);

/**
 * Unregister the service.
 *
 * @param disc The discovery instance
 */
void gaze_discovery_unregister(gaze_discovery_t *disc);

/**
 * Check if mDNS is available on this system.
 *
 * @return true if available
 */
bool gaze_discovery_available(void);

/**
 * Destroy discovery service.
 *
 * @param disc The discovery instance
 */
void gaze_discovery_destroy(gaze_discovery_t *disc);

#ifdef __cplusplus
}
#endif

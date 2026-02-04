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
#include "gaze-fec.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gaze Stream Packetizer
 *
 * Converts encoded video frames into RTP packets with optional FEC.
 *
 * Each frame is:
 * 1. Split into FEC blocks (if FEC enabled)
 * 2. Each shard becomes an RTP packet
 * 3. Parity shards are added for error recovery
 */

/**
 * Single RTP packet ready for transmission
 */
typedef struct gaze_packet {
	uint8_t data[GAZE_MTU];
	size_t size;
} gaze_packet_t;

/**
 * Packetizer state
 */
typedef struct gaze_packetizer {
	// RTP state
	uint16_t sequence;     // RTP sequence number
	uint32_t ssrc;         // Synchronization source ID
	uint32_t frame_index;  // Frame counter
	uint8_t payload_type;  // RTP payload type (96=HEVC, 97=H.264)

	// FEC
	gaze_fec_t fec;
	bool fec_enabled;

	// Codec
	gaze_codec_t codec;

	// Packet output buffer
	gaze_packet_t *packets;
	size_t packet_capacity;
	size_t packet_count;

	bool initialized;
} gaze_packetizer_t;

/**
 * Initialize the packetizer.
 *
 * @param pkt The packetizer instance
 * @param enable_fec Whether to enable FEC
 * @param fec_percent FEC parity percentage (0-50)
 * @param codec The video codec being used
 * @return true on success, false on failure
 */
bool gaze_packetizer_init(gaze_packetizer_t *pkt, bool enable_fec,
			  uint8_t fec_percent, gaze_codec_t codec);

/**
 * Packetize an encoded frame.
 *
 * @param pkt The packetizer instance
 * @param encoded_data Encoded video data (H.264 or HEVC NAL units)
 * @param encoded_size Size of encoded data
 * @param is_keyframe Whether this is a keyframe
 * @param capture_timestamp Capture timestamp in 100ns units (NDI-compatible)
 * @param packets Output: array of packets (valid until next call)
 * @param packet_count Output: number of packets
 * @return true on success, false on failure
 */
bool gaze_packetizer_packetize(gaze_packetizer_t *pkt,
			       const uint8_t *encoded_data, size_t encoded_size,
			       bool is_keyframe, uint32_t capture_timestamp,
			       gaze_packet_t **packets, size_t *packet_count);

/**
 * Get the current sequence number.
 *
 * @param pkt The packetizer instance
 * @return Current sequence number
 */
uint16_t gaze_packetizer_get_sequence(const gaze_packetizer_t *pkt);

/**
 * Get the current frame index.
 *
 * @param pkt The packetizer instance
 * @return Current frame index
 */
uint32_t gaze_packetizer_get_frame_index(const gaze_packetizer_t *pkt);

/**
 * Reset packetizer state (for stream restart).
 *
 * @param pkt The packetizer instance
 */
void gaze_packetizer_reset(gaze_packetizer_t *pkt);

/**
 * Destroy the packetizer.
 *
 * @param pkt The packetizer instance
 */
void gaze_packetizer_destroy(gaze_packetizer_t *pkt);

#ifdef __cplusplus
}
#endif

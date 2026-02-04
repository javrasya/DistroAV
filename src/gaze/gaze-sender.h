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
#include "gaze-packetizer.h"
#include <util/threading.h>
#include <stdbool.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET gaze_socket_t;
#define GAZE_INVALID_SOCKET INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int gaze_socket_t;
#define GAZE_INVALID_SOCKET -1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gaze Stream Sender
 *
 * Handles UDP/RTP transmission with receiver-initiated subscription.
 *
 * Features:
 * - Receiver-initiated subscription (receivers send subscribe/heartbeat)
 * - Multi-receiver unicast (up to GAZE_MAX_RECEIVERS)
 * - Automatic receiver timeout and cleanup
 * - Large socket buffers
 */

/**
 * Sender statistics
 */
typedef struct gaze_sender_stats {
	uint64_t packets_sent;
	uint64_t bytes_sent;
	uint64_t send_errors;
	uint64_t control_received;
} gaze_sender_stats_t;

/**
 * Single receiver entry
 */
typedef struct gaze_receiver {
	struct sockaddr_in addr;
	uint64_t last_seen_ns;
	bool active;
} gaze_receiver_t;

/**
 * Stream info for probe responses
 */
typedef struct gaze_stream_info {
	uint16_t width;
	uint16_t height;
	uint16_t fps_num;
	uint16_t fps_den;
} gaze_stream_info_t;

/**
 * Pre-built probe response cache (for double-buffering)
 *
 * Pre-builds the complete probe response (header + keyframe) so that
 * probe handlers can send without holding the keyframe mutex during
 * malloc/memcpy operations.
 */
typedef struct gaze_probe_cache {
	uint8_t *response;      // Pre-built: header (24 bytes) + frame data
	size_t response_size;   // Total size including header
	size_t capacity;        // Allocated capacity
	uint32_t frame_width;   // Cached frame dimensions
	uint32_t frame_height;
} gaze_probe_cache_t;

/**
 * Sender state
 */
typedef struct gaze_sender {
	// Sockets
	gaze_socket_t rtp_socket;
	gaze_socket_t rtcp_socket;  // Also used for control messages

	// Network binding
	uint16_t rtp_port;
	uint32_t bind_addr;  // IPv4 address in network byte order (0 = any)

	// Receiver list (subscription-based)
	gaze_receiver_t receivers[GAZE_MAX_RECEIVERS];
	int receiver_count;
	pthread_mutex_t receiver_mutex;

	// Control message listener thread
	pthread_t control_thread;
	volatile bool control_running;

	// Double-buffered probe response cache
	// Buffer 0 or 1 is active; encoder writes to inactive, then swaps
	gaze_probe_cache_t probe_cache[2];
	volatile int probe_cache_active;  // 0 or 1, index of active buffer
#ifdef _WIN32
	SRWLOCK probe_cache_lock;  // Slim reader-writer lock on Windows
#else
	pthread_rwlock_t probe_cache_lock;  // Read-write lock for cache access
#endif

	// Stream info for probe responses (atomic access for status-only probes)
	gaze_stream_info_t stream_info;
	volatile uint32_t frame_count;
	volatile bool stream_active;

	// Statistics
	gaze_sender_stats_t stats;

	bool initialized;
} gaze_sender_t;

/**
 * Initialize the sender.
 *
 * Uses receiver-initiated subscription: receivers discover via mDNS
 * and send subscribe messages to the sender's control port (rtp_port + 1).
 *
 * @param sender The sender instance
 * @param rtp_port RTP port (control uses port + 1)
 * @param bind_addr IPv4 address to bind to (0 = INADDR_ANY)
 * @return true on success, false on failure
 */
bool gaze_sender_init(gaze_sender_t *sender, uint16_t rtp_port,
		      uint32_t bind_addr);

/**
 * Send packets.
 *
 * @param sender The sender instance
 * @param packets Array of packets to send
 * @param packet_count Number of packets
 * @return true on success (some packets may still fail)
 */
bool gaze_sender_send_packets(gaze_sender_t *sender, gaze_packet_t *packets,
			      size_t packet_count);

/**
 * Check if there are active receivers.
 *
 * Returns true if there are subscribed receivers that have sent
 * heartbeats within the timeout period.
 * This can be used to skip encoding when no receivers are connected.
 *
 * @param sender The sender instance
 * @return true if receivers are subscribed
 */
bool gaze_sender_has_receivers(gaze_sender_t *sender);

/**
 * Get the current receiver count.
 *
 * @param sender The sender instance
 * @return Number of active receivers
 */
int gaze_sender_get_receiver_count(gaze_sender_t *sender);

/**
 * Get sender statistics.
 *
 * @param sender The sender instance
 * @param stats Output statistics structure
 */
void gaze_sender_get_stats(gaze_sender_t *sender, gaze_sender_stats_t *stats);

/**
 * Reset sender statistics.
 *
 * @param sender The sender instance
 */
void gaze_sender_reset_stats(gaze_sender_t *sender);

/**
 * Destroy the sender.
 *
 * @param sender The sender instance
 */
void gaze_sender_destroy(gaze_sender_t *sender);

/**
 * Initialize Winsock (Windows only, call once at startup).
 *
 * @return true on success
 */
bool gaze_sender_init_platform(void);

/**
 * Cleanup Winsock (Windows only, call at shutdown).
 */
void gaze_sender_cleanup_platform(void);

/**
 * Cache the latest keyframe for probe responses.
 *
 * Called after encoding a keyframe to store it for clients
 * that request a preview frame via GAZE_CTRL_PROBE.
 *
 * @param sender The sender instance
 * @param data Encoded keyframe data
 * @param size Size of encoded data in bytes
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 */
void gaze_sender_cache_keyframe(gaze_sender_t *sender, const uint8_t *data,
				size_t size, uint32_t width, uint32_t height);

/**
 * Set stream info for probe responses.
 *
 * @param sender The sender instance
 * @param width Stream width
 * @param height Stream height
 * @param fps_num FPS numerator
 * @param fps_den FPS denominator
 */
void gaze_sender_set_stream_info(gaze_sender_t *sender, uint16_t width,
				 uint16_t height, uint16_t fps_num,
				 uint16_t fps_den);

/**
 * Set whether the stream is actively producing frames.
 *
 * @param sender The sender instance
 * @param active true if stream is active
 */
void gaze_sender_set_active(gaze_sender_t *sender, bool active);

/**
 * Increment the frame count (for probe response statistics).
 *
 * @param sender The sender instance
 */
void gaze_sender_increment_frame_count(gaze_sender_t *sender);

#ifdef __cplusplus
}
#endif

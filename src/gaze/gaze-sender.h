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
 * Handles UDP/RTP transmission and RTCP receiver detection.
 *
 * Features:
 * - Rate-controlled UDP sending
 * - RTCP listener for receiver detection
 * - Multicast/unicast support
 * - Large socket buffers
 */

/**
 * Sender statistics
 */
typedef struct gaze_sender_stats {
	uint64_t packets_sent;
	uint64_t bytes_sent;
	uint64_t send_errors;
	uint64_t rtcp_received;
} gaze_sender_stats_t;

/**
 * Sender state
 */
typedef struct gaze_sender {
	// Sockets
	gaze_socket_t rtp_socket;
	gaze_socket_t rtcp_socket;

	// Target endpoint
	struct sockaddr_in target_addr;
	uint16_t rtp_port;
	bool multicast;

	// RTCP receiver detection
	pthread_t rtcp_thread;
	volatile bool rtcp_running;
	volatile uint64_t last_rtcp_received_ns;
	pthread_mutex_t rtcp_mutex;

	// Statistics
	gaze_sender_stats_t stats;

	bool initialized;
} gaze_sender_t;

/**
 * Initialize the sender.
 *
 * @param sender The sender instance
 * @param target_host Target IP address (ignored for multicast)
 * @param rtp_port RTP port (RTCP uses port + 1)
 * @param multicast Whether to use multicast
 * @return true on success, false on failure
 */
bool gaze_sender_init(gaze_sender_t *sender, const char *target_host,
		      uint16_t rtp_port, bool multicast);

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
 * Returns true if RTCP has been received within the timeout period.
 * This can be used to skip encoding when no receivers are connected.
 *
 * @param sender The sender instance
 * @return true if receivers are detected
 */
bool gaze_sender_has_receivers(gaze_sender_t *sender);

/**
 * Force receiver detection to true.
 *
 * Useful for unicast where RTCP may not be available.
 *
 * @param sender The sender instance
 */
void gaze_sender_assume_receiver(gaze_sender_t *sender);

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
 * Update target host (for unicast).
 *
 * @param sender The sender instance
 * @param target_host New target IP address
 * @return true on success, false on failure
 */
bool gaze_sender_set_target(gaze_sender_t *sender, const char *target_host);

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

#ifdef __cplusplus
}
#endif

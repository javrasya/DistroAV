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

#ifdef __cplusplus
}
#endif

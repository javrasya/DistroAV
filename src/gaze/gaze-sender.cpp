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

#include "gaze-sender.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <cstring>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
static bool winsock_initialized = false;
#endif

// Default multicast group
#define GAZE_MULTICAST_GROUP "239.255.42.99"

// Close socket helper
static void close_socket(gaze_socket_t sock)
{
	if (sock == GAZE_INVALID_SOCKET)
		return;

#ifdef _WIN32
	closesocket(sock);
#else
	close(sock);
#endif
}

// Set socket non-blocking
static bool set_nonblocking(gaze_socket_t sock)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
		return false;
	return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool gaze_sender_init_platform(void)
{
#ifdef _WIN32
	if (winsock_initialized)
		return true;

	WSADATA wsa_data;
	int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
	if (result != 0) {
		obs_log(LOG_ERROR,
			"[gaze-sender] WSAStartup failed: %d", result);
		return false;
	}
	winsock_initialized = true;
#endif
	return true;
}

void gaze_sender_cleanup_platform(void)
{
#ifdef _WIN32
	if (winsock_initialized) {
		WSACleanup();
		winsock_initialized = false;
	}
#endif
}

// RTCP listener thread
static void *rtcp_thread_func(void *arg)
{
	gaze_sender_t *sender = (gaze_sender_t *)arg;
	char buffer[1500];

	while (sender->rtcp_running) {
		fd_set read_fds;
		FD_ZERO(&read_fds);
		FD_SET(sender->rtcp_socket, &read_fds);

		struct timeval timeout;
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		int result = select((int)sender->rtcp_socket + 1, &read_fds,
				    nullptr, nullptr, &timeout);

		if (result > 0 && FD_ISSET(sender->rtcp_socket, &read_fds)) {
			struct sockaddr_in from_addr;
			socklen_t from_len = sizeof(from_addr);

			int received = recvfrom(sender->rtcp_socket, buffer,
						sizeof(buffer), 0,
						(struct sockaddr *)&from_addr,
						&from_len);

			if (received >= (int)sizeof(gaze_rtcp_header_t)) {
				gaze_rtcp_header_t *rtcp =
					(gaze_rtcp_header_t *)buffer;

				// Check for valid RTCP packet
				uint8_t version =
					GAZE_RTCP_GET_VERSION(rtcp->flags);
				if (version == 2 &&
				    (rtcp->packet_type == GAZE_RTCP_RR ||
				     rtcp->packet_type == GAZE_RTCP_SR)) {
					pthread_mutex_lock(&sender->rtcp_mutex);
					sender->last_rtcp_received_ns =
						os_gettime_ns();
					sender->stats.rtcp_received++;
					pthread_mutex_unlock(
						&sender->rtcp_mutex);
				}
			}
		}
	}

	return nullptr;
}

bool gaze_sender_init(gaze_sender_t *sender, const char *target_host,
		      uint16_t rtp_port, bool multicast)
{
	if (!sender)
		return false;

	memset(sender, 0, sizeof(*sender));
	sender->rtp_socket = GAZE_INVALID_SOCKET;
	sender->rtcp_socket = GAZE_INVALID_SOCKET;

	// Initialize platform (Winsock on Windows)
	if (!gaze_sender_init_platform()) {
		return false;
	}

	sender->rtp_port = rtp_port;
	sender->multicast = multicast;

	// Create RTP socket
	sender->rtp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sender->rtp_socket == GAZE_INVALID_SOCKET) {
		obs_log(LOG_ERROR, "[gaze-sender] Failed to create RTP socket");
		return false;
	}

	// Create RTCP socket
	sender->rtcp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sender->rtcp_socket == GAZE_INVALID_SOCKET) {
		obs_log(LOG_ERROR,
			"[gaze-sender] Failed to create RTCP socket");
		close_socket(sender->rtp_socket);
		sender->rtp_socket = GAZE_INVALID_SOCKET;
		return false;
	}

	// Set large send buffer
	int send_buf = GAZE_SOCKET_BUFFER_SIZE;
	setsockopt(sender->rtp_socket, SOL_SOCKET, SO_SNDBUF,
		   (const char *)&send_buf, sizeof(send_buf));

	// Set socket options for multicast
	if (multicast) {
		// Set TTL for multicast
		int ttl = 1;
		setsockopt(sender->rtp_socket, IPPROTO_IP, IP_MULTICAST_TTL,
			   (const char *)&ttl, sizeof(ttl));

		// Set loopback off
		int loopback = 0;
		setsockopt(sender->rtp_socket, IPPROTO_IP, IP_MULTICAST_LOOP,
			   (const char *)&loopback, sizeof(loopback));

		// Use default multicast group
		memset(&sender->target_addr, 0, sizeof(sender->target_addr));
		sender->target_addr.sin_family = AF_INET;
		sender->target_addr.sin_port = htons(rtp_port);
		inet_pton(AF_INET, GAZE_MULTICAST_GROUP,
			  &sender->target_addr.sin_addr);
	} else {
		// Unicast - set target address
		if (!target_host || target_host[0] == '\0') {
			obs_log(LOG_ERROR,
				"[gaze-sender] No target host for unicast");
			close_socket(sender->rtp_socket);
			close_socket(sender->rtcp_socket);
			sender->rtp_socket = GAZE_INVALID_SOCKET;
			sender->rtcp_socket = GAZE_INVALID_SOCKET;
			return false;
		}

		memset(&sender->target_addr, 0, sizeof(sender->target_addr));
		sender->target_addr.sin_family = AF_INET;
		sender->target_addr.sin_port = htons(rtp_port);

		if (inet_pton(AF_INET, target_host,
			      &sender->target_addr.sin_addr) != 1) {
			obs_log(LOG_ERROR,
				"[gaze-sender] Invalid target host: %s",
				target_host);
			close_socket(sender->rtp_socket);
			close_socket(sender->rtcp_socket);
			sender->rtp_socket = GAZE_INVALID_SOCKET;
			sender->rtcp_socket = GAZE_INVALID_SOCKET;
			return false;
		}
	}

	// Bind RTCP socket to receive responses
	struct sockaddr_in rtcp_addr;
	memset(&rtcp_addr, 0, sizeof(rtcp_addr));
	rtcp_addr.sin_family = AF_INET;
	rtcp_addr.sin_addr.s_addr = INADDR_ANY;
	rtcp_addr.sin_port = htons(rtp_port + 1);

	// Allow address reuse
	int reuse = 1;
	setsockopt(sender->rtcp_socket, SOL_SOCKET, SO_REUSEADDR,
		   (const char *)&reuse, sizeof(reuse));

	if (bind(sender->rtcp_socket, (struct sockaddr *)&rtcp_addr,
		 sizeof(rtcp_addr)) != 0) {
		obs_log(LOG_WARNING,
			"[gaze-sender] Failed to bind RTCP socket to port %d",
			rtp_port + 1);
		// Continue anyway - RTCP is optional
	}

	// Set RTCP socket non-blocking
	set_nonblocking(sender->rtcp_socket);

	// Initialize RTCP mutex
	pthread_mutex_init(&sender->rtcp_mutex, nullptr);

	// Start RTCP listener thread
	sender->rtcp_running = true;
	if (pthread_create(&sender->rtcp_thread, nullptr, rtcp_thread_func,
			   sender) != 0) {
		obs_log(LOG_WARNING,
			"[gaze-sender] Failed to create RTCP thread");
		sender->rtcp_running = false;
	}

	sender->initialized = true;

	obs_log(LOG_INFO, "[gaze-sender] Initialized: %s:%d (%s)",
		multicast ? GAZE_MULTICAST_GROUP : target_host, rtp_port,
		multicast ? "multicast" : "unicast");

	return true;
}

bool gaze_sender_send_packets(gaze_sender_t *sender, gaze_packet_t *packets,
			      size_t packet_count)
{
	if (!sender || !sender->initialized || !packets || packet_count == 0)
		return false;

	// Send packets in batches
	for (size_t i = 0; i < packet_count; i++) {
		gaze_packet_t *pkt = &packets[i];

		int sent = sendto(sender->rtp_socket, (const char *)pkt->data,
				  (int)pkt->size, 0,
				  (struct sockaddr *)&sender->target_addr,
				  sizeof(sender->target_addr));

		if (sent > 0) {
			sender->stats.packets_sent++;
			sender->stats.bytes_sent += sent;
		} else {
			sender->stats.send_errors++;
		}
	}

	return true;
}

bool gaze_sender_has_receivers(gaze_sender_t *sender)
{
	if (!sender || !sender->initialized)
		return false;

	// For unicast, always assume receiver is present
	// (we're sending to a specific host, no need for RTCP detection)
	if (!sender->multicast)
		return true;

	// For multicast, use RTCP detection
	if (!sender->rtcp_running)
		return true;

	pthread_mutex_lock(&sender->rtcp_mutex);
	uint64_t last_rtcp = sender->last_rtcp_received_ns;
	pthread_mutex_unlock(&sender->rtcp_mutex);

	if (last_rtcp == 0)
		return false;

	// Check if within timeout
	uint64_t now = os_gettime_ns();
	return (now - last_rtcp) < GAZE_RTCP_TIMEOUT_NS;
}

void gaze_sender_assume_receiver(gaze_sender_t *sender)
{
	if (!sender)
		return;

	pthread_mutex_lock(&sender->rtcp_mutex);
	sender->last_rtcp_received_ns = os_gettime_ns();
	pthread_mutex_unlock(&sender->rtcp_mutex);
}

void gaze_sender_get_stats(gaze_sender_t *sender, gaze_sender_stats_t *stats)
{
	if (!sender || !stats)
		return;

	*stats = sender->stats;
}

void gaze_sender_reset_stats(gaze_sender_t *sender)
{
	if (!sender)
		return;

	memset(&sender->stats, 0, sizeof(sender->stats));
}

bool gaze_sender_set_target(gaze_sender_t *sender, const char *target_host)
{
	if (!sender || !sender->initialized || sender->multicast)
		return false;

	if (!target_host || target_host[0] == '\0')
		return false;

	struct sockaddr_in new_addr;
	memset(&new_addr, 0, sizeof(new_addr));
	new_addr.sin_family = AF_INET;
	new_addr.sin_port = htons(sender->rtp_port);

	if (inet_pton(AF_INET, target_host, &new_addr.sin_addr) != 1) {
		return false;
	}

	sender->target_addr = new_addr;
	return true;
}

void gaze_sender_destroy(gaze_sender_t *sender)
{
	if (!sender || !sender->initialized)
		return;

	// Stop RTCP thread
	if (sender->rtcp_running) {
		sender->rtcp_running = false;
		pthread_join(sender->rtcp_thread, nullptr);
	}

	pthread_mutex_destroy(&sender->rtcp_mutex);

	// Close sockets
	close_socket(sender->rtcp_socket);
	close_socket(sender->rtp_socket);

	sender->rtp_socket = GAZE_INVALID_SOCKET;
	sender->rtcp_socket = GAZE_INVALID_SOCKET;
	sender->initialized = false;
}

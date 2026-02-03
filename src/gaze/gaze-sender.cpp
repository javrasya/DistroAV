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

// Compare sockaddr_in addresses
static bool addr_equals(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
	       a->sin_port == b->sin_port;
}

// Add or update a receiver in the list
static void add_or_update_receiver(gaze_sender_t *sender,
				   const struct sockaddr_in *from_addr)
{
	uint64_t now = os_gettime_ns();

	// Check if receiver already exists
	for (int i = 0; i < GAZE_MAX_RECEIVERS; i++) {
		if (sender->receivers[i].active &&
		    addr_equals(&sender->receivers[i].addr, from_addr)) {
			// Update last seen time
			sender->receivers[i].last_seen_ns = now;
			return;
		}
	}

	// Find empty slot
	for (int i = 0; i < GAZE_MAX_RECEIVERS; i++) {
		if (!sender->receivers[i].active) {
			sender->receivers[i].addr = *from_addr;
			sender->receivers[i].last_seen_ns = now;
			sender->receivers[i].active = true;
			sender->receiver_count++;

			char ip_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &from_addr->sin_addr, ip_str,
				  sizeof(ip_str));
			obs_log(LOG_INFO,
				"[gaze-sender] Receiver subscribed: %s:%d (total: %d)",
				ip_str, ntohs(from_addr->sin_port),
				sender->receiver_count);
			return;
		}
	}

	// No slots available
	obs_log(LOG_WARNING,
		"[gaze-sender] Max receivers reached (%d), ignoring new subscription",
		GAZE_MAX_RECEIVERS);
}

// Remove a receiver from the list
static void remove_receiver(gaze_sender_t *sender,
			    const struct sockaddr_in *from_addr)
{
	for (int i = 0; i < GAZE_MAX_RECEIVERS; i++) {
		if (sender->receivers[i].active &&
		    addr_equals(&sender->receivers[i].addr, from_addr)) {
			char ip_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &from_addr->sin_addr, ip_str,
				  sizeof(ip_str));
			obs_log(LOG_INFO,
				"[gaze-sender] Receiver unsubscribed: %s:%d",
				ip_str, ntohs(from_addr->sin_port));

			sender->receivers[i].active = false;
			sender->receiver_count--;
			return;
		}
	}
}

// Prune receivers that haven't sent heartbeats within timeout
static void prune_inactive_receivers(gaze_sender_t *sender)
{
	uint64_t now = os_gettime_ns();

	for (int i = 0; i < GAZE_MAX_RECEIVERS; i++) {
		if (sender->receivers[i].active &&
		    (now - sender->receivers[i].last_seen_ns) > GAZE_RECEIVER_TIMEOUT_NS) {
			char ip_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &sender->receivers[i].addr.sin_addr,
				  ip_str, sizeof(ip_str));
			obs_log(LOG_INFO,
				"[gaze-sender] Receiver timed out: %s:%d",
				ip_str, ntohs(sender->receivers[i].addr.sin_port));

			sender->receivers[i].active = false;
			sender->receiver_count--;
		}
	}
}

// Handle incoming control message
static void handle_control_message(gaze_sender_t *sender,
				   const uint8_t *data, int len,
				   struct sockaddr_in *from_addr)
{
	if (len < 4)
		return;

	// Check magic bytes
	if (data[0] != GAZE_CTRL_MAGIC_0 || data[1] != GAZE_CTRL_MAGIC_1)
		return;

	uint8_t msg_type = data[2];

	pthread_mutex_lock(&sender->receiver_mutex);

	switch (msg_type) {
	case GAZE_CTRL_SUBSCRIBE:
	case GAZE_CTRL_HEARTBEAT:
		add_or_update_receiver(sender, from_addr);
		sender->stats.control_received++;
		break;
	case GAZE_CTRL_UNSUBSCRIBE:
		remove_receiver(sender, from_addr);
		sender->stats.control_received++;
		break;
	default:
		// Unknown message type, ignore
		break;
	}

	pthread_mutex_unlock(&sender->receiver_mutex);
}

// Control message listener thread
static void *control_thread_func(void *arg)
{
	gaze_sender_t *sender = (gaze_sender_t *)arg;
	uint8_t buffer[64];

	while (sender->control_running) {
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

			int received = recvfrom(sender->rtcp_socket,
						(char *)buffer, sizeof(buffer),
						0,
						(struct sockaddr *)&from_addr,
						&from_len);

			if (received > 0) {
				handle_control_message(sender, buffer, received,
						       &from_addr);
			}
		}
	}

	return nullptr;
}

bool gaze_sender_init(gaze_sender_t *sender, uint16_t rtp_port,
		      uint32_t bind_addr)
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
	sender->bind_addr = bind_addr;

	// Create RTP socket
	sender->rtp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sender->rtp_socket == GAZE_INVALID_SOCKET) {
		obs_log(LOG_ERROR, "[gaze-sender] Failed to create RTP socket");
		return false;
	}

	// Create control/RTCP socket
	sender->rtcp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sender->rtcp_socket == GAZE_INVALID_SOCKET) {
		obs_log(LOG_ERROR,
			"[gaze-sender] Failed to create control socket");
		close_socket(sender->rtp_socket);
		sender->rtp_socket = GAZE_INVALID_SOCKET;
		return false;
	}

	// Set large send buffer
	int send_buf = GAZE_SOCKET_BUFFER_SIZE;
	setsockopt(sender->rtp_socket, SOL_SOCKET, SO_SNDBUF,
		   (const char *)&send_buf, sizeof(send_buf));

	// Bind control socket to receive subscriptions
	struct sockaddr_in ctrl_addr;
	memset(&ctrl_addr, 0, sizeof(ctrl_addr));
	ctrl_addr.sin_family = AF_INET;
	ctrl_addr.sin_addr.s_addr = bind_addr;  // 0 = INADDR_ANY
	ctrl_addr.sin_port = htons(rtp_port + 1);

	// Allow address reuse
	int reuse = 1;
	setsockopt(sender->rtcp_socket, SOL_SOCKET, SO_REUSEADDR,
		   (const char *)&reuse, sizeof(reuse));

	if (bind(sender->rtcp_socket, (struct sockaddr *)&ctrl_addr,
		 sizeof(ctrl_addr)) != 0) {
		obs_log(LOG_ERROR,
			"[gaze-sender] Failed to bind control socket to port %d",
			rtp_port + 1);
		close_socket(sender->rtp_socket);
		close_socket(sender->rtcp_socket);
		sender->rtp_socket = GAZE_INVALID_SOCKET;
		sender->rtcp_socket = GAZE_INVALID_SOCKET;
		return false;
	}

	// Set control socket non-blocking
	set_nonblocking(sender->rtcp_socket);

	// Initialize receiver mutex
	pthread_mutex_init(&sender->receiver_mutex, nullptr);

	// Start control message listener thread
	sender->control_running = true;
	if (pthread_create(&sender->control_thread, nullptr, control_thread_func,
			   sender) != 0) {
		obs_log(LOG_ERROR,
			"[gaze-sender] Failed to create control thread");
		pthread_mutex_destroy(&sender->receiver_mutex);
		close_socket(sender->rtp_socket);
		close_socket(sender->rtcp_socket);
		sender->rtp_socket = GAZE_INVALID_SOCKET;
		sender->rtcp_socket = GAZE_INVALID_SOCKET;
		return false;
	}

	sender->initialized = true;

	obs_log(LOG_INFO,
		"[gaze-sender] Initialized on port %d (control: %d), waiting for receivers",
		rtp_port, rtp_port + 1);

	return true;
}

bool gaze_sender_send_packets(gaze_sender_t *sender, gaze_packet_t *packets,
			      size_t packet_count)
{
	if (!sender || !sender->initialized || !packets || packet_count == 0)
		return false;

	pthread_mutex_lock(&sender->receiver_mutex);

	// Prune timed-out receivers
	prune_inactive_receivers(sender);

	// Send to each active receiver
	for (int r = 0; r < GAZE_MAX_RECEIVERS; r++) {
		if (!sender->receivers[r].active)
			continue;

		for (size_t i = 0; i < packet_count; i++) {
			gaze_packet_t *pkt = &packets[i];

			int sent = sendto(sender->rtp_socket,
					  (const char *)pkt->data,
					  (int)pkt->size, 0,
					  (struct sockaddr *)&sender->receivers[r].addr,
					  sizeof(sender->receivers[r].addr));

			if (sent > 0) {
				sender->stats.packets_sent++;
				sender->stats.bytes_sent += sent;
			} else {
				sender->stats.send_errors++;
			}
		}
	}

	pthread_mutex_unlock(&sender->receiver_mutex);

	return true;
}

bool gaze_sender_has_receivers(gaze_sender_t *sender)
{
	if (!sender || !sender->initialized)
		return false;

	pthread_mutex_lock(&sender->receiver_mutex);
	prune_inactive_receivers(sender);
	bool has = (sender->receiver_count > 0);
	pthread_mutex_unlock(&sender->receiver_mutex);

	return has;
}

int gaze_sender_get_receiver_count(gaze_sender_t *sender)
{
	if (!sender || !sender->initialized)
		return 0;

	pthread_mutex_lock(&sender->receiver_mutex);
	prune_inactive_receivers(sender);
	int count = sender->receiver_count;
	pthread_mutex_unlock(&sender->receiver_mutex);

	return count;
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

void gaze_sender_destroy(gaze_sender_t *sender)
{
	if (!sender || !sender->initialized)
		return;

	// Stop control thread
	if (sender->control_running) {
		sender->control_running = false;
		pthread_join(sender->control_thread, nullptr);
	}

	pthread_mutex_destroy(&sender->receiver_mutex);

	// Close sockets
	close_socket(sender->rtcp_socket);
	close_socket(sender->rtp_socket);

	sender->rtp_socket = GAZE_INVALID_SOCKET;
	sender->rtcp_socket = GAZE_INVALID_SOCKET;
	sender->initialized = false;
}

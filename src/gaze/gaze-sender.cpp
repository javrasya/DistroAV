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
#include <cstdlib>

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

// Cross-platform read-write lock helpers
#ifdef _WIN32
static void rwlock_init(SRWLOCK *lock) { InitializeSRWLock(lock); }
static void rwlock_destroy(SRWLOCK *lock) { (void)lock; /* SRWLock needs no cleanup */ }
static void rwlock_rdlock(SRWLOCK *lock) { AcquireSRWLockShared(lock); }
static void rwlock_rdunlock(SRWLOCK *lock) { ReleaseSRWLockShared(lock); }
static void rwlock_wrlock(SRWLOCK *lock) { AcquireSRWLockExclusive(lock); }
static void rwlock_wrunlock(SRWLOCK *lock) { ReleaseSRWLockExclusive(lock); }
#else
static void rwlock_init(pthread_rwlock_t *lock) { pthread_rwlock_init(lock, nullptr); }
static void rwlock_destroy(pthread_rwlock_t *lock) { pthread_rwlock_destroy(lock); }
static void rwlock_rdlock(pthread_rwlock_t *lock) { pthread_rwlock_rdlock(lock); }
static void rwlock_rdunlock(pthread_rwlock_t *lock) { pthread_rwlock_unlock(lock); }
static void rwlock_wrlock(pthread_rwlock_t *lock) { pthread_rwlock_wrlock(lock); }
static void rwlock_wrunlock(pthread_rwlock_t *lock) { pthread_rwlock_unlock(lock); }
#endif

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
// Throttled: only runs once per second to avoid per-frame overhead
static void prune_inactive_receivers(gaze_sender_t *sender)
{
	uint64_t now = os_gettime_ns();

	// Only prune once per second (timeout is 5s, so 1s granularity is fine)
	if (now - sender->last_prune_ns < 1000000000ULL)
		return;
	sender->last_prune_ns = now;

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

// Handle PROBE message and send response
// Optimized with two fast paths:
// 1. Status-only: no locks, uses volatile atomic reads
// 2. With frame: uses read lock on pre-built cache (no malloc/memcpy in hot path)
static void handle_probe_message(gaze_sender_t *sender, const uint8_t *data,
				 int len, struct sockaddr_in *from_addr)
{
	if (len < GAZE_PROBE_REQUEST_SIZE)
		return;

	uint8_t flags = data[3];
	uint32_t request_id = gaze_ntohl(*(uint32_t *)(data + 4));
	bool request_frame = (flags & GAZE_PROBE_FLAG_REQUEST_FRAME) != 0;

	char ip_str[INET_ADDRSTRLEN];

	// FAST PATH: Status-only probe (no frame requested)
	// No locks needed - uses volatile reads for atomic access
	if (!request_frame) {
		uint8_t response[GAZE_PROBE_RESPONSE_HEADER_SIZE];

		// Build header
		response[0] = GAZE_CTRL_MAGIC_0;
		response[1] = GAZE_CTRL_MAGIC_1;
		response[2] = GAZE_CTRL_PROBE_RESPONSE;

		// Status flags (volatile reads - atomic on all platforms)
		uint8_t status = 0;
		if (sender->stream_active)
			status |= GAZE_PROBE_STATUS_ACTIVE;
		if (sender->receiver_count > 0)
			status |= GAZE_PROBE_STATUS_HAS_RECEIVERS;
		// No frame included
		response[3] = status;

		// Request ID (echoed)
		*(uint32_t *)(response + 4) = gaze_htonl(request_id);

		// Stream info
		*(uint16_t *)(response + 8) = gaze_htons(sender->stream_info.width);
		*(uint16_t *)(response + 10) = gaze_htons(sender->stream_info.height);
		*(uint16_t *)(response + 12) = gaze_htons(sender->stream_info.fps_num);
		*(uint16_t *)(response + 14) = gaze_htons(sender->stream_info.fps_den);

		// Frame count (volatile read)
		*(uint32_t *)(response + 16) = gaze_htonl(sender->frame_count);

		// Frame data size = 0
		*(uint32_t *)(response + 20) = 0;

		// Send response
		sendto(sender->rtcp_socket, (const char *)response,
		       GAZE_PROBE_RESPONSE_HEADER_SIZE, 0,
		       (struct sockaddr *)from_addr, sizeof(*from_addr));

		inet_ntop(AF_INET, &from_addr->sin_addr, ip_str, sizeof(ip_str));
		obs_log(LOG_DEBUG,
			"[gaze-sender] Probe response (status-only) sent to %s:%d (status=0x%02x)",
			ip_str, ntohs(from_addr->sin_port), status);
		return;
	}

	// FRAME PATH: Use pre-built cache with read lock
	rwlock_rdlock(&sender->probe_cache_lock);

	int active_idx = sender->probe_cache_active;
	gaze_probe_cache_t *cache = &sender->probe_cache[active_idx];

	if (!cache->response || cache->response_size == 0) {
		// No cached frame available - send status-only response
		rwlock_rdunlock(&sender->probe_cache_lock);

		uint8_t response[GAZE_PROBE_RESPONSE_HEADER_SIZE];
		response[0] = GAZE_CTRL_MAGIC_0;
		response[1] = GAZE_CTRL_MAGIC_1;
		response[2] = GAZE_CTRL_PROBE_RESPONSE;

		uint8_t status = 0;
		if (sender->stream_active)
			status |= GAZE_PROBE_STATUS_ACTIVE;
		if (sender->receiver_count > 0)
			status |= GAZE_PROBE_STATUS_HAS_RECEIVERS;
		response[3] = status;

		*(uint32_t *)(response + 4) = gaze_htonl(request_id);
		*(uint16_t *)(response + 8) = gaze_htons(sender->stream_info.width);
		*(uint16_t *)(response + 10) = gaze_htons(sender->stream_info.height);
		*(uint16_t *)(response + 12) = gaze_htons(sender->stream_info.fps_num);
		*(uint16_t *)(response + 14) = gaze_htons(sender->stream_info.fps_den);
		*(uint32_t *)(response + 16) = gaze_htonl(sender->frame_count);
		*(uint32_t *)(response + 20) = 0;

		sendto(sender->rtcp_socket, (const char *)response,
		       GAZE_PROBE_RESPONSE_HEADER_SIZE, 0,
		       (struct sockaddr *)from_addr, sizeof(*from_addr));

		inet_ntop(AF_INET, &from_addr->sin_addr, ip_str, sizeof(ip_str));
		obs_log(LOG_DEBUG,
			"[gaze-sender] Probe response (no frame cached) sent to %s:%d",
			ip_str, ntohs(from_addr->sin_port));
		return;
	}

	// Patch the pre-built response with current request_id and status
	// We modify bytes 3-7 (status byte and request_id) before sending
	// The frame data at offset 24+ is already correct in the cache
	uint8_t *response = cache->response;
	size_t response_size = cache->response_size;
	size_t frame_size = response_size - GAZE_PROBE_RESPONSE_HEADER_SIZE;

	// Update status flags (volatile reads for current state)
	uint8_t status = GAZE_PROBE_STATUS_FRAME_INCLUDED;  // Frame is included
	if (sender->stream_active)
		status |= GAZE_PROBE_STATUS_ACTIVE;
	if (sender->receiver_count > 0)
		status |= GAZE_PROBE_STATUS_HAS_RECEIVERS;
	response[3] = status;

	// Update request ID
	*(uint32_t *)(response + 4) = gaze_htonl(request_id);

	// Update frame count (volatile read)
	*(uint32_t *)(response + 16) = gaze_htonl(sender->frame_count);

	// Send pre-built response directly (no copy needed)
	sendto(sender->rtcp_socket, (const char *)response, (int)response_size, 0,
	       (struct sockaddr *)from_addr, sizeof(*from_addr));

	rwlock_rdunlock(&sender->probe_cache_lock);

	inet_ntop(AF_INET, &from_addr->sin_addr, ip_str, sizeof(ip_str));
	obs_log(LOG_DEBUG,
		"[gaze-sender] Probe response sent to %s:%d (status=0x%02x, frame=%zu bytes)",
		ip_str, ntohs(from_addr->sin_port), status, frame_size);
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

	switch (msg_type) {
	case GAZE_CTRL_SUBSCRIBE:
	case GAZE_CTRL_HEARTBEAT:
		pthread_mutex_lock(&sender->receiver_mutex);
		add_or_update_receiver(sender, from_addr);
		sender->stats.control_received++;
		pthread_mutex_unlock(&sender->receiver_mutex);
		break;
	case GAZE_CTRL_UNSUBSCRIBE:
		pthread_mutex_lock(&sender->receiver_mutex);
		remove_receiver(sender, from_addr);
		sender->stats.control_received++;
		pthread_mutex_unlock(&sender->receiver_mutex);
		break;
	case GAZE_CTRL_PROBE:
		handle_probe_message(sender, data, len, from_addr);
		sender->stats.control_received++;
		break;
	default:
		// Unknown message type, ignore
		break;
	}
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

	// Initialize double-buffered probe cache
	rwlock_init(&sender->probe_cache_lock);
	sender->probe_cache_active = 0;
	for (int i = 0; i < 2; i++) {
		sender->probe_cache[i].response = nullptr;
		sender->probe_cache[i].response_size = 0;
		sender->probe_cache[i].capacity = 0;
		sender->probe_cache[i].frame_width = 0;
		sender->probe_cache[i].frame_height = 0;
	}
	sender->frame_count = 0;
	sender->stream_active = false;
	memset(&sender->stream_info, 0, sizeof(sender->stream_info));

	// Start control message listener thread
	sender->control_running = true;
	if (pthread_create(&sender->control_thread, nullptr, control_thread_func,
			   sender) != 0) {
		obs_log(LOG_ERROR,
			"[gaze-sender] Failed to create control thread");
		pthread_mutex_destroy(&sender->receiver_mutex);
		rwlock_destroy(&sender->probe_cache_lock);
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

	// Lock-free read: receiver_count is updated under mutex by
	// add_or_update_receiver, remove_receiver, and prune_inactive_receivers.
	// A brief stale read is acceptable since the timeout is 5 seconds
	// and pruning happens in send_packets().
	return (sender->receiver_count > 0);
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

	// Free double-buffered probe cache
	rwlock_wrlock(&sender->probe_cache_lock);
	for (int i = 0; i < 2; i++) {
		if (sender->probe_cache[i].response) {
			free(sender->probe_cache[i].response);
			sender->probe_cache[i].response = nullptr;
			sender->probe_cache[i].response_size = 0;
			sender->probe_cache[i].capacity = 0;
		}
	}
	rwlock_wrunlock(&sender->probe_cache_lock);
	rwlock_destroy(&sender->probe_cache_lock);

	// Close sockets
	close_socket(sender->rtcp_socket);
	close_socket(sender->rtp_socket);

	sender->rtp_socket = GAZE_INVALID_SOCKET;
	sender->rtcp_socket = GAZE_INVALID_SOCKET;
	sender->initialized = false;
}

void gaze_sender_cache_keyframe(gaze_sender_t *sender, const uint8_t *data,
				size_t size, uint32_t width, uint32_t height)
{
	if (!sender || !sender->initialized || !data || size == 0)
		return;

	// Build complete probe response into the INACTIVE buffer
	// This happens outside the critical section for minimal lock hold time
	int inactive_idx = 1 - sender->probe_cache_active;
	gaze_probe_cache_t *cache = &sender->probe_cache[inactive_idx];

	size_t response_size = GAZE_PROBE_RESPONSE_HEADER_SIZE + size;

	// Reallocate buffer if needed (outside lock)
	if (cache->capacity < response_size) {
		size_t new_capacity = response_size + (response_size / 4);  // 25% extra
		uint8_t *new_buf = (uint8_t *)realloc(cache->response, new_capacity);
		if (!new_buf) {
			return;  // Allocation failed, keep old cache
		}
		cache->response = new_buf;
		cache->capacity = new_capacity;
	}

	// Build the complete response header
	uint8_t *response = cache->response;
	response[0] = GAZE_CTRL_MAGIC_0;
	response[1] = GAZE_CTRL_MAGIC_1;
	response[2] = GAZE_CTRL_PROBE_RESPONSE;
	response[3] = 0;  // Status flags - will be patched at send time

	// Request ID placeholder (will be patched at send time)
	*(uint32_t *)(response + 4) = 0;

	// Stream info - use frame dimensions
	*(uint16_t *)(response + 8) = gaze_htons((uint16_t)width);
	*(uint16_t *)(response + 10) = gaze_htons((uint16_t)height);
	*(uint16_t *)(response + 12) = gaze_htons(sender->stream_info.fps_num);
	*(uint16_t *)(response + 14) = gaze_htons(sender->stream_info.fps_den);

	// Frame count placeholder (will be patched at send time)
	*(uint32_t *)(response + 16) = 0;

	// Frame data size
	*(uint32_t *)(response + 20) = gaze_htonl((uint32_t)size);

	// Copy frame data
	memcpy(response + GAZE_PROBE_RESPONSE_HEADER_SIZE, data, size);

	// Update cache metadata
	cache->response_size = response_size;
	cache->frame_width = width;
	cache->frame_height = height;

	// Atomic swap: briefly hold write lock to update active index
	rwlock_wrlock(&sender->probe_cache_lock);
	sender->probe_cache_active = inactive_idx;
	rwlock_wrunlock(&sender->probe_cache_lock);
}

void gaze_sender_set_stream_info(gaze_sender_t *sender, uint16_t width,
				 uint16_t height, uint16_t fps_num,
				 uint16_t fps_den)
{
	if (!sender)
		return;

	sender->stream_info.width = width;
	sender->stream_info.height = height;
	sender->stream_info.fps_num = fps_num;
	sender->stream_info.fps_den = fps_den;
}

void gaze_sender_set_active(gaze_sender_t *sender, bool active)
{
	if (!sender)
		return;

	sender->stream_active = active;
}

void gaze_sender_increment_frame_count(gaze_sender_t *sender)
{
	if (!sender)
		return;

	sender->frame_count++;
}

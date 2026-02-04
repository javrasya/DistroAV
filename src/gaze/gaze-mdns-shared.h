/******************************************************************************
    Copyright (C) 2016-2024 DistroAV <contact@distroav.org>

    Shared mDNS manager for Windows.
    Provides a single mDNS socket per network interface that can announce
    multiple services. This is required because mDNS uses a fixed port (5353)
    and multicast group, so multiple sockets on the same interface conflict.
******************************************************************************/

#pragma once

#ifdef _WIN32

#include <WinSock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define MDNS_PORT 5353
#define MDNS_MULTICAST_ADDR "224.0.0.251"
#define GAZE_MDNS_MAX_SERVICES 16

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
struct gaze_mdns_shared;

/**
 * A single mDNS service entry
 */
typedef struct gaze_mdns_service {
	bool active;
	char service_name[256];
	char service_type[64];
	uint16_t port;
	uint8_t txt_records[512];
	size_t txt_len;
} gaze_mdns_service_t;

/**
 * Shared mDNS instance for a network interface.
 * Manages a single socket that announces multiple services.
 */
typedef struct gaze_mdns_shared {
	// Socket and network
	int sock;
	uint32_t bind_ip;      // Actual IP for A records (may be auto-detected)
	uint32_t lookup_ip;    // Original bind_ip parameter (for instance lookup)
	struct sockaddr_in mcast_addr;
	char hostname[256];

	// Registered services
	gaze_mdns_service_t services[GAZE_MDNS_MAX_SERVICES];
	int service_count;
	pthread_mutex_t services_mutex;

	// Announcement thread
	pthread_t announce_thread;
	bool thread_running;
	bool initialized;

	// Reference counting
	int ref_count;

	// Link for global list
	struct gaze_mdns_shared *next;
} gaze_mdns_shared_t;

// Global list of shared instances (one per interface)
static gaze_mdns_shared_t *g_mdns_instances = NULL;
static pthread_mutex_t g_mdns_mutex = PTHREAD_MUTEX_INITIALIZER;

// Helper to write a DNS name (label format)
static inline size_t mdns_write_dns_name(uint8_t *buf, const char *name)
{
	size_t pos = 0;
	const char *p = name;

	while (*p) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);
		if (len > 63)
			len = 63;
		buf[pos++] = (uint8_t)len;
		memcpy(buf + pos, p, len);
		pos += len;
		p += len;
		if (*p == '.')
			p++;
	}
	buf[pos++] = 0; // Null terminator
	return pos;
}

// Helper to read a DNS name from a packet (handles compression)
static inline size_t mdns_read_dns_name(const uint8_t *packet, size_t packet_len,
					size_t offset, char *out, size_t out_size)
{
	size_t pos = offset;
	size_t out_pos = 0;
	bool jumped = false;
	size_t jump_pos = 0;
	int jumps = 0;

	while (pos < packet_len && jumps < 10) {
		uint8_t len = packet[pos];
		if (len == 0) {
			pos++;
			break;
		}

		// Check for compression pointer
		if ((len & 0xC0) == 0xC0) {
			if (pos + 1 >= packet_len)
				break;
			if (!jumped) {
				jump_pos = pos + 2;
				jumped = true;
			}
			pos = ((len & 0x3F) << 8) | packet[pos + 1];
			jumps++;
			continue;
		}

		pos++;
		if (pos + len > packet_len)
			break;

		if (out_pos > 0 && out_pos < out_size - 1) {
			out[out_pos++] = '.';
		}

		size_t copy_len = len;
		if (out_pos + copy_len >= out_size - 1)
			copy_len = out_size - 1 - out_pos;
		memcpy(out + out_pos, packet + pos, copy_len);
		out_pos += copy_len;
		pos += len;
	}

	out[out_pos] = '\0';

	// Convert to lowercase for comparison
	for (size_t i = 0; i < out_pos; i++) {
		if (out[i] >= 'A' && out[i] <= 'Z')
			out[i] = out[i] + ('a' - 'A');
	}

	return jumped ? jump_pos : pos;
}

// Build response packet for a single service
static inline size_t mdns_build_service_response(gaze_mdns_shared_t *mdns,
						  gaze_mdns_service_t *svc,
						  uint8_t *packet, size_t capacity)
{
	if (!mdns || !svc || !packet || capacity < 512)
		return 0;

	// Build service type with .local suffix
	char service_type_local[128];
	snprintf(service_type_local, sizeof(service_type_local), "%s.local",
		 svc->service_type);

	// Build instance name: "ServiceName._gazestream._udp.local"
	char instance_name[384];
	snprintf(instance_name, sizeof(instance_name), "%s.%s.local",
		 svc->service_name, svc->service_type);

	size_t pos = 0;

	// DNS Header (12 bytes)
	packet[pos++] = 0;    // Transaction ID
	packet[pos++] = 0;
	packet[pos++] = 0x84; // Flags: Response, Authoritative
	packet[pos++] = 0x00;
	packet[pos++] = 0;    // Questions: 0
	packet[pos++] = 0;
	packet[pos++] = 0;    // Answers: 4 (PTR, SRV, TXT, A)
	packet[pos++] = 4;
	packet[pos++] = 0;    // Authority: 0
	packet[pos++] = 0;
	packet[pos++] = 0;    // Additional: 0
	packet[pos++] = 0;

	// PTR Record: _gazestream._udp.local -> instance
	pos += mdns_write_dns_name(packet + pos, service_type_local);
	packet[pos++] = 0;
	packet[pos++] = 12;   // Type: PTR
	packet[pos++] = 0x80;
	packet[pos++] = 1;    // Class: IN with cache flush
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 0x11;
	packet[pos++] = 0x94; // TTL: 4500 seconds
	size_t ptr_rdlen_pos = pos;
	pos += 2;
	size_t ptr_rdata_start = pos;
	pos += mdns_write_dns_name(packet + pos, instance_name);
	size_t ptr_rdlen = pos - ptr_rdata_start;
	packet[ptr_rdlen_pos] = (uint8_t)(ptr_rdlen >> 8);
	packet[ptr_rdlen_pos + 1] = (uint8_t)(ptr_rdlen & 0xFF);

	// SRV Record: instance -> hostname:port
	pos += mdns_write_dns_name(packet + pos, instance_name);
	packet[pos++] = 0;
	packet[pos++] = 33;   // Type: SRV
	packet[pos++] = 0x80;
	packet[pos++] = 1;    // Class: IN with cache flush
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 120;  // TTL: 120 seconds
	size_t srv_rdlen_pos = pos;
	pos += 2;
	size_t srv_rdata_start = pos;
	packet[pos++] = 0;    // Priority
	packet[pos++] = 0;
	packet[pos++] = 0;    // Weight
	packet[pos++] = 0;
	packet[pos++] = (uint8_t)(svc->port >> 8);
	packet[pos++] = (uint8_t)(svc->port & 0xFF);
	pos += mdns_write_dns_name(packet + pos, mdns->hostname);
	size_t srv_rdlen = pos - srv_rdata_start;
	packet[srv_rdlen_pos] = (uint8_t)(srv_rdlen >> 8);
	packet[srv_rdlen_pos + 1] = (uint8_t)(srv_rdlen & 0xFF);

	// TXT Record
	pos += mdns_write_dns_name(packet + pos, instance_name);
	packet[pos++] = 0;
	packet[pos++] = 16;   // Type: TXT
	packet[pos++] = 0x80;
	packet[pos++] = 1;    // Class: IN with cache flush
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 0x11;
	packet[pos++] = 0x94; // TTL: 4500 seconds
	packet[pos++] = (uint8_t)(svc->txt_len >> 8);
	packet[pos++] = (uint8_t)(svc->txt_len & 0xFF);
	if (svc->txt_len > 0) {
		memcpy(packet + pos, svc->txt_records, svc->txt_len);
		pos += svc->txt_len;
	}

	// A Record: hostname -> IP
	pos += mdns_write_dns_name(packet + pos, mdns->hostname);
	packet[pos++] = 0;
	packet[pos++] = 1;    // Type: A
	packet[pos++] = 0x80;
	packet[pos++] = 1;    // Class: IN with cache flush
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 120;  // TTL: 120 seconds
	packet[pos++] = 0;
	packet[pos++] = 4;    // RData length: 4
	memcpy(packet + pos, &mdns->bind_ip, 4);
	pos += 4;

	return pos;
}

// Handle incoming mDNS query
static inline bool mdns_handle_query(gaze_mdns_shared_t *mdns,
				     const uint8_t *packet, size_t len)
{
	if (!mdns || !packet || len < 12)
		return false;

	// Parse DNS header
	uint16_t flags = (packet[2] << 8) | packet[3];
	uint16_t qdcount = (packet[4] << 8) | packet[5];

	// Check if this is a query (QR bit = 0)
	if (flags & 0x8000)
		return false;

	// Parse questions
	size_t pos = 12;
	bool responded = false;

	for (uint16_t i = 0; i < qdcount && pos < len; i++) {
		char qname[256];
		pos = mdns_read_dns_name(packet, len, pos, qname, sizeof(qname));
		if (pos + 4 > len)
			break;

		uint16_t qtype = (packet[pos] << 8) | packet[pos + 1];
		pos += 4;

		// Check for PTR query for our service type
		if (qtype != 12 && qtype != 255)
			continue;

		// Check each registered service
		pthread_mutex_lock(&mdns->services_mutex);
		for (int j = 0; j < GAZE_MDNS_MAX_SERVICES; j++) {
			gaze_mdns_service_t *svc = &mdns->services[j];
			if (!svc->active)
				continue;

			char our_service[128];
			snprintf(our_service, sizeof(our_service), "%s.local",
				 svc->service_type);

			// Convert to lowercase
			for (size_t k = 0; our_service[k]; k++) {
				if (our_service[k] >= 'A' && our_service[k] <= 'Z')
					our_service[k] = our_service[k] + ('a' - 'A');
			}

			if (strcmp(qname, our_service) == 0) {
				// Build and send response for this service
				uint8_t response[1024];
				size_t resp_len = mdns_build_service_response(
					mdns, svc, response, sizeof(response));
				if (resp_len > 0) {
					sendto(mdns->sock, (const char *)response,
					       (int)resp_len, 0,
					       (struct sockaddr *)&mdns->mcast_addr,
					       sizeof(mdns->mcast_addr));
					responded = true;
				}
			}
		}
		pthread_mutex_unlock(&mdns->services_mutex);
	}

	return responded;
}

// Process incoming queries (non-blocking)
static inline int mdns_process_queries(gaze_mdns_shared_t *mdns)
{
	if (!mdns || !mdns->initialized || mdns->sock < 0)
		return -1;

	int query_count = 0;
	fd_set read_fds;
	struct timeval tv = {0, 10000};

	FD_ZERO(&read_fds);
	FD_SET(mdns->sock, &read_fds);

	while (select(mdns->sock + 1, &read_fds, NULL, NULL, &tv) > 0) {
		uint8_t packet[1500];
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);

		int received = recvfrom(mdns->sock, (char *)packet, sizeof(packet),
					0, (struct sockaddr *)&from, &from_len);
		if (received > 12) {
			if (mdns_handle_query(mdns, packet, received)) {
				query_count++;
			}
		}

		FD_ZERO(&read_fds);
		FD_SET(mdns->sock, &read_fds);
		tv.tv_sec = 0;
		tv.tv_usec = 10000;
	}

	return query_count;
}

// Announcement thread
static inline void *mdns_shared_thread(void *arg)
{
	gaze_mdns_shared_t *mdns = (gaze_mdns_shared_t *)arg;
	int loop_count = 0;
	const int ANNOUNCE_INTERVAL = 200; // 200 * 100ms = 20 seconds

	while (mdns->thread_running) {
		// Process any incoming queries
		mdns_process_queries(mdns);

		// Periodic announcements
		if (loop_count == 0 || loop_count % ANNOUNCE_INTERVAL == 0) {
			pthread_mutex_lock(&mdns->services_mutex);
			for (int i = 0; i < GAZE_MDNS_MAX_SERVICES; i++) {
				gaze_mdns_service_t *svc = &mdns->services[i];
				if (!svc->active)
					continue;

				uint8_t packet[1024];
				size_t len = mdns_build_service_response(
					mdns, svc, packet, sizeof(packet));
				if (len > 0) {
					for (int j = 0; j < 3; j++) {
						sendto(mdns->sock, (const char *)packet,
						       (int)len, 0,
						       (struct sockaddr *)&mdns->mcast_addr,
						       sizeof(mdns->mcast_addr));
						Sleep(20);
					}
				}
			}
			pthread_mutex_unlock(&mdns->services_mutex);
		}

		loop_count++;
		Sleep(50);
	}

	return NULL;
}

// Get the primary local IP address (for when bind_ip is 0/INADDR_ANY)
static inline uint32_t mdns_get_local_ip(void)
{
	// Create a UDP socket and "connect" to a public IP (doesn't actually send)
	// This tells us which local interface would be used for outbound traffic
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return 0;

	struct sockaddr_in remote;
	memset(&remote, 0, sizeof(remote));
	remote.sin_family = AF_INET;
	remote.sin_port = htons(53);  // DNS port (doesn't matter, we don't send)
	inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

	if (connect(sock, (struct sockaddr *)&remote, sizeof(remote)) != 0) {
		closesocket(sock);
		return 0;
	}

	struct sockaddr_in local;
	int local_len = sizeof(local);
	if (getsockname(sock, (struct sockaddr *)&local, &local_len) != 0) {
		closesocket(sock);
		return 0;
	}

	closesocket(sock);
	return local.sin_addr.s_addr;
}

// Initialize socket for a shared instance
static inline bool mdns_shared_init_socket(gaze_mdns_shared_t *mdns, uint32_t bind_ip)
{
	mdns->sock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (mdns->sock < 0)
		return false;

	// Allow address reuse
	int reuse = 1;
	setsockopt(mdns->sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
		   sizeof(reuse));

	// Bind to specific interface
	struct sockaddr_in bind_addr;
	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = bind_ip;
	bind_addr.sin_port = htons(MDNS_PORT);

	if (bind(mdns->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		// Try any port
		bind_addr.sin_port = 0;
		if (bind(mdns->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
			closesocket(mdns->sock);
			mdns->sock = -1;
			return false;
		}
	}

	// Join multicast group
	struct ip_mreq mreq;
	inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &mreq.imr_multiaddr);
	mreq.imr_interface.s_addr = bind_ip;
	setsockopt(mdns->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
		   (const char *)&mreq, sizeof(mreq));

	// Set multicast TTL and loopback
	unsigned char ttl = 255;
	setsockopt(mdns->sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl,
		   sizeof(ttl));
	unsigned char loop = 1;
	setsockopt(mdns->sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop,
		   sizeof(loop));

	// Set multicast interface
	if (bind_ip != 0) {
		struct in_addr iface_addr;
		iface_addr.s_addr = bind_ip;
		setsockopt(mdns->sock, IPPROTO_IP, IP_MULTICAST_IF,
			   (const char *)&iface_addr, sizeof(iface_addr));
	}

	// Setup multicast destination
	memset(&mdns->mcast_addr, 0, sizeof(mdns->mcast_addr));
	mdns->mcast_addr.sin_family = AF_INET;
	mdns->mcast_addr.sin_port = htons(MDNS_PORT);
	inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &mdns->mcast_addr.sin_addr);

	// Get hostname
	char hostname[128];
	if (gethostname(hostname, sizeof(hostname)) != 0) {
		strcpy(hostname, "gaze-host");
	}
	snprintf(mdns->hostname, sizeof(mdns->hostname), "%s.local", hostname);

	// Store lookup_ip (original parameter) for instance lookup/matching
	// Store bind_ip (actual IP) for mDNS A records
	mdns->lookup_ip = bind_ip;  // Original value for lookup
	if (bind_ip == 0) {
		mdns->bind_ip = mdns_get_local_ip();  // Detect for A records
	} else {
		mdns->bind_ip = bind_ip;
	}

	return true;
}

// Forward declare for logging (defined in obs-module.h)
#ifdef __cplusplus
extern "C" {
#endif
void obs_log(int log_level, const char *format, ...);
#define LOG_INFO 300
#define LOG_DEBUG 400
#ifdef __cplusplus
}
#endif

/**
 * Get or create a shared mDNS instance for the given interface.
 * Returns NULL on failure.
 */
static inline gaze_mdns_shared_t *gaze_mdns_shared_get(uint32_t bind_ip)
{
	pthread_mutex_lock(&g_mdns_mutex);

	// Look for existing instance (match on original lookup_ip, not detected bind_ip)
	gaze_mdns_shared_t *inst = g_mdns_instances;
	int instance_count = 0;
	while (inst) {
		instance_count++;
		obs_log(LOG_INFO, "[mdns-shared] Checking instance %d: lookup_ip=%u, looking for %u",
			instance_count, inst->lookup_ip, bind_ip);
		if (inst->lookup_ip == bind_ip) {
			inst->ref_count++;
			obs_log(LOG_INFO, "[mdns-shared] Found existing instance, ref_count now %d",
				inst->ref_count);
			pthread_mutex_unlock(&g_mdns_mutex);
			return inst;
		}
		inst = inst->next;
	}
	obs_log(LOG_INFO, "[mdns-shared] No matching instance found (checked %d), creating new",
		instance_count);

	// Create new instance
	inst = (gaze_mdns_shared_t *)calloc(1, sizeof(gaze_mdns_shared_t));
	if (!inst) {
		pthread_mutex_unlock(&g_mdns_mutex);
		return NULL;
	}

	inst->sock = -1;
	pthread_mutex_init(&inst->services_mutex, NULL);

	if (!mdns_shared_init_socket(inst, bind_ip)) {
		pthread_mutex_destroy(&inst->services_mutex);
		free(inst);
		pthread_mutex_unlock(&g_mdns_mutex);
		return NULL;
	}

	// Start announcement thread
	inst->thread_running = true;
	if (pthread_create(&inst->announce_thread, NULL, mdns_shared_thread, inst) != 0) {
		closesocket(inst->sock);
		pthread_mutex_destroy(&inst->services_mutex);
		free(inst);
		pthread_mutex_unlock(&g_mdns_mutex);
		return NULL;
	}

	inst->ref_count = 1;
	inst->initialized = true;

	// Add to global list
	inst->next = g_mdns_instances;
	g_mdns_instances = inst;

	pthread_mutex_unlock(&g_mdns_mutex);
	return inst;
}

/**
 * Release a reference to a shared mDNS instance.
 * Destroys the instance when the last reference is released.
 */
static inline void gaze_mdns_shared_release(gaze_mdns_shared_t *inst)
{
	if (!inst)
		return;

	obs_log(LOG_INFO, "[mdns-shared] RELEASE inst=%p current_ref=%d",
		(void *)inst, inst->ref_count);

	pthread_mutex_lock(&g_mdns_mutex);

	inst->ref_count--;
	obs_log(LOG_INFO, "[mdns-shared] ref_count now %d", inst->ref_count);
	if (inst->ref_count > 0) {
		pthread_mutex_unlock(&g_mdns_mutex);
		return;
	}

	obs_log(LOG_INFO, "[mdns-shared] DESTROYING instance %p (ref_count=0)",
		(void *)inst);

	// Remove from global list
	gaze_mdns_shared_t **pp = &g_mdns_instances;
	while (*pp) {
		if (*pp == inst) {
			*pp = inst->next;
			break;
		}
		pp = &(*pp)->next;
	}

	pthread_mutex_unlock(&g_mdns_mutex);

	// Stop thread
	if (inst->thread_running) {
		inst->thread_running = false;
		pthread_join(inst->announce_thread, NULL);
	}

	// Close socket
	if (inst->sock >= 0) {
		struct ip_mreq mreq;
		inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &mreq.imr_multiaddr);
		mreq.imr_interface.s_addr = inst->bind_ip;
		setsockopt(inst->sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
			   (const char *)&mreq, sizeof(mreq));
		closesocket(inst->sock);
	}

	pthread_mutex_destroy(&inst->services_mutex);
	free(inst);
	obs_log(LOG_INFO, "[mdns-shared] instance destroyed");
}

/**
 * Register a service with a shared mDNS instance.
 * Returns the service slot index, or -1 on failure.
 */
static inline int gaze_mdns_shared_register(gaze_mdns_shared_t *inst,
					     const char *name,
					     const char *type,
					     uint16_t port,
					     const uint8_t *txt,
					     size_t txt_len)
{
	if (!inst || !name || !type)
		return -1;

	obs_log(LOG_INFO, "[mdns-shared] register: name=%s port=%u inst=%p",
		name, port, (void *)inst);

	pthread_mutex_lock(&inst->services_mutex);

	// Log current state of all slots
	obs_log(LOG_INFO, "[mdns-shared] current services (count=%d):",
		inst->service_count);
	for (int i = 0; i < GAZE_MDNS_MAX_SERVICES && i < 4; i++) {
		obs_log(LOG_INFO, "[mdns-shared]   slot[%d]: active=%d name=%s port=%u",
			i, inst->services[i].active,
			inst->services[i].active ? inst->services[i].service_name : "(none)",
			inst->services[i].port);
	}

	// Find empty slot
	int slot = -1;
	for (int i = 0; i < GAZE_MDNS_MAX_SERVICES; i++) {
		if (!inst->services[i].active) {
			slot = i;
			break;
		}
	}

	if (slot < 0) {
		obs_log(LOG_ERROR, "[mdns-shared] no free slot!");
		pthread_mutex_unlock(&inst->services_mutex);
		return -1;
	}

	obs_log(LOG_INFO, "[mdns-shared] using slot %d for %s", slot, name);

	gaze_mdns_service_t *svc = &inst->services[slot];
	svc->active = true;
	strncpy(svc->service_name, name, sizeof(svc->service_name) - 1);
	strncpy(svc->service_type, type, sizeof(svc->service_type) - 1);
	svc->port = port;
	if (txt && txt_len > 0 && txt_len < sizeof(svc->txt_records)) {
		memcpy(svc->txt_records, txt, txt_len);
		svc->txt_len = txt_len;
	} else {
		svc->txt_len = 0;
	}
	inst->service_count++;

	obs_log(LOG_INFO, "[mdns-shared] registered! service_count now %d",
		inst->service_count);

	pthread_mutex_unlock(&inst->services_mutex);
	return slot;
}

/**
 * Update a service's TXT records.
 */
static inline bool gaze_mdns_shared_update(gaze_mdns_shared_t *inst,
					    int slot,
					    const uint8_t *txt,
					    size_t txt_len)
{
	if (!inst || slot < 0 || slot >= GAZE_MDNS_MAX_SERVICES)
		return false;

	pthread_mutex_lock(&inst->services_mutex);

	gaze_mdns_service_t *svc = &inst->services[slot];
	if (!svc->active) {
		pthread_mutex_unlock(&inst->services_mutex);
		return false;
	}

	if (txt && txt_len > 0 && txt_len < sizeof(svc->txt_records)) {
		memcpy(svc->txt_records, txt, txt_len);
		svc->txt_len = txt_len;
	}

	pthread_mutex_unlock(&inst->services_mutex);
	return true;
}

/**
 * Unregister a service.
 */
static inline void gaze_mdns_shared_unregister(gaze_mdns_shared_t *inst, int slot)
{
	if (!inst || slot < 0 || slot >= GAZE_MDNS_MAX_SERVICES)
		return;

	obs_log(LOG_INFO, "[mdns-shared] UNREGISTER slot %d from inst %p", slot, (void *)inst);

	pthread_mutex_lock(&inst->services_mutex);

	gaze_mdns_service_t *svc = &inst->services[slot];
	if (svc->active) {
		obs_log(LOG_INFO, "[mdns-shared] unregistering %s from slot %d",
			svc->service_name, slot);
		svc->active = false;
		inst->service_count--;
		obs_log(LOG_INFO, "[mdns-shared] service_count now %d", inst->service_count);
	} else {
		obs_log(LOG_INFO, "[mdns-shared] slot %d already inactive", slot);
	}

	pthread_mutex_unlock(&inst->services_mutex);
}

/**
 * Helper to add a single TXT entry
 */
static inline size_t gaze_mdns_shared_add_txt(uint8_t *buf, size_t pos,
					       size_t capacity,
					       const char *key,
					       const char *value)
{
	char entry[256];
	int len = snprintf(entry, sizeof(entry), "%s=%s", key, value);
	if (len <= 0 || pos + 1 + len > capacity)
		return pos;
	buf[pos++] = (uint8_t)len;
	memcpy(buf + pos, entry, len);
	return pos + len;
}

#ifdef __cplusplus
}
#endif

#endif // _WIN32

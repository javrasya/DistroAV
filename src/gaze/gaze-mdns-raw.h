/******************************************************************************
    Copyright (C) 2016-2024 DistroAV <contact@distroav.org>

    Raw mDNS announcement using multicast sockets.
    Bypasses Windows DNS-SD API which has issues with secondary interfaces.
******************************************************************************/

#pragma once

#ifdef _WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MDNS_PORT 5353
#define MDNS_MULTICAST_ADDR "224.0.0.251"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gaze_mdns_raw {
	int sock;
	struct sockaddr_in bind_addr;
	struct sockaddr_in mcast_addr;
	char service_name[256];
	char service_type[64];
	char hostname[256];
	uint16_t port;
	uint32_t ip_addr;
	char txt_records[512];
	size_t txt_len;
	bool initialized;
} gaze_mdns_raw_t;

// Initialize raw mDNS on a specific interface
static inline bool gaze_mdns_raw_init(gaze_mdns_raw_t *mdns, uint32_t bind_ip)
{
	if (!mdns)
		return false;

	memset(mdns, 0, sizeof(*mdns));
	mdns->sock = -1;

#ifdef _WIN32
	mdns->sock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
	mdns->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
	if (mdns->sock < 0)
		return false;

	// Allow address reuse BEFORE bind (needed to share port 5353)
	int reuse = 1;
	setsockopt(mdns->sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
		   sizeof(reuse));
#ifdef SO_REUSEPORT
	setsockopt(mdns->sock, SOL_SOCKET, SO_REUSEPORT, (const char *)&reuse,
		   sizeof(reuse));
#endif

	// Bind to the SPECIFIC interface IP to ensure packets go out on that interface
	// This is more reliable than IP_MULTICAST_IF on some systems
	memset(&mdns->bind_addr, 0, sizeof(mdns->bind_addr));
	mdns->bind_addr.sin_family = AF_INET;
	mdns->bind_addr.sin_addr.s_addr = bind_ip;  // Bind to specific IP
	mdns->bind_addr.sin_port = htons(MDNS_PORT);

	bool bound_to_5353 = true;
	if (bind(mdns->sock, (struct sockaddr *)&mdns->bind_addr,
		 sizeof(mdns->bind_addr)) < 0) {
		// Port 5353 in use on this interface, try any port
		bound_to_5353 = false;
		mdns->bind_addr.sin_port = 0;
		if (bind(mdns->sock, (struct sockaddr *)&mdns->bind_addr,
			 sizeof(mdns->bind_addr)) < 0) {
			// Last resort: bind to any address
			mdns->bind_addr.sin_addr.s_addr = INADDR_ANY;
			if (bind(mdns->sock, (struct sockaddr *)&mdns->bind_addr,
				 sizeof(mdns->bind_addr)) < 0) {
#ifdef _WIN32
				closesocket(mdns->sock);
#else
				close(mdns->sock);
#endif
				mdns->sock = -1;
				return false;
			}
		}
	}

	// Join multicast group on the specific interface to receive queries
	struct ip_mreq mreq;
	inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &mreq.imr_multiaddr);
	mreq.imr_interface.s_addr = bind_ip;
	setsockopt(mdns->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
		   (const char *)&mreq, sizeof(mreq));

	// Set multicast TTL (255 for local network, required by mDNS spec)
	unsigned char ttl = 255;
	setsockopt(mdns->sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl,
		   sizeof(ttl));

	// Enable multicast loopback for local testing
	unsigned char loop = 1;
	setsockopt(mdns->sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop,
		   sizeof(loop));

	// Set multicast interface to send from
	if (bind_ip != 0) {
		struct in_addr iface_addr;
		iface_addr.s_addr = bind_ip;
		setsockopt(mdns->sock, IPPROTO_IP, IP_MULTICAST_IF,
			   (const char *)&iface_addr, sizeof(iface_addr));
	}

	// Setup multicast destination address
	memset(&mdns->mcast_addr, 0, sizeof(mdns->mcast_addr));
	mdns->mcast_addr.sin_family = AF_INET;
	mdns->mcast_addr.sin_port = htons(MDNS_PORT);
	inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &mdns->mcast_addr.sin_addr);

	mdns->ip_addr = bind_ip;
	mdns->initialized = true;

	(void)bound_to_5353;
	return true;
}

// Helper to write a DNS name (label format)
static inline size_t write_dns_name(uint8_t *buf, const char *name)
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
static inline size_t read_dns_name(const uint8_t *packet, size_t packet_len,
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

// Build response packet for a query
static inline size_t gaze_mdns_build_response(gaze_mdns_raw_t *mdns,
					      uint8_t *packet, size_t capacity,
					      uint16_t query_id)
{
	if (!mdns || !packet || capacity < 512)
		return 0;

	// Build service type with .local suffix
	char service_type_local[128];
	snprintf(service_type_local, sizeof(service_type_local), "%s.local",
		 mdns->service_type);

	// Build instance name: "ServiceName._gazestream._udp.local"
	char instance_name[384];
	snprintf(instance_name, sizeof(instance_name), "%s.%s.local",
		 mdns->service_name, mdns->service_type);

	size_t pos = 0;

	// DNS Header (12 bytes)
	// Transaction ID (echo from query for unicast, 0 for multicast)
	packet[pos++] = (uint8_t)(query_id >> 8);
	packet[pos++] = (uint8_t)(query_id & 0xFF);
	// Flags: Response, Authoritative
	packet[pos++] = 0x84;
	packet[pos++] = 0x00;
	// Questions: 0
	packet[pos++] = 0;
	packet[pos++] = 0;
	// Answers: 4 (PTR, SRV, TXT, A)
	packet[pos++] = 0;
	packet[pos++] = 4;
	// Authority: 0
	packet[pos++] = 0;
	packet[pos++] = 0;
	// Additional: 0
	packet[pos++] = 0;
	packet[pos++] = 0;

	// PTR Record: _gazestream._udp.local -> instance
	pos += write_dns_name(packet + pos, service_type_local);
	// Type: PTR (12)
	packet[pos++] = 0;
	packet[pos++] = 12;
	// Class: IN with cache flush
	packet[pos++] = 0x80;
	packet[pos++] = 1;
	// TTL: 4500 seconds
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 0x11;
	packet[pos++] = 0x94;
	// RData length (placeholder)
	size_t ptr_rdlen_pos = pos;
	pos += 2;
	size_t ptr_rdata_start = pos;
	pos += write_dns_name(packet + pos, instance_name);
	// Fill in RData length
	size_t ptr_rdlen = pos - ptr_rdata_start;
	packet[ptr_rdlen_pos] = (uint8_t)(ptr_rdlen >> 8);
	packet[ptr_rdlen_pos + 1] = (uint8_t)(ptr_rdlen & 0xFF);

	// SRV Record: instance -> hostname:port
	pos += write_dns_name(packet + pos, instance_name);
	// Type: SRV (33)
	packet[pos++] = 0;
	packet[pos++] = 33;
	// Class: IN with cache flush
	packet[pos++] = 0x80;
	packet[pos++] = 1;
	// TTL: 120 seconds
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 120;
	// RData length placeholder
	size_t srv_rdlen_pos = pos;
	pos += 2;
	size_t srv_rdata_start = pos;
	// Priority
	packet[pos++] = 0;
	packet[pos++] = 0;
	// Weight
	packet[pos++] = 0;
	packet[pos++] = 0;
	// Port
	packet[pos++] = (uint8_t)(mdns->port >> 8);
	packet[pos++] = (uint8_t)(mdns->port & 0xFF);
	// Target hostname
	pos += write_dns_name(packet + pos, mdns->hostname);
	size_t srv_rdlen = pos - srv_rdata_start;
	packet[srv_rdlen_pos] = (uint8_t)(srv_rdlen >> 8);
	packet[srv_rdlen_pos + 1] = (uint8_t)(srv_rdlen & 0xFF);

	// TXT Record
	pos += write_dns_name(packet + pos, instance_name);
	// Type: TXT (16)
	packet[pos++] = 0;
	packet[pos++] = 16;
	// Class: IN with cache flush
	packet[pos++] = 0x80;
	packet[pos++] = 1;
	// TTL: 4500 seconds
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 0x11;
	packet[pos++] = 0x94;
	// RData length
	packet[pos++] = (uint8_t)(mdns->txt_len >> 8);
	packet[pos++] = (uint8_t)(mdns->txt_len & 0xFF);
	// TXT data
	if (mdns->txt_len > 0) {
		memcpy(packet + pos, mdns->txt_records, mdns->txt_len);
		pos += mdns->txt_len;
	}

	// A Record: hostname -> IP
	pos += write_dns_name(packet + pos, mdns->hostname);
	// Type: A (1)
	packet[pos++] = 0;
	packet[pos++] = 1;
	// Class: IN with cache flush
	packet[pos++] = 0x80;
	packet[pos++] = 1;
	// TTL: 120 seconds
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 0;
	packet[pos++] = 120;
	// RData length: 4
	packet[pos++] = 0;
	packet[pos++] = 4;
	// IP address (already in network byte order)
	memcpy(packet + pos, &mdns->ip_addr, 4);
	pos += 4;

	return pos;
}

// Check if we should respond to this query and send response
static inline bool gaze_mdns_handle_query(gaze_mdns_raw_t *mdns,
					  const uint8_t *packet, size_t len,
					  struct sockaddr_in *from)
{
	if (!mdns || !mdns->initialized || mdns->sock < 0 || !packet || len < 12)
		return false;

	// Parse DNS header
	// uint16_t id = (packet[0] << 8) | packet[1];
	uint16_t flags = (packet[2] << 8) | packet[3];
	uint16_t qdcount = (packet[4] << 8) | packet[5];

	// Check if this is a query (QR bit = 0)
	if (flags & 0x8000)
		return false;

	// Parse questions
	size_t pos = 12;
	for (uint16_t i = 0; i < qdcount && pos < len; i++) {
		char qname[256];
		pos = read_dns_name(packet, len, pos, qname, sizeof(qname));
		if (pos + 4 > len)
			break;

		uint16_t qtype = (packet[pos] << 8) | packet[pos + 1];
		// uint16_t qclass = (packet[pos + 2] << 8) | packet[pos + 3];
		pos += 4;

		// Check if query matches our service type
		char our_service[128];
		snprintf(our_service, sizeof(our_service), "%s.local",
			 mdns->service_type);

		// Convert to lowercase for comparison
		for (size_t j = 0; our_service[j]; j++) {
			if (our_service[j] >= 'A' && our_service[j] <= 'Z')
				our_service[j] = our_service[j] + ('a' - 'A');
		}

		// Check for PTR query for our service type, or ANY query
		if ((qtype == 12 || qtype == 255) &&
		    strcmp(qname, our_service) == 0) {
			// Build and send response
			uint8_t response[1024];
			size_t resp_len = gaze_mdns_build_response(mdns, response,
								   sizeof(response), 0);
			if (resp_len > 0) {
				// Send to multicast address (standard mDNS)
				sendto(mdns->sock, (const char *)response, (int)resp_len, 0,
				       (struct sockaddr *)&mdns->mcast_addr,
				       sizeof(mdns->mcast_addr));
				return true;
			}
		}
	}

	return false;
}

// Check for and handle incoming queries (non-blocking)
static inline int gaze_mdns_process_queries(gaze_mdns_raw_t *mdns)
{
	if (!mdns || !mdns->initialized || mdns->sock < 0)
		return -1;

	int query_count = 0;

	// Set socket to non-blocking for this check
	fd_set read_fds;
	struct timeval tv = {0, 10000};  // 10ms timeout
	FD_ZERO(&read_fds);
	FD_SET(mdns->sock, &read_fds);

	while (select(mdns->sock + 1, &read_fds, NULL, NULL, &tv) > 0) {
		uint8_t packet[1500];
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);

		int received = recvfrom(mdns->sock, (char *)packet, sizeof(packet), 0,
					(struct sockaddr *)&from, &from_len);
		if (received > 12) {
			if (gaze_mdns_handle_query(mdns, packet, received, &from)) {
				query_count++;
			}
		}

		// Reset for next iteration
		FD_ZERO(&read_fds);
		FD_SET(mdns->sock, &read_fds);
		tv.tv_sec = 0;
		tv.tv_usec = 10000;
	}

	return query_count;
}

// Build and send mDNS announcement packet
static inline bool gaze_mdns_raw_announce(gaze_mdns_raw_t *mdns,
					  const char *service_name,
					  const char *service_type,
					  uint16_t port, const char *txt_data,
					  size_t txt_len)
{
	if (!mdns || !mdns->initialized || mdns->sock < 0)
		return false;

	// Save for responding to queries
	strncpy(mdns->service_name, service_name, sizeof(mdns->service_name) - 1);
	strncpy(mdns->service_type, service_type, sizeof(mdns->service_type) - 1);
	mdns->port = port;
	if (txt_data && txt_len > 0 && txt_len < sizeof(mdns->txt_records)) {
		memcpy(mdns->txt_records, txt_data, txt_len);
		mdns->txt_len = txt_len;
	}

	// Get hostname
	char hostname[128];
	if (gethostname(hostname, sizeof(hostname)) != 0) {
		strcpy(hostname, "gaze-host");
	}
	snprintf(mdns->hostname, sizeof(mdns->hostname), "%s.local", hostname);

	// Process any pending queries first
	gaze_mdns_process_queries(mdns);

	// Build and send announcement
	uint8_t packet[1024];
	size_t packet_len = gaze_mdns_build_response(mdns, packet, sizeof(packet), 0);
	if (packet_len == 0)
		return false;

	// Send packet multiple times for reliability (mDNS spec recommends this)
	int sent = 0;
	for (int i = 0; i < 3; i++) {
		sent = sendto(mdns->sock, (const char *)packet, (int)packet_len, 0,
			      (struct sockaddr *)&mdns->mcast_addr,
			      sizeof(mdns->mcast_addr));
		if (sent <= 0) {
#ifdef _WIN32
			// Get Windows error
			int err = WSAGetLastError();
			(void)err; // For debugging
#endif
			break;
		}
#ifdef _WIN32
		Sleep(20);
#else
		usleep(20000);
#endif
	}

	return sent > 0;
}

// Build TXT record data from key-value pairs
static inline size_t gaze_mdns_build_txt(uint8_t *buf, size_t capacity, ...)
{
	// This is a simplified version - caller provides pre-formatted TXT
	return 0;
}

// Helper to add a single TXT entry
static inline size_t gaze_mdns_add_txt_entry(uint8_t *buf, size_t pos,
					     size_t capacity, const char *key,
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

// Send goodbye packet (TTL=0)
static inline void gaze_mdns_raw_goodbye(gaze_mdns_raw_t *mdns)
{
	if (!mdns || !mdns->initialized || mdns->sock < 0)
		return;

	// Similar to announce but with TTL=0
	// For simplicity, we just close the socket and let the TTL expire
}

// Cleanup
static inline void gaze_mdns_raw_destroy(gaze_mdns_raw_t *mdns)
{
	if (!mdns)
		return;

	if (mdns->sock >= 0) {
		// Leave multicast group
		struct ip_mreq mreq;
		inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &mreq.imr_multiaddr);
		mreq.imr_interface.s_addr = mdns->ip_addr;
		setsockopt(mdns->sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
			   (const char *)&mreq, sizeof(mreq));

#ifdef _WIN32
		closesocket(mdns->sock);
#else
		close(mdns->sock);
#endif
		mdns->sock = -1;
	}
	mdns->initialized = false;
}

#ifdef __cplusplus
}
#endif

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

#include "gaze-discovery.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <cstring>
#include <cstdio>

/**
 * mDNS/DNS-SD Implementation
 *
 * Platform-specific implementations:
 * - Windows: Native dnsapi.dll (DnsServiceRegister)
 * - macOS: dns_sd.h (native)
 * - Linux: Avahi client library
 */

#if defined(__APPLE__)
// macOS - native dns_sd.h
#include <dns_sd.h>

struct gaze_discovery_platform {
	DNSServiceRef service_ref;
};

static void DNSSD_API register_callback(DNSServiceRef sdref,
					DNSServiceFlags flags,
					DNSServiceErrorType error,
					const char *name, const char *regtype,
					const char *domain, void *context)
{
	(void)sdref;
	(void)flags;
	(void)regtype;
	(void)domain;

	gaze_discovery_t *disc = (gaze_discovery_t *)context;

	if (error == kDNSServiceErr_NoError) {
		obs_log(LOG_INFO, "[gaze-discovery] Registered service: %s",
			name);
		disc->registered = true;
	} else {
		obs_log(LOG_WARNING,
			"[gaze-discovery] Registration failed: %d", error);
		disc->registered = false;
	}
}

bool gaze_discovery_available(void)
{
	return true;
}

bool gaze_discovery_init(gaze_discovery_t *disc)
{
	if (!disc)
		return false;

	memset(disc, 0, sizeof(*disc));

	disc->platform_handle = new gaze_discovery_platform();
	if (!disc->platform_handle)
		return false;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;
	platform->service_ref = nullptr;

	disc->initialized = true;
	return true;
}

bool gaze_discovery_register(gaze_discovery_t *disc, const char *name,
			     uint16_t port, gaze_codec_t codec,
			     uint32_t width, uint32_t height, uint32_t fps,
			     uint32_t if_index, uint32_t ip_addr)
{
	if (!disc || !disc->initialized || !name)
		return false;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;

	// Unregister existing service
	if (platform->service_ref) {
		DNSServiceRefDeallocate(platform->service_ref);
		platform->service_ref = nullptr;
		disc->registered = false;
	}

	// Create TXT record
	TXTRecordRef txt_ref;
	TXTRecordCreate(&txt_ref, 0, nullptr);

	const char *codec_str = (codec == GAZE_CODEC_HEVC) ? "hevc" : "h264";
	TXTRecordSetValue(&txt_ref, "codec", strlen(codec_str), codec_str);

	char buf[16];
	snprintf(buf, sizeof(buf), "%u", width);
	TXTRecordSetValue(&txt_ref, "width", strlen(buf), buf);
	snprintf(buf, sizeof(buf), "%u", height);
	TXTRecordSetValue(&txt_ref, "height", strlen(buf), buf);
	snprintf(buf, sizeof(buf), "%u", fps);
	TXTRecordSetValue(&txt_ref, "fps", strlen(buf), buf);
	snprintf(buf, sizeof(buf), "%d", GAZE_PROTOCOL_VERSION);
	TXTRecordSetValue(&txt_ref, "version", strlen(buf), buf);

	// Add IP address to TXT record if bound to specific interface
	if (ip_addr != 0) {
		struct in_addr addr;
		addr.s_addr = ip_addr;
		char ip_str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
		TXTRecordSetValue(&txt_ref, "ip", strlen(ip_str), ip_str);
	}

	// Register on specific interface (0 = all)
	DNSServiceErrorType err =
		DNSServiceRegister(&platform->service_ref, 0, if_index, name,
				   GAZE_MDNS_SERVICE_TYPE, nullptr, nullptr,
				   htons(port), TXTRecordGetLength(&txt_ref),
				   TXTRecordGetBytesPtr(&txt_ref),
				   register_callback, disc);

	TXTRecordDeallocate(&txt_ref);

	if (err != kDNSServiceErr_NoError) {
		obs_log(LOG_ERROR,
			"[gaze-discovery] DNSServiceRegister failed: %d", err);
		return false;
	}

	// Store service info
	strncpy(disc->service_name, name, sizeof(disc->service_name) - 1);
	disc->port = port;
	disc->codec = codec;
	disc->width = width;
	disc->height = height;
	disc->fps = fps;
	disc->if_index = if_index;
	disc->ip_addr = ip_addr;

	// Process the registration (non-blocking check)
	int fd = DNSServiceRefSockFD(platform->service_ref);
	if (fd >= 0) {
		fd_set read_fds;
		struct timeval tv = {0, 100000}; // 100ms timeout
		FD_ZERO(&read_fds);
		FD_SET(fd, &read_fds);
		if (select(fd + 1, &read_fds, nullptr, nullptr, &tv) > 0) {
			DNSServiceProcessResult(platform->service_ref);
		}
	}

	return true;
}

bool gaze_discovery_update(gaze_discovery_t *disc, uint32_t width,
			   uint32_t height, uint32_t fps)
{
	if (!disc || !disc->initialized || !disc->registered)
		return false;

	// Re-register with new parameters
	return gaze_discovery_register(disc, disc->service_name, disc->port,
				       disc->codec, width, height, fps,
				       disc->if_index, disc->ip_addr);
}

void gaze_discovery_unregister(gaze_discovery_t *disc)
{
	if (!disc || !disc->platform_handle)
		return;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;

	if (platform->service_ref) {
		DNSServiceRefDeallocate(platform->service_ref);
		platform->service_ref = nullptr;
	}

	disc->registered = false;
}

void gaze_discovery_destroy(gaze_discovery_t *disc)
{
	if (!disc)
		return;

	gaze_discovery_unregister(disc);

	if (disc->platform_handle) {
		delete (gaze_discovery_platform *)disc->platform_handle;
		disc->platform_handle = nullptr;
	}

	disc->initialized = false;
}

#elif defined(_WIN32)
// Windows - Use shared mDNS socket per interface
// This is required because mDNS uses port 5353 and multicast, so multiple
// sockets on the same interface would conflict. We share a single socket
// that can announce multiple services.
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include "gaze-mdns-shared.h"

struct gaze_discovery_platform {
	gaze_mdns_shared_t *shared_mdns;  // Shared instance (refcounted)
	int service_slot;                  // Our slot in the shared instance
	uint8_t txt_buffer[512];
	size_t txt_len;
};

bool gaze_discovery_available(void)
{
	return true;  // Shared mDNS is always available
}

bool gaze_discovery_init(gaze_discovery_t *disc)
{
	if (!disc)
		return false;

	memset(disc, 0, sizeof(*disc));

	disc->platform_handle = new gaze_discovery_platform();
	if (!disc->platform_handle)
		return false;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;
	memset(platform, 0, sizeof(*platform));
	platform->service_slot = -1;

	obs_log(LOG_INFO, "[gaze-discovery] Shared mDNS initialized");

	disc->initialized = true;
	return true;
}

// Helper to build TXT record
static size_t build_txt_record(uint8_t *txt, size_t capacity,
			       gaze_codec_t codec, uint32_t width,
			       uint32_t height, uint32_t fps, uint32_t ip_addr)
{
	size_t pos = 0;
	char buf[32];

	const char *codec_str = (codec == GAZE_CODEC_HEVC) ? "hevc" : "h264";
	pos = gaze_mdns_shared_add_txt(txt, pos, capacity, "codec", codec_str);

	snprintf(buf, sizeof(buf), "%u", width);
	pos = gaze_mdns_shared_add_txt(txt, pos, capacity, "width", buf);

	snprintf(buf, sizeof(buf), "%u", height);
	pos = gaze_mdns_shared_add_txt(txt, pos, capacity, "height", buf);

	snprintf(buf, sizeof(buf), "%u", fps);
	pos = gaze_mdns_shared_add_txt(txt, pos, capacity, "fps", buf);

	snprintf(buf, sizeof(buf), "%d", GAZE_PROTOCOL_VERSION);
	pos = gaze_mdns_shared_add_txt(txt, pos, capacity, "version", buf);

	if (ip_addr != 0) {
		struct in_addr addr;
		addr.s_addr = ip_addr;
		char ip_str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
		pos = gaze_mdns_shared_add_txt(txt, pos, capacity, "ip", ip_str);
	}

	return pos;
}

bool gaze_discovery_register(gaze_discovery_t *disc, const char *name,
			     uint16_t port, gaze_codec_t codec,
			     uint32_t width, uint32_t height, uint32_t fps,
			     uint32_t if_index, uint32_t ip_addr)
{
	if (!disc || !disc->initialized || !name)
		return false;

	obs_log(LOG_INFO,
		"[gaze-discovery] register called: name=%s port=%u ip_addr=%u",
		name, port, ip_addr);

	// Store service info
	strncpy(disc->service_name, name, sizeof(disc->service_name) - 1);
	disc->port = port;
	disc->codec = codec;
	disc->width = width;
	disc->height = height;
	disc->fps = fps;
	disc->if_index = if_index;
	disc->ip_addr = ip_addr;

	if (!disc->platform_handle)
		return false;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;

	obs_log(LOG_INFO,
		"[gaze-discovery] current state: shared_mdns=%p service_slot=%d",
		(void *)platform->shared_mdns, platform->service_slot);

	// Unregister from previous shared instance if different IP
	// Use lookup_ip (original request) not bind_ip (detected IP)
	if (platform->shared_mdns) {
		obs_log(LOG_INFO,
			"[gaze-discovery] checking switch: lookup_ip=%u vs ip_addr=%u",
			platform->shared_mdns->lookup_ip, ip_addr);
	}
	if (platform->shared_mdns &&
	    platform->shared_mdns->lookup_ip != ip_addr) {
		obs_log(LOG_INFO,
			"[gaze-discovery] SWITCHING shared instance (lookup_ip mismatch)");
		if (platform->service_slot >= 0) {
			obs_log(LOG_INFO,
				"[gaze-discovery] unregistering from slot %d",
				platform->service_slot);
			gaze_mdns_shared_unregister(platform->shared_mdns,
						    platform->service_slot);
			platform->service_slot = -1;
		}
		gaze_mdns_shared_release(platform->shared_mdns);
		platform->shared_mdns = nullptr;
	}

	// Get or create shared instance for this interface
	if (!platform->shared_mdns) {
		obs_log(LOG_INFO,
			"[gaze-discovery] getting shared instance for ip_addr=%u",
			ip_addr);
		platform->shared_mdns = gaze_mdns_shared_get(ip_addr);
		if (!platform->shared_mdns) {
			obs_log(LOG_ERROR,
				"[gaze-discovery] Failed to get shared mDNS for IP");
			return false;
		}
		obs_log(LOG_INFO,
			"[gaze-discovery] got shared instance %p (lookup_ip=%u, bind_ip=%u, ref=%d)",
			(void *)platform->shared_mdns,
			platform->shared_mdns->lookup_ip,
			platform->shared_mdns->bind_ip,
			platform->shared_mdns->ref_count);
	} else {
		obs_log(LOG_INFO,
			"[gaze-discovery] reusing existing shared instance %p",
			(void *)platform->shared_mdns);
	}

	// Build TXT record
	platform->txt_len = build_txt_record(
		platform->txt_buffer, sizeof(platform->txt_buffer),
		codec, width, height, fps, ip_addr);

	// Register or update service
	if (platform->service_slot >= 0) {
		// Update existing registration
		obs_log(LOG_INFO,
			"[gaze-discovery] updating existing slot %d",
			platform->service_slot);
		gaze_mdns_shared_update(platform->shared_mdns,
					platform->service_slot,
					platform->txt_buffer,
					platform->txt_len);
	} else {
		// New registration
		obs_log(LOG_INFO,
			"[gaze-discovery] registering NEW service in shared instance");
		platform->service_slot = gaze_mdns_shared_register(
			platform->shared_mdns, name, GAZE_MDNS_SERVICE_TYPE,
			port, platform->txt_buffer, platform->txt_len);

		if (platform->service_slot < 0) {
			obs_log(LOG_ERROR,
				"[gaze-discovery] Failed to register service");
			return false;
		}
		obs_log(LOG_INFO,
			"[gaze-discovery] registered in slot %d",
			platform->service_slot);
	}

	disc->registered = true;

	// Log
	char ip_str[INET_ADDRSTRLEN] = "any";
	if (ip_addr != 0) {
		struct in_addr addr;
		addr.s_addr = ip_addr;
		inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
	}
	obs_log(LOG_INFO,
		"[gaze-discovery] DONE: %s on port %u, ip=%s (slot %d, shared=%p)",
		name, port, ip_str, platform->service_slot, (void *)platform->shared_mdns);

	return true;
}

bool gaze_discovery_update(gaze_discovery_t *disc, uint32_t width,
			   uint32_t height, uint32_t fps)
{
	if (!disc || !disc->initialized)
		return false;

	// Update stored values
	disc->width = width;
	disc->height = height;
	disc->fps = fps;

	// If not registered yet, do initial registration
	if (!disc->registered && disc->service_name[0] != '\0') {
		return gaze_discovery_register(disc, disc->service_name,
					       disc->port, disc->codec, width,
					       height, fps, disc->if_index,
					       disc->ip_addr);
	}

	// Update TXT record
	if (disc->platform_handle) {
		auto *platform = (gaze_discovery_platform *)disc->platform_handle;

		if (platform->shared_mdns && platform->service_slot >= 0) {
			platform->txt_len = build_txt_record(
				platform->txt_buffer, sizeof(platform->txt_buffer),
				disc->codec, width, height, fps, disc->ip_addr);

			gaze_mdns_shared_update(platform->shared_mdns,
						platform->service_slot,
						platform->txt_buffer,
						platform->txt_len);
		}
	}

	return true;
}

void gaze_discovery_unregister(gaze_discovery_t *disc)
{
	if (!disc || !disc->platform_handle)
		return;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;

	if (platform->shared_mdns && platform->service_slot >= 0) {
		gaze_mdns_shared_unregister(platform->shared_mdns,
					    platform->service_slot);
		platform->service_slot = -1;
		obs_log(LOG_INFO, "[gaze-discovery] Service unregistered: %s",
			disc->service_name);
	}

	disc->registered = false;
}

void gaze_discovery_destroy(gaze_discovery_t *disc)
{
	if (!disc)
		return;

	gaze_discovery_unregister(disc);

	if (disc->platform_handle) {
		auto *platform = (gaze_discovery_platform *)disc->platform_handle;

		if (platform->shared_mdns) {
			gaze_mdns_shared_release(platform->shared_mdns);
			platform->shared_mdns = nullptr;
		}

		delete platform;
		disc->platform_handle = nullptr;
	}

	disc->initialized = false;
}

#else
// Linux - Avahi client library (dynamically loaded)
#include <dlfcn.h>

// Avahi types and constants
typedef struct AvahiClient AvahiClient;
typedef struct AvahiEntryGroup AvahiEntryGroup;
typedef struct AvahiSimplePoll AvahiSimplePoll;
typedef struct AvahiPoll AvahiPoll;

typedef enum {
	AVAHI_CLIENT_S_REGISTERING = 1,
	AVAHI_CLIENT_S_RUNNING = 2,
	AVAHI_CLIENT_S_COLLISION = 3,
	AVAHI_CLIENT_FAILURE = 100,
	AVAHI_CLIENT_CONNECTING = 101
} AvahiClientState;

typedef enum {
	AVAHI_ENTRY_GROUP_UNCOMMITED = 0,
	AVAHI_ENTRY_GROUP_REGISTERING = 1,
	AVAHI_ENTRY_GROUP_ESTABLISHED = 2,
	AVAHI_ENTRY_GROUP_COLLISION = 3,
	AVAHI_ENTRY_GROUP_FAILURE = 4
} AvahiEntryGroupState;

typedef enum {
	AVAHI_IF_UNSPEC = -1,
	AVAHI_PROTO_UNSPEC = -1
} AvahiIfIndex;

typedef void (*AvahiClientCallback)(AvahiClient *, AvahiClientState, void *);
typedef void (*AvahiEntryGroupCallback)(AvahiEntryGroup *,
					AvahiEntryGroupState, void *);

// Function pointers
static AvahiSimplePoll *(*avahi_simple_poll_new)(void) = nullptr;
static void (*avahi_simple_poll_free)(AvahiSimplePoll *) = nullptr;
static AvahiPoll *(*avahi_simple_poll_get)(AvahiSimplePoll *) = nullptr;
static AvahiClient *(*avahi_client_new)(const AvahiPoll *, int,
					AvahiClientCallback, void *,
					int *) = nullptr;
static void (*avahi_client_free)(AvahiClient *) = nullptr;
static AvahiEntryGroup *(*avahi_entry_group_new)(AvahiClient *,
						 AvahiEntryGroupCallback,
						 void *) = nullptr;
static int (*avahi_entry_group_add_service)(AvahiEntryGroup *, int, int, int,
					    const char *, const char *,
					    const char *, const char *,
					    uint16_t, ...) = nullptr;
static int (*avahi_entry_group_commit)(AvahiEntryGroup *) = nullptr;
static int (*avahi_entry_group_reset)(AvahiEntryGroup *) = nullptr;
static void (*avahi_entry_group_free)(AvahiEntryGroup *) = nullptr;
static const char *(*avahi_strerror)(int) = nullptr;
static int (*avahi_client_errno)(AvahiClient *) = nullptr;

static void *avahi_common_handle = nullptr;
static void *avahi_client_handle = nullptr;
static bool avahi_loaded = false;

struct gaze_discovery_platform {
	AvahiSimplePoll *poll;
	AvahiClient *client;
	AvahiEntryGroup *group;
};

static bool load_avahi(void)
{
	if (avahi_loaded)
		return true;

	avahi_common_handle = dlopen("libavahi-common.so.3", RTLD_NOW);
	if (!avahi_common_handle) {
		avahi_common_handle = dlopen("libavahi-common.so", RTLD_NOW);
	}
	if (!avahi_common_handle) {
		obs_log(LOG_DEBUG,
			"[gaze-discovery] libavahi-common not found");
		return false;
	}

	avahi_client_handle = dlopen("libavahi-client.so.3", RTLD_NOW);
	if (!avahi_client_handle) {
		avahi_client_handle = dlopen("libavahi-client.so", RTLD_NOW);
	}
	if (!avahi_client_handle) {
		obs_log(LOG_DEBUG,
			"[gaze-discovery] libavahi-client not found");
		dlclose(avahi_common_handle);
		return false;
	}

	// Load common functions
	avahi_simple_poll_new = (decltype(avahi_simple_poll_new))dlsym(
		avahi_common_handle, "avahi_simple_poll_new");
	avahi_simple_poll_free = (decltype(avahi_simple_poll_free))dlsym(
		avahi_common_handle, "avahi_simple_poll_free");
	avahi_simple_poll_get = (decltype(avahi_simple_poll_get))dlsym(
		avahi_common_handle, "avahi_simple_poll_get");
	avahi_strerror = (decltype(avahi_strerror))dlsym(avahi_common_handle,
							 "avahi_strerror");

	// Load client functions
	avahi_client_new = (decltype(avahi_client_new))dlsym(
		avahi_client_handle, "avahi_client_new");
	avahi_client_free = (decltype(avahi_client_free))dlsym(
		avahi_client_handle, "avahi_client_free");
	avahi_client_errno = (decltype(avahi_client_errno))dlsym(
		avahi_client_handle, "avahi_client_errno");
	avahi_entry_group_new = (decltype(avahi_entry_group_new))dlsym(
		avahi_client_handle, "avahi_entry_group_new");
	avahi_entry_group_add_service =
		(decltype(avahi_entry_group_add_service))dlsym(
			avahi_client_handle, "avahi_entry_group_add_service");
	avahi_entry_group_commit = (decltype(avahi_entry_group_commit))dlsym(
		avahi_client_handle, "avahi_entry_group_commit");
	avahi_entry_group_reset = (decltype(avahi_entry_group_reset))dlsym(
		avahi_client_handle, "avahi_entry_group_reset");
	avahi_entry_group_free = (decltype(avahi_entry_group_free))dlsym(
		avahi_client_handle, "avahi_entry_group_free");

	if (!avahi_simple_poll_new || !avahi_client_new ||
	    !avahi_entry_group_new || !avahi_entry_group_add_service) {
		obs_log(LOG_WARNING,
			"[gaze-discovery] Failed to load Avahi functions");
		dlclose(avahi_client_handle);
		dlclose(avahi_common_handle);
		return false;
	}

	avahi_loaded = true;
	obs_log(LOG_INFO, "[gaze-discovery] Avahi mDNS support loaded");
	return true;
}

static void client_callback(AvahiClient *c, AvahiClientState state,
			    void *userdata)
{
	(void)c;
	(void)state;
	(void)userdata;
	// Simplified callback - just log state changes
}

static void group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state,
			   void *userdata)
{
	(void)g;
	gaze_discovery_t *disc = (gaze_discovery_t *)userdata;

	switch (state) {
	case AVAHI_ENTRY_GROUP_ESTABLISHED:
		disc->registered = true;
		obs_log(LOG_INFO,
			"[gaze-discovery] Avahi service registered: %s",
			disc->service_name);
		break;
	case AVAHI_ENTRY_GROUP_FAILURE:
		disc->registered = false;
		obs_log(LOG_WARNING,
			"[gaze-discovery] Avahi registration failed");
		break;
	default:
		break;
	}
}

bool gaze_discovery_available(void)
{
	return load_avahi();
}

bool gaze_discovery_init(gaze_discovery_t *disc)
{
	if (!disc)
		return false;

	memset(disc, 0, sizeof(*disc));

	if (!load_avahi()) {
		disc->initialized = true;
		return true;
	}

	disc->platform_handle = new gaze_discovery_platform();
	if (!disc->platform_handle)
		return false;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;
	platform->poll = nullptr;
	platform->client = nullptr;
	platform->group = nullptr;

	// Create simple poll
	platform->poll = avahi_simple_poll_new();
	if (!platform->poll) {
		obs_log(LOG_WARNING,
			"[gaze-discovery] Failed to create Avahi poll");
		delete platform;
		disc->platform_handle = nullptr;
		disc->initialized = true;
		return true;
	}

	// Create client
	int error = 0;
	platform->client =
		avahi_client_new(avahi_simple_poll_get(platform->poll), 0,
				 client_callback, disc, &error);
	if (!platform->client) {
		obs_log(LOG_WARNING,
			"[gaze-discovery] Failed to create Avahi client: %s",
			avahi_strerror ? avahi_strerror(error) : "unknown");
		avahi_simple_poll_free(platform->poll);
		delete platform;
		disc->platform_handle = nullptr;
		disc->initialized = true;
		return true;
	}

	disc->initialized = true;
	return true;
}

bool gaze_discovery_register(gaze_discovery_t *disc, const char *name,
			     uint16_t port, gaze_codec_t codec,
			     uint32_t width, uint32_t height, uint32_t fps,
			     uint32_t if_index, uint32_t ip_addr)
{
	if (!disc || !disc->initialized || !name)
		return false;

	strncpy(disc->service_name, name, sizeof(disc->service_name) - 1);
	disc->port = port;
	disc->codec = codec;
	disc->width = width;
	disc->height = height;
	disc->fps = fps;
	disc->if_index = if_index;
	disc->ip_addr = ip_addr;

	if (!disc->platform_handle) {
		obs_log(LOG_DEBUG,
			"[gaze-discovery] Avahi not available, service info stored");
		return true;
	}

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;

	// Reset existing group
	if (platform->group) {
		avahi_entry_group_reset(platform->group);
	} else {
		platform->group = avahi_entry_group_new(
			platform->client, group_callback, disc);
		if (!platform->group) {
			obs_log(LOG_WARNING,
				"[gaze-discovery] Failed to create entry group");
			return false;
		}
	}

	// Build TXT record strings
	char txt_codec[32], txt_width[32], txt_height[32], txt_fps[32],
		txt_version[32], txt_ip[48];
	snprintf(txt_codec, sizeof(txt_codec), "codec=%s",
		 codec == GAZE_CODEC_HEVC ? "hevc" : "h264");
	snprintf(txt_width, sizeof(txt_width), "width=%u", width);
	snprintf(txt_height, sizeof(txt_height), "height=%u", height);
	snprintf(txt_fps, sizeof(txt_fps), "fps=%u", fps);
	snprintf(txt_version, sizeof(txt_version), "version=%d",
		 GAZE_PROTOCOL_VERSION);

	// Add IP address to TXT record if bound to specific interface
	bool has_ip = (ip_addr != 0);
	if (has_ip) {
		struct in_addr addr;
		addr.s_addr = ip_addr;
		char ip_str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
		snprintf(txt_ip, sizeof(txt_ip), "ip=%s", ip_str);
	}

	// Use specific interface if provided, otherwise all interfaces
	int avahi_if = (if_index > 0) ? (int)if_index : AVAHI_IF_UNSPEC;

	int ret;
	if (has_ip) {
		ret = avahi_entry_group_add_service(
			platform->group, avahi_if, AVAHI_PROTO_UNSPEC, 0, name,
			"_gazestream._udp", nullptr, nullptr, port, txt_codec,
			txt_width, txt_height, txt_fps, txt_version, txt_ip,
			nullptr);
	} else {
		ret = avahi_entry_group_add_service(
			platform->group, avahi_if, AVAHI_PROTO_UNSPEC, 0, name,
			"_gazestream._udp", nullptr, nullptr, port, txt_codec,
			txt_width, txt_height, txt_fps, txt_version, nullptr);
	}

	if (ret < 0) {
		obs_log(LOG_WARNING,
			"[gaze-discovery] Failed to add service: %s",
			avahi_strerror ? avahi_strerror(ret) : "unknown");
		return false;
	}

	ret = avahi_entry_group_commit(platform->group);
	if (ret < 0) {
		obs_log(LOG_WARNING,
			"[gaze-discovery] Failed to commit service: %s",
			avahi_strerror ? avahi_strerror(ret) : "unknown");
		return false;
	}

	return true;
}

bool gaze_discovery_update(gaze_discovery_t *disc, uint32_t width,
			   uint32_t height, uint32_t fps)
{
	if (!disc || !disc->initialized)
		return false;

	if (!disc->registered && disc->service_name[0] != '\0') {
		return gaze_discovery_register(disc, disc->service_name,
					       disc->port, disc->codec, width,
					       height, fps, disc->if_index,
					       disc->ip_addr);
	}

	if (disc->registered) {
		return gaze_discovery_register(disc, disc->service_name,
					       disc->port, disc->codec, width,
					       height, fps, disc->if_index,
					       disc->ip_addr);
	}

	return true;
}

void gaze_discovery_unregister(gaze_discovery_t *disc)
{
	if (!disc || !disc->platform_handle)
		return;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;

	if (platform->group) {
		avahi_entry_group_reset(platform->group);
		avahi_entry_group_free(platform->group);
		platform->group = nullptr;
	}

	disc->registered = false;
}

void gaze_discovery_destroy(gaze_discovery_t *disc)
{
	if (!disc)
		return;

	gaze_discovery_unregister(disc);

	if (disc->platform_handle) {
		auto *platform =
			(gaze_discovery_platform *)disc->platform_handle;

		if (platform->client) {
			avahi_client_free(platform->client);
		}
		if (platform->poll) {
			avahi_simple_poll_free(platform->poll);
		}

		delete platform;
		disc->platform_handle = nullptr;
	}

	disc->initialized = false;
}

#endif

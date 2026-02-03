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
			     uint32_t if_index)
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
				       disc->if_index);
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
// Windows - Native dnsapi.dll (like Sunshine)
#include <WinSock2.h>
#include <Windows.h>
#include <WinDNS.h>

// Function pointer types (use SDK types, they're already defined)
typedef DWORD (*DnsServiceRegister_fn)(PDNS_SERVICE_REGISTER_REQUEST pRequest,
				       PDNS_SERVICE_CANCEL pCancel);
typedef DWORD (*DnsServiceDeRegister_fn)(PDNS_SERVICE_REGISTER_REQUEST pRequest,
					 PDNS_SERVICE_CANCEL pCancel);
typedef VOID (*DnsServiceFreeInstance_fn)(PDNS_SERVICE_INSTANCE pInstance);

// Global function pointers
static DnsServiceRegister_fn pDnsServiceRegister = nullptr;
static DnsServiceDeRegister_fn pDnsServiceDeRegister = nullptr;
static DnsServiceFreeInstance_fn pDnsServiceFreeInstance = nullptr;
static HMODULE hDnsApi = nullptr;
static bool dns_funcs_loaded = false;

struct gaze_discovery_platform {
	PDNS_SERVICE_INSTANCE instance;
	wchar_t *instance_name;
	wchar_t *host_name;
};

// Convert UTF-8 to wide string
static wchar_t *utf8_to_wide(const char *utf8)
{
	if (!utf8)
		return nullptr;
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
	if (len <= 0)
		return nullptr;
	wchar_t *wide = (wchar_t *)malloc(len * sizeof(wchar_t));
	if (!wide)
		return nullptr;
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, len);
	return wide;
}

// Get computer name
static char *get_hostname(void)
{
	static char hostname[256] = {0};
	if (hostname[0] == '\0') {
		DWORD size = sizeof(hostname);
		GetComputerNameExA(ComputerNameDnsHostname, hostname, &size);
	}
	return hostname;
}

static bool load_dns_functions(void)
{
	if (dns_funcs_loaded)
		return true;

	hDnsApi = LoadLibraryA("dnsapi.dll");
	if (!hDnsApi) {
		obs_log(LOG_WARNING,
			"[gaze-discovery] Failed to load dnsapi.dll");
		return false;
	}

	pDnsServiceRegister = (DnsServiceRegister_fn)GetProcAddress(
		hDnsApi, "DnsServiceRegister");
	pDnsServiceDeRegister = (DnsServiceDeRegister_fn)GetProcAddress(
		hDnsApi, "DnsServiceDeRegister");
	pDnsServiceFreeInstance = (DnsServiceFreeInstance_fn)GetProcAddress(
		hDnsApi, "DnsServiceFreeInstance");

	if (!pDnsServiceRegister || !pDnsServiceDeRegister ||
	    !pDnsServiceFreeInstance) {
		obs_log(LOG_WARNING,
			"[gaze-discovery] mDNS functions not available in dnsapi.dll (requires Windows 10+)");
		FreeLibrary(hDnsApi);
		hDnsApi = nullptr;
		return false;
	}

	dns_funcs_loaded = true;
	obs_log(LOG_INFO, "[gaze-discovery] Windows mDNS support loaded");
	return true;
}

// Registration callback (matches DNS_SERVICE_REGISTER_COMPLETE signature)
static VOID WINAPI register_callback(DWORD status, PVOID context,
				     PDNS_SERVICE_INSTANCE instance)
{
	gaze_discovery_t *disc = (gaze_discovery_t *)context;
	auto *platform = (gaze_discovery_platform *)disc->platform_handle;

	if (status == ERROR_SUCCESS) {
		platform->instance = instance;
		disc->registered = true;
		obs_log(LOG_INFO,
			"[gaze-discovery] Registered mDNS service: %s",
			disc->service_name);
	} else {
		disc->registered = false;
		obs_log(LOG_WARNING,
			"[gaze-discovery] mDNS registration failed: %lu",
			status);
	}
}

bool gaze_discovery_available(void)
{
	return load_dns_functions();
}

bool gaze_discovery_init(gaze_discovery_t *disc)
{
	if (!disc)
		return false;

	memset(disc, 0, sizeof(*disc));

	if (!load_dns_functions()) {
		disc->initialized = true; // Still mark as initialized for fallback
		return true;
	}

	disc->platform_handle = new gaze_discovery_platform();
	if (!disc->platform_handle)
		return false;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;
	platform->instance = nullptr;
	platform->instance_name = nullptr;
	platform->host_name = nullptr;

	disc->initialized = true;
	return true;
}

bool gaze_discovery_register(gaze_discovery_t *disc, const char *name,
			     uint16_t port, gaze_codec_t codec,
			     uint32_t width, uint32_t height, uint32_t fps,
			     uint32_t if_index)
{
	if (!disc || !disc->initialized || !name)
		return false;

	// Store service info regardless of mDNS availability
	strncpy(disc->service_name, name, sizeof(disc->service_name) - 1);
	disc->port = port;
	disc->codec = codec;
	disc->width = width;
	disc->height = height;
	disc->fps = fps;
	disc->if_index = if_index;

	if (!dns_funcs_loaded || !disc->platform_handle) {
		obs_log(LOG_DEBUG,
			"[gaze-discovery] mDNS not available, service info stored: %s on port %u",
			name, port);
		return true;
	}

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;

	// Unregister existing service
	if (platform->instance) {
		gaze_discovery_unregister(disc);
	}

	// Build instance name: "StreamName._gazestream._udp.local"
	char instance_name_utf8[512];
	snprintf(instance_name_utf8, sizeof(instance_name_utf8),
		 "%s._gazestream._udp.local", name);

	// Build host name: "HOSTNAME.local"
	char host_name_utf8[256];
	snprintf(host_name_utf8, sizeof(host_name_utf8), "%s.local",
		 get_hostname());

	// Convert to wide strings
	platform->instance_name = utf8_to_wide(instance_name_utf8);
	platform->host_name = utf8_to_wide(host_name_utf8);

	if (!platform->instance_name || !platform->host_name) {
		obs_log(LOG_ERROR,
			"[gaze-discovery] Failed to convert strings");
		return false;
	}

	// Build TXT record properties
	// Format: key=value as wide strings
	wchar_t txt_codec[32], txt_width[32], txt_height[32], txt_fps[32],
		txt_version[32];
	swprintf(txt_codec, 32, L"codec=%hs",
		 codec == GAZE_CODEC_HEVC ? "hevc" : "h264");
	swprintf(txt_width, 32, L"width=%u", width);
	swprintf(txt_height, 32, L"height=%u", height);
	swprintf(txt_fps, 32, L"fps=%u", fps);
	swprintf(txt_version, 32, L"version=%d", GAZE_PROTOCOL_VERSION);

	PWSTR keys[] = {(PWSTR)L"codec", (PWSTR)L"width", (PWSTR)L"height",
			(PWSTR)L"fps", (PWSTR)L"version", nullptr};
	PWSTR values[] = {txt_codec + 6,   txt_width + 6, txt_height + 7,
			  txt_fps + 4,     txt_version + 8, nullptr};

	// Create service instance
	DNS_SERVICE_INSTANCE instance = {};
	instance.pszInstanceName = platform->instance_name;
	instance.pszHostName = platform->host_name;
	instance.wPort = port;
	instance.dwPropertyCount = 5;
	instance.keys = keys;
	instance.values = values;
	// Note: Windows DNS-SD API doesn't directly support interface binding
	// The if_index is stored for consistency but Windows registers on all interfaces

	// Register
	DNS_SERVICE_REGISTER_REQUEST request = {};
	request.Version = DNS_QUERY_REQUEST_VERSION1;
	request.InterfaceIndex = if_index;  // 0 = all interfaces, or specific interface
	request.pServiceInstance = &instance;
	request.pRegisterCompletionCallback = register_callback;
	request.pQueryContext = disc;
	request.unicastEnabled = FALSE;

	obs_log(LOG_INFO,
		"[gaze-discovery] Registering mDNS: %s on port %u, interface %u",
		name, port, if_index);

	DWORD status = pDnsServiceRegister(&request, nullptr);

	obs_log(LOG_INFO,
		"[gaze-discovery] DnsServiceRegister returned: %lu (PENDING=%lu, SUCCESS=%lu)",
		status, (DWORD)DNS_REQUEST_PENDING, (DWORD)ERROR_SUCCESS);

	if (status != DNS_REQUEST_PENDING && status != ERROR_SUCCESS) {
		obs_log(LOG_WARNING,
			"[gaze-discovery] DnsServiceRegister failed: %lu",
			status);
		return false;
	}

	// Wait for callback - may need longer on some systems
	Sleep(200);

	obs_log(LOG_INFO,
		"[gaze-discovery] After wait: registered=%d",
		disc->registered ? 1 : 0);

	return true;
}

bool gaze_discovery_update(gaze_discovery_t *disc, uint32_t width,
			   uint32_t height, uint32_t fps)
{
	if (!disc || !disc->initialized)
		return false;

	// If not registered yet, do initial registration
	if (!disc->registered && disc->service_name[0] != '\0') {
		return gaze_discovery_register(disc, disc->service_name,
					       disc->port, disc->codec, width,
					       height, fps, disc->if_index);
	}

	// Re-register with new parameters
	if (disc->registered) {
		return gaze_discovery_register(disc, disc->service_name,
					       disc->port, disc->codec, width,
					       height, fps, disc->if_index);
	}

	return true;
}

void gaze_discovery_unregister(gaze_discovery_t *disc)
{
	if (!disc || !disc->platform_handle)
		return;

	auto *platform = (gaze_discovery_platform *)disc->platform_handle;

	if (platform->instance && pDnsServiceDeRegister) {
		DNS_SERVICE_REGISTER_REQUEST request = {};
		request.Version = DNS_QUERY_REQUEST_VERSION1;
		request.pServiceInstance = platform->instance;
		request.pRegisterCompletionCallback =
			register_callback;
		request.pQueryContext = disc;

		pDnsServiceDeRegister(&request, nullptr);
		Sleep(50);

		if (pDnsServiceFreeInstance) {
			pDnsServiceFreeInstance(platform->instance);
		}
		platform->instance = nullptr;
	}

	if (platform->instance_name) {
		free(platform->instance_name);
		platform->instance_name = nullptr;
	}
	if (platform->host_name) {
		free(platform->host_name);
		platform->host_name = nullptr;
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
			     uint32_t if_index)
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
		txt_version[32];
	snprintf(txt_codec, sizeof(txt_codec), "codec=%s",
		 codec == GAZE_CODEC_HEVC ? "hevc" : "h264");
	snprintf(txt_width, sizeof(txt_width), "width=%u", width);
	snprintf(txt_height, sizeof(txt_height), "height=%u", height);
	snprintf(txt_fps, sizeof(txt_fps), "fps=%u", fps);
	snprintf(txt_version, sizeof(txt_version), "version=%d",
		 GAZE_PROTOCOL_VERSION);

	// Use specific interface if provided, otherwise all interfaces
	int avahi_if = (if_index > 0) ? (int)if_index : AVAHI_IF_UNSPEC;

	int ret = avahi_entry_group_add_service(
		platform->group, avahi_if, AVAHI_PROTO_UNSPEC, 0, name,
		"_gazestream._udp", nullptr, nullptr, port, txt_codec,
		txt_width, txt_height, txt_fps, txt_version, nullptr);

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
					       height, fps, disc->if_index);
	}

	if (disc->registered) {
		return gaze_discovery_register(disc, disc->service_name,
					       disc->port, disc->codec, width,
					       height, fps, disc->if_index);
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

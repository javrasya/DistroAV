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
 * - Windows: dns-sd.h (Bonjour SDK) or stub
 * - macOS: dns_sd.h (native)
 * - Linux: Avahi client library or stub
 *
 * For initial implementation, we provide a stub that logs
 * but doesn't actually register. This allows the filter to work
 * without mDNS dependencies while providing a hook for future
 * implementation.
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
			     uint32_t width, uint32_t height, uint32_t fps)
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

	// Build TXT record
	char txt_codec[32], txt_width[32], txt_height[32], txt_fps[32],
		txt_version[32];
	snprintf(txt_codec, sizeof(txt_codec), "codec=%s",
		 codec == GAZE_CODEC_HEVC ? "hevc" : "h264");
	snprintf(txt_width, sizeof(txt_width), "width=%u", width);
	snprintf(txt_height, sizeof(txt_height), "height=%u", height);
	snprintf(txt_fps, sizeof(txt_fps), "fps=%u", fps);
	snprintf(txt_version, sizeof(txt_version), "version=%d",
		 GAZE_PROTOCOL_VERSION);

	// Create TXT record
	TXTRecordRef txt_ref;
	TXTRecordCreate(&txt_ref, 0, nullptr);
	TXTRecordSetValue(&txt_ref, "codec",
			  strlen(codec == GAZE_CODEC_HEVC ? "hevc" : "h264"),
			  codec == GAZE_CODEC_HEVC ? "hevc" : "h264");

	char buf[16];
	snprintf(buf, sizeof(buf), "%u", width);
	TXTRecordSetValue(&txt_ref, "width", strlen(buf), buf);
	snprintf(buf, sizeof(buf), "%u", height);
	TXTRecordSetValue(&txt_ref, "height", strlen(buf), buf);
	snprintf(buf, sizeof(buf), "%u", fps);
	TXTRecordSetValue(&txt_ref, "fps", strlen(buf), buf);
	snprintf(buf, sizeof(buf), "%d", GAZE_PROTOCOL_VERSION);
	TXTRecordSetValue(&txt_ref, "version", strlen(buf), buf);

	DNSServiceErrorType err =
		DNSServiceRegister(&platform->service_ref, 0, 0, name,
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
				       disc->codec, width, height, fps);
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
// Windows - stub implementation (Bonjour SDK not always available)
// TODO: Add optional Bonjour support when SDK is available

bool gaze_discovery_available(void)
{
	// Return false until Bonjour SDK is integrated
	return false;
}

bool gaze_discovery_init(gaze_discovery_t *disc)
{
	if (!disc)
		return false;

	memset(disc, 0, sizeof(*disc));
	disc->initialized = true;

	obs_log(LOG_DEBUG,
		"[gaze-discovery] mDNS not available on Windows (Bonjour SDK required)");

	return true;
}

bool gaze_discovery_register(gaze_discovery_t *disc, const char *name,
			     uint16_t port, gaze_codec_t codec,
			     uint32_t width, uint32_t height, uint32_t fps)
{
	if (!disc || !disc->initialized)
		return false;

	// Store info for logging purposes
	strncpy(disc->service_name, name, sizeof(disc->service_name) - 1);
	disc->port = port;
	disc->codec = codec;
	disc->width = width;
	disc->height = height;
	disc->fps = fps;

	obs_log(LOG_DEBUG,
		"[gaze-discovery] Would register: %s (%s %ux%u@%u on port %u)",
		name, codec == GAZE_CODEC_HEVC ? "HEVC" : "H.264", width,
		height, fps, port);

	// Pretend success but don't actually register
	return true;
}

bool gaze_discovery_update(gaze_discovery_t *disc, uint32_t width,
			   uint32_t height, uint32_t fps)
{
	if (!disc || !disc->initialized)
		return false;

	disc->width = width;
	disc->height = height;
	disc->fps = fps;

	return true;
}

void gaze_discovery_unregister(gaze_discovery_t *disc)
{
	if (!disc)
		return;

	disc->registered = false;
}

void gaze_discovery_destroy(gaze_discovery_t *disc)
{
	if (!disc)
		return;

	disc->initialized = false;
}

#else
// Linux - stub implementation (Avahi integration TODO)
// TODO: Add Avahi client support

bool gaze_discovery_available(void)
{
	// Return false until Avahi is integrated
	return false;
}

bool gaze_discovery_init(gaze_discovery_t *disc)
{
	if (!disc)
		return false;

	memset(disc, 0, sizeof(*disc));
	disc->initialized = true;

	obs_log(LOG_DEBUG,
		"[gaze-discovery] mDNS not available on Linux (Avahi required)");

	return true;
}

bool gaze_discovery_register(gaze_discovery_t *disc, const char *name,
			     uint16_t port, gaze_codec_t codec,
			     uint32_t width, uint32_t height, uint32_t fps)
{
	if (!disc || !disc->initialized)
		return false;

	strncpy(disc->service_name, name, sizeof(disc->service_name) - 1);
	disc->port = port;
	disc->codec = codec;
	disc->width = width;
	disc->height = height;
	disc->fps = fps;

	obs_log(LOG_DEBUG,
		"[gaze-discovery] Would register: %s (%s %ux%u@%u on port %u)",
		name, codec == GAZE_CODEC_HEVC ? "HEVC" : "H.264", width,
		height, fps, port);

	return true;
}

bool gaze_discovery_update(gaze_discovery_t *disc, uint32_t width,
			   uint32_t height, uint32_t fps)
{
	if (!disc || !disc->initialized)
		return false;

	disc->width = width;
	disc->height = height;
	disc->fps = fps;

	return true;
}

void gaze_discovery_unregister(gaze_discovery_t *disc)
{
	if (!disc)
		return;

	disc->registered = false;
}

void gaze_discovery_destroy(gaze_discovery_t *disc)
{
	if (!disc)
		return;

	disc->initialized = false;
}

#endif

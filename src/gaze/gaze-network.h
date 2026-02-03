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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAZE_MAX_INTERFACES 16
#define GAZE_INTERFACE_NAME_LEN 64
#define GAZE_INTERFACE_IP_LEN 46  // IPv6 max length

/**
 * Network interface information
 */
typedef struct gaze_network_interface {
	char name[GAZE_INTERFACE_NAME_LEN];
	char ip[GAZE_INTERFACE_IP_LEN];
	uint32_t ip_addr;      // IPv4 address in network byte order
	uint32_t index;        // Interface index (for mDNS)
	bool is_primary;       // Has default gateway
	bool is_up;            // Interface is up
} gaze_network_interface_t;

/**
 * List of network interfaces
 */
typedef struct gaze_network_list {
	gaze_network_interface_t interfaces[GAZE_MAX_INTERFACES];
	int count;
	int primary_index;  // Index of primary interface (-1 if none)
} gaze_network_list_t;

/**
 * Get list of available network interfaces.
 *
 * @param list Output list of interfaces
 * @return true on success
 */
bool gaze_network_get_interfaces(gaze_network_list_t *list);

/**
 * Get the primary network interface (one with default gateway).
 *
 * @param iface Output interface info
 * @return true if found, false if no primary interface
 */
bool gaze_network_get_primary(gaze_network_interface_t *iface);

/**
 * Find interface by IP address string.
 *
 * @param list Interface list
 * @param ip IP address string to find
 * @return Pointer to interface or NULL if not found
 */
const gaze_network_interface_t *gaze_network_find_by_ip(
	const gaze_network_list_t *list, const char *ip);

#ifdef __cplusplus
}
#endif

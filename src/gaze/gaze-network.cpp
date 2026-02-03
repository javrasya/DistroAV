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

#include "gaze-network.h"
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

bool gaze_network_get_interfaces(gaze_network_list_t *list)
{
	if (!list)
		return false;

	memset(list, 0, sizeof(*list));
	list->primary_index = -1;

	// Get adapter addresses
	ULONG buf_len = 15000;
	PIP_ADAPTER_ADDRESSES addresses = nullptr;
	ULONG result;

	do {
		addresses = (PIP_ADAPTER_ADDRESSES)malloc(buf_len);
		if (!addresses)
			return false;

		result = GetAdaptersAddresses(
			AF_INET,
			GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
				GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
			nullptr, addresses, &buf_len);

		if (result == ERROR_BUFFER_OVERFLOW) {
			free(addresses);
			addresses = nullptr;
		}
	} while (result == ERROR_BUFFER_OVERFLOW);

	if (result != NO_ERROR) {
		if (addresses)
			free(addresses);
		return false;
	}

	// Find primary interface (one with default gateway)
	uint32_t primary_if_index = 0;
	for (PIP_ADAPTER_ADDRESSES adapter = addresses; adapter;
	     adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp)
			continue;
		if (adapter->FirstGatewayAddress) {
			primary_if_index = adapter->IfIndex;
			break;
		}
	}

	// Enumerate interfaces
	for (PIP_ADAPTER_ADDRESSES adapter = addresses; adapter;
	     adapter = adapter->Next) {
		if (list->count >= GAZE_MAX_INTERFACES)
			break;

		// Skip non-operational interfaces
		if (adapter->OperStatus != IfOperStatusUp)
			continue;

		// Skip loopback
		if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;

		// Get first IPv4 unicast address
		PIP_ADAPTER_UNICAST_ADDRESS unicast =
			adapter->FirstUnicastAddress;
		while (unicast) {
			if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
				struct sockaddr_in *addr =
					(struct sockaddr_in *)
						unicast->Address.lpSockaddr;

				gaze_network_interface_t *iface =
					&list->interfaces[list->count];

				// Convert friendly name to UTF-8
				WideCharToMultiByte(
					CP_UTF8, 0, adapter->FriendlyName, -1,
					iface->name, sizeof(iface->name),
					nullptr, nullptr);

				// Get IP string
				inet_ntop(AF_INET, &addr->sin_addr, iface->ip,
					  sizeof(iface->ip));

				iface->ip_addr = addr->sin_addr.s_addr;
				iface->index = adapter->IfIndex;
				iface->is_up = true;
				iface->is_primary =
					(adapter->IfIndex == primary_if_index);

				if (iface->is_primary) {
					list->primary_index = list->count;
				}

				list->count++;
				break;
			}
			unicast = unicast->Next;
		}
	}

	free(addresses);
	return true;
}

#else
// Linux/macOS
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/rtnetlink.h>
#include <sys/socket.h>

// Get interface with default gateway on Linux
static uint32_t get_default_gateway_ifindex(void)
{
	int sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (sock < 0)
		return 0;

	struct {
		struct nlmsghdr nlh;
		struct rtmsg rtm;
	} request;

	memset(&request, 0, sizeof(request));
	request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	request.nlh.nlmsg_type = RTM_GETROUTE;
	request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	request.rtm.rtm_family = AF_INET;
	request.rtm.rtm_table = RT_TABLE_MAIN;

	if (send(sock, &request, request.nlh.nlmsg_len, 0) < 0) {
		close(sock);
		return 0;
	}

	char buffer[8192];
	int len = recv(sock, buffer, sizeof(buffer), 0);
	close(sock);

	if (len < 0)
		return 0;

	uint32_t ifindex = 0;
	for (struct nlmsghdr *nlh = (struct nlmsghdr *)buffer;
	     NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len)) {
		if (nlh->nlmsg_type == NLMSG_DONE)
			break;
		if (nlh->nlmsg_type != RTM_NEWROUTE)
			continue;

		struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
		if (rtm->rtm_dst_len == 0) {  // Default route
			struct rtattr *rta = RTM_RTA(rtm);
			int rtl = RTM_PAYLOAD(nlh);
			while (RTA_OK(rta, rtl)) {
				if (rta->rta_type == RTA_OIF) {
					ifindex = *(uint32_t *)RTA_DATA(rta);
					break;
				}
				rta = RTA_NEXT(rta, rtl);
			}
			if (ifindex)
				break;
		}
	}

	return ifindex;
}
#else
// macOS - simpler approach, just pick first non-loopback
static uint32_t get_default_gateway_ifindex(void)
{
	return 0;  // Will use first non-loopback
}
#endif

bool gaze_network_get_interfaces(gaze_network_list_t *list)
{
	if (!list)
		return false;

	memset(list, 0, sizeof(*list));
	list->primary_index = -1;

	struct ifaddrs *ifaddr;
	if (getifaddrs(&ifaddr) == -1)
		return false;

	uint32_t primary_ifindex = get_default_gateway_ifindex();

	for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
		if (list->count >= GAZE_MAX_INTERFACES)
			break;

		if (!ifa->ifa_addr)
			continue;

		// Only IPv4
		if (ifa->ifa_addr->sa_family != AF_INET)
			continue;

		// Skip loopback
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;

		// Skip down interfaces
		if (!(ifa->ifa_flags & IFF_UP))
			continue;

		struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
		gaze_network_interface_t *iface =
			&list->interfaces[list->count];

		strncpy(iface->name, ifa->ifa_name, sizeof(iface->name) - 1);
		inet_ntop(AF_INET, &addr->sin_addr, iface->ip,
			  sizeof(iface->ip));
		iface->ip_addr = addr->sin_addr.s_addr;
		iface->index = if_nametoindex(ifa->ifa_name);
		iface->is_up = true;
		iface->is_primary = (iface->index == primary_ifindex);

		// On macOS, if no primary detected, use first one
		if (list->primary_index < 0 &&
		    (iface->is_primary || primary_ifindex == 0)) {
			iface->is_primary = true;
			list->primary_index = list->count;
		}

		list->count++;
	}

	freeifaddrs(ifaddr);
	return true;
}

#endif

bool gaze_network_get_primary(gaze_network_interface_t *iface)
{
	if (!iface)
		return false;

	gaze_network_list_t list;
	if (!gaze_network_get_interfaces(&list))
		return false;

	if (list.primary_index >= 0) {
		*iface = list.interfaces[list.primary_index];
		return true;
	}

	// Fallback: return first interface
	if (list.count > 0) {
		*iface = list.interfaces[0];
		return true;
	}

	return false;
}

const gaze_network_interface_t *
gaze_network_find_by_ip(const gaze_network_list_t *list, const char *ip)
{
	if (!list || !ip)
		return nullptr;

	for (int i = 0; i < list->count; i++) {
		if (strcmp(list->interfaces[i].ip, ip) == 0) {
			return &list->interfaces[i];
		}
	}

	return nullptr;
}

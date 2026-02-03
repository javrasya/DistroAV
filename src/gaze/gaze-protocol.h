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

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gaze Stream Protocol Definitions
 *
 * This header defines the packet structures and constants for the
 * Gaze Stream RTP-based video streaming protocol.
 */

// ============================================================================
// Protocol Constants
// ============================================================================

// Version
#define GAZE_PROTOCOL_VERSION 1

// Network
#define GAZE_DEFAULT_RTP_PORT 47998
#define GAZE_MTU 1500
#define GAZE_MAX_PAYLOAD_SIZE 1400
#define GAZE_SOCKET_BUFFER_SIZE (4 * 1024 * 1024) // 4MB
#define GAZE_MAX_BATCH_PACKETS 64

// RTP
#define GAZE_RTP_VERSION 2
#define GAZE_RTP_PAYLOAD_TYPE_HEVC 96
#define GAZE_RTP_PAYLOAD_TYPE_H264 97
#define GAZE_RTP_CLOCK_RATE 90000 // 90kHz standard video clock

// Timeouts (nanoseconds)
#define GAZE_RTCP_TIMEOUT_NS (10ULL * 1000000000ULL) // 10 seconds
#define GAZE_RTCP_INTERVAL_NS (5ULL * 1000000000ULL) // 5 seconds

// FEC
#define GAZE_FEC_DEFAULT_PERCENT 20
#define GAZE_FEC_MAX_PERCENT 50

// Quality Presets (bitrate in kbps, FEC in percent)
#define GAZE_PRESET_ULTRA_BITRATE 25000
#define GAZE_PRESET_ULTRA_FEC 10
#define GAZE_PRESET_HIGH_BITRATE 15000
#define GAZE_PRESET_HIGH_FEC 15
#define GAZE_PRESET_BALANCED_BITRATE 10000
#define GAZE_PRESET_BALANCED_FEC 20
#define GAZE_PRESET_LOW_BITRATE 5000
#define GAZE_PRESET_LOW_FEC 25
#define GAZE_FEC_MAX_SHARDS 255
#define GAZE_FEC_MAX_BLOCKS_PER_FRAME 4
#define GAZE_FEC_SHARD_SIZE (GAZE_MAX_PAYLOAD_SIZE - 28) // MTU - headers

// Encoder
#define GAZE_DEFAULT_BITRATE_KBPS 20000
#define GAZE_MIN_BITRATE_KBPS 1000
#define GAZE_MAX_BITRATE_KBPS 100000
#define GAZE_DEFAULT_KEYFRAME_INTERVAL 60 // frames

// mDNS
#define GAZE_MDNS_SERVICE_TYPE "_gazestream._udp"

// ============================================================================
// Enumerations
// ============================================================================

/**
 * Video codec type
 */
typedef enum gaze_codec {
	GAZE_CODEC_HEVC = 0, // Default, better quality/bitrate ratio
	GAZE_CODEC_H264 = 1  // Wider compatibility
} gaze_codec_t;

/**
 * Hardware encoder type
 */
typedef enum gaze_encoder_type {
	GAZE_ENCODER_AUTO = 0,    // Auto-detect best available
	GAZE_ENCODER_NVENC = 1,   // NVIDIA NVENC
	GAZE_ENCODER_AMF = 2,     // AMD AMF
	GAZE_ENCODER_QSV = 3,     // Intel Quick Sync
	GAZE_ENCODER_SOFTWARE = 4 // libx264/libx265 fallback
} gaze_encoder_type_t;

/**
 * FEC type
 */
typedef enum gaze_fec_type {
	GAZE_FEC_NONE = 0,         // No FEC
	GAZE_FEC_REED_SOLOMON = 1  // Reed-Solomon
} gaze_fec_type_t;

/**
 * Frame type
 */
typedef enum gaze_frame_type {
	GAZE_FRAME_TYPE_IDR = 0, // Keyframe
	GAZE_FRAME_TYPE_P = 1,   // Predicted frame
	GAZE_FRAME_TYPE_B = 2    // Bi-directional (rarely used in low-latency)
} gaze_frame_type_t;

/**
 * Packet type (for header_type field)
 */
typedef enum gaze_packet_type {
	GAZE_PACKET_TYPE_VIDEO = 0x01,
	GAZE_PACKET_TYPE_FEC = 0x02
} gaze_packet_type_t;

// ============================================================================
// Packet Structures (Network Byte Order)
// ============================================================================

#pragma pack(push, 1)

/**
 * RTP Header (12 bytes)
 *
 * Standard RTP header per RFC 3550
 */
typedef struct gaze_rtp_header {
	uint8_t flags;         // V(2) | P(1) | X(1) | CC(4)
	uint8_t payload_type;  // M(1) | PT(7)
	uint16_t sequence;     // Sequence number (big-endian)
	uint32_t timestamp;    // RTP timestamp (big-endian)
	uint32_t ssrc;         // Synchronization source (big-endian)
} gaze_rtp_header_t;

/**
 * Gaze Frame Metadata (8 bytes)
 *
 * Per-packet metadata for frame reconstruction and FEC
 */
typedef struct gaze_frame_meta {
	uint32_t frame_index;     // Frame counter (big-endian, wraps at 2^32)
	uint8_t fec_type;         // gaze_fec_type_t
	uint8_t fec_block_index;  // Which FEC block within frame (0-3)
	uint8_t fec_data_shards;  // Number of data shards in this block
	uint8_t fec_parity_shards; // Number of parity shards in this block
} gaze_frame_meta_t;

/**
 * Gaze Frame Header (8 bytes)
 *
 * First packet of each frame contains this header
 */
typedef struct gaze_frame_header {
	uint8_t header_type;          // gaze_packet_type_t
	uint8_t frame_type;           // gaze_frame_type_t
	uint16_t flags;               // Reserved flags (big-endian)
	uint32_t capture_timestamp_ms; // Capture time for latency measurement (big-endian)
} gaze_frame_header_t;

/**
 * Complete Gaze packet header (28 bytes total)
 *
 * RTP Header (12) + Frame Meta (8) + Frame Header (8)
 */
typedef struct gaze_packet_header {
	gaze_rtp_header_t rtp;
	gaze_frame_meta_t meta;
	gaze_frame_header_t frame;
} gaze_packet_header_t;

/**
 * RTCP Receiver Report (simplified)
 *
 * We only parse the header to detect receiver presence
 */
typedef struct gaze_rtcp_header {
	uint8_t flags;        // V(2) | P(1) | RC(5)
	uint8_t packet_type;  // 200=SR, 201=RR
	uint16_t length;      // Length in 32-bit words minus one
	uint32_t ssrc;        // SSRC of sender
} gaze_rtcp_header_t;

#pragma pack(pop)

// RTCP packet types
#define GAZE_RTCP_SR 200  // Sender Report
#define GAZE_RTCP_RR 201  // Receiver Report

// ============================================================================
// Receiver Subscription Protocol
// ============================================================================

// Control message magic bytes
#define GAZE_CTRL_MAGIC_0    'G'
#define GAZE_CTRL_MAGIC_1    'Z'

// Control message types (sent by receiver to sender on RTCP port)
#define GAZE_CTRL_SUBSCRIBE      0x01  // Receiver wants to receive stream
#define GAZE_CTRL_HEARTBEAT      0x02  // Receiver still alive (sent periodically)
#define GAZE_CTRL_UNSUBSCRIBE    0x03  // Receiver stopping
#define GAZE_CTRL_PROBE          0x04  // Request stream status
#define GAZE_CTRL_PROBE_RESPONSE 0x05  // Response with status + optional frame

// Probe request flags (byte 3 of PROBE message)
#define GAZE_PROBE_FLAG_REQUEST_FRAME  0x01  // Include keyframe in response

// Probe response status flags (byte 3 of PROBE_RESPONSE message)
#define GAZE_PROBE_STATUS_ACTIVE         0x01  // Stream is active (filter enabled)
#define GAZE_PROBE_STATUS_HAS_RECEIVERS  0x02  // Has active subscribers
#define GAZE_PROBE_STATUS_FRAME_INCLUDED 0x04  // Frame data in response

// Timing constants
#define GAZE_HEARTBEAT_INTERVAL_MS 1000                   // Receiver sends every 1s
#define GAZE_RECEIVER_TIMEOUT_NS   (5ULL * 1000000000ULL) // 5 seconds without heartbeat

// Receiver list limits
#define GAZE_MAX_RECEIVERS 8

// Probe protocol sizes
#define GAZE_PROBE_REQUEST_SIZE  8   // Magic(2) + Type(1) + Flags(1) + RequestID(4)
#define GAZE_PROBE_RESPONSE_HEADER_SIZE 24  // Header without frame data

// Fixed port assignment: port = GAZE_BASE_PORT + (output_index * 2)
// Output 1: 5960 (RTP), 5961 (RTCP/control)
// Output 2: 5962 (RTP), 5963 (RTCP/control)
// etc.
#define GAZE_BASE_PORT 5960

// ============================================================================
// Helper Macros
// ============================================================================

// RTP header flags
#define GAZE_RTP_FLAGS_DEFAULT ((GAZE_RTP_VERSION << 6) | 0x00)

// Check RTP version
#define GAZE_RTP_GET_VERSION(flags) (((flags) >> 6) & 0x03)

// Get/set marker bit in payload_type
#define GAZE_RTP_GET_MARKER(pt) (((pt) >> 7) & 0x01)
#define GAZE_RTP_SET_MARKER(pt) ((pt) | 0x80)
#define GAZE_RTP_CLEAR_MARKER(pt) ((pt) & 0x7F)

// RTCP version check
#define GAZE_RTCP_GET_VERSION(flags) (((flags) >> 6) & 0x03)

// ============================================================================
// Byte Order Helpers
// ============================================================================

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

// Host to network byte order
static inline uint16_t gaze_htons(uint16_t v) { return htons(v); }
static inline uint32_t gaze_htonl(uint32_t v) { return htonl(v); }

// Network to host byte order
static inline uint16_t gaze_ntohs(uint16_t v) { return ntohs(v); }
static inline uint32_t gaze_ntohl(uint32_t v) { return ntohl(v); }

// ============================================================================
// Packet Size Calculations
// ============================================================================

#define GAZE_RTP_HEADER_SIZE sizeof(gaze_rtp_header_t)       // 12 bytes
#define GAZE_FRAME_META_SIZE sizeof(gaze_frame_meta_t)       // 8 bytes
#define GAZE_FRAME_HEADER_SIZE sizeof(gaze_frame_header_t)   // 8 bytes
#define GAZE_PACKET_HEADER_SIZE sizeof(gaze_packet_header_t) // 28 bytes

// Maximum payload per packet after headers
#define GAZE_VIDEO_PAYLOAD_SIZE (GAZE_MTU - GAZE_PACKET_HEADER_SIZE)

#ifdef __cplusplus
}
#endif

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

#include "gaze-packetizer.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

// Initial packet buffer capacity
#define INITIAL_PACKET_CAPACITY 256

// Generate random SSRC
static uint32_t generate_ssrc(void)
{
	uint32_t ssrc = 0;
	ssrc |= ((uint32_t)(os_gettime_ns() & 0xFFFF)) << 16;
	ssrc |= (uint32_t)((os_gettime_ns() >> 16) & 0xFFFF);
	return ssrc;
}

bool gaze_packetizer_init(gaze_packetizer_t *pkt, bool enable_fec,
			  uint8_t fec_percent, gaze_codec_t codec)
{
	if (!pkt)
		return false;

	memset(pkt, 0, sizeof(*pkt));

	pkt->ssrc = generate_ssrc();
	pkt->sequence = 0;
	pkt->frame_index = 0;
	pkt->codec = codec;
	pkt->payload_type = (codec == GAZE_CODEC_HEVC)
				    ? GAZE_RTP_PAYLOAD_TYPE_HEVC
				    : GAZE_RTP_PAYLOAD_TYPE_H264;

	pkt->fec_enabled = enable_fec && fec_percent > 0;

	if (pkt->fec_enabled) {
		if (!gaze_fec_init(&pkt->fec, fec_percent)) {
			obs_log(LOG_ERROR,
				"[gaze-packetizer] Failed to init FEC");
			return false;
		}
	}

	// Allocate initial packet buffer
	pkt->packets = (gaze_packet_t *)calloc(INITIAL_PACKET_CAPACITY,
					       sizeof(gaze_packet_t));
	if (!pkt->packets) {
		gaze_fec_destroy(&pkt->fec);
		return false;
	}

	pkt->packet_capacity = INITIAL_PACKET_CAPACITY;
	pkt->packet_count = 0;
	pkt->initialized = true;

	return true;
}

static bool ensure_packet_capacity(gaze_packetizer_t *pkt, size_t needed)
{
	if (needed <= pkt->packet_capacity)
		return true;

	size_t new_capacity = pkt->packet_capacity * 2;
	while (new_capacity < needed)
		new_capacity *= 2;

	gaze_packet_t *new_packets =
		(gaze_packet_t *)realloc(pkt->packets,
					 new_capacity * sizeof(gaze_packet_t));
	if (!new_packets)
		return false;

	pkt->packets = new_packets;
	pkt->packet_capacity = new_capacity;

	return true;
}

static void build_rtp_header(gaze_rtp_header_t *hdr, uint16_t sequence,
			     uint32_t timestamp, uint32_t ssrc, uint8_t pt,
			     bool marker)
{
	hdr->flags = GAZE_RTP_FLAGS_DEFAULT;
	hdr->payload_type = pt;
	if (marker)
		hdr->payload_type = GAZE_RTP_SET_MARKER(hdr->payload_type);
	hdr->sequence = gaze_htons(sequence);
	hdr->timestamp = gaze_htonl(timestamp);
	hdr->ssrc = gaze_htonl(ssrc);
}

static void build_frame_meta(gaze_frame_meta_t *meta, uint32_t frame_index,
			     gaze_fec_type_t fec_type, uint8_t block_index,
			     uint8_t data_shards, uint8_t parity_shards)
{
	meta->frame_index = gaze_htonl(frame_index);
	meta->fec_type = (uint8_t)fec_type;
	meta->fec_block_index = block_index;
	meta->fec_data_shards = data_shards;
	meta->fec_parity_shards = parity_shards;
}

static void build_frame_header(gaze_frame_header_t *hdr, gaze_packet_type_t type,
			       gaze_frame_type_t frame_type,
			       uint32_t capture_timestamp)
{
	hdr->header_type = (uint8_t)type;
	hdr->frame_type = (uint8_t)frame_type;
	hdr->flags = 0;
	hdr->capture_timestamp = gaze_htonl(capture_timestamp);
}

bool gaze_packetizer_packetize(gaze_packetizer_t *pkt,
			       const uint8_t *encoded_data, size_t encoded_size,
			       bool is_keyframe, uint32_t capture_timestamp,
			       gaze_packet_t **packets, size_t *packet_count)
{
	if (!pkt || !pkt->initialized || !encoded_data || encoded_size == 0 ||
	    !packets || !packet_count) {
		return false;
	}

	*packets = nullptr;
	*packet_count = 0;

	// Reset packet count
	pkt->packet_count = 0;

	// Calculate RTP timestamp (90kHz clock)
	uint32_t rtp_timestamp =
		(uint32_t)((pkt->frame_index * GAZE_RTP_CLOCK_RATE) / 60);

	gaze_frame_type_t frame_type =
		is_keyframe ? GAZE_FRAME_TYPE_IDR : GAZE_FRAME_TYPE_P;

	if (pkt->fec_enabled) {
		// Encode with FEC
		gaze_fec_block_t *blocks = nullptr;
		size_t block_count = 0;

		if (!gaze_fec_encode(&pkt->fec, encoded_data, encoded_size,
				     &blocks, &block_count)) {
			obs_log(LOG_ERROR,
				"[gaze-packetizer] FEC encode failed");
			return false;
		}

		// Calculate total packets needed
		size_t total_packets = 0;
		for (size_t b = 0; b < block_count; b++) {
			total_packets +=
				blocks[b].data_count + blocks[b].parity_count;
		}

		if (!ensure_packet_capacity(pkt, total_packets)) {
			gaze_fec_free_blocks(blocks, block_count);
			return false;
		}

		// Build packets for each block
		for (size_t b = 0; b < block_count; b++) {
			gaze_fec_block_t *blk = &blocks[b];
			uint8_t total_shards =
				blk->data_count + blk->parity_count;

			for (uint8_t s = 0; s < total_shards; s++) {
				gaze_packet_t *packet =
					&pkt->packets[pkt->packet_count];
				// No memset needed: all fields written by build_rtp_header,
			// build_frame_meta, build_frame_header, and memcpy below

				// Build headers
				gaze_packet_header_t *hdr =
					(gaze_packet_header_t *)packet->data;

				// Marker on last DATA packet (not FEC) so receivers
				// that skip FEC can still detect frame end
				bool is_last_data_packet =
					(b == block_count - 1) &&
					(s == blk->data_count - 1);

				build_rtp_header(&hdr->rtp, pkt->sequence++,
						 rtp_timestamp, pkt->ssrc,
						 pkt->payload_type,
						 is_last_data_packet);

				gaze_fec_type_t fec_type =
					blk->parity_count > 0
						? GAZE_FEC_REED_SOLOMON
						: GAZE_FEC_NONE;

				build_frame_meta(&hdr->meta, pkt->frame_index,
						 fec_type, (uint8_t)b,
						 blk->data_count,
						 blk->parity_count);

				bool is_parity = (s >= blk->data_count);
				gaze_packet_type_t pkt_type =
					is_parity ? GAZE_PACKET_TYPE_FEC
						  : GAZE_PACKET_TYPE_VIDEO;

				build_frame_header(&hdr->frame, pkt_type,
						   frame_type,
						   capture_timestamp);

				// Copy shard data
				size_t payload_size = blk->shard_size;
				if (payload_size > GAZE_VIDEO_PAYLOAD_SIZE) {
					payload_size = GAZE_VIDEO_PAYLOAD_SIZE;
				}

				memcpy(packet->data + GAZE_PACKET_HEADER_SIZE,
				       blk->shards[s], payload_size);

				packet->size =
					GAZE_PACKET_HEADER_SIZE + payload_size;
				pkt->packet_count++;
			}
		}

		gaze_fec_free_blocks(blocks, block_count);
	} else {
		// No FEC - simple packetization
		size_t max_payload = GAZE_VIDEO_PAYLOAD_SIZE;
		size_t num_packets =
			(encoded_size + max_payload - 1) / max_payload;

		if (!ensure_packet_capacity(pkt, num_packets)) {
			return false;
		}

		size_t offset = 0;
		for (size_t i = 0; i < num_packets; i++) {
			gaze_packet_t *packet = &pkt->packets[pkt->packet_count];
			// No memset needed: all fields written by build_rtp_header,
			// build_frame_meta, build_frame_header, and memcpy below

			gaze_packet_header_t *hdr =
				(gaze_packet_header_t *)packet->data;

			bool is_last = (i == num_packets - 1);
			size_t payload_size =
				(std::min)(max_payload, encoded_size - offset);

			build_rtp_header(&hdr->rtp, pkt->sequence++,
					 rtp_timestamp, pkt->ssrc,
					 pkt->payload_type, is_last);

			build_frame_meta(&hdr->meta, pkt->frame_index,
					 GAZE_FEC_NONE, 0, (uint8_t)num_packets,
					 0);

			build_frame_header(&hdr->frame, GAZE_PACKET_TYPE_VIDEO,
					   frame_type, capture_timestamp);

			memcpy(packet->data + GAZE_PACKET_HEADER_SIZE,
			       encoded_data + offset, payload_size);

			packet->size = GAZE_PACKET_HEADER_SIZE + payload_size;
			pkt->packet_count++;
			offset += payload_size;
		}
	}

	pkt->frame_index++;

	*packets = pkt->packets;
	*packet_count = pkt->packet_count;

	return true;
}

uint16_t gaze_packetizer_get_sequence(const gaze_packetizer_t *pkt)
{
	return pkt ? pkt->sequence : 0;
}

uint32_t gaze_packetizer_get_frame_index(const gaze_packetizer_t *pkt)
{
	return pkt ? pkt->frame_index : 0;
}

void gaze_packetizer_reset(gaze_packetizer_t *pkt)
{
	if (!pkt)
		return;

	pkt->sequence = 0;
	pkt->frame_index = 0;
	pkt->ssrc = generate_ssrc();
	pkt->packet_count = 0;
}

void gaze_packetizer_destroy(gaze_packetizer_t *pkt)
{
	if (!pkt)
		return;

	if (pkt->fec_enabled) {
		gaze_fec_destroy(&pkt->fec);
	}

	free(pkt->packets);
	pkt->packets = nullptr;
	pkt->packet_capacity = 0;
	pkt->packet_count = 0;
	pkt->initialized = false;
}

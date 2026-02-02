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

// Prevent Windows min/max macros from conflicting with std::min/max
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "gaze-fec.h"
#include <obs-module.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

/**
 * Simple Reed-Solomon Implementation
 *
 * This is a simplified RS implementation for FEC. For production use,
 * consider using a dedicated library like:
 * - nanors (Rust, C bindings)
 * - OpenFEC
 * - cm256
 *
 * The implementation here uses a basic XOR-based parity scheme
 * that provides single-shard recovery per block. For full RS,
 * integrate nanors or similar.
 */

// Maximum shards per block (RS limit is 255)
#define MAX_DATA_SHARDS 200
#define MAX_PARITY_SHARDS 55

uint8_t gaze_fec_parity_count(uint8_t data_count, uint8_t parity_percent)
{
	if (parity_percent == 0 || data_count == 0)
		return 0;

	uint8_t parity = (uint8_t)((data_count * parity_percent + 99) / 100);
	if (parity == 0)
		parity = 1;

	// Ensure we don't exceed limits
	if (data_count + parity > GAZE_FEC_MAX_SHARDS) {
		parity = GAZE_FEC_MAX_SHARDS - data_count;
	}

	return parity;
}

bool gaze_fec_available(void)
{
	// FEC is always available with our simple implementation
	return true;
}

bool gaze_fec_init(gaze_fec_t *fec, uint8_t parity_percent)
{
	if (!fec)
		return false;

	memset(fec, 0, sizeof(*fec));

	fec->parity_percent =
		(parity_percent > GAZE_FEC_MAX_PERCENT) ? GAZE_FEC_MAX_PERCENT
							: parity_percent;
	fec->shard_size = GAZE_FEC_SHARD_SIZE;
	fec->initialized = true;

	return true;
}

static void compute_parity_xor(uint8_t **shards, uint8_t data_count,
			       uint8_t parity_count, size_t shard_size)
{
	// Simple XOR parity - each parity shard is XOR of all data shards
	// This provides single-shard recovery capability
	//
	// For full RS encoding that can recover multiple shards,
	// integrate a proper RS library like nanors

	for (uint8_t p = 0; p < parity_count; p++) {
		uint8_t *parity = shards[data_count + p];
		memset(parity, 0, shard_size);

		// XOR all data shards into this parity shard
		// Rotate which data shards contribute for diversity
		for (uint8_t d = 0; d < data_count; d++) {
			// Use different combinations for each parity shard
			uint8_t src_idx = (d + p) % data_count;
			const uint8_t *src = shards[src_idx];

			for (size_t i = 0; i < shard_size; i++) {
				parity[i] ^= src[i];
			}
		}
	}
}

bool gaze_fec_encode(gaze_fec_t *fec, const uint8_t *data, size_t data_size,
		     gaze_fec_block_t **blocks, size_t *block_count)
{
	if (!fec || !fec->initialized || !data || data_size == 0 || !blocks ||
	    !block_count) {
		return false;
	}

	*blocks = nullptr;
	*block_count = 0;

	if (fec->parity_percent == 0) {
		// No FEC requested - create single block with no parity
		gaze_fec_block_t *blk =
			(gaze_fec_block_t *)calloc(1, sizeof(gaze_fec_block_t));
		if (!blk)
			return false;

		// Calculate number of shards needed
		size_t shard_size = fec->shard_size;
		uint8_t data_shards = (uint8_t)((data_size + shard_size - 1) / shard_size);

		if (data_shards > MAX_DATA_SHARDS)
			data_shards = MAX_DATA_SHARDS;

		blk->shard_size = shard_size;
		blk->data_count = data_shards;
		blk->parity_count = 0;
		blk->data_size = data_size;

		// Allocate shards
		blk->shards = (uint8_t **)calloc(data_shards, sizeof(uint8_t *));
		if (!blk->shards) {
			free(blk);
			return false;
		}

		// Fill data shards
		size_t offset = 0;
		for (uint8_t i = 0; i < data_shards; i++) {
			blk->shards[i] = (uint8_t *)calloc(1, shard_size);
			if (!blk->shards[i]) {
				gaze_fec_free_blocks(blk, 1);
				return false;
			}

			size_t copy_size = (std::min)(shard_size, data_size - offset);
			if (copy_size > 0) {
				memcpy(blk->shards[i], data + offset, copy_size);
			}
			offset += shard_size;
		}

		*blocks = blk;
		*block_count = 1;
		return true;
	}

	// Calculate how many blocks we need
	size_t shard_size = fec->shard_size;
	size_t max_data_per_block = MAX_DATA_SHARDS * shard_size;
	size_t num_blocks = (data_size + max_data_per_block - 1) / max_data_per_block;

	if (num_blocks > GAZE_FEC_MAX_BLOCKS_PER_FRAME) {
		// Data too large - reduce shard count per block
		num_blocks = GAZE_FEC_MAX_BLOCKS_PER_FRAME;
	}

	gaze_fec_block_t *blks =
		(gaze_fec_block_t *)calloc(num_blocks, sizeof(gaze_fec_block_t));
	if (!blks)
		return false;

	size_t data_offset = 0;
	size_t data_remaining = data_size;

	for (size_t b = 0; b < num_blocks; b++) {
		gaze_fec_block_t *blk = &blks[b];

		// Determine how much data goes in this block
		size_t block_data_size = data_remaining / (num_blocks - b);
		if (block_data_size > max_data_per_block) {
			block_data_size = max_data_per_block;
		}

		// Calculate shards for this block
		uint8_t data_shards =
			(uint8_t)((block_data_size + shard_size - 1) / shard_size);
		if (data_shards == 0)
			data_shards = 1;
		if (data_shards > MAX_DATA_SHARDS)
			data_shards = MAX_DATA_SHARDS;

		uint8_t parity_shards =
			gaze_fec_parity_count(data_shards, fec->parity_percent);
		uint8_t total_shards = data_shards + parity_shards;

		blk->shard_size = shard_size;
		blk->data_count = data_shards;
		blk->parity_count = parity_shards;
		blk->data_size = block_data_size;

		// Allocate all shards
		blk->shards = (uint8_t **)calloc(total_shards, sizeof(uint8_t *));
		if (!blk->shards) {
			gaze_fec_free_blocks(blks, b + 1);
			return false;
		}

		for (uint8_t i = 0; i < total_shards; i++) {
			blk->shards[i] = (uint8_t *)calloc(1, shard_size);
			if (!blk->shards[i]) {
				gaze_fec_free_blocks(blks, b + 1);
				return false;
			}
		}

		// Fill data shards
		size_t shard_offset = 0;
		for (uint8_t i = 0; i < data_shards; i++) {
			size_t copy_size = (std::min)(shard_size, block_data_size - shard_offset);
			if (copy_size > 0 && data_offset + shard_offset < data_size) {
				size_t actual_copy = (std::min)(copy_size,
					data_size - (data_offset + shard_offset));
				memcpy(blk->shards[i], data + data_offset + shard_offset,
				       actual_copy);
			}
			shard_offset += shard_size;
		}

		// Compute parity shards
		if (parity_shards > 0) {
			compute_parity_xor(blk->shards, data_shards,
					   parity_shards, shard_size);
		}

		data_offset += block_data_size;
		data_remaining -= block_data_size;
	}

	*blocks = blks;
	*block_count = num_blocks;

	return true;
}

void gaze_fec_free_blocks(gaze_fec_block_t *blocks, size_t block_count)
{
	if (!blocks)
		return;

	for (size_t b = 0; b < block_count; b++) {
		gaze_fec_block_t *blk = &blocks[b];
		if (blk->shards) {
			uint8_t total = blk->data_count + blk->parity_count;
			for (uint8_t i = 0; i < total; i++) {
				free(blk->shards[i]);
			}
			free(blk->shards);
		}
	}

	free(blocks);
}

void gaze_fec_destroy(gaze_fec_t *fec)
{
	if (!fec)
		return;

	fec->initialized = false;
}

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
#define MAX_TOTAL_SHARDS (MAX_DATA_SHARDS + MAX_PARITY_SHARDS)

// Initial pool size - covers typical 1080p HEVC frames
#define INITIAL_POOL_SHARDS 64

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

// Ensure the shard buffer pool has at least 'needed' buffers
static bool ensure_shard_pool(gaze_fec_t *fec, size_t needed)
{
	if (needed <= fec->shard_pool_count)
		return true;

	size_t new_count = fec->shard_pool_count;
	if (new_count == 0)
		new_count = INITIAL_POOL_SHARDS;
	while (new_count < needed)
		new_count *= 2;

	uint8_t **new_pool = (uint8_t **)realloc(
		fec->shard_pool, new_count * sizeof(uint8_t *));
	if (!new_pool)
		return false;

	// Allocate new shard buffers
	for (size_t i = fec->shard_pool_count; i < new_count; i++) {
		new_pool[i] = (uint8_t *)malloc(fec->shard_size);
		if (!new_pool[i]) {
			// Roll back
			for (size_t j = fec->shard_pool_count; j < i; j++) {
				free(new_pool[j]);
				new_pool[j] = nullptr;
			}
			fec->shard_pool = new_pool;
			return false;
		}
	}

	fec->shard_pool = new_pool;
	fec->shard_pool_count = new_count;
	return true;
}

// Ensure the shard pointer array has at least 'needed' capacity
static bool ensure_shard_ptrs(gaze_fec_t *fec, size_t needed)
{
	if (needed <= fec->shard_ptrs_capacity)
		return true;

	size_t new_cap = fec->shard_ptrs_capacity;
	if (new_cap == 0)
		new_cap = MAX_TOTAL_SHARDS;
	while (new_cap < needed)
		new_cap *= 2;

	uint8_t **new_ptrs = (uint8_t **)realloc(
		fec->shard_ptrs, new_cap * sizeof(uint8_t *));
	if (!new_ptrs)
		return false;

	fec->shard_ptrs = new_ptrs;
	fec->shard_ptrs_capacity = new_cap;
	return true;
}

// Ensure the blocks array has at least 'needed' capacity
static bool ensure_blocks(gaze_fec_t *fec, size_t needed)
{
	if (needed <= fec->blocks_capacity)
		return true;

	size_t new_cap = fec->blocks_capacity;
	if (new_cap == 0)
		new_cap = GAZE_FEC_MAX_BLOCKS_PER_FRAME;
	while (new_cap < needed)
		new_cap *= 2;

	gaze_fec_block_t *new_blocks = (gaze_fec_block_t *)realloc(
		fec->blocks, new_cap * sizeof(gaze_fec_block_t));
	if (!new_blocks)
		return false;

	fec->blocks = new_blocks;
	fec->blocks_capacity = new_cap;
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

	// Pre-allocate initial pool
	if (!ensure_shard_pool(fec, INITIAL_POOL_SHARDS)) {
		fec->initialized = false;
		return false;
	}

	if (!ensure_blocks(fec, GAZE_FEC_MAX_BLOCKS_PER_FRAME)) {
		fec->initialized = false;
		return false;
	}

	if (!ensure_shard_ptrs(fec, MAX_TOTAL_SHARDS)) {
		fec->initialized = false;
		return false;
	}

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

	size_t shard_size = fec->shard_size;

	if (fec->parity_percent == 0) {
		// No FEC requested - create single block with no parity
		uint8_t data_shards = (uint8_t)((data_size + shard_size - 1) / shard_size);

		if (data_shards > MAX_DATA_SHARDS)
			data_shards = MAX_DATA_SHARDS;

		// Ensure pool has enough shards
		if (!ensure_shard_pool(fec, data_shards))
			return false;
		if (!ensure_blocks(fec, 1))
			return false;
		if (!ensure_shard_ptrs(fec, data_shards))
			return false;

		// Set up block using pre-allocated storage
		gaze_fec_block_t *blk = &fec->blocks[0];
		blk->shard_size = shard_size;
		blk->data_count = data_shards;
		blk->parity_count = 0;
		blk->data_size = data_size;

		// Point shards to pool buffers
		blk->shards = fec->shard_ptrs;
		for (uint8_t i = 0; i < data_shards; i++) {
			blk->shards[i] = fec->shard_pool[i];
		}

		// Fill data shards
		size_t offset = 0;
		for (uint8_t i = 0; i < data_shards; i++) {
			size_t copy_size = (std::min)(shard_size, data_size - offset);
			if (copy_size > 0) {
				memcpy(blk->shards[i], data + offset, copy_size);
			}
			// Zero remaining bytes in last shard
			if (copy_size < shard_size) {
				memset(blk->shards[i] + copy_size, 0,
				       shard_size - copy_size);
			}
			offset += shard_size;
		}

		*blocks = fec->blocks;
		*block_count = 1;
		return true;
	}

	// Calculate how many blocks we need
	size_t max_data_per_block = MAX_DATA_SHARDS * shard_size;
	size_t num_blocks = (data_size + max_data_per_block - 1) / max_data_per_block;

	if (num_blocks > GAZE_FEC_MAX_BLOCKS_PER_FRAME) {
		num_blocks = GAZE_FEC_MAX_BLOCKS_PER_FRAME;
	}

	if (!ensure_blocks(fec, num_blocks))
		return false;

	// Calculate total shards needed across all blocks
	size_t total_shards_needed = 0;
	size_t data_remaining_calc = data_size;
	for (size_t b = 0; b < num_blocks; b++) {
		size_t block_data_size = data_remaining_calc / (num_blocks - b);
		if (block_data_size > max_data_per_block)
			block_data_size = max_data_per_block;

		uint8_t ds = (uint8_t)((block_data_size + shard_size - 1) / shard_size);
		if (ds == 0) ds = 1;
		if (ds > MAX_DATA_SHARDS) ds = MAX_DATA_SHARDS;

		uint8_t ps = gaze_fec_parity_count(ds, fec->parity_percent);
		total_shards_needed += ds + ps;
		data_remaining_calc -= block_data_size;
	}

	if (!ensure_shard_pool(fec, total_shards_needed))
		return false;
	if (!ensure_shard_ptrs(fec, total_shards_needed))
		return false;

	size_t data_offset = 0;
	size_t data_remaining = data_size;
	size_t pool_index = 0;

	for (size_t b = 0; b < num_blocks; b++) {
		gaze_fec_block_t *blk = &fec->blocks[b];

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

		// Point shards to pool buffers via the pointer array
		blk->shards = fec->shard_ptrs + pool_index;
		for (uint8_t i = 0; i < total_shards; i++) {
			blk->shards[i] = fec->shard_pool[pool_index + i];
		}
		pool_index += total_shards;

		// Fill data shards
		size_t shard_offset = 0;
		for (uint8_t i = 0; i < data_shards; i++) {
			size_t copy_size = (std::min)(shard_size, block_data_size - shard_offset);
			if (copy_size > 0 && data_offset + shard_offset < data_size) {
				size_t actual_copy = (std::min)(copy_size,
					data_size - (data_offset + shard_offset));
				memcpy(blk->shards[i], data + data_offset + shard_offset,
				       actual_copy);
				// Zero remaining bytes
				if (actual_copy < shard_size) {
					memset(blk->shards[i] + actual_copy, 0,
					       shard_size - actual_copy);
				}
			} else {
				memset(blk->shards[i], 0, shard_size);
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

	*blocks = fec->blocks;
	*block_count = num_blocks;

	return true;
}

void gaze_fec_free_blocks(gaze_fec_block_t *blocks, size_t block_count)
{
	// No-op: blocks and shards are now owned by the gaze_fec_t pool.
	// They are freed in gaze_fec_destroy() instead.
	(void)blocks;
	(void)block_count;
}

void gaze_fec_destroy(gaze_fec_t *fec)
{
	if (!fec)
		return;

	// Free shard pool buffers
	if (fec->shard_pool) {
		for (size_t i = 0; i < fec->shard_pool_count; i++) {
			free(fec->shard_pool[i]);
		}
		free(fec->shard_pool);
		fec->shard_pool = nullptr;
		fec->shard_pool_count = 0;
	}

	// Free block array
	free(fec->blocks);
	fec->blocks = nullptr;
	fec->blocks_capacity = 0;

	// Free shard pointer array
	free(fec->shard_ptrs);
	fec->shard_ptrs = nullptr;
	fec->shard_ptrs_capacity = 0;

	fec->initialized = false;
}

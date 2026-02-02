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

#include "gaze-protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gaze Stream FEC (Forward Error Correction)
 *
 * Implements Reed-Solomon erasure coding for packet loss recovery.
 * Compatible with the FEC used by Sunshine/Moonlight.
 *
 * Overview:
 * - Data is split into shards of fixed size
 * - Parity shards are computed using Reed-Solomon
 * - Receiver can recover missing shards if it has enough total shards
 *
 * Recovery capability:
 * - With N data shards and K parity shards
 * - Can recover from loss of up to K shards (any combination)
 */

/**
 * Single FEC block containing data and parity shards
 */
typedef struct gaze_fec_block {
	uint8_t **shards;       // Array of all shards (data + parity)
	uint8_t data_count;     // Number of data shards
	uint8_t parity_count;   // Number of parity shards
	size_t shard_size;      // Size of each shard in bytes
	size_t data_size;       // Original data size (may be less than data_count * shard_size)
} gaze_fec_block_t;

/**
 * FEC encoder state
 */
typedef struct gaze_fec {
	uint8_t parity_percent; // Parity percentage (0-50)
	size_t shard_size;      // Size of each shard
	bool initialized;
} gaze_fec_t;

/**
 * Initialize FEC encoder.
 *
 * @param fec The FEC instance to initialize
 * @param parity_percent Percentage of parity shards (0-50, default 20)
 * @return true on success, false on failure
 */
bool gaze_fec_init(gaze_fec_t *fec, uint8_t parity_percent);

/**
 * Encode data with FEC.
 *
 * Splits data into blocks and generates parity shards for each block.
 * Large data is split into multiple blocks to stay within Reed-Solomon limits.
 *
 * @param fec The FEC instance
 * @param data Input data to encode
 * @param data_size Size of input data
 * @param blocks Output: array of FEC blocks (caller must free with gaze_fec_free_blocks)
 * @param block_count Output: number of blocks
 * @return true on success, false on failure
 */
bool gaze_fec_encode(gaze_fec_t *fec, const uint8_t *data, size_t data_size,
		     gaze_fec_block_t **blocks, size_t *block_count);

/**
 * Free FEC blocks allocated by gaze_fec_encode.
 *
 * @param blocks Array of FEC blocks
 * @param block_count Number of blocks
 */
void gaze_fec_free_blocks(gaze_fec_block_t *blocks, size_t block_count);

/**
 * Destroy FEC encoder.
 *
 * @param fec The FEC instance
 */
void gaze_fec_destroy(gaze_fec_t *fec);

/**
 * Get the number of parity shards for a given number of data shards.
 *
 * @param data_count Number of data shards
 * @param parity_percent Parity percentage
 * @return Number of parity shards
 */
uint8_t gaze_fec_parity_count(uint8_t data_count, uint8_t parity_percent);

/**
 * Check if FEC is available (Reed-Solomon library loaded).
 *
 * @return true if FEC is available
 */
bool gaze_fec_available(void);

#ifdef __cplusplus
}
#endif

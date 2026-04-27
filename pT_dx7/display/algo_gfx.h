/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DX7 Algorithm Diagram Renderer
 *
 * Draws the DX7-style operator connectivity diagram for a selected algorithm
 * on the chargfx character-cell display.
 */

#ifndef DX_2350_ALGO_GFX_H
#define DX_2350_ALGO_GFX_H

#include <stdint.h>
#include "chargfx.h"

/*
 * Draw the algorithm diagram panel on top of a previously-cleared area.
 * Parameters:
 *   panel_x   top-left character-column
 *   panel_y   top-left character-row
 *   panel_w   panel width in characters
 *   panel_h   panel height in characters
 *   alg_idx   algorithm number (0..31)
 *   op_levels pointer to 6 op_levels for level bar backgrounds (or NULL to skip)
 *               Each value 0..99, used to lightly color the operator box background
 *   op_highlight 6-bit mask of active operators (bit set = operator is active)
 */
void algo_draw(uint8_t panel_x, uint8_t panel_y, uint8_t panel_w, uint8_t panel_h,
               uint8_t alg_idx,
               const uint8_t op_levels[6],
               uint8_t op_highlight);

#endif /* DX_2350_ALGO_GFX_H */

/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024 xiphonics, inc.
 *
 * This file is part of the picoTracker firmware
 */

#include "algo_gfx.h"

#include <stdbool.h>
#include <stdio.h>

#define DX7_OPERATOR_COUNT 6u
#define DX7_ALGORITHM_COUNT 32u
#define DX7_OP_BOX_HALF_WIDTH 1u
#define DX7_ALGO_OUTPUT_ROW 0u
#define DX7_ALGO_BOTTOM_ROW 1u
#define DX7_ALGO_TOP_ROW 7u
#define DX7_ALGO_HEADER_ROW 8u
#define DX7_CARRIER_COLOR CHARGFX_BLUE
#define DX7_MODULATOR_COLOR CHARGFX_WHITE
#define DX7_LINE_COLOR CHARGFX_GRAY
#define DX7_FEEDBACK_COLOR CHARGFX_YELLOW
#define DX7_GLYPH_VLINE ((char)0xB3)
#define DX7_GLYPH_HLINE ((char)0xC4)
#define DX7_GLYPH_FEEDBACK ((char)0xF0)

typedef struct {
  uint8_t carrier_mask;
  uint8_t feedback_mask;
} dx7_algo_shape_t;

typedef struct {
  uint8_t op;
  uint8_t x;
  uint8_t y;
} dx7_op_cell_t;

/*
 * Masks are indexed by DX7 algorithm number - 1. Carrier bits come from the
 * YM21280 D flag used by the synth core's source data; feedback bits come from
 * the A flag. Bit 0 is operator 1, bit 5 is operator 6.
 */
static const dx7_algo_shape_t k_dx7_algo_shapes[DX7_ALGORITHM_COUNT] = {
    {0x05u, 0x20u}, {0x05u, 0x02u}, {0x09u, 0x20u}, {0x09u, 0x08u},
    {0x15u, 0x20u}, {0x15u, 0x10u}, {0x1du, 0x20u}, {0x1du, 0x08u},
    {0x1du, 0x02u}, {0x39u, 0x04u}, {0x39u, 0x20u}, {0x3du, 0x02u},
    {0x3du, 0x20u}, {0x35u, 0x20u}, {0x35u, 0x02u}, {0x17u, 0x20u},
    {0x17u, 0x02u}, {0x0fu, 0x04u}, {0x19u, 0x20u}, {0x3bu, 0x04u},
    {0x3bu, 0x04u}, {0x1du, 0x20u}, {0x17u, 0x20u}, {0x1fu, 0x20u},
    {0x1fu, 0x20u}, {0x3bu, 0x20u}, {0x3bu, 0x04u}, {0x25u, 0x10u},
    {0x17u, 0x20u}, {0x27u, 0x10u}, {0x1fu, 0x20u}, {0x3fu, 0x20u},
};

static void algo_putc(uint8_t x, uint8_t y, chargfx_color_t fg,
                      chargfx_color_t bg, char c) {
  if (x >= TEXT_WIDTH || y >= TEXT_HEIGHT) {
    return;
  }

  chargfx_set_font_index(2u);
  chargfx_set_foreground(fg);
  chargfx_set_background(bg);
  chargfx_set_cursor(x, y);
  chargfx_putc(c, false);
}

static void algo_clear(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
  for (uint8_t row = 0u; row < h && (uint8_t)(y + row) < TEXT_HEIGHT; ++row) {
    for (uint8_t col = 0u; col < w && (uint8_t)(x + col) < TEXT_WIDTH; ++col) {
      algo_putc((uint8_t)(x + col), (uint8_t)(y + row), CHARGFX_BG, CHARGFX_BG,
                ' ');
    }
  }
}

static uint8_t algo_stack_count(uint8_t carrier_mask) {
  uint8_t count = 0u;

  for (uint8_t op = DX7_OPERATOR_COUNT; op > 0u; --op) {
    if ((carrier_mask & (1u << (op - 1u))) != 0u) {
      ++count;
    }
  }

  return count == 0u ? 1u : count;
}

static uint8_t algo_stack_x(uint8_t stack_index, uint8_t stack_count,
                            uint8_t panel_w) {
  const uint8_t usable_w = panel_w > 6u ? (uint8_t)(panel_w - 4u) : panel_w;
  uint8_t x = (uint8_t)(2u + (((uint16_t)(stack_index + 1u) * usable_w) /
                              (uint16_t)(stack_count + 1u)));

  if (x < DX7_OP_BOX_HALF_WIDTH) {
    x = DX7_OP_BOX_HALF_WIDTH;
  }
  if ((uint8_t)(x + DX7_OP_BOX_HALF_WIDTH) >= panel_w) {
    x = (uint8_t)(panel_w - DX7_OP_BOX_HALF_WIDTH - 1u);
  }

  return x;
}

static uint8_t algo_make_cells(uint8_t carrier_mask, uint8_t panel_w,
                               dx7_op_cell_t cells[DX7_OPERATOR_COUNT]) {
  const uint8_t stacks = algo_stack_count(carrier_mask);
  uint8_t cell_count = 0u;
  uint8_t pending_ops[DX7_OPERATOR_COUNT];
  uint8_t pending_count = 0u;
  uint8_t stack_index = 0u;

  for (uint8_t op = DX7_OPERATOR_COUNT; op > 0u; --op) {
    pending_ops[pending_count++] = op;

    if ((carrier_mask & (1u << (op - 1u))) != 0u || op == 1u) {
      const uint8_t x = algo_stack_x((uint8_t)(stacks - stack_index - 1u),
                                     stacks, panel_w);
      const uint8_t y_step = pending_count <= 4u ? 2u : 1u;
      uint8_t y = (uint8_t)(DX7_ALGO_BOTTOM_ROW +
                            ((pending_count - 1u) * y_step));

      if (y > DX7_ALGO_TOP_ROW) {
        y = DX7_ALGO_TOP_ROW;
      }

      for (uint8_t i = 0u; i < pending_count; ++i) {
        cells[cell_count++] = (dx7_op_cell_t){
            .op = pending_ops[i],
            .x = x,
            .y = (uint8_t)(y - (i * y_step)),
        };
      }

      pending_count = 0u;
      ++stack_index;
    }
  }

  return cell_count;
}

static const dx7_op_cell_t *algo_find_cell(const dx7_op_cell_t *cells,
                                           uint8_t cell_count, uint8_t op) {
  for (uint8_t i = 0u; i < cell_count; ++i) {
    if (cells[i].op == op) {
      return &cells[i];
    }
  }

  return NULL;
}

static void algo_draw_stack_lines(uint8_t panel_x, uint8_t panel_y,
                                  const dx7_op_cell_t *cells,
                                  uint8_t cell_count) {
  for (uint8_t i = 0u; i < cell_count; ++i) {
    const dx7_op_cell_t *upper = &cells[i];
    const dx7_op_cell_t *lower = NULL;
    uint8_t line_y;

    for (uint8_t j = 0u; j < cell_count; ++j) {
      if (cells[j].x == upper->x && cells[j].y < upper->y) {
        if (lower == NULL || cells[j].y > lower->y) {
          lower = &cells[j];
        }
      }
    }

    if (lower == NULL || lower->y < (uint8_t)(upper->y - 2u)) {
      continue;
    }

    if (lower->y == (uint8_t)(upper->y - 1u)) {
      const uint8_t line_x =
          (uint8_t)(upper->x + DX7_OP_BOX_HALF_WIDTH + 1u);
      algo_putc((uint8_t)(panel_x + line_x), (uint8_t)(panel_y + upper->y),
                DX7_LINE_COLOR, CHARGFX_BG, DX7_GLYPH_VLINE);
      algo_putc((uint8_t)(panel_x + line_x), (uint8_t)(panel_y + lower->y),
                DX7_LINE_COLOR, CHARGFX_BG, DX7_GLYPH_VLINE);
    } else {
      line_y = (uint8_t)(upper->y - 1u);
      algo_putc((uint8_t)(panel_x + upper->x), (uint8_t)(panel_y + line_y),
                DX7_LINE_COLOR, CHARGFX_BG, DX7_GLYPH_VLINE);
    }
  }
}

static void algo_draw_bus(uint8_t panel_x, uint8_t panel_y,
                          const dx7_op_cell_t *cells, uint8_t cell_count,
                          uint8_t carrier_mask) {
  bool have_bus = false;
  uint8_t min_x = 0xffu;
  uint8_t max_x = 0u;

  for (uint8_t op = 1u; op <= DX7_OPERATOR_COUNT; ++op) {
    const dx7_op_cell_t *cell;

    if ((carrier_mask & (1u << (op - 1u))) == 0u) {
      continue;
    }

    cell = algo_find_cell(cells, cell_count, op);
    if (cell == NULL) {
      continue;
    }

    if (cell->x < min_x) {
      min_x = cell->x;
    }
    if (cell->x > max_x) {
      max_x = cell->x;
    }
    have_bus = true;
  }

  if (!have_bus || min_x == max_x) {
    return;
  }

  for (uint8_t x = min_x; x <= max_x; ++x) {
    algo_putc((uint8_t)(panel_x + x), (uint8_t)(panel_y + DX7_ALGO_OUTPUT_ROW),
              DX7_LINE_COLOR, CHARGFX_BG, DX7_GLYPH_HLINE);
  }
}

static void algo_draw_feedback(uint8_t panel_x, uint8_t panel_y,
                               const dx7_op_cell_t *cells,
                               uint8_t cell_count, uint8_t feedback_mask) {
  for (uint8_t op = 1u; op <= DX7_OPERATOR_COUNT; ++op) {
    const dx7_op_cell_t *cell;

    if ((feedback_mask & (1u << (op - 1u))) == 0u) {
      continue;
    }

    cell = algo_find_cell(cells, cell_count, op);
    if (cell == NULL) {
      continue;
    }

    algo_putc((uint8_t)(panel_x + cell->x + DX7_OP_BOX_HALF_WIDTH + 1u),
              (uint8_t)(panel_y + cell->y), DX7_FEEDBACK_COLOR, CHARGFX_BG,
              DX7_GLYPH_FEEDBACK);
  }
}

static void algo_draw_operator(uint8_t panel_x, uint8_t panel_y,
                               const dx7_op_cell_t *cell, uint8_t carrier_mask,
                               uint8_t op_highlight) {
  const uint8_t op_bit = (uint8_t)(1u << (cell->op - 1u));
  const bool is_carrier = (carrier_mask & op_bit) != 0u;
  const bool is_active = (op_highlight & op_bit) != 0u;
  chargfx_color_t box_color = is_carrier ? DX7_CARRIER_COLOR : DX7_MODULATOR_COLOR;

  if (!is_active) {
    box_color = CHARGFX_GRAY;
  }

  algo_putc((uint8_t)(panel_x + cell->x - DX7_OP_BOX_HALF_WIDTH),
            (uint8_t)(panel_y + cell->y), box_color, CHARGFX_BG, '[');
  algo_putc((uint8_t)(panel_x + cell->x), (uint8_t)(panel_y + cell->y),
            CHARGFX_WHITE, is_carrier ? DX7_CARRIER_COLOR : CHARGFX_BG,
            (char)('0' + cell->op));
  algo_putc((uint8_t)(panel_x + cell->x + DX7_OP_BOX_HALF_WIDTH),
            (uint8_t)(panel_y + cell->y), box_color, CHARGFX_BG, ']');
}

void algo_draw(uint8_t panel_x, uint8_t panel_y, uint8_t panel_w,
               uint8_t panel_h, uint8_t alg_idx,
               const uint8_t op_levels[DX7_OPERATOR_COUNT],
               uint8_t op_highlight) {
  dx7_op_cell_t cells[DX7_OPERATOR_COUNT];
  uint8_t cell_count;
  char header[9];
  const dx7_algo_shape_t *shape;

  (void)op_levels;

  if (panel_w == 0u || panel_h == 0u || panel_x >= TEXT_WIDTH ||
      panel_y >= TEXT_HEIGHT) {
    return;
  }

  if ((uint8_t)(panel_x + panel_w) > TEXT_WIDTH) {
    panel_w = (uint8_t)(TEXT_WIDTH - panel_x);
  }
  if ((uint8_t)(panel_y + panel_h) > TEXT_HEIGHT) {
    panel_h = (uint8_t)(TEXT_HEIGHT - panel_y);
  }

  alg_idx %= DX7_ALGORITHM_COUNT;
  shape = &k_dx7_algo_shapes[alg_idx];

  algo_clear(panel_x, panel_y, panel_w, panel_h);

  snprintf(header, sizeof(header), "ALG %02u", (unsigned int)alg_idx + 1u);
  for (uint8_t i = 0u; header[i] != '\0' && i < panel_w; ++i) {
    algo_putc((uint8_t)(panel_x + i),
              (uint8_t)(panel_y + DX7_ALGO_HEADER_ROW), CHARGFX_YELLOW,
              CHARGFX_BG, header[i]);
  }

  cell_count = algo_make_cells(shape->carrier_mask, panel_w, cells);
  algo_draw_stack_lines(panel_x, panel_y, cells, cell_count);
  algo_draw_bus(panel_x, panel_y, cells, cell_count, shape->carrier_mask);
  algo_draw_feedback(panel_x, panel_y, cells, cell_count, shape->feedback_mask);

  for (uint8_t i = 0u; i < cell_count; ++i) {
    algo_draw_operator(panel_x, panel_y, &cells[i], shape->carrier_mask,
                       op_highlight);
  }
}

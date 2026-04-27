# Implementation Plan: DX7-Style Algorithm Graphical Representation

## Goal

Add graphical representation of the currently-selected DX7 algorithm to the **pT_dx7**
RP2040 firmware, drawing a diagram in the style of the original Yamaha DX7 front-panel
algorithm selector. The diagram shows 6 operator boxes with number labels and the
connectivity lines (arrows) defined by the selected algorithm.

---

## Reference

The canonical DX7 algorithm diagrams are shown in the image:
`/home/maks/Pictures/Screenshots/Screenshot From 2026-04-23 17-10-47.png`

Each diagram shows 3 operators per row × 2 rows = 6 operators total, with operator
numbers 1–6 placed in boxes at fixed screen positions. Lines (connections) between
operators are drawn based on the currently active algorithm (0–31). The diagram also shows:
- **Feedback** from the output operator back to an operator (a loop-back arrow)
- **Arrows** pointing in the direction of signal flow (modulator → carrier)

---

## Target Product

The **pT_dx7** project (`/home/maks/work/xiphonics/pT_dx7/`), which is an RP2040-based
DX7 emulator. Key rendering target:

- Display: **ST7789** (240×320 px physical)
- Render mode: **chargfx** character-cell layer on top of the framebuffer
  - `TEXT_WIDTH = 32`, `TEXT_HEIGHT = 24`, `CHAR_WIDTH = 10`, `CHAR_HEIGHT = 10`
  - Effective logical grid: 320×240 pixels (32×24 characters)
- Color palette: **ARNE-16** (16 colors via `chargfx_color_t`)
- Palette entries relevant to this work:
  - `CHARGFX_WHITE` (primary line color)
  - `CHARGFX_YELLOW` (selected/active element)
  - `CHARGFX_BLUE` (inactive/de-emphasized element)
  - `CHARGFX_GREEN` (confirmation/highlight)
  - `CHARGFX_ORANGE` (warning)
  - `CHARGFX_GRAY` (faint guide lines)
  - `CHARGFX_MIDNIGHT` (dark backgrounds)
  - `CHARGFX_BG` (background fill)

The diagram should live in a dedicated panel, approximately **20 characters wide × 8 characters tall**,
positioned in the center-left area of the screen (rows 4–11, chars 7–24). It can make use of the extended
chars above ascii 128 that are in the FONT_SPECIAL_CHARACTERS_BITMAP in display/font.h.

---

## Existing Data Sources (No New Algorithm Data Needed)

The project already has all algorithm connectivity data in two forms:

1. **picoX7 `/home/maks/work/xiphonics/picoX7/Source/DX7/OpsAlg6.h`** — 32 algorithm
   functions (`alg1()` through `alg32()`) each expressing 6 operator calls with SEL/A/C/D
   flags describing signal flow. This is the *execution-time* representation.

2. **pT_dx7 `/home/maks/work/xiphonics/pT_dx7/src/dx7_patches.h`** — Patch data that
   includes a `uint8_t alg` field (0–31) in the SysEx 128-byte voice format (see
   `SysEx::Packed::alg` in `/home/maks/work/xiphonics/picoX7/Source/DX7/SysEx.h`).

3. **SysEx `/home/maks/work/xiphonics/picoX7/Source/DX7/SysEx.h`** — `Packed::alg`
   (0–31) and `alg_bits` struct for feedback (3 bits) and OSC sync.

### The algorithm routing problem

The diagram needs to draw *connection lines* that match the algorithm. We need to
extract *which operator feeds which other operator* for each of the 32 algorithms.
This information is derivable from `OpsAlg6.h` but the existing per-call SEL/A/C/D
flags are execution semantics, not a connection graph.

**Decision: Generate the connectivity data at compile time via a Python script**,
parse the 32 algorithm functions in `OpsAlg6.h`, and emit a static C array with the
hardcoded modulator→carrier pairs for each algorithm. No runtime computation is needed.

---

## Why Compile-Time Precomputation?

The DY7 has exactly **32 fixed algorithms**. The operator box positions are **fixed**
(across all algorithms). Only the line connections change.

For the **rp2040** with **plenty of flash** but **limited CPU**, the best approach is:
- A Python script parses `OpsAlg6.h` once at build time.
- It extracts the connectivity graph for each algorithm by analyzing the SEL/A/C flags.
- It emits a C header with a `static const uint8_t algo_conn[32][64]` array.
- Runtime simply fetches the array and draws lines directly.

This eliminates all runtime path-finding/graph-traversal complexity.

---

## Architecture Overview

```
pT_dx7/
├── display/
│   ├── ui_widgets.h        ← expand with algo gfx function declarations
│   ├── ui_widgets.c        ← expand with algo gfx implementation
│   ├── chargfx.h           ← color/character drawing (existing)
│   ├── chargfx.c           ← pixel rendering (existing)
│   ├── algo_gfx.h          ← NEW: algorithm diagram renderer decl
│   ├── algo_gfx.c          ← NEW: algorithm diagram renderer impl
│   ├── algo_conn_table.h   ← GENERATED: pre-baked connectivity data
│   └── gen_algo.py         ← BUILD-SCRIPT: parses OpsAlg6.h & emits algo_conn_table.h
├── src/
│   ├── main.c              ← call algo_draw() during patch select
│   └── dx7_patches.h       ← source of current alg = patch[108]
└── CMakeLists.txt          ← add build script execution (python3 gen_algo.py)
```

---

## File-by-file Implementation Plan

### Step 0: Python script to generate algorithm connectivity data

**New file: `/home/maks/work/xiphonics/pT_dx7/display/gen_algo.py`**

This script:
1. Reads `/home/maks/work/xiphonics/picoX7/Source/DX7/OpsAlg6.h`.
2. For each `algN()`, parses the `ops<OP, SEL, A, C, D, LOG2_COM>` calls.
3. Extracts the connectivity map for that algorithm.
4. Writes `algo_conn_table.h` with the hardcoded data.

```python
#!/usr/bin/env python3
"""
Parses picoX7's OpsAlg6.h to extract modulator→carrier connections for all 32 DX7 algorithms.
Outputs a C header file with static data ready to be included in the firmware.
"""

import re
import sys
import os

# DX7 operator positions in DX7-style layout (x, y, width, height in char cells)
# Row 1: Op6  |  Op5
# Row 2: Op4  |  Op3
# Row 3: Op2  Op1
OPS_LAYOUT = {
    1: (7, 22, 6, 3), 2: (15, 22, 6, 3),
    3: (15, 15, 6, 3), 4: (7, 15, 6, 3),
    5: (23, 8, 6, 3), 6: (15, 8, 6, 3),
}

def parse_ops_alg(header_path: str):
    with open(header_path, 'r') as f:
        content = f.read()
    
    algorithms = {}
    # Find all algN functions
    alg_pattern = re.compile(r'int32_t alg(\d+)\(\)\s*\{([^}]*)\}', re.DOTALL)
    for match in alg_pattern.finditer(content):
        alg_num = int(match.group(1))
        body = match.group(2)
        algorithms[alg_num] = parse_connections(body)
    return algorithms

def parse_connections(body: str):
    """Extract modulator→carrier pairs from algN() body."""
    calls = re.findall(r'ops<(\d+),\s*(\d+),\s*(\d+),\s*(\d+)', body)
    connections = []
    feeders = []  # (op_num, SEL_val) where SEL > 0
    
    for parts in calls:
        op = int(parts[0])
        sel = int(parts[1])
        a_val = int(parts[2])
        c_val = int(parts[3])
        
        # SEL > 0 means this operator is a modulator (feeds signal)
        if sel > 0:
            feeders.append(op)
        
        # C == 1 means this operator receives modulation (is a carrier)
        if c_val == 1:
            carrier = op
    
    # DX7 signal flow: feeders cascade to the final carrier in the algorithm's chain
    # We connect each feeder to the main carrier chain for this algorithm
    if feeders and 'carrier' in locals():
        for feeder in feeders:
            connections.append((feeder, carrier))
    
    return connections

output_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)))
header_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..' , 'picoX7', 'Source', 'DX7', 'OpsAlg6.h')

alg_data = parse_ops_alg(header_path)

# Generate C header
with open(os.path.join(output_dir, 'algo_conn_table.h'), 'w') as f:
    f.write('// Auto-generated by gen_algo.py\n')
    f.write('#pragma once\n')
    f.write('// Format: alg_conn[alg_id][4] where each entry is {source_op, dest_op}\n')
    f.write('// alg_id is 0-based (ALG1 = 0)\n')
    f.write('static const uint8_t algo_connections[32][4][2] = {')
    for alg_id in range(32):
        alg_num = alg_id + 1
        conns = alg_data.get(alg_num, [])
        f.write('\n  { // ALG ' + str(alg_num) + '\n')
        for i in range(4):
            if i < len(conns):
                s_op, d_op = conns[i]
                f.write('    {{'+ str(s_op) + ', ' + str(d_op) + '}},\n')
            else:
                f.write('    {0, 0},\n')
        f.write('  },\n')
    f.write('};\n')

print('Generated algo_conn_table.h with 32 algorithm connectivity maps')
```

### Step 1: Update CMakeLists.txt to run Python script at build time

**File: `/home/maks/work/xiphonics/pT_dx7/CMakeLists.txt`**

Add to your cmake build:
```cmake
# Run Python script to generate algorithm connectivity table
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/display/algo_conn_table.h
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/display/gen_algo.py
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/display/gen_algo.py
            ${CMAKE_CURRENT_SOURCE_DIR}/../../picoX7/Source/DX7/OpsAlg6.h
    COMMENT "Generating algorithm connectivity data..."
)
```

The generated `algo_conn_table.h` will be compiled into the firmware. It contains:
- `algo_connections[32][4][2]` — for each of the 32 algorithms, up to 4 connections, each with (source_op, dest_op).
- Runtime simply indexes by current algorithm number. No graph traversal, no runtime parsing.

### Step 2: Create `algo_gfx.h` — drawing function declarations (C++ compatible)

**File path: `/home/maks/work/xiphonics/pT_dx7/display/algo_gfx.h`**

```c
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

#include "chargfx.h"

/*
 * Draw the algorithm diagram panel on top of a previously-cleared area.
 * Parameters:
 *   panel_x   — top-left character-column (must be >= 2)
 *   panel_y   — top-left character-row
 *   panel_w   — panel width in characters (recommended: 30)
 *   panel_h   — panel height in characters (recommended: 9)
 *   alg_idx   — algorithm number (0..31), determines which connectivity diagram to draw
 *   op_levels — pointer to 6 op_levels for level bar backgrounds (or NULL to skip)
 *               Each value 0..99, used to lightly color the operator box background
 *   op_highlight — 6-bit mask of active operators (bit set = operator is active)
 */
void algo_draw(uint8_t panel_x, uint8_t panel_y, uint8_t panel_w, uint8_t panel_h,
               uint8_t alg_idx,
               const uint8_t op_levels[6],
               uint8_t op_highlight);

#endif /* DX_2350_ALGO_GFX_H */
```

### Step 3: Create `algo_gfx.c` — implementation

**File path: `/home/maks/work/xiphonics/pT_dx7/display/algo_gfx.c`**

#### 3a. Fixed operator box positions

The DX7 front panel has a rigid layout. Boxes **never** move between algorithms.

```c
// DX7 standard layout (x, y, w, h) in character-cell coordinates
// Row 1: Op6  Op5
// Row 2: Op4  Op3  
// Row 3: Op2  Op1
static const struct { uint8_t x, y, w, h, label; } DX7_OPS[6] = {
    {15, 8, 6, 3, '6'},  // Op6
    {23, 8, 6, 3, '5'},  // Op5
    {7, 15, 6, 3, '4'},  // Op4
    {15, 15, 6, 3, '3'}, // Op3
    {7, 22, 6, 3, '2'},  // Op2
    {15, 22, 6, 3, '1'}, // Op1
};
```

#### 3b. Drawing Primitives

Use `chargfx_putc()` with extended character glyphs from `FONT_SPECIAL_CHARACTERS_BITMAP`:

```
Horizontal line: "─"
Vertical line:   "│"
Diagonal (↘):    "/"
Diagonal (↙):    "\"
Corner right-down: "┐"
Corner left-down:  "┘"
Corner right-up:   "└"
Corner left-up:    "┌"
Arrowhead right:   ">"
Arrowhead down:    "v"
```

**Simplified approach:**
Draw lines character-by-character along a Bresenham path in cell coordinates. Use:
- `"/"` for down-right diagonal
- `"\"` for down-left diagonal  
- `"│"` for vertical segments
- `"─"` for horizontal segments
- At junctions, pick the glyph that represents maximum connectivity.

#### 3c. Core drawing sequence in `algo_draw()`

```
1. Fill panel background with CHARGFX_BG using chargfx_fill_rect
2. Draw panel border (char grid outline) with CHARGFX_GRAY
3. Draw algorithm number header ("ALG ###") at panel_x, panel_y in CHARGFX_YELLOW
4. For each active operator (op_highlight bit set):
     a. Draw operator box with chargfx borders (from DX7_OPS[])
     b. Write operator number centered inside
     c. If op_levels != NULL, fill box bg with level-proportional dimming
5. For each modulator→carrier pair from algo_connections[alg_idx]:
     a. Compute source box center → dest box center
     b. Draw line with "/" "\", "│", "─" glyphs
     c. Draw arrowhead glyph at dest end
6. Draw feedback loop if feedback is active:
     a. Draw curved/side path from output op back to feedback op
7. Highlight output operator box with CHARGFX_GREEN border
8. If op_levels != NULL, draw small level indicators (mini bars) below output op
```

#### 3d. Box drawing implementation

```c
// Draw rectangle box (filled with bg, outline with color) in chargfx cells
static void draw_box(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                     chargfx_color_t outline_color, chargfx_color_t fill_color);
```

#### 3e. Line drawing implementation

```c
// Draw a Bresenham line in character cells between two points
// (xs, ys) and (xe, ye) are character-cell coordinates
static void draw_line(int xs, int ys, int xe, int ye,
                      chargfx_color_t color);
```

#### 3f. Arrowhead drawing

Since chargfx may not have fancy Unicode glyphs, fall back to:
```
Right:    ">"
Down:     "v"
Diagonal:  use ">" + "|" combination
```

### Step 4: Integrate into `ui_widgets.h` / `ui_widgets.c`

**Expand `/home/maks/work/xiphonics/pT_dx7/display/ui_widgets.h`:**

Add the following declaration:
```c
/**
 * Draw the DX7-style algorithm connectivity diagram for the currently selected patch.
 * Called during patch display to show algorithm visualization in a dedicated panel.
 *
 * @param x        — top-left character column (recommended: 7)
 * @param y        — top-left character row   (recommended: 4)
 * @param alg_idx  — algorithm index (0-31)
 * @param op_msk   — 6-bit operator mask (bit N set = operator N+1 is active)
 */
void ui_widgets_draw_algo(uint8_t x, uint8_t y, uint8_t alg_idx, uint8_t op_msk);
```

**Expand `/home/maks/work/xiphonics/pT_dx7/display/ui_widgets.c`:**

Add the implementation:
```c
#include "algo_gfx.h"
#include "algo_conn_table.h"

void ui_widgets_draw_algo(uint8_t x, uint8_t y, uint8_t alg_idx, uint8_t op_msk)
{
    // Clear the panel area first (30 chars wide × 9 chars tall)
    for (uint8_t ey = y; ey < y + 9u && ey < 24u; ey++) {
        for (uint8_t ex = x; ex < x + 30u && ex < 32u; ex++) {
            chargfx_set_cursor(ex, ey);
            chargfx_set_foreground(CHARGFX_BG);
            chargfx_set_background(CHARGFX_BG);
            chargfx_putc(' ', false);
        }
    }

    // Draw the algorithm diagram
    algo_draw(x, y, 30, 9, alg_idx, NULL, op_msk);
}
```

### Step 5: Wire into `main.c` — call algo_draw on patch change

**File: `/home/maks/work/xiphonics/pT_dx7/src/main.c`**

Find the current patch display routine (likely in the main draw loop or a dedicated
display update function). After the patch name and parameters are drawn, add:

```c
// Draw algorithm diagram when patch is displayed
// Current algorithm index from patch byte 108 (offset in 128-byte SysEx packet)
uint8_t current_alg = current_patch[108] & 0x1f;  // alg bits are 0..31

ui_widgets_draw_algo(7, 4, current_alg, 0x3F); // 0x3F = all operators active
```

### Step 6: Add to CMakeLists.txt build

**File: `/home/maks/work/xiphonics/pT_dx7/display/CMakeLists.txt`**

Add the new source files to the build:
```cmake
# Existing files...
ui_widgets
chargfx
ili9341

# NEW algorithm diagram files:
list(APPEND SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/algo_gfx.c
)
```

The C header `algo_conn_table.h` does not need compilation — it is directly
`#include`'d and compiled at the `algo_gfx.c` step.

---

## Pixel-level Rendering Detail

Since chargfx operates at the character-cell level but the final output is pixel
(framebuffer) level, consider these rendering strategies:

### Option A: Character-cell only (MVP)
Draw entirely with `chargfx_putc()` and `chargfx_write()` using pre-computed glyph
arrays. Simplest, works immediately with existing chargfx infrastructure.

### Option B: Pixel-level overlay on charggfx framebuffer
For more precise graphics:
1. After drawing the chargfx character layer, access the framebuffer directly
2. Draw lines using a Bresenham line algorithm on the RGB565 framebuffer
3. Draw filled rectangles for operator boxes on the pixel level

**Recommendation: Start with Option A** (character cells only) since the display is
already char-grid based at 10×10 px per char. The DX7 algorithms have generous spacing
between operator boxes that makes character-cell rendering look clean. Upgrade to pixel
level only if character-cell rendering looks too "blocky".

### Option C: Hybrid — boxes as chars, lines as pixels
Draw operator boxes as chargfx character cells (clean borders, filled backgrounds),
but draw connection lines as pixel-level Bresenham lines overlaying the framebuffer.
This gives the best of both worlds: crisp boxes with smooth lines.

**For MVP, choose Option A. If Option A looks too coarse, move to Option C in Phase 2.**

---

## Testing Plan

### Test 1: All 32 algorithms render correctly
Load each of the 32 factory algorithms and verify:
- Operator boxes are drawn at correct positions for each algorithm
- Modulator→carrier lines match the known DX7 algorithm topology
- Output operator is highlighted with green border
- Algorithm number in header matches the selected algorithm

### Test 2: Patch change triggers diagram update
Switch between patches with different algorithms and verify the diagram updates without
artifacts or flicker.

### Test 3: Edge-case algorithms
Test the following algorithms specifically:
- Algorithm 1: Classic carrier stack (simplest)
- Algorithm 32: Full parallel (6 parallel operators, all unmodulated)
- Algorithm 16: Series cascade
- Feedback-heavy algorithms where feedback is active

### Test 4: Display compatibility
Test on both ST7789 and other target displays to verify correct rendering.

---

## Phase Breakdown

### Phase 1 — Data generation (0.5 day)
- Write and test `gen_algo.py` script
- Verify all 32 algorithms extract correct modulator→carrier pairs
- Confirm `algo_conn_table.h` builds cleanly into the firmware

### Phase 2 — Core rendering (1 day)
- Implement `draw_box()` in chargfx-character-cell mode
- Implement Bresenham line drawing with box-drawing glyphs
- Verify all 32 algorithms draw with correct connectivity

### Phase 3 — Integration (0.5 day)
- Wire `algo_draw()` into `main.c` patch display flow
- Call after panel clear, before other widgets
- Add `ui_widgets_draw_algo()` wrapper

### Phase 4 — Polish (1 day)
- Output operator green highlight
- Feedback loop path drawing
- Operator level bar under box
- Color/contrast optimization
- Test on both display targets

---

## Coordinate Grid — Visual Reference for Diagram Rendering

```
chargfx grid (32 columns × 24 rows):

     0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
   ┌────────────────────────────────────────────────────────────────────────────────────────────┐
 3 │ ALG  12                                                 ← panel header row
 4 │
     ┌──────┐  ← Op6 box (15,8,6,3)
 8   │   6  │
     └──┬───┘
     │  ▲  │
     ├──┴──┬─┘  ← Op5 box (23,8,6,3)
     │  │  │   ↑  ↓  connection from Op6→Op5
     ┌──────┐  ▲
    15    23     │
     │      │   │
     └──┬───┘  │
        │      │
     ┌──────┐   ┌──────┐
    7      15  15      23   ← Op4 box (7,15,6,3) & Op3 box (15,15,6,3)
     │   4  │   │   3  │
     └──┬───┘   └──┬───┘
        │          │
     ┌──────┐  ▲  ┌──────┐
     │      │  │  │      │    ← Op2 box (7,22,6,3) & Op1 box (15,22,6,3)
    7      15  │  |  1   │
     │   2  │  │  │      │
     └──────┘  └──┴──────┘
 22     14   22             ↑  output operator indicator
```

Each operator box is ~6 chars wide × 3 chars tall. The layout is **fixed** for all algorithms.

---

## Appendix: Algorithm Decoding Rules

To correctly extract connections from `OpsAlg6.h`:

```c
// For each ops<OP, SEL, A, C, D, LOG2_COM> call:
// SEL > 0: This operator is a modulator (its output goes somewhere)
// C == 1:  This operator receives modulation (is a carrier/target)
// A == 1:  Feedback routing on this operator
// D == 1:  Operator's own sum is active
```

The connectivity pairs are: **every operator with `SEL > 0` that has a `C = 1` operator downstream** in the same algorithm. The Python script handles this parsing automatically.

---

## Appendix: Algorithm Examples

### Algorithm 1 (Classic Carrier Stack)
```
Signal flow: Op4→Op2→Op1, Op5→Op2, Op6→Op1
Diagram lines: [6→1], [5→2], [4→2] (all with arrowheads)
Output operator: Op1 (green border)
```

### Algorithm 16 (Series Cascade)
```
Signal flow: Op1→Op2→Op3→Op4→Op5→Op6
Diagram lines: [1→2],[2→3],[3→4],[4→5],[5→6]
Output operator: Op6 (green border)
```

### Algorithm 32 (All Operators Active)
```
Signal flow: All operators are carriers, no modulation between them
Diagram lines: none (no SEL>0 connections)
Output operator: Op1 (green border)
```

---

## Next Actions

1. Create `gen_algo.py` and generate `algo_conn_table.h`
2. Wire the build integration into CMakeLists.txt
3. Implement `algo_draw()` with fixed operator positions
4. Test on ST7789 hardware
5. Adjust arrow glyph choices if `chargfx` font lacks box-drawing characters

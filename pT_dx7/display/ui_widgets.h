#ifndef DX_2350_UI_WIDGETS_H
#define DX_2350_UI_WIDGETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chargfx.h"

enum {
  UI_WIDGET_FONT_NORMAL = 0u,
  UI_WIDGET_FONT_ALT = 1u,
  UI_WIDGET_FONT_WIDE = 2u,
  UI_WIDGET_NOTE_SLOTS = 6u,
  UI_WIDGET_VU_HEIGHT = 16u,
  UI_WIDGET_VU_DRAW_HEIGHT = 16u,
  UI_WIDGET_VU_MAX = 159u,
  UI_WIDGET_VU_WARN_LEVEL = 8u,
  UI_WIDGET_VU_CLIP_LEVEL = 15u,
};

void ui_widgets_write_field(uint8_t x, uint8_t y, uint8_t width,
                                   chargfx_color_t color, uint8_t font_index,
                                   const char *text, bool right_align);
void ui_widgets_reset_animation_state(void);
void ui_widgets_format_note(char *buffer, size_t buffer_size, uint8_t midi_note);
void ui_widgets_clear_row(uint8_t y);
void ui_widgets_draw_top_band(const char *patch_name, uint8_t patch_index,
                              const char *rom_name, bool usb_midi_ready,
                              bool patch_highlight, bool bank_highlight);
void ui_widgets_draw_level_bar(uint8_t row, const char *label, uint8_t value,
                               bool bipolar, chargfx_color_t accent_color,
                               bool highlight);
void ui_widgets_draw_voice_area(const uint8_t active_notes[UI_WIDGET_NOTE_SLOTS],
                                const bool active_note_used[UI_WIDGET_NOTE_SLOTS]);
void ui_widgets_clear_stereo_vu(uint8_t x, uint8_t bottom_y);
/* bottom_y uses the normal UI grid coordinates: y=0 is the physical bottom row. */
void ui_widgets_draw_stereo_vu(uint8_t x, uint8_t bottom_y, int32_t left_bars,
                               int32_t right_bars);
void ui_widgets_draw_status_edge(bool usb_midi_ready, const char *rom_name,
                                 uint8_t active_voices, uint8_t peak_voices,
                                 uint8_t battery_percent,
                                 bool battery_charging);
void ui_widgets_draw_settings_row(uint8_t row, const char *label,
                                  const char *value, bool highlighted);

/**
 * Draw the DX7-style algorithm connectivity diagram for the currently selected patch.
 * Called during patch display to show algorithm visualization in a dedicated panel.
 *
 * @param x        top-left character column
 * @param y        top-left character row
 * @param alg_idx  algorithm index (0-31)
 * @param op_msk   6-bit operator mask (bit N set = operator N+1 is active)
 */
void ui_widgets_draw_algo(uint8_t x, uint8_t y, uint8_t alg_idx, uint8_t op_msk);

#endif

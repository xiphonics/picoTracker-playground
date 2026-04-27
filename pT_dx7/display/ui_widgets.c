#include "ui_widgets.h"

#include <stdio.h>
#include <string.h>

#include "algo_gfx.h"

static const char *const k_note_names[12] = {
    "C",  "C#", "D",  "D#", "E", "F",
    "F#", "G",  "G#", "A",  "A#", "B",
};

static const char *const k_battery_icons[] = {
    "\x80\x83\x83\x86",
    "\x80\x82\x83\x86",
    "\x80\x81\x83\x86",
    "\x80\x81\x82\x86",
    "\x80\x84\x85\x86",
};

static const char k_bargraph_glyphs[] = {
    ' ',        (char)0xA1, (char)0xA2, (char)0xA3, (char)0xA4, (char)0xA5,
    (char)0xA6, (char)0xA7, (char)0xA8, (char)0xA8, (char)0xDB,
};

static const char k_block_light = (char)0xB0;
static const char k_block_full = (char)0xDB;

#define UI_WIDGET_PATCH_ROW 23u
#define UI_WIDGET_BANK_ROW 22u
#define UI_WIDGET_OLD_BANK_ROW 21u
#define UI_WIDGET_USB_STATUS_ROW 1u
#define UI_WIDGET_ALGO_HEIGHT 9u

static int32_t g_prev_left_vu = 0;
static int32_t g_prev_right_vu = 0;

static uint8_t ui_widgets_clamp_u8(int value, uint8_t min_value,
                                   uint8_t max_value) {
  if (value < (int)min_value) {
    return min_value;
  }
  if (value > (int)max_value) {
    return max_value;
  }
  return (uint8_t)value;
}

static int32_t ui_widgets_clamp_i32(int32_t value, int32_t min_value,
                                    int32_t max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static void ui_widgets_write_char(uint8_t x, uint8_t y, chargfx_color_t color,
                                  uint8_t font_index, char c) {
  chargfx_set_font_index(font_index);
  chargfx_set_foreground(color);
  chargfx_set_background(CHARGFX_BG);
  chargfx_set_cursor(x, y);
  chargfx_putc(c, false);
}

void ui_widgets_write_field(uint8_t x, uint8_t y, uint8_t width,
                                   chargfx_color_t color, uint8_t font_index,
                                   const char *text, bool right_align) {
  char field[TEXT_WIDTH + 1];
  size_t copy_length = 0u;
  size_t offset = 0u;

  if (width == 0u || x >= TEXT_WIDTH || y >= TEXT_HEIGHT) {
    return;
  }

  if (width > (uint8_t)(TEXT_WIDTH - x)) {
    width = (uint8_t)(TEXT_WIDTH - x);
  }

  memset(field, ' ', width);
  field[width] = '\0';

  if (text != NULL) {
    copy_length = strlen(text);
    if (copy_length > width) {
      copy_length = width;
    }
    if (right_align && copy_length < width) {
      offset = width - copy_length;
    }
    memcpy(field + offset, text, copy_length);
  }

  for (uint8_t i = 0u; i < width; ++i) {
    ui_widgets_write_char((uint8_t)(x + i), y, color, font_index, field[i]);
  }
}

static void ui_widgets_trim_rom_name(char *buffer, size_t buffer_size,
                                     const char *rom_name) {
  size_t length;

  if (buffer == NULL || buffer_size == 0u) {
    return;
  }

  buffer[0] = '\0';
  if (rom_name == NULL) {
    return;
  }

  length = strlen(rom_name);
  if (length > 4u && rom_name[length - 4u] == '.' &&
      ((rom_name[length - 3u] | 0x20) == 's') &&
      ((rom_name[length - 2u] | 0x20) == 'y') &&
      ((rom_name[length - 1u] | 0x20) == 'x')) {
    length -= 4u;
  }

  if (length >= buffer_size) {
    length = buffer_size - 1u;
  }

  memcpy(buffer, rom_name, length);
  buffer[length] = '\0';
}

static const char *ui_widgets_battery_icon(uint8_t battery_percent,
                                           bool battery_charging) {
  if (battery_charging) {
    return k_battery_icons[4];
  }
  if (battery_percent > 90u) {
    return k_battery_icons[3];
  }
  if (battery_percent > 65u) {
    return k_battery_icons[2];
  }
  if (battery_percent > 35u) {
    return k_battery_icons[1];
  }
  return k_battery_icons[0];
}

static chargfx_color_t ui_widgets_battery_color(uint8_t battery_percent,
                                                bool battery_charging) {
  if (battery_percent <= 5u) {
    return CHARGFX_HILITE;
  }
  if (battery_percent < 20u) {
    return CHARGFX_ORANGE;
  }
  if (battery_charging) {
    return CHARGFX_BLUE;
  }
  return CHARGFX_GREEN;
}

void ui_widgets_reset_animation_state(void) {
  g_prev_left_vu = 0;
  g_prev_right_vu = 0;
}

void ui_widgets_format_note(char *buffer, size_t buffer_size, uint8_t midi_note) {
  const char *note_name;
  int octave;
  size_t note_length;

  if (buffer == NULL || buffer_size == 0u) {
    return;
  }

  note_name = k_note_names[midi_note % 12u];
  octave = (int)(midi_note / 12u) - 1;
  note_length = strlen(note_name);

  if (note_length == 1u) {
    snprintf(buffer, buffer_size, "%c-%d", note_name[0], octave);
  } else {
    snprintf(buffer, buffer_size, "%s%d", note_name, octave);
  }
}

void ui_widgets_clear_row(uint8_t y) {
  ui_widgets_write_field(0u, y, TEXT_WIDTH, CHARGFX_BG, UI_WIDGET_FONT_WIDE,
                         "", false);
}

void ui_widgets_draw_top_band(const char *patch_name, uint8_t patch_index,
                              const char *rom_name, bool usb_midi_ready,
                              bool patch_highlight, bool bank_highlight) {
  char patch_number[5];
  char rom_title[23];
  char patch_title[20];
  char bank_title[29];

  snprintf(patch_number, sizeof(patch_number), "[%02u]",
           (unsigned int)patch_index + 1u);
  ui_widgets_trim_rom_name(rom_title, sizeof(rom_title), rom_name);
  snprintf(patch_title, sizeof(patch_title), "%cPatch: %-12.12s",
           patch_highlight ? '>' : ' ',
           patch_name == NULL ? "" : patch_name);
  snprintf(bank_title, sizeof(bank_title), "%cBank: %s", 
           bank_highlight ? '>' : ' ',
           rom_title);

  ui_widgets_clear_row(UI_WIDGET_PATCH_ROW);
  ui_widgets_clear_row(UI_WIDGET_BANK_ROW);
  ui_widgets_clear_row(UI_WIDGET_OLD_BANK_ROW);

  ui_widgets_write_field(1u, UI_WIDGET_PATCH_ROW, 19u, patch_highlight ? CHARGFX_YELLOW : CHARGFX_WHITE, UI_WIDGET_FONT_WIDE,
                         patch_title, false);
  ui_widgets_write_field(20u, UI_WIDGET_PATCH_ROW, 4u, patch_highlight ? CHARGFX_YELLOW : CHARGFX_YELLOW, UI_WIDGET_FONT_WIDE,
                         patch_number, false);

  ui_widgets_write_field(1u, UI_WIDGET_BANK_ROW, 28u, bank_highlight ? CHARGFX_YELLOW : CHARGFX_BLUE, UI_WIDGET_FONT_WIDE,
                         bank_title, false);

  ui_widgets_write_field(1u, UI_WIDGET_USB_STATUS_ROW, 7u,
                         usb_midi_ready ? CHARGFX_GREEN : CHARGFX_ORANGE,
                         UI_WIDGET_FONT_WIDE,
                         usb_midi_ready ? "USB ON" : "USB OFF", false);
  ui_widgets_write_field(9u, UI_WIDGET_USB_STATUS_ROW, 7u, CHARGFX_GREEN, UI_WIDGET_FONT_WIDE,
                         "TRS ON", false);
}

void ui_widgets_draw_level_bar(uint8_t row, const char *label, uint8_t value,
                               bool bipolar, chargfx_color_t accent_color,
                               bool highlight) {
  enum {
    k_label_x = 1u,
    k_bar_x = 8u,
    k_bar_width = 21u,
    k_bar_inner_width = 19u,
  };

  chargfx_color_t field_color = highlight ? CHARGFX_YELLOW : accent_color;

  ui_widgets_write_field(k_label_x, row, 6u, field_color, UI_WIDGET_FONT_WIDE,
                         label, false);
  ui_widgets_write_char(k_bar_x, row, CHARGFX_GRAY, UI_WIDGET_FONT_WIDE, '[');
  ui_widgets_write_char((uint8_t)(k_bar_x + k_bar_width - 1u), row, CHARGFX_GRAY,
                        UI_WIDGET_FONT_WIDE, ']');

  for (uint8_t i = 0u; i < k_bar_inner_width; ++i) {
    ui_widgets_write_char((uint8_t)(k_bar_x + 1u + i), row, CHARGFX_DARK_SLATE_GRAY,
                          UI_WIDGET_FONT_WIDE, k_block_light);
  }

  if (bipolar) {
    const int center = (int)(k_bar_inner_width / 2u);
    const int delta = (((int)value - 128) * center) / 127;
    ui_widgets_write_char((uint8_t)(k_bar_x + 1u + center), row, CHARGFX_WHITE,
                          UI_WIDGET_FONT_WIDE, '|');
    if (delta > 0) {
      for (int i = 0; i < delta; ++i) {
        ui_widgets_write_char((uint8_t)(k_bar_x + 2u + center + i), row,
                              field_color, UI_WIDGET_FONT_WIDE,
                              k_block_full);
      }
    } else if (delta < 0) {
      for (int i = 0; i < -delta; ++i) {
        ui_widgets_write_char((uint8_t)(k_bar_x + center - i), row, field_color,
                              UI_WIDGET_FONT_WIDE, k_block_full);
      }
    }
    return;
  }

  {
    const uint8_t filled =
        (uint8_t)(((uint16_t)value * k_bar_inner_width) / 255u);
    for (uint8_t i = 0u; i < filled; ++i) {
      ui_widgets_write_char((uint8_t)(k_bar_x + 1u + i), row, field_color,
                            UI_WIDGET_FONT_WIDE, k_block_full);
    }
  }
}

void ui_widgets_draw_voice_area(
    const uint8_t active_notes[UI_WIDGET_NOTE_SLOTS],
    const bool active_note_used[UI_WIDGET_NOTE_SLOTS]) {
  char note_text[8];
  char slot_line[(UI_WIDGET_NOTE_SLOTS * 4u) + 2u];
  size_t offset = 0u;

  for (uint8_t i = 0u; i < UI_WIDGET_NOTE_SLOTS; ++i) {
    if (active_note_used != NULL && active_note_used[i]) {
      ui_widgets_format_note(note_text, sizeof(note_text), active_notes[i]);
    } else {
      snprintf(note_text, sizeof(note_text), "---");
    }

    slot_line[offset++] = '|';
    memcpy(slot_line + offset, note_text, 3u);
    offset += 3u;
  }

  slot_line[offset++] = '|';
  slot_line[offset] = '\0';

  ui_widgets_write_field(0u, 10u, TEXT_WIDTH, CHARGFX_WHITE, UI_WIDGET_FONT_WIDE,
                         slot_line, false);
}

void ui_widgets_clear_stereo_vu(uint8_t x, uint8_t bottom_y) {
  for (uint8_t i = 0u; i < UI_WIDGET_VU_DRAW_HEIGHT; ++i) {
    const uint8_t row_y = (uint8_t)(bottom_y + i);

    if (row_y >= TEXT_HEIGHT || x >= TEXT_WIDTH) {
      break;
    }

    ui_widgets_write_char(x, row_y, CHARGFX_BG, UI_WIDGET_FONT_WIDE, ' ');
    if ((uint8_t)(x + 1u) < TEXT_WIDTH) {
      ui_widgets_write_char((uint8_t)(x + 1u), row_y, CHARGFX_BG,
                            UI_WIDGET_FONT_WIDE, ' ');
    }
  }
}

void ui_widgets_draw_stereo_vu(uint8_t x, uint8_t bottom_y, int32_t left_bars,
                               int32_t right_bars) {
  const int max_rise_step = 20;
  const int max_fall_step = 10;
  const int full_scale = (int)(UI_WIDGET_VU_DRAW_HEIGHT * 10u);

  left_bars = ui_widgets_clamp_i32(left_bars, 0, UI_WIDGET_VU_MAX);
  right_bars = ui_widgets_clamp_i32(right_bars, 0, UI_WIDGET_VU_MAX);

  if (left_bars > g_prev_left_vu + max_rise_step) {
    left_bars = g_prev_left_vu + max_rise_step;
  } else if (left_bars < g_prev_left_vu - max_fall_step) {
    left_bars = g_prev_left_vu - max_fall_step;
  }

  if (right_bars > g_prev_right_vu + max_rise_step) {
    right_bars = g_prev_right_vu + max_rise_step;
  } else if (right_bars < g_prev_right_vu - max_fall_step) {
    right_bars = g_prev_right_vu - max_fall_step;
  }

  for (uint8_t i = 0u; i < UI_WIDGET_VU_DRAW_HEIGHT; ++i) {
    chargfx_color_t color = CHARGFX_GREEN;
    const uint8_t row_y = (uint8_t)(bottom_y + i);
    const uint8_t rows_from_bottom = i;
    const uint8_t threshold_row = (uint8_t)(
        (rows_from_bottom * UI_WIDGET_VU_HEIGHT) / UI_WIDGET_VU_DRAW_HEIGHT);
    const int left_scaled = (left_bars * full_scale) / UI_WIDGET_VU_MAX;
    const int right_scaled = (right_bars * full_scale) / UI_WIDGET_VU_MAX;
    const int left_segment = left_scaled - (int)(10u * rows_from_bottom);
    const int right_segment = right_scaled - (int)(10u * rows_from_bottom);

    if (row_y >= TEXT_HEIGHT) {
      break;
    }

    if (threshold_row >= UI_WIDGET_VU_CLIP_LEVEL) {
      color = CHARGFX_HILITE;
    } else if (threshold_row > UI_WIDGET_VU_WARN_LEVEL) {
      color = CHARGFX_ORANGE;
    }

    ui_widgets_write_char(x, row_y, color,
                          UI_WIDGET_FONT_WIDE,
                          k_bargraph_glyphs[ui_widgets_clamp_u8(left_segment, 0u,
                                                                10u)]);
    ui_widgets_write_char((uint8_t)(x + 1u), row_y, color,
                          UI_WIDGET_FONT_WIDE,
                          k_bargraph_glyphs[ui_widgets_clamp_u8(right_segment, 0u,
                                                                10u)]);
  }

  g_prev_left_vu = left_bars;
  g_prev_right_vu = right_bars;
}

void ui_widgets_draw_status_edge(bool usb_midi_ready, const char *rom_name,
                                 uint8_t active_voices, uint8_t peak_voices,
                                 uint8_t battery_percent,
                                 bool battery_charging) {
  (void)usb_midi_ready;
  (void)rom_name;
  (void)active_voices;
  (void)peak_voices;

  ui_widgets_write_field(28u, 23u, 4u,
                         ui_widgets_battery_color(battery_percent,
                                                  battery_charging),
                         UI_WIDGET_FONT_WIDE,
                         ui_widgets_battery_icon(battery_percent,
                                                 battery_charging),
                         false);
}

void ui_widgets_draw_settings_row(uint8_t row, const char *label,
                                  const char *value, bool highlighted) {
  chargfx_color_t color = highlighted ? CHARGFX_YELLOW : CHARGFX_WHITE;
  char line[TEXT_WIDTH + 1];
  size_t label_len, value_len;

  memset(line, ' ', TEXT_WIDTH);
  line[TEXT_WIDTH] = '\0';

  if (highlighted) {
    line[0] = '>';
  }

  if (label != NULL) {
    label_len = strlen(label);
    if (label_len > 20) label_len = 20;
    memcpy(line + 2, label, label_len);
  }

  if (value != NULL) {
    value_len = strlen(value);
    if (value_len > 8) value_len = 8;
    memcpy(line + TEXT_WIDTH - value_len, value, value_len);
  }

  ui_widgets_write_field(0u, row, TEXT_WIDTH, color, UI_WIDGET_FONT_WIDE, line,
                         false);
}

void ui_widgets_draw_algo(uint8_t x, uint8_t y, uint8_t alg_idx,
                          uint8_t op_msk) {
  algo_draw(x, y, 32u, UI_WIDGET_ALGO_HEIGHT, alg_idx, NULL, op_msk);
}

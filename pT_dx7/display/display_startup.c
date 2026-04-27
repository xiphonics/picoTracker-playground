#include "display_startup.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "chargfx.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico_gpio.h"
#include "ui_widgets.h"


#ifndef NOTE_UART_LOG
#define NOTE_UART_LOG 0
#endif

#define STATUS_LINE_Y 0

enum {
  DISPLAY_FONT_WIDE = 2u,
  DISPLAY_NOTE_ROW = TEXT_HEIGHT - 4u,
  DISPLAY_PERF_ROW_0 = TEXT_HEIGHT - 6u,
  DISPLAY_PERF_ROW_1 = TEXT_HEIGHT - 5u,
};

static const char *const kNoteNames[12] = {
    "C",  "C#", "D",  "D#", "E", "F",
    "F#", "G",  "G#", "A",  "A#", "B",
};

static void display_format_note(char *buffer, size_t buffer_size,
                                uint8_t midi_note) {
  const char *note_name = kNoteNames[midi_note % 12u];
  const int octave = (int)(midi_note / 12u) - 1;

  if (buffer == NULL || buffer_size == 0u) {
    return;
  }

  snprintf(buffer, buffer_size, "%s%d", note_name, octave);
}


static char g_sdcard_status[TEXT_WIDTH + 1];

static void display_enable_backlight(void) {
  const uint slice_num = pwm_gpio_to_slice_num(DISPLAY_PWM);
  const float divider =
      (float)clock_get_hz(clk_sys) / (1000.0f * 256.0f);

  gpio_set_function(DISPLAY_PWM, GPIO_FUNC_PWM);
  pwm_set_clkdiv(slice_num, divider);
  pwm_set_wrap(slice_num, 255u);
  pwm_set_gpio_level(DISPLAY_PWM, 255u);
  pwm_set_enabled(slice_num, true);
}

static void display_status_line(chargfx_color_t color,
                               const char *text) {
  char padded[TEXT_WIDTH + 1];
  size_t length = strlen(text);

  if (length > TEXT_WIDTH) {
    length = TEXT_WIDTH;
  }

  memset(padded, ' ', sizeof(padded) - 1u);
  memcpy(padded, text, length);
  padded[TEXT_WIDTH] = '\0';

  chargfx_set_foreground(color);
  chargfx_set_font_index(DISPLAY_FONT_WIDE);
  for (uint8_t x = 0; x < TEXT_WIDTH; ++x) {
    chargfx_set_cursor(x, STATUS_LINE_Y);
    chargfx_putc(padded[x], false);
  }
}

static void clear_status_line() {
  ui_widgets_clear_row(STATUS_LINE_Y);
}

static void display_draw_static_layout(uint8_t patch_index, const char *patch_name,
                                       const char *rom_name, uint8_t master_level,
                                       uint8_t voice_level, uint8_t reverb_wet,
                                       uint8_t reverb_feedback, uint8_t reverb_damping,
                                       uint8_t reverb_input_gain,
                                       const char *output_level_text,
                                       uint8_t algorithm_index,
                                       int8_t cursor,
                                       uint8_t preview_note,
                                       bool preview_note_active,
                                       bool usb_midi_ready,
                                       uint8_t battery_percent,
                                       bool battery_charging) {
  char line[TEXT_WIDTH + 1];

  ui_widgets_draw_top_band(patch_name, patch_index, rom_name, usb_midi_ready, cursor == 0, cursor == 1);
  
  // Parameter area below the algorithm panel.
  ui_widgets_draw_level_bar(11u, "MASTER", master_level, false, CHARGFX_WHITE, cursor == 2);
  ui_widgets_draw_level_bar(10u, "VOICE", voice_level, false, CHARGFX_WHITE, cursor == 3);
  ui_widgets_draw_level_bar(8u, "REV WET", reverb_wet, false, CHARGFX_BLUE, cursor == 4);
  ui_widgets_draw_level_bar(7u, "REV FB", reverb_feedback, false, CHARGFX_BLUE, cursor == 5);
  ui_widgets_draw_level_bar(6u, "REV DMP", reverb_damping, false, CHARGFX_BLUE, cursor == 6);
  
  if (reverb_input_gain == 0) {
    snprintf(line, sizeof(line), "%cREV GAIN: OFF", cursor == 7 ? '>' : ' ');
  } else {
    // Maps 1..4 to 0dB..-18dB
    const char* db_labels[] = {"0dB", "-6dB", "-12dB", "-18dB"};
    snprintf(line, sizeof(line), "%cREV GAIN: %s", cursor == 7 ? '>' : ' ', db_labels[reverb_input_gain - 1]);
  }
  ui_widgets_write_field(1u, 4u, 20u, cursor == 7 ? CHARGFX_YELLOW : CHARGFX_WHITE, DISPLAY_FONT_WIDE, line, false);
  
  snprintf(line, sizeof(line), "%cOUT: %s", cursor == 8 ? '>' : ' ', output_level_text);
  ui_widgets_write_field(1u, 3u, 20u, cursor == 8 ? CHARGFX_YELLOW : CHARGFX_WHITE, DISPLAY_FONT_WIDE, line, false);

  (void)preview_note;
  (void)preview_note_active;

  ui_widgets_clear_row(2u); // Clear unused rows
  ui_widgets_clear_row(5u);
  ui_widgets_clear_row(9u);
  ui_widgets_clear_row(12u);
  ui_widgets_clear_row(21u);

  ui_widgets_draw_algo(1u, 13u, algorithm_index, 0x3Fu);

  if (g_sdcard_status[0] != '\0') {
    display_status_line(CHARGFX_GREEN, g_sdcard_status);
  } else {
    clear_status_line();
  }

  ui_widgets_draw_status_edge(usb_midi_ready, rom_name, 0u, 0u,
                              battery_percent, battery_charging);
}


void display_startup_init(void) {

  spi_init(DISPLAY_SPI, 500 * 1000);
  int spi_baud = spi_set_baudrate(DISPLAY_SPI, 75000 * 1000);

  gpio_set_function(DISPLAY_SCK, GPIO_FUNC_SPI);
  gpio_set_function(DISPLAY_MOSI, GPIO_FUNC_SPI);
  gpio_set_function(DISPLAY_MISO, GPIO_FUNC_SPI);

  display_enable_backlight();

  gpio_init(DISPLAY_CS);
  gpio_set_dir(DISPLAY_CS, GPIO_OUT);
  gpio_put(DISPLAY_CS, 1);

  gpio_init(DISPLAY_RESET);
  gpio_set_dir(DISPLAY_RESET, GPIO_OUT);
  gpio_put(DISPLAY_RESET, 1);

  gpio_init(DISPLAY_DC);
  gpio_set_dir(DISPLAY_DC, GPIO_OUT);
  gpio_put(DISPLAY_DC, 1);

  chargfx_init();
  chargfx_clear(CHARGFX_BG);
  ui_widgets_reset_animation_state();
  chargfx_draw_changed_rows();

  printf("LCD ST7789 SPI:%d CS:%d DC:%d RESET:%d BL:%d\n", spi_baud,
         DISPLAY_CS, DISPLAY_DC, DISPLAY_RESET, DISPLAY_PWM);

}

void display_sdcard_status_set(const char *status) {
  if (status == NULL) {
    g_sdcard_status[0] = '\0';
    return;
  }

  snprintf(g_sdcard_status, sizeof(g_sdcard_status), "%s", status);
}

void display_status_update(uint8_t patch_index, const char *patch_name,
                           const char *rom_name, uint8_t master_level,
                           uint8_t voice_level, uint8_t reverb_wet,
                           uint8_t reverb_feedback, uint8_t reverb_damping,
                           uint8_t reverb_input_gain,
                           const char *output_level_text,
                           uint8_t algorithm_index,
                           int8_t cursor,
                           uint8_t preview_note,
                           bool preview_note_active, bool usb_midi_ready,
                           uint8_t battery_percent, bool battery_charging,
                           bool clear_screen) {
  if (clear_screen) {
    chargfx_clear(CHARGFX_BG);
  }
  display_draw_static_layout(patch_index, patch_name, rom_name, master_level,
                             voice_level, reverb_wet, reverb_feedback, reverb_damping,
                             reverb_input_gain, output_level_text, algorithm_index, cursor,
                             preview_note, preview_note_active,
                             usb_midi_ready, battery_percent, battery_charging);
  chargfx_draw_changed_rows();
}

void display_note_update(uint8_t midi_note, bool note_on) {
#if NOTE_UART_LOG
  char line[TEXT_WIDTH + 1];

  if (!note_on) {
    printf("note ---\n");
    (void)midi_note;
    return;
  }

  display_format_note(line, sizeof(line), midi_note);
  printf("note %s\n", line);
#else
  (void)midi_note;
  (void)note_on;
#endif
}

void display_perf_update(uint32_t stereo_frames, uint32_t buffer_time_us,
                         uint32_t average_render_us, uint32_t max_render_us,
                         uint32_t audio_xruns, uint32_t voice_steals,
                         uint32_t same_note_retriggers,
                         uint32_t output_clips) {
  char line0[TEXT_WIDTH + 1];
  char line1[TEXT_WIDTH + 1];
  const uint32_t load_tenths =
      buffer_time_us == 0u ? 0u : (average_render_us * 1000u) / buffer_time_us;

  snprintf(line0, sizeof(line0), "buf %3luf %2lu.%02lums",
           (unsigned long)stereo_frames,
           (unsigned long)(buffer_time_us / 1000u),
           (unsigned long)((buffer_time_us % 1000u) / 10u));
  snprintf(line1, sizeof(line1), "avg %lu.%02lu max %lu.%02lu x%lu v%lu r%lu c%lu",
           (unsigned long)(average_render_us / 1000u),
           (unsigned long)((average_render_us % 1000u) / 10u),
           (unsigned long)(max_render_us / 1000u),
           (unsigned long)((max_render_us % 1000u) / 10u),
           (unsigned long)audio_xruns,
           (unsigned long)voice_steals,
           (unsigned long)same_note_retriggers,
           (unsigned long)output_clips);
}

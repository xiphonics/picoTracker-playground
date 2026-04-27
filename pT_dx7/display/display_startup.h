#ifndef DX_2350_DISPLAY_STARTUP_H
#define DX_2350_DISPLAY_STARTUP_H

#include <stdbool.h>
#include <stdint.h>

void display_startup_init(void);
void display_sdcard_status_set(const char *status);
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
                           bool clear_screen);
void display_note_update(uint8_t midi_note, bool note_on);
void display_perf_update(uint32_t stereo_frames, uint32_t buffer_time_us,
                         uint32_t average_render_us, uint32_t max_render_us,
                         uint32_t audio_xruns, uint32_t voice_steals,
                         uint32_t same_note_retriggers,
                         uint32_t output_clips);

#endif

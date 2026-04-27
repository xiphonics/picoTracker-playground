#include <stdio.h>
#include <string.h>

#include "audio_output.h"
#include "audio_render_core.h"
#include "display_startup.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "midi.h"
#include "pico/stdlib.h"
#include "pico_gpio.h"
#include "platform_clocks.h"
#include "platform_input.h"
#include "sdcard_fs.h"
#include "dx7_rom_bank.h"

#ifndef USB_MIDI_HOST
#define USB_MIDI_HOST 0
#endif

#ifndef AUDIO_PERF_LOG_ENABLED
#define AUDIO_PERF_LOG_ENABLED 0
#endif

#define KEY_LEFT_BIT (1u << 0)
#define KEY_DOWN_BIT (1u << 1)
#define KEY_RIGHT_BIT (1u << 2)
#define KEY_UP_BIT (1u << 3)
#define KEY_ALT_BIT (1u << 4)
#define KEY_EDIT_BIT (1u << 5)
#define KEY_ENTER_BIT (1u << 6)
#define KEY_NAV_BIT (1u << 7)
#define KEY_PLAY_BIT (1u << 8)

#define PREVIEW_NOTE_STEP 1
#define PREVIEW_OCTAVE_STEP 12
#define PREVIEW_VELOCITY 100u
#define BUTTON_VOLUME_STEP 16u

enum {
  BATTERY_SAMPLE_PERIOD_US = 1000000u,
  DX7_PATCH_ALGORITHM_OFFSET = 110u,
  DX7_ROM_FILE_READ_CAPACITY = 8192u,
};

typedef struct {
  uint32_t last_audio_xruns;
  uint32_t last_perf_sequence;
} RenderPerfCounters;

static uint8_t g_current_patch_index = 0;
static size_t g_active_rom_file_index = 0u;
static bool g_active_rom_file_valid = false;

typedef struct {
  bool was_ready;
  uint8_t bank_select_msb;
  uint8_t bank_select_lsb;
} MidiRuntime;

typedef struct {
  uint8_t preview_note;
  uint8_t preview_note_playing;
  uint8_t preview_velocity;
  uint8_t battery_percentage;
  bool preview_note_active;
  bool usb_midi_ready;
  bool battery_charging;
} UiState;

typedef enum {
  FIELD_PATCH = 0,
  FIELD_BANK,
  FIELD_MASTER_LEVEL,
  FIELD_VOICE_LEVEL,
  FIELD_REVERB_WET,
  FIELD_REVERB_FEEDBACK,
  FIELD_REVERB_DAMPING,
  FIELD_REVERB_INPUT_GAIN,
  FIELD_OUTPUT_LEVEL,
  FIELD_COUNT,
} UiField;

typedef struct {
  uint8_t cursor;          // index into UiField
  AudioOutputLevel level;
  uint8_t reverb_wet;      // 0..255
  uint8_t reverb_feedback; // 0..255
  uint8_t reverb_damping;  // 0..255
  uint8_t reverb_input_gain; // 0..4 (0=OFF, 1=0dB, 2=-6dB, 3=-12dB, 4=-18dB)
} SettingsState;

static SettingsState g_settings = {
  .cursor = 0,
  .level = AUDIO_OUTPUT_LEVEL_HEADPHONES,
  .reverb_wet = 128,
  .reverb_feedback = 188, // ~0.7 * 255
  .reverb_damping = 230,  // ~0.9 * 255
  .reverb_input_gain = 2, // Maps to shift 1 (>> 1)
};

typedef struct {
  uint32_t last_sample_us;
  bool initialized;
  bool charging;
  uint8_t percentage;
  uint16_t voltage_mv;
} BatteryMonitor;

static uint8_t battery_monitor_display_bucket(uint8_t percentage,
                                              bool charging) {
  if (charging) {
    return 4u;
  }
  if (percentage > 65u) {
    return 3u;
  }
  if (percentage > 35u) {
    return 2u;
  }
  if (percentage > 10u) {
    return 1u;
  }
  return 0u;
}

static void battery_monitor_init(BatteryMonitor *monitor) {
  if (monitor == NULL) {
    return;
  }

  adc_gpio_init(BATT_VOLTAGE_IN);
  adc_init();
  adc_select_input(3);

  monitor->last_sample_us = 0u;
  monitor->initialized = false;
  monitor->charging = false;
  monitor->percentage = 0u;
  monitor->voltage_mv = 0u;
}

static void battery_monitor_sample(BatteryMonitor *monitor) {
  uint32_t adc_reading;
  uint32_t q;

  if (monitor == NULL) {
    return;
  }

  adc_reading = adc_read();
  monitor->voltage_mv = (uint16_t)((adc_reading * 8u) / 5u);

  if (monitor->voltage_mv < 3325u) {
    monitor->percentage = 0u;
  } else if (monitor->voltage_mv > 3900u) {
    monitor->percentage = 100u;
  } else {
    q = 100u * (uint32_t)monitor->voltage_mv - 390000u;
    q *= q;
    q >>= 25;
    monitor->percentage = (uint8_t)(100u - q);
  }

  monitor->charging = monitor->voltage_mv > 4000u;
  monitor->initialized = true;
}

static bool battery_monitor_refresh_if_due(BatteryMonitor *monitor,
                                           UiState *ui, uint32_t now_us) {
  uint8_t previous_bucket;
  uint8_t previous_percentage;
  bool previous_charging;
  bool was_initialized;

  if (monitor == NULL || ui == NULL) {
    return false;
  }

  if (monitor->initialized &&
      (now_us - monitor->last_sample_us) < BATTERY_SAMPLE_PERIOD_US) {
    return false;
  }

  was_initialized = monitor->initialized;
  previous_percentage = monitor->percentage;
  previous_charging = monitor->charging;
  previous_bucket =
      battery_monitor_display_bucket(previous_percentage, previous_charging);
  battery_monitor_sample(monitor);
  monitor->last_sample_us = now_us;

  ui->battery_percentage = monitor->percentage;
  ui->battery_charging = monitor->charging;

  return !was_initialized ||
         previous_bucket != battery_monitor_display_bucket(
                                monitor->percentage, monitor->charging) ||
         previous_charging != monitor->charging ||
         (previous_percentage <= 5u) != (monitor->percentage <= 5u);
}

static uint8_t current_patch_algorithm_index(void) {
  const uint8_t *patch_data = dx7_rom_bank_patch_data(g_current_patch_index);

  if (patch_data == NULL) {
    return 0u;
  }

  return patch_data[DX7_PATCH_ALGORITHM_OFFSET] & 0x1fu;
}

static void refresh_ui(const UiState *ui, uint8_t volume, uint8_t voice_level, bool force_clear) {
  char patch_name[11];

  if (ui == NULL) {
    return;
  }

  dx7_rom_bank_patch_name_copy(g_current_patch_index, patch_name,
                               sizeof(patch_name));
  display_status_update(g_current_patch_index, patch_name,
                        dx7_rom_bank_active_name(), volume, voice_level,
                        g_settings.reverb_wet,
                        g_settings.reverb_feedback,
                        g_settings.reverb_damping,
                        g_settings.reverb_input_gain,
                        (g_settings.level == AUDIO_OUTPUT_LEVEL_LINE) ? "LINE" : "HP",
                        current_patch_algorithm_index(),
                        (int8_t)g_settings.cursor,
                        60, false, ui->usb_midi_ready,
                        ui->battery_percentage, ui->battery_charging,
                        force_clear);
}

static bool select_patch(uint8_t patch_index) {
  char patch_name[11];
  const size_t patch_count = dx7_rom_bank_patch_count();

  if (patch_index >= patch_count || patch_index == g_current_patch_index) {
    return false;
  }

  /* Voices keep a pointer to the current converted patch, so switching
   * patches safely requires clearing any active voices first. */
  audio_render_core_panic();
  g_current_patch_index = patch_index;
  audio_render_core_set_patch((FmSynthPatchId)patch_index);
  dx7_rom_bank_patch_name_copy(patch_index, patch_name, sizeof(patch_name));
  printf("Patch: %u", (unsigned int)patch_index);
  if (patch_name[0] != '\0') {
    printf(" (%s)", patch_name);
  }
  printf(" [ROM: %s]\n", dx7_rom_bank_active_name());
  return true;
}

static void boot_log_sdcard_status(void) {
  char summary[32];
  char rom_name[DX7_ROM_NAME_MAX];
  char error[96];

  if (!sdcard_fs_init()) {
    printf("SD: mount failed\n");
    if (sdcard_fs_last_error(error, sizeof(error)) &&
        strcmp(error, "mount failed") != 0) {
      printf("SD: %s\n", error);
    }

    snprintf(summary, sizeof(summary), "sd missing");
    display_sdcard_status_set(summary);
    return;
  }

  printf("SD: mounted\n");

  if (!sdcard_fs_refresh_rom_dir()) {
    if (sdcard_fs_rom_dir_found()) {
      printf("SD: /dx7patches scan failed\n");
    } else {
      printf("SD: /dx7patches missing\n");
    }

    if (sdcard_fs_last_error(error, sizeof(error)) &&
        strcmp(error, "/dx7patches missing") != 0) {
      printf("SD: %s\n", error);
    }

    snprintf(summary, sizeof(summary), "dx7patches missing");
    display_sdcard_status_set(summary);
    return;
  }

  printf("SD: /dx7patches found\n");
  printf("SD: %u rom files\n", (unsigned int)sdcard_fs_rom_count());
  for (size_t i = 0; i < sdcard_fs_rom_count(); ++i) {
    if (sdcard_fs_get_rom_name(i, rom_name, sizeof(rom_name))) {
      printf("ROM[%u]: %s\n", (unsigned int)i, rom_name);
    }
  }

  snprintf(summary, sizeof(summary), "sd ok %u sysex files found",
           (unsigned int)sdcard_fs_rom_count());
  display_sdcard_status_set(summary);
}

static bool select_patch_for_program_change(uint8_t program_number) {
  const size_t patch_count = dx7_rom_bank_patch_count();

  if (patch_count == 0u) {
    return false;
  }

  return select_patch((uint8_t)(program_number % patch_count));
}

static void clear_note_activity(UiState *ui) {
  if (ui != NULL) {
    ui->preview_note_active = false;
  }
}

static bool preview_note_start(UiState *ui) {
  if (ui == NULL || ui->preview_note_active) {
    return false;
  }

  audio_render_core_note_on(60, ui->preview_velocity);
  ui->preview_note_playing = 60;
  ui->preview_note_active = true;
  return true;
}

static bool preview_note_stop(UiState *ui) {
  if (ui == NULL || !ui->preview_note_active) {
    return false;
  }

  audio_render_core_note_off(ui->preview_note_playing);
  ui->preview_note_active = false;
  return true;
}

static bool adjust_level(uint8_t *level, int delta) {
  int next_level;

  if (level == NULL || delta == 0) {
    return false;
  }

  next_level = (int)(*level) + delta;
  if (next_level < 0) {
    next_level = 0;
  } else if (next_level > 255) {
    next_level = 255;
  }

  if ((uint8_t)next_level == *level) {
    return false;
  }

  *level = (uint8_t)next_level;
  return true;
}

static void update_status_message(const char *format, const char *detail) {
  char summary[32];

  if (format == NULL) {
    display_sdcard_status_set(NULL);
    return;
  }

  snprintf(summary, sizeof(summary), format, detail == NULL ? "" : detail);
  display_sdcard_status_set(summary);
}

static bool load_rom_by_index(UiState *ui, size_t rom_index) {
  static uint8_t file_buffer[DX7_ROM_FILE_READ_CAPACITY];
  char rom_name[DX7_ROM_NAME_MAX];
  char error[96];
  size_t bytes_read = 0u;
  size_t patch_count;

  if (ui == NULL) {
    return false;
  }

  if (!sdcard_fs_get_rom_name(rom_index, rom_name, sizeof(rom_name))) {
    if (!sdcard_fs_last_error(error, sizeof(error))) {
      snprintf(error, sizeof(error), "rom name unavailable");
    }
    printf("ROM: %s\n", error);
    update_status_message("rom err %.23s", error);
    return false;
  }

  if (!sdcard_fs_read_rom_file(rom_index, file_buffer, sizeof(file_buffer),
                               &bytes_read)) {
    if (!sdcard_fs_last_error(error, sizeof(error))) {
      snprintf(error, sizeof(error), "read failed");
    }
    printf("ROM: %s (%s)\n", error, rom_name);
    update_status_message("rom err %.23s", error);
    return false;
  }

  if (!dx7_rom_bank_load_sysex(file_buffer, bytes_read, rom_name)) {
    if (!dx7_rom_bank_last_error(error, sizeof(error))) {
      snprintf(error, sizeof(error), "unsupported SysEx format");
    }
    printf("ROM: %s (%s)\n", error, rom_name);
    update_status_message("rom err %.23s", error);
    return false;
  }

  audio_render_core_panic();
  clear_note_activity(ui);
  patch_count = dx7_rom_bank_patch_count();
  if (patch_count == 0u || g_current_patch_index >= patch_count) {
    g_current_patch_index = 0u;
  }
  audio_render_core_set_patch((FmSynthPatchId)g_current_patch_index);
  printf("ROM: loaded %s\n", dx7_rom_bank_active_name());
  g_active_rom_file_index = rom_index;
  g_active_rom_file_valid = true;
  return true;
}

static bool select_rom_by_index(UiState *ui, size_t rom_index) {
  const size_t rom_count = sdcard_fs_rom_count();

  if (ui == NULL) {
    return false;
  }

  if (!sdcard_fs_is_mounted()) {
    update_status_message("rom err mount failed", NULL);
    printf("ROM: mount failed\n");
    return false;
  }

  if (!sdcard_fs_rom_dir_found()) {
    update_status_message("rom err dx7patches", NULL);
    printf("ROM: /dx7patches missing\n");
    return false;
  }

  if (rom_count == 0u) {
    update_status_message("rom err no files", NULL);
    printf("ROM: no ROM files\n");
    return false;
  }

  if (rom_index >= rom_count) {
    printf("ROM: bank index %u out of range (count=%u)\n",
           (unsigned int)rom_index, (unsigned int)rom_count);
    update_status_message("rom err bank range", NULL);
    return false;
  }

  if (g_active_rom_file_valid && g_active_rom_file_index == rom_index) {
    return false;
  }

  return load_rom_by_index(ui, rom_index);
}

static bool cycle_rom(UiState *ui, int direction) {
  const size_t rom_count = sdcard_fs_rom_count();
  size_t next_index;

  if (ui == NULL || direction == 0) {
    return false;
  }

  if (!g_active_rom_file_valid) {
    next_index = direction > 0 ? 0u : (rom_count - 1u);
  } else if (direction > 0) {
    next_index = (g_active_rom_file_index + 1u) % rom_count;
  } else {
    next_index = (g_active_rom_file_index + rom_count - 1u) % rom_count;
  }

  return select_rom_by_index(ui, next_index);
}

static void boot_autoload_first_valid_rom(UiState *ui) {
  const size_t rom_count = sdcard_fs_rom_count();

  if (ui == NULL || !sdcard_fs_is_mounted() || !sdcard_fs_rom_dir_found() ||
      rom_count == 0u) {
    return;
  }

  for (size_t i = 0; i < rom_count; ++i) {
    if (select_rom_by_index(ui, i)) {
      printf("ROM: boot-loaded index %u\n", (unsigned int)i);
      return;
    }
  }

  printf("ROM: no valid SD ROMs, using built-in fallback\n");
  display_sdcard_status_set("rom built-in");
}

static void render_perf_publish_if_due(RenderPerfCounters *counters,
                                       uint32_t stereo_frames_per_buffer) {
#if AUDIO_PERF_LOG_ENABLED
  AudioRenderPerfSnapshot perf_snapshot;
  AudioOutputStats audio_stats;
  uint32_t average_load_tenths;
  uint32_t max_load_tenths;
  uint32_t window_audio_xruns;
  const uint32_t buffer_time_us =
      (stereo_frames_per_buffer * 1000000u) / AUDIO_OUTPUT_SAMPLE_RATE_HZ;

  if (counters == NULL) {
    return;
  }

  if (!audio_render_core_get_perf_snapshot(&perf_snapshot) ||
      perf_snapshot.sequence == counters->last_perf_sequence) {
    return;
  }

  counters->last_perf_sequence = perf_snapshot.sequence;
  average_load_tenths =
      buffer_time_us == 0u ? 0u
                           : (perf_snapshot.average_render_us * 1000u) /
                                 buffer_time_us;
  max_load_tenths =
      buffer_time_us == 0u
          ? 0u
          : (perf_snapshot.max_render_us * 1000u) / buffer_time_us;
  audio_output_get_stats(&audio_stats);
  window_audio_xruns = audio_stats.dma_starvation_count - counters->last_audio_xruns;
  counters->last_audio_xruns = audio_stats.dma_starvation_count;
  if (perf_snapshot.active_voice_count != 0u || window_audio_xruns != 0u ||
      perf_snapshot.voice_steal_count != 0u ||
      perf_snapshot.same_note_retrigger_count != 0u ||
      perf_snapshot.output_clip_count != 0u) {
    printf(
        "PERF v=%u/%u r=%lu/%luus rv=%luus l=%lu/%lu%% x=%lu s=%lu c=%lu\n",
        (unsigned int)perf_snapshot.active_voice_count,
        (unsigned int)perf_snapshot.peak_active_voice_count,
        (unsigned long)perf_snapshot.average_render_us,
        (unsigned long)perf_snapshot.max_render_us,
        (unsigned long)perf_snapshot.average_reverb_us,
        (unsigned long)(average_load_tenths / 10u),
        (unsigned long)(max_load_tenths / 10u),
        (unsigned long)window_audio_xruns,
        (unsigned long)perf_snapshot.voice_steal_count,
        (unsigned long)perf_snapshot.output_clip_count);
  }
#else
  (void)counters;
  (void)stereo_frames_per_buffer;
#endif
}

static bool midi_handle_message(MidiMessage msg, uint8_t *volume,
                                uint8_t *voice_level, UiState *ui,
                                MidiRuntime *runtime, bool *state_changed) {
  uint16_t bank_index;

  switch (midi_type(msg)) {
  case MIDI_NOTEON:
    if (msg.note.velocity == 0u) {
      audio_render_core_note_off(msg.note.note);
      return true;
    }
    audio_render_core_note_on(msg.note.note, msg.note.velocity);
    return true;
  case MIDI_NOTEOFF:
    audio_render_core_note_off(msg.note.note);
    return true;
  case MIDI_CONTROLCHANGE:
    switch (msg.cc.control) {
    case 0:
      if (runtime == NULL) {
        return false;
      }
      runtime->bank_select_msb = msg.cc.value & 0x7Fu;
      bank_index = (uint16_t)(((uint16_t)runtime->bank_select_msb << 7) |
                              runtime->bank_select_lsb);
      if (select_rom_by_index(ui, bank_index)) {
        printf("ROM: MIDI bank %u\n", (unsigned int)bank_index);
        if (state_changed != NULL) {
          *state_changed = true;
        }
        return true;
      }
      return false;
    case 32:
      if (runtime == NULL) {
        return false;
      }
      runtime->bank_select_lsb = msg.cc.value & 0x7Fu;
      bank_index = (uint16_t)(((uint16_t)runtime->bank_select_msb << 7) |
                              runtime->bank_select_lsb);
      if (select_rom_by_index(ui, bank_index)) {
        printf("ROM: MIDI bank %u\n", (unsigned int)bank_index);
        if (state_changed != NULL) {
          *state_changed = true;
        }
        return true;
      }
      return false;
    case 7:
      *volume = (uint8_t)((uint16_t)msg.cc.value * 255u / 127u);
      audio_render_core_set_master_level(*volume);
      if (state_changed != NULL) {
        *state_changed = true;
      }
      printf("master = %u, voice = %u      \r", *volume, *voice_level);
      return true;
    case 11:
      *voice_level = (uint8_t)((uint16_t)msg.cc.value * 255u / 127u);
      audio_render_core_set_voice_level(*voice_level);
      if (state_changed != NULL) {
        *state_changed = true;
      }
      printf("master = %u, voice = %u      \r", *volume, *voice_level);
      return true;
    case 120:
    case 123:
      audio_render_core_all_notes_off();
      clear_note_activity(ui);
      if (state_changed != NULL) {
        *state_changed = true;
      }
      return true;
    default:
      return false;
    }
  case MIDI_PROGRAMCHANGE:
    if (select_patch_for_program_change(msg.program.program)) {
      clear_note_activity(ui);
      if (state_changed != NULL) {
        *state_changed = true;
      }
      return true;
    }
    return false;
  case MIDI_STOP:
  case MIDI_RESET:
    audio_render_core_all_notes_off();
    clear_note_activity(ui);
    if (state_changed != NULL) {
      *state_changed = true;
    }
    return true;
  default:
    return false;
  }
}

static void service_midi(MidiRuntime *runtime, uint8_t *volume,
                         uint8_t *voice_level, UiState *ui) {
  MidiMessage msg;
  bool ready_now;
  bool state_changed = false;

  if (runtime == NULL || volume == NULL || voice_level == NULL || ui == NULL) {
    return;
  }

  midi_service();
  ready_now = midi_ready();
  ui->usb_midi_ready = ready_now;
  if (ready_now && !runtime->was_ready) {
#if USB_MIDI_HOST
    printf("USB MIDI host ready\n");
#else
    printf("USB MIDI mounted\n");
#endif
    audio_render_core_all_notes_off();
    clear_note_activity(ui);
    state_changed = true;
  } else if (!ready_now && runtime->was_ready) {
#if USB_MIDI_HOST
    printf("USB MIDI host unmounted\n");
#else
    printf("USB MIDI unmounted\n");
#endif
    audio_render_core_all_notes_off();
    clear_note_activity(ui);
    state_changed = true;
  }
  runtime->was_ready = ready_now;

  while (midi_receive(&msg)) {
    (void)midi_handle_message(msg, volume, voice_level, ui, runtime,
                              &state_changed);
  }

  if (state_changed) {
    refresh_ui(ui, *volume, *voice_level, false);
  }
}

static bool poll_controls(UiState *ui, uint8_t *volume, uint8_t *voice_level,
                          uint16_t *prev_keys) {
  bool controls_changed = false;
  bool rom_combo_used = false;
  size_t patch_count;
  const uint8_t previous_volume = volume == NULL ? 0u : *volume;
  const uint8_t previous_voice_level =
      voice_level == NULL ? 0u : *voice_level;
  uint16_t keys = platform_input_scan_keys();
  uint16_t pressed = keys & (uint16_t)~(*prev_keys);
  uint16_t released = (uint16_t)(*prev_keys) & (uint16_t)~keys;

  /* Debug: print key states when ALT or arrow keys are involved */
  // if ((keys & KEY_ALT_BIT) || (pressed & (KEY_LEFT_BIT | KEY_RIGHT_BIT | KEY_UP_BIT | KEY_DOWN_BIT))) {
  //   printf("KEYS=0x%04X PRES=0x%04X REL=0x%04X\r", (unsigned)keys, (unsigned)pressed, (unsigned)released);
  // }

  if (ui == NULL || volume == NULL || voice_level == NULL || prev_keys == NULL) {
    return false;
  }

  if (pressed & KEY_UP_BIT) {
    if (g_settings.cursor > 0) g_settings.cursor--;
    else g_settings.cursor = FIELD_COUNT - 1;
    controls_changed = true;
  }
  if (pressed & KEY_DOWN_BIT) {
    if (g_settings.cursor < FIELD_COUNT - 1) g_settings.cursor++;
    else g_settings.cursor = 0;
    controls_changed = true;
  }

  int delta = 0;
  if (pressed & KEY_LEFT_BIT) delta = (keys & KEY_EDIT_BIT) ? -16 : -1;
  if (pressed & KEY_RIGHT_BIT) delta = (keys & KEY_EDIT_BIT) ? 16 : 1;

  if (delta != 0) {
    switch (g_settings.cursor) {
    case FIELD_PATCH: {
      const size_t patch_count = dx7_rom_bank_patch_count();
      if (delta > 0 && g_current_patch_index < (patch_count - 1u)) {
        select_patch(g_current_patch_index + 1u);
      } else if (delta < 0 && g_current_patch_index > 0u) {
        select_patch(g_current_patch_index - 1u);
      }
      clear_note_activity(ui);
    } break;
    case FIELD_BANK:
      (void)cycle_rom(ui, delta > 0 ? 1 : -1);
      break;
    case FIELD_MASTER_LEVEL:
      adjust_level(volume, delta);
      audio_render_core_set_master_level(*volume);
      break;
    case FIELD_VOICE_LEVEL:
      adjust_level(voice_level, delta);
      audio_render_core_set_voice_level(*voice_level);
      break;
    case FIELD_REVERB_WET:
      adjust_level(&g_settings.reverb_wet, delta);
      audio_render_core_set_reverb_wet(g_settings.reverb_wet);
      break;
    case FIELD_REVERB_FEEDBACK:
      adjust_level(&g_settings.reverb_feedback, delta);
      audio_render_core_set_reverb_feedback(g_settings.reverb_feedback);
      break;
    case FIELD_REVERB_DAMPING:
      adjust_level(&g_settings.reverb_damping, delta);
      audio_render_core_set_reverb_damping(g_settings.reverb_damping);
      break;
    case FIELD_REVERB_INPUT_GAIN:
      if (delta > 0 && g_settings.reverb_input_gain < 4)
        g_settings.reverb_input_gain++;
      else if (delta < 0 && g_settings.reverb_input_gain > 0)
        g_settings.reverb_input_gain--;
      audio_render_core_set_reverb_input_gain(g_settings.reverb_input_gain);
      break;
    case FIELD_OUTPUT_LEVEL:
      g_settings.level = (g_settings.level == AUDIO_OUTPUT_LEVEL_HEADPHONES)
                             ? AUDIO_OUTPUT_LEVEL_LINE
                             : AUDIO_OUTPUT_LEVEL_HEADPHONES;
      audio_render_core_set_output_level(g_settings.level);
      break;
    default:
      break;
    }
    controls_changed = true;
  }

  if (pressed & KEY_ENTER_BIT) {
    audio_render_core_panic();
    clear_note_activity(ui);
    printf("panic\n");
    controls_changed = true;
  }

  if (pressed & KEY_PLAY_BIT) {
    controls_changed |= preview_note_start(ui);
  }

  if (released & KEY_PLAY_BIT) {
    controls_changed |= preview_note_stop(ui);
  }

  *prev_keys = keys;

  int c = getchar_timeout_us(0);
  if (c >= 0) {
    patch_count = dx7_rom_bank_patch_count();
    if (c == 'q')
      return false;
    // Reverb: 'r' = off, 't' = full, '+' increase, '-' decrease reverb
    if (c == 'r') {
      audio_render_core_set_reverb_wet(0);
      g_settings.reverb_wet = 0;
      printf("Reverb: off");
      controls_changed = true;
    }
    if (c == 't') {
      audio_render_core_set_reverb_wet(255);
      g_settings.reverb_wet = 255;
      printf("Reverb: full");
      controls_changed = true;
    }
    if (c == 'l') {
      AudioOutputLevel new_level =
          (g_settings.level == AUDIO_OUTPUT_LEVEL_HEADPHONES)
              ? AUDIO_OUTPUT_LEVEL_LINE
              : AUDIO_OUTPUT_LEVEL_HEADPHONES;
      audio_render_core_set_output_level(new_level);
      g_settings.level = new_level;
      printf("Output: %s\n",
             new_level == AUDIO_OUTPUT_LEVEL_LINE ? "line-level" : "headphones");
      controls_changed = true;
    }
    if (c == '+') {
      controls_changed |= adjust_level(voice_level, (int)PREVIEW_NOTE_STEP * 4);
    }
    if (c == '-') {
      controls_changed |= adjust_level(voice_level, -(int)PREVIEW_NOTE_STEP * 4);
    }
    // Patch switching with 'o' and 'p' keys
    if (c == 'p' && patch_count > 0u && g_current_patch_index < patch_count - 1u &&
        select_patch(g_current_patch_index + 1u)) {
      clear_note_activity(ui);
      controls_changed = true;
    }
    if (c == 'o' && g_current_patch_index > 0u &&
        select_patch(g_current_patch_index - 1u)) {
      clear_note_activity(ui);
      controls_changed = true;
    }

    controls_changed = true;
  }

  if (*volume != previous_volume) {
    audio_render_core_set_master_level(*volume);
  }

  if (*voice_level != previous_voice_level) {
    audio_render_core_set_voice_level(*voice_level);
  }

  if (controls_changed) {
    refresh_ui(ui, *volume, *voice_level, false);
    printf("master = %u, voice = %u      \r", *volume, *voice_level);
  }

  return true;
}

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
bi_decl(bi_3pins_with_names(AUDIO_SDATA, "I2S DIN", AUDIO_BCLK, "I2S BCK",
                            AUDIO_BCLK + 1, "I2S LRCK"));
#endif

int main(void) {
  stdio_init_all();

  platform_init_system_clocks();
  stdio_init_all();

  display_startup_init();
  dx7_rom_bank_init();
  audio_output_init();
  audio_render_core_init();
  platform_input_init_buttons();
  boot_log_sdcard_status();

  printf("I2S DATA:%d BCLK:%d LRCLK:%d\n", AUDIO_SDATA, AUDIO_BCLK,
         AUDIO_BCLK + 1);
  printf("BTN UP:%d DOWN:%d\n", INPUT_UP, INPUT_DOWN);
  printf("clk_sys=%lu clk_peri=%lu sample_rate=%u\n",
         (unsigned long)clock_get_hz(clk_sys),
         (unsigned long)clock_get_hz(clk_peri), AUDIO_OUTPUT_SAMPLE_RATE_HZ);
  printf("MIDI transports: USB device + UART GPIO%d/%d\n", MIDI_OUT_PIN,
         MIDI_IN_PIN);
  printf("Use +/- or up/down for level\n");

  const size_t buffer_samples = audio_output_buffer_samples();
  uint8_t volume = AUDIO_RENDER_DEFAULT_MASTER_LEVEL;
  uint8_t voice_level = AUDIO_RENDER_DEFAULT_VOICE_LEVEL;
  uint16_t prev_keys = 0;
  RenderPerfCounters perf_counters = {0};
  MidiRuntime midi_runtime = {0};
  UiState ui = {
      .preview_note = 60u,
      .preview_note_playing = 60u,
      .preview_velocity = PREVIEW_VELOCITY,
      .battery_percentage = 0u,
      .preview_note_active = false,
      .usb_midi_ready = false,
      .battery_charging = false,
  };
  BatteryMonitor battery_monitor;

  battery_monitor_init(&battery_monitor);
  battery_monitor_sample(&battery_monitor);
  battery_monitor.last_sample_us = time_us_32();
  ui.battery_percentage = battery_monitor.percentage;
  ui.battery_charging = battery_monitor.charging;
  midi_init();
  ui.usb_midi_ready = midi_ready();
  boot_autoload_first_valid_rom(&ui);
  refresh_ui(&ui, volume, voice_level, true);

  printf("USB MIDI: TinyUSB service in main loop\n");
  printf("Buttons: L/R patch U/D note ALT/EDIT octave NAV+U/D ROM PLAY gate ENTER panic\n");
  printf("Levels: ALT+LEFT/RIGHT=voice, ALT+UP/DOWN=master\n");printf("Serial: r=reverb off, t=full, l=toggle output level (HP/line), +=increase voice, -=decrease voice\n");

  while (true) {
    uint32_t now_us;

    if (!poll_controls(&ui, &volume, &voice_level, &prev_keys))
      break;

    service_midi(&midi_runtime, &volume, &voice_level, &ui);
    now_us = time_us_32();
    if (battery_monitor_refresh_if_due(&battery_monitor, &ui, now_us)) {
      refresh_ui(&ui, volume, voice_level, false);
    }

    render_perf_publish_if_due(&perf_counters, (uint32_t)(buffer_samples / 2u));
    tight_loop_contents();
  }

  (void)preview_note_stop(&ui);
  audio_render_core_all_notes_off();
  audio_render_core_shutdown();
  puts("\n");
  return 0;
}

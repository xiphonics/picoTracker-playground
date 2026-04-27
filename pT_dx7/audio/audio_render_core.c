#include "audio_render_core.h"

#include <stdbool.h>
#include <string.h>

#include "audio_output.h"
#include "reverb.h"
#include "pico/critical_section.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#include "pico/time.h"
#include "pico/util/queue.h"

#define AUDIO_RENDER_COMMAND_QUEUE_LENGTH 64u

typedef enum {
  AUDIO_COMMAND_NOTE_ON = 0,
  AUDIO_COMMAND_NOTE_OFF,
  AUDIO_COMMAND_SET_PATCH,
  AUDIO_COMMAND_SET_MASTER_LEVEL,
  AUDIO_COMMAND_SET_VOICE_LEVEL,
  AUDIO_COMMAND_SET_REVERB_WET,
  AUDIO_COMMAND_SET_REVERB_FEEDBACK,
  AUDIO_COMMAND_SET_REVERB_DAMPING,
  AUDIO_COMMAND_SET_REVERB_INPUT_GAIN,
  AUDIO_COMMAND_SET_OUTPUT_LEVEL,
  AUDIO_COMMAND_ALL_NOTES_OFF,
  AUDIO_COMMAND_PANIC,
  AUDIO_COMMAND_SHUTDOWN,
} AudioCommandType;

typedef struct {
  uint8_t type;
  uint8_t arg0;
  uint8_t arg1;
} AudioCommand;

typedef struct {
  uint32_t window_start_us;
  uint32_t total_render_us;
  uint32_t max_render_us;
  uint32_t buffers_rendered;
  uint32_t last_voice_steals;
  uint32_t last_same_note_retriggers;
  uint32_t last_output_clips;
  uint32_t last_reverb_render_us;
} AudioRenderPerfState;

enum {
  AUDIO_RENDER_PERF_UPDATE_PERIOD_US = 5000000u,
};

static queue_t g_audio_command_queue;
static semaphore_t g_render_semaphore;
static critical_section_t g_perf_snapshot_lock;
static volatile bool g_audio_render_core_running;
static AudioRenderPerfSnapshot g_perf_snapshot;

static void audio_render_core_publish_perf_snapshot(
    const AudioRenderPerfState *perf_state, const FmSynthStats *synth_stats) {
  AudioRenderPerfSnapshot snapshot;

  if (perf_state == NULL || synth_stats == NULL) {
    return;
  }

  snapshot.sequence = g_perf_snapshot.sequence + 1u;
  snapshot.average_render_us =
      perf_state->buffers_rendered == 0u
          ? 0u
          : perf_state->total_render_us / perf_state->buffers_rendered;
  snapshot.max_render_us = perf_state->max_render_us;
  snapshot.buffers_rendered = perf_state->buffers_rendered;
  snapshot.average_reverb_us =
      perf_state->buffers_rendered == 0u
          ? 0u
          : (synth_stats->reverb_render_us -
             perf_state->last_reverb_render_us) /
                perf_state->buffers_rendered;
  snapshot.voice_steal_count =
      synth_stats->voice_steal_count - perf_state->last_voice_steals;
  snapshot.same_note_retrigger_count =
      synth_stats->same_note_retrigger_count -
      perf_state->last_same_note_retriggers;
  snapshot.output_clip_count =
      synth_stats->output_clip_count - perf_state->last_output_clips;
  snapshot.active_voice_count = synth_stats->active_voice_count;
  snapshot.peak_active_voice_count = synth_stats->peak_active_voice_count;

  critical_section_enter_blocking(&g_perf_snapshot_lock);
  snapshot.sequence = g_perf_snapshot.sequence + 1u;
  g_perf_snapshot = snapshot;
  critical_section_exit(&g_perf_snapshot_lock);
}

static void audio_render_core_reset_perf_state(AudioRenderPerfState *perf_state,
                                               uint32_t now_us,
                                               const FmSynthStats *synth_stats) {
  if (perf_state == NULL || synth_stats == NULL) {
    return;
  }

  perf_state->window_start_us = now_us;
  perf_state->total_render_us = 0u;
  perf_state->max_render_us = 0u;
  perf_state->buffers_rendered = 0u;
  perf_state->last_voice_steals = synth_stats->voice_steal_count;
  perf_state->last_same_note_retriggers = synth_stats->same_note_retrigger_count;
  perf_state->last_output_clips = synth_stats->output_clip_count;
  perf_state->last_reverb_render_us = synth_stats->reverb_render_us;
}

static void audio_render_core_process_command(const AudioCommand *command) {
  if (command == NULL) {
    return;
  }

  switch ((AudioCommandType)command->type) {
  case AUDIO_COMMAND_NOTE_ON:
    fm_synth_note_on(command->arg0, command->arg1);
    break;
  case AUDIO_COMMAND_NOTE_OFF:
    fm_synth_note_off(command->arg0);
    break;
  case AUDIO_COMMAND_SET_PATCH:
    fm_synth_set_patch((FmSynthPatchId)command->arg0);
    break;
  case AUDIO_COMMAND_SET_MASTER_LEVEL:
    fm_synth_set_master_level(command->arg0);
    break;
  case AUDIO_COMMAND_SET_VOICE_LEVEL:
    fm_synth_set_voice_level(command->arg0);
    break;
  case AUDIO_COMMAND_SET_REVERB_WET:
    fm_synth_set_reverb_wet(command->arg0);
    break;
  case AUDIO_COMMAND_SET_REVERB_FEEDBACK: {
    // Cap feedback at 0.85 to prevent runaway oscillation
    uint16_t q15 = ((uint32_t)command->arg0 * 27851u) / 255u;
    reverb_set_feedback_amount((int16_t)q15);
  } break;
  case AUDIO_COMMAND_SET_REVERB_DAMPING: {
    uint16_t q15 = ((uint32_t)command->arg0 * 32767u) / 255u;
    reverb_set_damp_amount((int16_t)q15);
  } break;
  case AUDIO_COMMAND_SET_REVERB_INPUT_GAIN:
    reverb_set_input_shift(command->arg0);
    break;
  case AUDIO_COMMAND_SET_OUTPUT_LEVEL:
    audio_output_set_level((AudioOutputLevel)command->arg0);
    break;
  case AUDIO_COMMAND_ALL_NOTES_OFF:
    fm_synth_all_notes_off();
    break;
  case AUDIO_COMMAND_PANIC:
    fm_synth_panic();
    break;
  case AUDIO_COMMAND_SHUTDOWN:
    g_audio_render_core_running = false;
    break;
  default:
    break;
  }
}

static void audio_render_core_drain_commands(void) {
  AudioCommand command;

  while (queue_try_remove(&g_audio_command_queue, &command)) {
    audio_render_core_process_command(&command);
    if (!g_audio_render_core_running) {
      break;
    }
  }
}

static void audio_render_core_entry(void) {
  AudioRenderPerfState perf_state = {0};
  FmSynthStats synth_stats = {0};
  const size_t buffer_samples = audio_output_buffer_samples();

  multicore_lockout_victim_init();
  fm_synth_init();
  fm_synth_get_stats(&synth_stats);
  audio_render_core_reset_perf_state(&perf_state, time_us_32(), &synth_stats);

  while (g_audio_render_core_running) {
    int16_t *buffer = NULL;
    uint32_t render_start_us;
    uint32_t render_time_us;
    uint32_t now_us;

    sem_acquire_blocking(&g_render_semaphore);
    if (!g_audio_render_core_running) {
      break;
    }

    audio_render_core_drain_commands();
    if (!g_audio_render_core_running) {
      break;
    }

    while (g_audio_render_core_running &&
           !audio_output_try_acquire_buffer(&buffer)) {
      tight_loop_contents();
    }
    if (!g_audio_render_core_running) {
      break;
    }

    render_start_us = time_us_32();
    // buffer_samples is the total interleaved sample count (L+R); the render
    // function expects the number of stereo frames, so divide by 2.
    fm_synth_render_block(buffer, buffer_samples / 2u);
    render_time_us = time_us_32() - render_start_us;
    audio_output_submit_buffer(buffer);

    perf_state.total_render_us += render_time_us;
    if (render_time_us > perf_state.max_render_us) {
      perf_state.max_render_us = render_time_us;
    }
    ++perf_state.buffers_rendered;

    now_us = time_us_32();
    if ((now_us - perf_state.window_start_us) >=
        AUDIO_RENDER_PERF_UPDATE_PERIOD_US) {
      fm_synth_get_stats(&synth_stats);
      audio_render_core_publish_perf_snapshot(&perf_state, &synth_stats);
      audio_render_core_reset_perf_state(&perf_state, now_us, &synth_stats);
    }
  }
}

static void audio_render_core_enqueue_command(AudioCommandType type, uint8_t arg0,
                                              uint8_t arg1) {
  const AudioCommand command = {
      .type = (uint8_t)type,
      .arg0 = arg0,
      .arg1 = arg1,
  };

  while (!queue_try_add(&g_audio_command_queue, &command)) {
    tight_loop_contents();
  }
}

void audio_render_core_init(void) {
  const size_t prefill_count = audio_output_buffer_count() > 0u
                                   ? audio_output_buffer_count() - 1u
                                   : 0u;

  queue_init(&g_audio_command_queue, sizeof(AudioCommand),
             AUDIO_RENDER_COMMAND_QUEUE_LENGTH);
  sem_init(&g_render_semaphore, 0, 32767);
  critical_section_init(&g_perf_snapshot_lock);
  memset(&g_perf_snapshot, 0, sizeof(g_perf_snapshot));

  g_audio_render_core_running = true;
  audio_output_set_render_request_callback(audio_render_core_request_render);
  multicore_reset_core1();
  multicore_launch_core1(audio_render_core_entry);

  for (size_t i = 0; i < prefill_count; ++i) {
    audio_render_core_request_render();
  }
}

void audio_render_core_shutdown(void) {
  if (!g_audio_render_core_running) {
    return;
  }

  audio_render_core_enqueue_command(AUDIO_COMMAND_SHUTDOWN, 0u, 0u);
  audio_output_set_render_request_callback(NULL);
  sem_release(&g_render_semaphore);
  multicore_reset_core1();
  g_audio_render_core_running = false;
}

void audio_render_core_request_render(void) { sem_release(&g_render_semaphore); }

void audio_render_core_note_on(uint8_t note, uint8_t velocity) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_NOTE_ON, note, velocity);
}

void audio_render_core_note_off(uint8_t note) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_NOTE_OFF, note, 0u);
}

void audio_render_core_set_patch(FmSynthPatchId patch_id) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_SET_PATCH, patch_id, 0u);
}

void audio_render_core_set_master_level(uint8_t level) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_SET_MASTER_LEVEL, level, 0u);
}

void audio_render_core_set_voice_level(uint8_t level) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_SET_VOICE_LEVEL, level, 0u);
}

void audio_render_core_set_reverb_wet(uint8_t wet) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_SET_REVERB_WET, wet, 0u);
}

void audio_render_core_set_reverb_feedback(uint8_t q8) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_SET_REVERB_FEEDBACK, q8, 0u);
}

void audio_render_core_set_reverb_damping(uint8_t q8) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_SET_REVERB_DAMPING, q8, 0u);
}

void audio_render_core_set_reverb_input_gain(uint8_t level) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_SET_REVERB_INPUT_GAIN, level, 0u);
}

void audio_render_core_set_output_level(AudioOutputLevel level) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_SET_OUTPUT_LEVEL, (uint8_t)level, 0u);
}

void audio_render_core_all_notes_off(void) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_ALL_NOTES_OFF, 0u, 0u);
}

void audio_render_core_panic(void) {
  audio_render_core_enqueue_command(AUDIO_COMMAND_PANIC, 0u, 0u);
}

bool audio_render_core_get_perf_snapshot(AudioRenderPerfSnapshot *snapshot) {
  if (snapshot == NULL) {
    return false;
  }

  critical_section_enter_blocking(&g_perf_snapshot_lock);
  *snapshot = g_perf_snapshot;
  critical_section_exit(&g_perf_snapshot_lock);

  return snapshot->sequence != 0u;
}

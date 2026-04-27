#ifndef DX_2350_AUDIO_RENDER_CORE_H
#define DX_2350_AUDIO_RENDER_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "fm_synth.h"
#include "audio_output.h"

enum {
  AUDIO_RENDER_DEFAULT_MASTER_LEVEL = 255u,
  AUDIO_RENDER_DEFAULT_VOICE_LEVEL = 48u,
};

typedef struct {
  uint32_t sequence;
  uint32_t average_render_us;
  uint32_t max_render_us;
  uint32_t average_reverb_us;
  uint32_t buffers_rendered;
  uint32_t voice_steal_count;
  uint32_t same_note_retrigger_count;
  uint32_t output_clip_count;
  uint8_t active_voice_count;
  uint8_t peak_active_voice_count;
} AudioRenderPerfSnapshot;

void audio_render_core_init(void);
void audio_render_core_shutdown(void);
void audio_render_core_request_render(void);
void audio_render_core_note_on(uint8_t note, uint8_t velocity);
void audio_render_core_note_off(uint8_t note);
void audio_render_core_set_patch(FmSynthPatchId patch_id);
void audio_render_core_set_master_level(uint8_t level);
void audio_render_core_set_voice_level(uint8_t level);
void audio_render_core_set_reverb_wet(uint8_t wet);
void audio_render_core_set_reverb_feedback(uint8_t q8);  // 0..255
void audio_render_core_set_reverb_damping(uint8_t q8);
void audio_render_core_set_reverb_input_gain(uint8_t level);  // 0..3
void audio_render_core_set_output_level(AudioOutputLevel level);
void audio_render_core_all_notes_off(void);
void audio_render_core_panic(void);
bool audio_render_core_get_perf_snapshot(AudioRenderPerfSnapshot *snapshot);

#endif

#ifndef DX_2350_FM_SYNTH_H
#define DX_2350_FM_SYNTH_H

#include <stddef.h>
#include <stdint.h>

enum {
  FM_SYNTH_VOICE_COUNT = 6,
  FM_SYNTH_OPERATOR_COUNT = 6,
};

typedef uint8_t FmSynthPatchId;

typedef struct {
  uint32_t voice_steal_count;
  uint32_t same_note_retrigger_count;
  uint32_t output_clip_count;
  uint32_t reverb_render_us;  // cumulative microseconds spent in reverb
  uint8_t active_voice_count;
  uint8_t peak_active_voice_count;
} FmSynthStats;

void fm_synth_init(void);
void fm_synth_set_master_level(uint8_t level);
uint8_t fm_synth_master_level(void);
void fm_synth_set_voice_level(uint8_t level);
uint8_t fm_synth_voice_level(void);
void fm_synth_set_reverb_wet(uint8_t wet);  // 0-255, dry to fully wet
uint8_t fm_synth_reverb_wet(void);
void fm_synth_set_patch(FmSynthPatchId patch_id);
void fm_synth_note_on(uint8_t note, uint8_t velocity);
void fm_synth_note_off(uint8_t note);
void fm_synth_all_notes_off(void);
void fm_synth_panic(void);
void fm_synth_render_block(int16_t *buffer, size_t samples);
void fm_synth_get_stats(FmSynthStats *stats);

#endif

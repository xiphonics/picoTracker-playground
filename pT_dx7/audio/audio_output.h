#ifndef DX_2350_AUDIO_OUTPUT_H
#define DX_2350_AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_OUTPUT_SAMPLE_RATE_HZ 44100u

// Audio output now handles interleaved stereo (LRLR format)
// Each sample is 16-bit, so a stereo frame takes 2 samples
#define AUDIO_OUTPUT_STEREO_FRAME_COUNT 128u
#define AUDIO_OUTPUT_BUFFER_SAMPLES (AUDIO_OUTPUT_STEREO_FRAME_COUNT * 2u)

typedef struct {
  uint32_t dma_starvation_count;
} AudioOutputStats;

// Output amplitude level. Implemented by patching the I2S PIO program's
// SET Y immediates to change MSB sign-extension padding (OFFSET_COUNT) and
// LSB zero-padding (BACKFILL_COUNT). More OFFSET = quieter.
typedef enum {
  AUDIO_OUTPUT_LEVEL_HEADPHONES = 0,  // OFFSET=3, BACKFILL=10 — comfortable for headphones
  AUDIO_OUTPUT_LEVEL_LINE = 1,        // OFFSET=1, BACKFILL=12 — full line-level
} AudioOutputLevel;

typedef void (*AudioOutputRenderRequestCallback)(void);

void audio_output_init(void);
size_t audio_output_buffer_count(void);
size_t audio_output_buffer_samples(void);
bool audio_output_try_acquire_buffer(int16_t **buffer);
void audio_output_submit_buffer(int16_t *buffer);
void audio_output_get_stats(AudioOutputStats *stats);
void audio_output_set_render_request_callback(
    AudioOutputRenderRequestCallback callback);
void audio_output_set_level(AudioOutputLevel level);
AudioOutputLevel audio_output_get_level(void);

#endif

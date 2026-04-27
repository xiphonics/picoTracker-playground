#include "sine_wave_table.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t sine_wave_table[SINE_WAVE_TABLE_LEN];

void sine_wave_table_init(void) {
  for (uint32_t i = 0; i < SINE_WAVE_TABLE_LEN; ++i) {
    const float angle = (float)i * 2.0f * (float)(M_PI / SINE_WAVE_TABLE_LEN);
    sine_wave_table[i] = (int16_t)(32767.0f * cosf(angle));
  }
}

int16_t sine_wave_table_sample(uint32_t phase_q16, uint32_t volume) {
  const uint32_t index = phase_q16 >> 16u;
  const int32_t scaled = (int32_t)volume * (int32_t)sine_wave_table[index];
  return (int16_t)(scaled >> 8u);
}

uint32_t sine_wave_table_phase_limit(void) { return 0x10000u * SINE_WAVE_TABLE_LEN; }

uint32_t sine_wave_table_min_step(void) { return 0x10000u; }

uint32_t sine_wave_table_max_step(void) {
  return (SINE_WAVE_TABLE_LEN / 16u) * 0x20000u;
}

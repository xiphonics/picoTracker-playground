#ifndef RP_SINE_WAVE_SINE_WAVE_TABLE_H
#define RP_SINE_WAVE_SINE_WAVE_TABLE_H

#include <stdint.h>

enum { SINE_WAVE_TABLE_LEN = 2048 };

void sine_wave_table_init(void);
int16_t sine_wave_table_sample(uint32_t phase_q16, uint32_t volume);
uint32_t sine_wave_table_phase_limit(void);
uint32_t sine_wave_table_min_step(void);
uint32_t sine_wave_table_max_step(void);

#endif

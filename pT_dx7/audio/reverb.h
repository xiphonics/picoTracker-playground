// Reverb implementation based on Steve Yeadon's reverberation algorithm
// Uses allpass filters and delay lines for stereo reverb effect

#ifndef REVERB_REF_H
#define REVERB_REF_H

#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/critical_section.h"

// Q15 helper functions for fixed-point arithmetic.
// add/sub saturate so the reverb's allpass + feedback network can't wrap
// the int16 accumulators on sustained / stacked-note input.
static inline int16_t reverb_add_q15(int16_t a, int16_t b) {
    int32_t s = (int32_t)a + (int32_t)b;
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

static inline int16_t reverb_sub_q15(int16_t a, int16_t b) {
    int32_t s = (int32_t)a - (int32_t)b;
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

static inline int16_t reverb_mult_q15(int16_t a, int16_t b) {
    // Q15 multiplication: result is in Q15
    return (int16_t)(((int32_t)a * (int32_t)b) >> 15);
}

#define REVERB_Q15_MAX 32767

// Simple interpolation function
static inline int16_t reverb_interpolate824(const int16_t* table, uint32_t phase) {
    int32_t a = table[phase >> 24];
    int32_t b = table[(phase >> 24) + 1];
    int32_t frac = (phase >> 8) & 0xffff;
    return (int16_t)(a + ((b - a) * frac >> 16));
}

// Mix function for crossfading between two values
static inline int16_t reverb_mix(int16_t a, int16_t b, uint16_t balance) {
    return (int16_t)((a * (65535 - balance) + b * balance) >> 16);
}

#define VERB_LENGTH 8203
#define VERB_AP1 113
#define VERB_AP2 162
#define VERB_AP3 241
#define VERB_AP4 373
#define VERB_AP5 615
#define VERB_AP6 773
#define VERB_D1 915
#define VERB_AP7 513
#define VERB_AP8 849
#define VERB_D2 1515

typedef struct {
    int16_t *buf;
    int count;
    int length;
    int16_t c;
    uint32_t phase;
    uint16_t phaseOffset;
} ReverbAllPass;

typedef struct {
    ReverbAllPass allpass[10];
    int16_t buf[VERB_LENGTH];
    int16_t feedback;
    uint8_t count;
    int length;
    int16_t position[4];
    int16_t damp[2];
    int16_t dampAmount;
    int16_t invDampAmount;
    int16_t feedbackAmount;
    int16_t last_l;
    int16_t last_r;
} Reverb2;

// Initialize the reverb (should be called once at startup)
void Reverb2_Init(Reverb2 *rv);

// Process mono input to stereo output
void Reverb2_Process(Reverb2 *rv, int16_t in, int16_t *l, int16_t *r);

// === Public API for integration ===
// Initialize the reverb (should be called once at startup)
void reverb_init(void);

// Get the internal reverb state pointer
void reverb_get_state(Reverb2 **out_state);

// Runtime parameter setters
void reverb_set_feedback_amount(int16_t q15);   // 0..0x7FFF
void reverb_set_damp_amount(int16_t q15);
void reverb_set_input_shift(uint8_t shift);     // 0..3, applied in reverb_process_stereo
bool reverb_is_enabled(void);

// Q15 constants for wet/dry mixing
#define REVERB_Q15_ONE      0x7FFF
#define REVERB_Q15_NEG_ONE  0x8000

// Process stereo audio through reverb with wet/dry mixing
// left_samples and right_samples contain the dry input
// output_left and output_right receive the mixed wet/dry output
// num_samples is the number of stereo samples to process
// wet_amount is from 0 (dry) to REVERB_Q15_ONE (fully wet)
void reverb_process_stereo(int16_t *left_samples, int16_t *right_samples,
                          int16_t *output_left, int16_t *output_right,
                          int num_samples, int16_t wet_amount);

#endif /* REVERB_REF_H */

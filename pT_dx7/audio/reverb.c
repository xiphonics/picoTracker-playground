#include "reverb.h"
#include <string.h>

#define REVERB_BUF_MASK 32767
static int16_t s_delay_buf[32768];
static uint16_t s_write_ptr = 0;

static int16_t s_lp_1 = 0;
static int16_t s_lp_2 = 0;

static uint32_t s_lfo1_phase = 0;
static uint32_t s_lfo2_phase = 0;

static int16_t s_krt = 20000;
static int16_t s_klp = 25000;
static int16_t s_kap = 20480; // 0.625 * 32768

static uint8_t s_reverb_input_shift = 1;
static bool s_reverb_enabled = true;

#define LEN_AP1 150
#define LEN_AP2 214
#define LEN_AP3 319
#define LEN_AP4 527
#define LEN_DAP1A 2182
#define LEN_DAP1B 2690
#define LEN_DEL1 4501
#define LEN_DAP2A 2525
#define LEN_DAP2B 2197
#define LEN_DEL2 6312

#define BASE_AP1 0
#define BASE_AP2 (BASE_AP1 + LEN_AP1 + 1)
#define BASE_AP3 (BASE_AP2 + LEN_AP2 + 1)
#define BASE_AP4 (BASE_AP3 + LEN_AP3 + 1)
#define BASE_DAP1A (BASE_AP4 + LEN_AP4 + 1)
#define BASE_DAP1B (BASE_DAP1A + LEN_DAP1A + 1)
#define BASE_DEL1 (BASE_DAP1B + LEN_DAP1B + 1)
#define BASE_DAP2A (BASE_DEL1 + LEN_DEL1 + 1)
#define BASE_DAP2B (BASE_DAP2A + LEN_DAP2A + 1)
#define BASE_DEL2 (BASE_DAP2B + LEN_DAP2B + 1)

void Reverb2_Init(Reverb2 *rv) {
    memset(s_delay_buf, 0, sizeof(s_delay_buf));
    s_write_ptr = 0;
    s_lp_1 = 0;
    s_lp_2 = 0;
    s_lfo1_phase = 0;
    s_lfo2_phase = 0;
    (void)rv;
}

void reverb_init(void) {
    Reverb2_Init(NULL);
}

void reverb_get_state(Reverb2 **out_state) {
    if (out_state != NULL) {
        *out_state = NULL;
    }
}

void reverb_set_feedback_amount(int16_t q15) {
    s_krt = q15;
}

void reverb_set_damp_amount(int16_t q15) {
    // Invert so higher value = more damping (lower cutoff)
    s_klp = 32767 - q15;
}

void reverb_set_input_shift(uint8_t shift) {
    if (shift == 0) {
        s_reverb_enabled = false;
    } else {
        s_reverb_enabled = true;
        s_reverb_input_shift = shift - 1;
        if (s_reverb_input_shift > 3) s_reverb_input_shift = 3;
    }
}

bool reverb_is_enabled(void) {
    return s_reverb_enabled;
}

static __not_in_flash_func(int16_t) triangle_lfo(uint32_t phase) {
    uint16_t p = phase >> 16;
    if (p < 32768) {
        return (int16_t)((p << 1) - 32768);
    } else {
        return (int16_t)(32767 - ((p - 32768) << 1));
    }
}

static __not_in_flash_func(int16_t) read_delay(uint16_t base, uint16_t offset) {
    return s_delay_buf[(s_write_ptr + base + offset) & REVERB_BUF_MASK];
}

static __not_in_flash_func(void) write_delay(uint16_t base, uint16_t offset, int16_t val) {
    s_delay_buf[(s_write_ptr + base + offset) & REVERB_BUF_MASK] = val;
}

static __not_in_flash_func(int16_t) read_delay_interpolated(uint16_t base, int32_t offset_q16) {
    int32_t offset_int = offset_q16 >> 16;
    uint16_t offset_frac = offset_q16 & 0xFFFF;
    
    int16_t a = read_delay(base, (uint16_t)offset_int);
    int16_t b = read_delay(base, (uint16_t)(offset_int + 1));
    
    return a + (((b - a) * offset_frac) >> 16);
}

static __not_in_flash_func(int32_t) process_allpass_rings(uint16_t base, uint16_t len, int32_t in, int16_t kap) {
    int16_t tail = read_delay(base, len - 1);
    int32_t write_val = in + ((tail * kap) >> 15);
    if (write_val > 32767) write_val = 32767;
    else if (write_val < -32768) write_val = -32768;
    write_delay(base, 0, (int16_t)write_val);
    int32_t out = tail - ((write_val * kap) >> 15);
    return out;
}

// Dummy Reverb2_Process to satisfy header if needed
void Reverb2_Process(Reverb2 *rv, int16_t in, int16_t *l, int16_t *r) {
    (void)rv; (void)in; (void)l; (void)r;
}

void __not_in_flash_func(reverb_process_stereo)(int16_t *left_samples, int16_t *right_samples,
                          int16_t *output_left, int16_t *output_right,
                          int num_samples, int16_t wet_amount) {
    if (wet_amount == 0 || !s_reverb_enabled) {
        memcpy(output_left, left_samples, num_samples * sizeof(int16_t));
        memcpy(output_right, right_samples, num_samples * sizeof(int16_t));
        return;
    }
    
    uint8_t input_shift = s_reverb_input_shift;
    int16_t wet_scale = wet_amount;
    int16_t dry_scale = REVERB_Q15_ONE - wet_amount;
    
    for (int i = 0; i < num_samples; i++) {
        s_write_ptr = (s_write_ptr - 1) & REVERB_BUF_MASK;
        
        s_lfo1_phase += 48695;
        s_lfo2_phase += 29217;
        
        int16_t lfo1_val = triangle_lfo(s_lfo1_phase);
        int16_t lfo2_val = triangle_lfo(s_lfo2_phase);
        
        // Smear AP1
        int32_t offset_q16_smear = (10 << 16) + (80 * lfo1_val) * 2;
        int16_t smear_out = read_delay_interpolated(BASE_AP1, offset_q16_smear);
        write_delay(BASE_AP1, 100, smear_out);
        
        int16_t dry_l = left_samples[i];
        int16_t dry_r = right_samples[i];
        
        int32_t acc = ((dry_l + dry_r) >> (input_shift + 1));
        
        // Diffusers
        acc = process_allpass_rings(BASE_AP1, LEN_AP1, acc, s_kap);
        acc = process_allpass_rings(BASE_AP2, LEN_AP2, acc, s_kap);
        acc = process_allpass_rings(BASE_AP3, LEN_AP3, acc, s_kap);
        acc = process_allpass_rings(BASE_AP4, LEN_AP4, acc, s_kap);
        
        int32_t apout = acc;
        
        // Main reverb loop Left
        int32_t offset_q16_del2 = (6261 << 16) + (50 * lfo2_val) * 2;
        int16_t del2_out = read_delay_interpolated(BASE_DEL2, offset_q16_del2);
        acc = apout + ((del2_out * s_krt) >> 15);
        
        s_lp_1 += ((acc - s_lp_1) * s_klp) >> 15;
        acc = s_lp_1;
        
        acc = process_allpass_rings(BASE_DAP1A, LEN_DAP1A, acc, -s_kap);
        acc = process_allpass_rings(BASE_DAP1B, LEN_DAP1B, acc, s_kap);
        
        int32_t acc_clamp1 = acc;
        if (acc_clamp1 > 32767) acc_clamp1 = 32767;
        else if (acc_clamp1 < -32768) acc_clamp1 = -32768;
        write_delay(BASE_DEL1, 0, (int16_t)acc_clamp1);
        
        int32_t wet_left_32 = acc * 2;
        if (wet_left_32 > 32767) wet_left_32 = 32767;
        else if (wet_left_32 < -32768) wet_left_32 = -32768;
        int16_t wet_left = (int16_t)wet_left_32;
        
        // Main reverb loop Right
        int32_t offset_q16_del1 = (4460 << 16) + (40 * lfo1_val) * 2;
        int16_t del1_out = read_delay_interpolated(BASE_DEL1, offset_q16_del1);
        acc = apout + ((del1_out * s_krt) >> 15);
        
        s_lp_2 += ((acc - s_lp_2) * s_klp) >> 15;
        acc = s_lp_2;
        
        acc = process_allpass_rings(BASE_DAP2A, LEN_DAP2A, acc, s_kap);
        acc = process_allpass_rings(BASE_DAP2B, LEN_DAP2B, acc, -s_kap);
        
        int32_t acc_clamp2 = acc;
        if (acc_clamp2 > 32767) acc_clamp2 = 32767;
        else if (acc_clamp2 < -32768) acc_clamp2 = -32768;
        write_delay(BASE_DEL2, 0, (int16_t)acc_clamp2);
        
        int32_t wet_right_32 = acc * 2;
        if (wet_right_32 > 32767) wet_right_32 = 32767;
        else if (wet_right_32 < -32768) wet_right_32 = -32768;
        int16_t wet_right = (int16_t)wet_right_32;
        
        // Mix
        int16_t out_l = reverb_add_q15((dry_l * dry_scale) >> 15, (wet_left * wet_scale) >> 15);
        int16_t out_r = reverb_add_q15((dry_r * dry_scale) >> 15, (wet_right * wet_scale) >> 15);
        
        output_left[i] = out_l;
        output_right[i] = out_r;
    }
}

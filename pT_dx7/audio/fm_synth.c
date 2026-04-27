#include "fm_synth.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <pico.h>
#include "pico/time.h"
#include "hardware/interp.h"

#include "audio_output.h"
#include "dx7_rom_bank.h"
#include "reverb.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FM_LOGSIN_BITS 10u
#define FM_LOGSIN_SIZE (1u << FM_LOGSIN_BITS)
#define FM_PHASE_INDEX_BITS (FM_LOGSIN_BITS + 2u)
#define FM_PHASE_INDEX_SHIFT (32u - FM_PHASE_INDEX_BITS)
#define FM_EXP_LUT_SIZE 4096u
#define FM_MAX_ATTENUATION (FM_EXP_LUT_SIZE - 1u)
#define FM_NOTE_COUNT 128u
#define FM_MIX_SHIFT 8u
#define FM_DX7_LG_N 6u
#define FM_DX7_BLOCK_SAMPLES (1u << FM_DX7_LG_N)
#define FM_DX7_SIN_LG_N_SAMPLES 10u
#define FM_DX7_SIN_N_SAMPLES (1u << FM_DX7_SIN_LG_N_SAMPLES)
#define FM_DX7_EXP2_LG_N_SAMPLES 10u
#define FM_DX7_EXP2_N_SAMPLES (1u << FM_DX7_EXP2_LG_N_SAMPLES)
#define FM_DX7_Q24_TO_S16_SHIFT 9u

typedef struct {
  uint8_t rates[4];
  uint8_t levels[4];
  uint8_t break_point;
  uint8_t left_depth;
  uint8_t right_depth;
  uint8_t left_curve;
  uint8_t right_curve;
  uint8_t rate_scaling;
  uint8_t key_velocity_sensitivity;
  uint8_t output_level;
  uint8_t ratio_num;
  uint8_t ratio_den;
  uint8_t freq_coarse;
  uint8_t freq_fine;
  int8_t detune;
  bool fixed_freq;
  bool carrier;
} FmOperatorPatch;

typedef struct {
  uint8_t algorithm;
  uint8_t feedback;
  int8_t transpose;
  FmOperatorPatch op[FM_SYNTH_OPERATOR_COUNT];
} FmPatch;

typedef struct {
  int32_t level_q24;
  int32_t target_level_q24;
  uint32_t increment_q24;
  int32_t outlevel;
  uint8_t rate_scaling;
  uint8_t stage_index;
  bool rising;
  bool key_down;
} FmDx7EnvelopeState;

typedef struct {
  uint32_t phase;
  uint32_t phase_step;
  int32_t gain_q24;
  FmDx7EnvelopeState env;
} FmOperatorState;

typedef struct {
  bool active;
  uint8_t note;
  uint8_t velocity;
  uint32_t age;
  const FmPatch *patch;
  int32_t feedback_buffer[2];
  FmOperatorState op[FM_SYNTH_OPERATOR_COUNT];
} FmVoice;


static uint16_t __not_in_flash("fm_synth_luts") g_logsin_lut[FM_LOGSIN_SIZE];
static int16_t __not_in_flash("fm_synth_luts") g_exp_lut[FM_EXP_LUT_SIZE];

static int32_t __not_in_flash("fm_synth_luts") g_dx7_sin_lut[FM_DX7_SIN_N_SAMPLES << 1];
static int32_t __not_in_flash("fm_synth_luts") g_dx7_exp2_lut[FM_DX7_EXP2_N_SAMPLES << 1];


static uint32_t g_note_phase_step[FM_NOTE_COUNT];
static FmVoice g_voices[FM_SYNTH_VOICE_COUNT];
static uint8_t g_note_hold_count[FM_NOTE_COUNT];
static uint32_t g_voice_age_counter;
static uint32_t g_voice_steal_count;
static uint32_t g_same_note_retrigger_count;
static uint32_t g_output_clip_count;
static uint32_t g_reverb_render_us;
static uint8_t g_peak_active_voice_count;
static uint16_t g_mix_gain_q8;
static uint8_t g_master_level;
static uint8_t g_voice_level;
static uint8_t g_reverb_wet;  // 0-255, mapped to Q15 0-0x7FFF
static const FmPatch *g_current_patch;
static uint8_t g_current_patch_index = 0;
static const uint8_t k_voice_normalization_q8[FM_SYNTH_VOICE_COUNT + 1u] = {
    0u, 255u, 180u, 147u, 128u, 114u, 104u,
};

static const FmPatch k_orchestra_patch = {
    .algorithm = 1,
    .feedback = 7,
    .op =
        {
            {.rates = {72, 76, 10, 32},
             .levels = {99, 92, 0, 0},
             .break_point = 0,
             .left_depth = 0,
             .right_depth = 0,
             .left_curve = 0,
             .right_curve = 0,
             .rate_scaling = 0,
             .key_velocity_sensitivity = 0,
             .output_level = 82,
             .ratio_num = 2,
             .ratio_den = 1,
             .detune = 0,
             .carrier = false},
            {.rates = {76, 73, 10, 55},
             .levels = {99, 92, 0, 0},
             .break_point = 0,
             .left_depth = 0,
             .right_depth = 0,
             .left_curve = 0,
             .right_curve = 0,
             .rate_scaling = 0,
             .key_velocity_sensitivity = 0,
             .output_level = 80,
             .ratio_num = 2,
             .ratio_den = 1,
             .detune = 0,
             .carrier = false},
            {.rates = {56, 74, 10, 45},
             .levels = {98, 98, 36, 0},
             .break_point = 36,
             .left_depth = 0,
             .right_depth = 98,
             .left_curve = 0,
             .right_curve = 0,
             .rate_scaling = 0,
             .key_velocity_sensitivity = 0,
             .output_level = 72,
             .ratio_num = 2,
             .ratio_den = 1,
             .detune = 0,
             .carrier = false},
            {.rates = {54, 15, 10, 47},
             .levels = {99, 92, 0, 0},
             .break_point = 0,
             .left_depth = 0,
             .right_depth = 0,
             .left_curve = 0,
             .right_curve = 0,
             .rate_scaling = 0,
             .key_velocity_sensitivity = 0,
             .output_level = 96,
             .ratio_num = 2,
             .ratio_den = 1,
             .detune = 6,
             .carrier = true},
            {.rates = {53, 46, 32, 61},
             .levels = {99, 93, 90, 0},
             .break_point = 0,
             .left_depth = 0,
             .right_depth = 0,
             .left_curve = 0,
             .right_curve = 0,
             .rate_scaling = 0,
             .key_velocity_sensitivity = 0,
             .output_level = 83,
             .ratio_num = 1,
             .ratio_den = 1,
             .detune = -6,
             .carrier = false},
            {.rates = {80, 56, 10, 45},
             .levels = {98, 98, 36, 0},
             .break_point = 36,
             .left_depth = 0,
             .right_depth = 98,
             .left_curve = 0,
             .right_curve = 0,
             .rate_scaling = 0,
             .key_velocity_sensitivity = 0,
             .output_level = 99,
             .ratio_num = 1,
             .ratio_den = 1,
             .detune = 0,
             .carrier = true},
        }};

static int16_t clamp_s16(int32_t value) {
  if (value > 32767) {
    return 32767;
  }
  if (value < -32768) {
    return -32768;
  }
  return (int16_t)value;
}

static uint8_t fm_synth_active_voice_count(void) {
  uint8_t active_voice_count = 0u;

  for (size_t i = 0; i < FM_SYNTH_VOICE_COUNT; ++i) {
    if (g_voices[i].active) {
      ++active_voice_count;
    }
  }

  return active_voice_count;
}

enum {
  FM_DX7_OUT_BUS_ONE = 1 << 0,
  FM_DX7_OUT_BUS_TWO = 1 << 1,
  FM_DX7_OUT_BUS_ADD = 1 << 2,
  FM_DX7_IN_BUS_ONE = 1 << 4,
  FM_DX7_IN_BUS_TWO = 1 << 5,
  FM_DX7_FB_IN = 1 << 6,
  FM_DX7_FB_OUT = 1 << 7,
};

typedef struct {
  int32_t gain[2];
  uint32_t freq;
  uint32_t phase;
} FmDx7OpParams;

typedef struct {
  uint8_t ops[FM_SYNTH_OPERATOR_COUNT];
} FmDx7Algorithm;

static const FmDx7Algorithm k_dx7_algorithms[32] = {
    {{0xc1, 0x11, 0x11, 0x14, 0x01, 0x14}}, {{0x01, 0x11, 0x11, 0x14, 0xc1, 0x14}},
    {{0xc1, 0x11, 0x14, 0x01, 0x11, 0x14}}, {{0x41, 0x11, 0x94, 0x01, 0x11, 0x14}},
    {{0xc1, 0x14, 0x01, 0x14, 0x01, 0x14}}, {{0x41, 0x94, 0x01, 0x14, 0x01, 0x14}},
    {{0xc1, 0x11, 0x05, 0x14, 0x01, 0x14}}, {{0x01, 0x11, 0xc5, 0x14, 0x01, 0x14}},
    {{0x01, 0x11, 0x05, 0x14, 0xc1, 0x14}}, {{0x01, 0x05, 0x14, 0xc1, 0x11, 0x14}},
    {{0xc1, 0x05, 0x14, 0x01, 0x11, 0x14}}, {{0x01, 0x05, 0x05, 0x14, 0xc1, 0x14}},
    {{0xc1, 0x05, 0x05, 0x14, 0x01, 0x14}}, {{0xc1, 0x05, 0x11, 0x14, 0x01, 0x14}},
    {{0x01, 0x05, 0x11, 0x14, 0xc1, 0x14}}, {{0xc1, 0x11, 0x02, 0x25, 0x05, 0x14}},
    {{0x01, 0x11, 0x02, 0x25, 0xc5, 0x14}}, {{0x01, 0x11, 0x11, 0xc5, 0x05, 0x14}},
    {{0xc1, 0x14, 0x14, 0x01, 0x11, 0x14}}, {{0x01, 0x05, 0x14, 0xc1, 0x14, 0x14}},
    {{0x01, 0x14, 0x14, 0xc1, 0x14, 0x14}}, {{0xc1, 0x14, 0x14, 0x14, 0x01, 0x14}},
    {{0xc1, 0x14, 0x14, 0x01, 0x14, 0x04}}, {{0xc1, 0x14, 0x14, 0x14, 0x04, 0x04}},
    {{0xc1, 0x14, 0x14, 0x04, 0x04, 0x04}}, {{0xc1, 0x05, 0x14, 0x01, 0x14, 0x04}},
    {{0x01, 0x05, 0x14, 0xc1, 0x14, 0x04}}, {{0x04, 0xc1, 0x11, 0x14, 0x01, 0x14}},
    {{0xc1, 0x14, 0x01, 0x14, 0x04, 0x04}}, {{0x04, 0xc1, 0x11, 0x14, 0x04, 0x04}},
    {{0xc1, 0x14, 0x04, 0x04, 0x04, 0x04}}, {{0xc4, 0x04, 0x04, 0x04, 0x04, 0x04}},
};

static void fm_synth_set_ratio_from_dx7(uint8_t coarse, uint8_t fine,
                                        uint8_t *ratio_num,
                                        uint8_t *ratio_den) {
  double ratio = coarse == 0u ? 0.5 : (double)coarse;
  uint8_t denom;
  uint32_t numer;

  ratio *= 1.0 + ((double)fine / 100.0);
  denom = ratio < 1.0 ? 255u : (uint8_t)floor(255.0 / ratio);
  if (denom == 0u) {
    denom = 1u;
  }

  numer = (uint32_t)lround(ratio * (double)denom);
  if (numer == 0u) {
    numer = 1u;
  } else if (numer > 255u) {
    numer = 255u;
  }

  *ratio_num = (uint8_t)numer;
  *ratio_den = denom;
}

static void dx7_patch_to_fm_patch(const uint8_t dx7_data[128], FmPatch *fm_patch) {
  // DX7 32-voice bulk format stores operators in hardware order OP6..OP1.
  if (!dx7_data || !fm_patch) {
    return;
  }

  fm_patch->algorithm = dx7_data[110] & 0x1Fu;
  fm_patch->feedback = dx7_data[111] & 0x07u;
  fm_patch->transpose = (int8_t)dx7_data[117] - 24;

  for (int op = 0; op < FM_SYNTH_OPERATOR_COUNT; op++) {
    const int base_offset = op * 17;
    const uint8_t *op_data = &dx7_data[base_offset];
    const uint8_t curves = op_data[11];
    const uint8_t detune_rate = op_data[12];
    const uint8_t touch_mod = op_data[13];
    const uint8_t freq_mode = op_data[15];
    const uint8_t freq_coarse = (freq_mode >> 1) & 0x1Fu;
    const uint8_t freq_fine = op_data[16] <= 99u ? op_data[16] : 99u;
    uint8_t ratio_num = 1u;
    uint8_t ratio_den = 1u;

    fm_synth_set_ratio_from_dx7(freq_coarse, freq_fine, &ratio_num, &ratio_den);

    fm_patch->op[op] = (FmOperatorPatch){
      .rates = {op_data[0], op_data[1], op_data[2], op_data[3]},
      .levels = {op_data[4], op_data[5], op_data[6], op_data[7]},
      .break_point = op_data[8],
      .left_depth = op_data[9],
      .right_depth = op_data[10],
      .left_curve = curves & 0x03u,
      .right_curve = (curves >> 2) & 0x03u,
      .rate_scaling = detune_rate & 0x07u,
      .key_velocity_sensitivity = (touch_mod >> 2) & 0x07u,
      .output_level = op_data[14] <= 99u ? op_data[14] : 99u,
      .ratio_num = ratio_num,
      .ratio_den = ratio_den,
      .freq_coarse = freq_coarse,
      .freq_fine = freq_fine,
      .detune = (int8_t)((detune_rate >> 3) & 0x0Fu) - 7,
      .fixed_freq = (freq_mode & 0x01u) != 0u,
      .carrier = false
    };
  }
}

static void fm_synth_init_luts(void);
static void fm_synth_init_interp(void);
static uint32_t fm_synth_operator_phase_step(uint8_t note, const FmOperatorPatch *patch);
static void fm_synth_start_voice(FmVoice *voice, uint8_t note, uint8_t velocity);
static void fm_synth_retrigger_voice(FmVoice *voice, uint8_t note,
                                     uint8_t velocity);
static FmVoice *fm_synth_find_voice_for_note(uint8_t note);
static FmVoice *fm_synth_allocate_voice(uint8_t note);
static void fm_synth_release_voice(FmVoice *voice);
static bool fm_synth_voice_key_is_down(const FmVoice *voice);
static uint32_t fm_synth_voice_level_estimate(const FmVoice *voice);
static int fm_synth_feedback_shift(const FmPatch *patch);
static int16_t fm_synth_lookup_operator(uint32_t phase, uint16_t attenuation);
static bool fm_synth_voice_is_finished(const FmVoice *voice);
static int32_t fm_dx7_sin_lookup(uint32_t phase);
static int32_t fm_dx7_exp2_lookup(int32_t x);
static void fm_dx7_kernel_compute(int32_t *output, const int32_t *input,
                                 uint32_t phase0, uint32_t freq, int32_t gain1,
                                 int32_t gain2, bool add);
static void fm_dx7_kernel_compute_pure(int32_t *output, uint32_t phase0,
                                      uint32_t freq, int32_t gain1,
                                      int32_t gain2, bool add);
static void fm_dx7_kernel_compute_feedback(int32_t *output, uint32_t phase0,
                                          uint32_t freq, int32_t gain1,
                                          int32_t gain2, int32_t *fb_buf,
                                          int feedback_shift, bool add);
static void fm_dx7_core_compute(int32_t *output, FmDx7OpParams *params,
                               uint8_t algorithm, int32_t *fb_buf,
                               int feedback_shift);
static int fm_dx7_scale_output_level(int outlevel);
static int fm_dx7_scale_velocity(int velocity, int sensitivity);
static uint8_t fm_dx7_scale_rate(uint8_t note, uint8_t sensitivity);
static int fm_dx7_scale_curve(int group, int depth, int curve);
static int fm_dx7_scale_level(uint8_t note, const FmOperatorPatch *patch);
static void fm_dx7_env_advance(FmDx7EnvelopeState *env,
                               const FmOperatorPatch *patch,
                               uint8_t new_stage_index);
static void fm_dx7_env_init(FmDx7EnvelopeState *env,
                            const FmOperatorPatch *patch, uint8_t note,
                            uint8_t velocity);
static void fm_dx7_env_keydown(FmDx7EnvelopeState *env,
                               const FmOperatorPatch *patch, bool key_down);
static int32_t fm_dx7_env_get_sample(FmDx7EnvelopeState *env,
                                     const FmOperatorPatch *patch);
static bool fm_dx7_env_is_finished(const FmDx7EnvelopeState *env);

static const int k_dx7_level_lut[20] = {
    0,  5,  9,  13, 17, 20, 23, 25, 27, 29,
    31, 33, 35, 37, 39, 41, 42, 43, 45, 46,
};

static const uint8_t k_dx7_velocity_lut[64] = {
    0,   70,  86,  97,  106, 114, 121, 126, 132, 138, 142, 148, 152,
    156, 160, 163, 166, 170, 173, 174, 178, 181, 184, 186, 189, 190,
    194, 196, 198, 200, 202, 205, 206, 209, 211, 214, 216, 218, 220,
    222, 224, 225, 227, 229, 230, 232, 233, 235, 237, 238, 240, 241,
    242, 243, 244, 246, 246, 248, 249, 250, 251, 252, 253, 254,
};

static const uint8_t k_dx7_exp_scale_lut[33] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  11, 14, 16, 19, 23, 27, 33,
    39, 47, 56, 66, 80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238,
    250,
};

static int fm_dx7_scale_output_level(int outlevel) {
  if (outlevel >= 20) {
    return 28 + outlevel;
  }

  return k_dx7_level_lut[outlevel];
}

static int fm_dx7_scale_velocity(int velocity, int sensitivity) {
  int clamped_velocity = velocity;
  int velocity_value;

  if (clamped_velocity < 0) {
    clamped_velocity = 0;
  } else if (clamped_velocity > 127) {
    clamped_velocity = 127;
  }

  velocity_value = k_dx7_velocity_lut[clamped_velocity >> 1] - 239;
  return ((sensitivity * velocity_value + 7) >> 3) << 4;
}

static uint8_t fm_dx7_scale_rate(uint8_t note, uint8_t sensitivity) {
  int x = (int)note / 3 - 7;

  if (x < 0) {
    x = 0;
  } else if (x > 31) {
    x = 31;
  }

  return (uint8_t)((sensitivity * x) >> 3);
}

static int fm_dx7_scale_curve(int group, int depth, int curve) {
  int scale;

  if (curve == 0 || curve == 3) {
    scale = (group * depth * 329) >> 12;
  } else {
    const int raw_exp =
        k_dx7_exp_scale_lut[group < 33 ? group : 32];
    scale = (raw_exp * depth * 329) >> 15;
  }

  if (curve < 2) {
    scale = -scale;
  }

  return scale;
}

static int fm_dx7_scale_level(uint8_t note, const FmOperatorPatch *patch) {
  const int offset = (int)note - (int)patch->break_point - 17;

  if (offset >= 0) {
    return fm_dx7_scale_curve(offset / 3, patch->right_depth,
                              patch->right_curve);
  }

  return fm_dx7_scale_curve((-offset) / 3, patch->left_depth,
                            patch->left_curve);
}

static void fm_dx7_env_advance(FmDx7EnvelopeState *env,
                              const FmOperatorPatch *patch,
                              uint8_t new_stage_index) {
  env->stage_index = new_stage_index;

  if (new_stage_index < 4u) {
    int actual_level = fm_dx7_scale_output_level(patch->levels[new_stage_index]) >> 1;
    int qrate;

    actual_level = (actual_level << 6) + env->outlevel - 4256;
    if (actual_level < 16) {
      actual_level = 16;
    }

    env->target_level_q24 = actual_level << 16;
    env->rising = env->target_level_q24 > env->level_q24;

    qrate = ((int)patch->rates[new_stage_index] * 41) >> 6;
    qrate += env->rate_scaling;
    if (qrate > 63) {
      qrate = 63;
    }

    env->increment_q24 =
        (uint32_t)((4 + (qrate & 3)) << (2 + FM_DX7_LG_N + (qrate >> 2)));
  }
}

static void fm_dx7_env_init(FmDx7EnvelopeState *env,
                            const FmOperatorPatch *patch, uint8_t note,
                            uint8_t velocity) {
  int outlevel = fm_dx7_scale_output_level(patch->output_level);

  outlevel += fm_dx7_scale_level(note, patch);
  if (outlevel > 127) {
    outlevel = 127;
  }

  outlevel <<= 5;
  outlevel += fm_dx7_scale_velocity(velocity, patch->key_velocity_sensitivity);
  if (outlevel < 0) {
    outlevel = 0;
  }

  memset(env, 0, sizeof(*env));
  env->outlevel = outlevel;
  env->rate_scaling = fm_dx7_scale_rate(note, patch->rate_scaling);
  env->key_down = true;
  fm_dx7_env_advance(env, patch, 0u);
}

static void fm_dx7_env_keydown(FmDx7EnvelopeState *env,
                               const FmOperatorPatch *patch, bool key_down) {
  if (env->key_down == key_down) {
    return;
  }

  env->key_down = key_down;
  fm_dx7_env_advance(env, patch, key_down ? 0u : 3u);
}

static int32_t fm_dx7_env_get_sample(FmDx7EnvelopeState *env,
                                     const FmOperatorPatch *patch) {
  if (env->stage_index < 3u || (env->stage_index < 4u && !env->key_down)) {
    if (env->rising) {
      const int32_t jump_target = 1716 << 16;

      if (env->level_q24 < jump_target) {
        env->level_q24 = jump_target;
      }

      env->level_q24 +=
          (((17 << 24) - env->level_q24) >> 24) * (int32_t)env->increment_q24;
      if (env->level_q24 >= env->target_level_q24) {
        env->level_q24 = env->target_level_q24;
        fm_dx7_env_advance(env, patch, (uint8_t)(env->stage_index + 1u));
      }
    } else {
      env->level_q24 -= (int32_t)env->increment_q24;
      if (env->level_q24 <= env->target_level_q24) {
        env->level_q24 = env->target_level_q24;
        fm_dx7_env_advance(env, patch, (uint8_t)(env->stage_index + 1u));
      }
    }
  }

  return env->level_q24;
}

static bool fm_dx7_env_is_finished(const FmDx7EnvelopeState *env) {
  return env->stage_index >= 4u;
}

static void fm_synth_init_luts(void) {
  const double sin_phase_step = (2.0 * M_PI) / (double)FM_DX7_SIN_N_SAMPLES;
  const int64_t rounding = (int64_t)1 << 29;
  int32_t u = 1 << 30;
  int32_t v = 0;
  const int32_t c =
      (int32_t)floor(cos(sin_phase_step) * (double)(1 << 30) + 0.5);
  const int32_t s =
      (int32_t)floor(sin(sin_phase_step) * (double)(1 << 30) + 0.5);

  for (uint32_t i = 0; i < FM_LOGSIN_SIZE; ++i) {
    if (i == 0u) {
      g_logsin_lut[i] = FM_MAX_ATTENUATION;
      continue;
    }

    const double angle = ((double)i * (M_PI * 0.5)) / (double)FM_LOGSIN_SIZE;
    const double magnitude = sin(angle);
    double attenuation = -256.0 * log2(magnitude);

    if (attenuation > (double)FM_MAX_ATTENUATION) {
      attenuation = (double)FM_MAX_ATTENUATION;
    }

    g_logsin_lut[i] = (uint16_t)lround(attenuation);
  }

  for (uint32_t i = 0; i < FM_EXP_LUT_SIZE; ++i) {
    const double magnitude = 32767.0 * exp2(-(double)i / 256.0);
    g_exp_lut[i] = (int16_t)lround(magnitude < 1.0 ? 0.0 : magnitude);
  }

  for (uint32_t i = 0; i < (FM_DX7_SIN_N_SAMPLES / 2u); ++i) {
    g_dx7_sin_lut[(i << 1) + 1] = (v + 32) >> 6;
    g_dx7_sin_lut[((i + (FM_DX7_SIN_N_SAMPLES / 2u)) << 1) + 1] =
        -((v + 32) >> 6);
    {
      const int32_t t = (int32_t)((((int64_t)u * (int64_t)s) +
                                   ((int64_t)v * (int64_t)c) + rounding) >>
                                  30);
      u = (int32_t)((((int64_t)u * (int64_t)c) -
                     ((int64_t)v * (int64_t)s) + rounding) >>
                     30);
      v = t;
    }
  }

  for (uint32_t i = 0; i < FM_DX7_SIN_N_SAMPLES - 1u; ++i) {
    g_dx7_sin_lut[i << 1] =
        g_dx7_sin_lut[(i << 1) + 3] - g_dx7_sin_lut[(i << 1) + 1];
  }
  g_dx7_sin_lut[(FM_DX7_SIN_N_SAMPLES << 1) - 2] =
      -g_dx7_sin_lut[(FM_DX7_SIN_N_SAMPLES << 1) - 1];

  {
    const double exp2_inc = exp2(1.0 / (double)FM_DX7_EXP2_N_SAMPLES);
    double y = (double)(1 << 30);
    for (uint32_t i = 0; i < FM_DX7_EXP2_N_SAMPLES; ++i) {
      g_dx7_exp2_lut[(i << 1) + 1] = (int32_t)floor(y + 0.5);
      y *= exp2_inc;
    }
  }

  for (uint32_t i = 0; i < FM_DX7_EXP2_N_SAMPLES - 1u; ++i) {
    g_dx7_exp2_lut[i << 1] =
        g_dx7_exp2_lut[(i << 1) + 3] - g_dx7_exp2_lut[(i << 1) + 1];
  }
  g_dx7_exp2_lut[(FM_DX7_EXP2_N_SAMPLES << 1) - 2] =
      INT32_MAX - g_dx7_exp2_lut[(FM_DX7_EXP2_N_SAMPLES << 1) - 1];

  for (uint32_t note = 0; note < FM_NOTE_COUNT; ++note) {
    const double frequency =
        440.0 * exp2(((double)((int)note - 69)) / 12.0);
    const double phase_step =
        frequency * 16777216.0 / (double)AUDIO_OUTPUT_SAMPLE_RATE_HZ;
    g_note_phase_step[note] = (uint32_t)llround(phase_step);
  }
}

static uint32_t fm_synth_operator_phase_step(uint8_t note,
                                             const FmOperatorPatch *patch) {
  double base_phase_step;
  const double detune_scale =
      exp2((12606.0 * (double)patch->detune) / 16777216.0);

  if (patch->fixed_freq) {
    const double frequency =
        pow(10.0, (double)(patch->freq_coarse & 0x03u) +
                      ((double)patch->freq_fine / 100.0));
    base_phase_step =
        frequency * 16777216.0 / (double)AUDIO_OUTPUT_SAMPLE_RATE_HZ;
  } else {
    int transposed_note = (int)note + (int)g_current_patch->transpose;
    const uint64_t scaled =
        (uint64_t)g_note_phase_step[transposed_note < 0
                                        ? 0
                                        : (transposed_note >= (int)FM_NOTE_COUNT
                                               ? (FM_NOTE_COUNT - 1u)
                                               : (uint32_t)transposed_note)] *
        (uint64_t)patch->ratio_num;
    base_phase_step = (double)scaled / (double)patch->ratio_den;
  }

  return (uint32_t)llround(base_phase_step * detune_scale);
}

static void fm_synth_start_voice(FmVoice *voice, uint8_t note, uint8_t velocity) {
  memset(voice, 0, sizeof(*voice));
  voice->active = true;
  voice->note = note;
  voice->velocity = velocity;
  voice->patch = g_current_patch;
  voice->age = ++g_voice_age_counter;

  for (size_t i = 0; i < FM_SYNTH_OPERATOR_COUNT; ++i) {
    voice->op[i].phase_step =
        fm_synth_operator_phase_step(note, &voice->patch->op[i]);
    fm_dx7_env_init(&voice->op[i].env, &voice->patch->op[i], note, velocity);
  }
}

static void fm_synth_retrigger_voice(FmVoice *voice, uint8_t note,
                                     uint8_t velocity) {
  if (voice == NULL) {
    return;
  }

  voice->active = true;
  voice->note = note;
  voice->velocity = velocity;
  voice->patch = g_current_patch;
  voice->age = ++g_voice_age_counter;

  for (size_t i = 0; i < FM_SYNTH_OPERATOR_COUNT; ++i) {
    voice->op[i].phase_step =
        fm_synth_operator_phase_step(note, &voice->patch->op[i]);
    fm_dx7_env_init(&voice->op[i].env, &voice->patch->op[i], note, velocity);
  }
}

static FmVoice *fm_synth_find_voice_for_note(uint8_t note) {
  for (size_t i = 0; i < FM_SYNTH_VOICE_COUNT; ++i) {
    if (g_voices[i].active && g_voices[i].note == note) {
      return &g_voices[i];
    }
  }

  return NULL;
}

static FmVoice *fm_synth_allocate_voice(uint8_t note) {
  FmVoice *candidate;
  FmVoice *released_candidate = NULL;
  uint32_t released_candidate_level = 0u;

  (void)note;

  for (size_t i = 0; i < FM_SYNTH_VOICE_COUNT; ++i) {
    if (!g_voices[i].active) {
      return &g_voices[i];
    }
  }

  for (size_t i = 0; i < FM_SYNTH_VOICE_COUNT; ++i) {
    FmVoice *voice = &g_voices[i];
    uint32_t voice_level;

    if (fm_synth_voice_key_is_down(voice)) {
      continue;
    }

    voice_level = fm_synth_voice_level_estimate(voice);
    if (released_candidate == NULL || voice_level < released_candidate_level ||
        (voice_level == released_candidate_level &&
         voice->age < released_candidate->age)) {
      released_candidate = voice;
      released_candidate_level = voice_level;
    }
  }

  if (released_candidate != NULL) {
    ++g_voice_steal_count;
    return released_candidate;
  }

  candidate = &g_voices[0];
  for (size_t i = 1; i < FM_SYNTH_VOICE_COUNT; ++i) {
    if (g_voices[i].age < candidate->age) {
      candidate = &g_voices[i];
    }
  }

  ++g_voice_steal_count;
  return candidate;
}

static void fm_synth_release_voice(FmVoice *voice) {
  for (size_t i = 0; i < FM_SYNTH_OPERATOR_COUNT; ++i) {
    fm_dx7_env_keydown(&voice->op[i].env, &voice->patch->op[i], false);
  }
}

static bool fm_synth_voice_key_is_down(const FmVoice *voice) {
  if (voice == NULL || !voice->active) {
    return false;
  }

  for (size_t i = 0; i < FM_SYNTH_OPERATOR_COUNT; ++i) {
    if (voice->op[i].env.key_down) {
      return true;
    }
  }

  return false;
}

static uint32_t fm_synth_voice_level_estimate(const FmVoice *voice) {
  uint32_t total = 0u;

  if (voice == NULL || !voice->active) {
    return 0u;
  }

  for (size_t i = 0; i < FM_SYNTH_OPERATOR_COUNT; ++i) {
    if (voice->op[i].env.level_q24 > 0) {
      total += (uint32_t)voice->op[i].env.level_q24;
    }
  }

  return total;
}

static int fm_synth_feedback_shift(const FmPatch *patch) {
  if (patch == NULL || patch->feedback == 0u) {
    return 16;
  }

  return 8 - (int)patch->feedback;
}

static int16_t fm_synth_lookup_operator(uint32_t phase, uint16_t attenuation) {
  const uint32_t phase_index = phase >> FM_PHASE_INDEX_SHIFT;
  const uint32_t quadrant = phase_index >> FM_LOGSIN_BITS;
  const uint32_t phase_offset = phase_index & (FM_LOGSIN_SIZE - 1u);
  const uint32_t table_index =
      (quadrant & 1u) ? ((FM_LOGSIN_SIZE - 1u) - phase_offset) : phase_offset;
  uint32_t total_attenuation = (uint32_t)attenuation + g_logsin_lut[table_index];
  int16_t magnitude;

  if (total_attenuation > FM_MAX_ATTENUATION) {
    total_attenuation = FM_MAX_ATTENUATION;
  }

  magnitude = g_exp_lut[total_attenuation];
  if (quadrant >= 2u) {
    magnitude = (int16_t)-magnitude;
  }

  return magnitude;
}

static bool fm_synth_voice_is_finished(const FmVoice *voice) {
  for (size_t i = 0; i < FM_SYNTH_OPERATOR_COUNT; ++i) {
    if (!fm_dx7_env_is_finished(&voice->op[i].env)) {
      return false;
    }
  }
  return true;
}

static int32_t fm_dx7_sin_lookup(uint32_t phase) {
  interp0->accum[0] = phase;
  const int32_t *entry = (const int32_t *)interp0->peek[0];
  const int32_t lowbits = (int32_t)interp0->peek[1];
  return entry[1] + ((entry[0] * lowbits) >> (24 - FM_DX7_SIN_LG_N_SAMPLES));
}

static int32_t fm_dx7_exp2_lookup(int32_t x) {
  interp1->accum[0] = (uint32_t)x;
  const int32_t *entry = (const int32_t *)interp1->peek[0];
  const int32_t lowbits = (int32_t)interp1->peek[1];
  const int32_t y = entry[1] + ((entry[0] * lowbits) >> (24 - FM_DX7_EXP2_LG_N_SAMPLES));
  return y >> (6 - (x >> 24));
}

static __not_in_flash_func(void) fm_dx7_kernel_compute(int32_t *output, const int32_t *input,
                                 uint32_t phase0, uint32_t freq, int32_t gain1,
                                 int32_t gain2, bool add) {
  int32_t gain = gain1;
  const int32_t dgain =
      (gain2 - gain1 + ((int32_t)FM_DX7_BLOCK_SAMPLES >> 1)) >> FM_DX7_LG_N;
  uint32_t phase = phase0;

  for (size_t i = 0; i < FM_DX7_BLOCK_SAMPLES; ++i) {
    gain += dgain;
    if (add) {
      output[i] += (int32_t)(((int64_t)fm_dx7_sin_lookup(phase + (uint32_t)input[i]) *
                             (int64_t)gain) >>
                            24);
    } else {
      output[i] = (int32_t)(((int64_t)fm_dx7_sin_lookup(phase + (uint32_t)input[i]) *
                             (int64_t)gain) >>
                            24);
    }
    phase += freq;
  }
}

static __not_in_flash_func(void) fm_dx7_kernel_compute_pure(int32_t *output, uint32_t phase0,
                                       uint32_t freq, int32_t gain1,
                                       int32_t gain2, bool add) {
  int32_t gain = gain1;
  const int32_t dgain =
      (gain2 - gain1 + ((int32_t)FM_DX7_BLOCK_SAMPLES >> 1)) >> FM_DX7_LG_N;
  uint32_t phase = phase0;

  for (size_t i = 0; i < FM_DX7_BLOCK_SAMPLES; ++i) {
    gain += dgain;
    if (add) {
      output[i] +=
          (int32_t)(((int64_t)fm_dx7_sin_lookup(phase) * (int64_t)gain) >> 24);
    } else {
      output[i] =
          (int32_t)(((int64_t)fm_dx7_sin_lookup(phase) * (int64_t)gain) >> 24);
    }
    phase += freq;
  }
}

static __not_in_flash_func(void) fm_dx7_kernel_compute_feedback(int32_t *output, uint32_t phase0,
                                          uint32_t freq, int32_t gain1,
                                          int32_t gain2, int32_t *fb_buf,
                                          int feedback_shift, bool add) {
  int32_t gain = gain1;
  const int32_t dgain =
      (gain2 - gain1 + ((int32_t)FM_DX7_BLOCK_SAMPLES >> 1)) >> FM_DX7_LG_N;
  uint32_t phase = phase0;
  int32_t y0 = fb_buf[0];
  int32_t y = fb_buf[1];

  for (size_t i = 0; i < FM_DX7_BLOCK_SAMPLES; ++i) {
    const int32_t scaled_feedback = (y0 + y) >> (feedback_shift + 1);
    gain += dgain;
    y0 = y;
    y = fm_dx7_sin_lookup(phase + (uint32_t)scaled_feedback);
    y = (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
    if (add) {
      output[i] += y;
    } else {
      output[i] = y;
    }
    phase += freq;
  }

  fb_buf[0] = y0;
  fb_buf[1] = y;
}

static __not_in_flash_func(void) fm_dx7_core_compute(int32_t *output, FmDx7OpParams *params,
                               uint8_t algorithm, int32_t *fb_buf,
                               int feedback_shift) {
  static const int32_t kLevelThreshold = 1120;
  int32_t bus_one[FM_DX7_BLOCK_SAMPLES];
  int32_t bus_two[FM_DX7_BLOCK_SAMPLES];
  // All buses (output + bus_one + bus_two) start as "no contents" so the
  // first op writing to each one overwrites instead of adding. This lets the
  // caller pass an uninitialized output buffer.
  bool has_contents[3] = {false, false, false};
  const FmDx7Algorithm *alg = &k_dx7_algorithms[algorithm];

  for (size_t op = 0; op < FM_SYNTH_OPERATOR_COUNT; ++op) {
    const int flags = alg->ops[op];
    bool add = (flags & FM_DX7_OUT_BUS_ADD) != 0;
    const int in_bus = (flags >> 4) & 3;
    const int out_bus = flags & 3;
    int32_t *out_ptr = out_bus == 0 ? output : (out_bus == 1 ? bus_one : bus_two);
    FmDx7OpParams *param = &params[op];
    const int32_t gain1 = param->gain[0];
    const int32_t gain2 = param->gain[1];

    if (gain1 >= kLevelThreshold || gain2 >= kLevelThreshold) {
      if (!has_contents[out_bus]) {
        add = false;
      }
      if (in_bus == 0 || !has_contents[in_bus]) {
        if ((flags & 0xc0) == 0xc0 && feedback_shift < 16) {
          fm_dx7_kernel_compute_feedback(out_ptr, param->phase, param->freq, gain1,
                                       gain2, fb_buf, feedback_shift, add);
        } else {
          fm_dx7_kernel_compute_pure(out_ptr, param->phase, param->freq, gain1,
                                    gain2, add);
        }
      } else {
        fm_dx7_kernel_compute(out_ptr, in_bus == 1 ? bus_one : bus_two,
                             param->phase, param->freq, gain1, gain2, add);
      }
      has_contents[out_bus] = true;
    } else if (!add) {
      has_contents[out_bus] = false;
    }

    param->phase += param->freq << FM_DX7_LG_N;
  }

  // If no op wrote to the output bus, the caller-provided buffer is still
  // uninitialized — zero it so downstream accumulation sees silence.
  if (!has_contents[0]) {
    memset(output, 0, FM_DX7_BLOCK_SAMPLES * sizeof(int32_t));
  }
}

static void fm_synth_init_interp(void) {
  // LANE0: computes byte address of the dy entry for a given phase.
  // SHIFT=11, MASK=[3:12]: extracts bits [23:14] (10-bit index) into positions
  // [12:3], giving index*8 — the byte stride of each [dy,y0] pair. BASE0 is
  // the table base address, so peek[0] = &g_dx7_sin_lut[index*2].
  interp_config lane0_cfg = interp_default_config();
  interp_config_set_shift(&lane0_cfg, 11);
  interp_config_set_mask(&lane0_cfg, 3, 12);
  interp_set_config(interp0, 0, &lane0_cfg);
  interp_set_base(interp0, 0, (uint32_t)g_dx7_sin_lut);

  // LANE1: extracts the 14-bit fractional part of phase (bits [13:0]).
  // Used as lowbits in the 32-bit interpolation multiply.
  interp_config lane1_cfg = interp_default_config();
  interp_config_set_shift(&lane1_cfg, 0);
  interp_config_set_mask(&lane1_cfg, 0, 13);
  interp_set_base(interp0, 1, 0u);
  interp_set_config(interp0, 1, &lane1_cfg);

  // INTERP1 / LANE0: same geometry as INTERP0 — byte address into g_dx7_exp2_lut.
  interp_config exp2_lane0_cfg = interp_default_config();
  interp_config_set_shift(&exp2_lane0_cfg, 11);
  interp_config_set_mask(&exp2_lane0_cfg, 3, 12);
  interp_set_config(interp1, 0, &exp2_lane0_cfg);
  interp_set_base(interp1, 0, (uint32_t)g_dx7_exp2_lut);

  // INTERP1 / LANE1: 14-bit fraction (bits [13:0] of x).
  interp_config exp2_lane1_cfg = interp_default_config();
  interp_config_set_shift(&exp2_lane1_cfg, 0);
  interp_config_set_mask(&exp2_lane1_cfg, 0, 13);
  interp_set_base(interp1, 1, 0u);
  interp_set_config(interp1, 1, &exp2_lane1_cfg);
}

void fm_synth_init(void) {
  const uint8_t *patch_data;
  static FmPatch dx7_patch_converted;

  memset(g_voices, 0, sizeof(g_voices));
  memset(g_note_hold_count, 0, sizeof(g_note_hold_count));
  g_voice_age_counter = 0;
  g_voice_steal_count = 0u;
  g_same_note_retrigger_count = 0u;
  g_output_clip_count = 0u;
  g_reverb_render_us = 0u;
  g_peak_active_voice_count = 0u;
  g_mix_gain_q8 = 0u;
  g_master_level = 255u;
  g_voice_level = 48u;
  g_reverb_wet = 128u;  // 50% wet by default
  g_current_patch_index = 0;

  patch_data = dx7_rom_bank_patch_data(0u);
  if (patch_data != NULL) {
    dx7_patch_to_fm_patch(patch_data, &dx7_patch_converted);
    g_current_patch = &dx7_patch_converted;
  } else {
    g_current_patch = &k_orchestra_patch;
  }

  fm_synth_init_luts();
  fm_synth_init_interp();
  reverb_init();
}

void fm_synth_set_master_level(uint8_t level) { g_master_level = level; }

uint8_t fm_synth_master_level(void) { return g_master_level; }

void fm_synth_set_voice_level(uint8_t level) { g_voice_level = level; }

uint8_t fm_synth_voice_level(void) { return g_voice_level; }

void fm_synth_set_reverb_wet(uint8_t wet) { 
    g_reverb_wet = wet; 
}

uint8_t fm_synth_reverb_wet(void) { return g_reverb_wet; }

void fm_synth_set_patch(FmSynthPatchId patch_id) {
  const uint8_t *patch_data;
  static FmPatch dx7_patch_converted;

  patch_data = dx7_rom_bank_patch_data(patch_id);
  if (patch_data == NULL) {
    patch_id = 0u;
    patch_data = dx7_rom_bank_patch_data(0u);
  }

  if (patch_data != NULL) {
    dx7_patch_to_fm_patch(patch_data, &dx7_patch_converted);
    g_current_patch = &dx7_patch_converted;
  }
  g_current_patch_index = patch_id;
}

void fm_synth_note_on(uint8_t note, uint8_t velocity) {
  FmVoice *voice;

  if (note >= FM_NOTE_COUNT) {
    return;
  }

  if (velocity == 0u) {
    fm_synth_note_off(note);
    return;
  }

  if (g_note_hold_count[note] < UINT8_MAX) {
    ++g_note_hold_count[note];
  }

  voice = fm_synth_find_voice_for_note(note);
  if (voice != NULL) {
    ++g_same_note_retrigger_count;
    fm_synth_retrigger_voice(voice, note, velocity);
    return;
  }

  voice = fm_synth_allocate_voice(note);
  fm_synth_start_voice(voice, note, velocity);
}

void fm_synth_note_off(uint8_t note) {
  if (note >= FM_NOTE_COUNT) {
    return;
  }

  if (g_note_hold_count[note] == 0u) {
    return;
  }

  --g_note_hold_count[note];
  if (g_note_hold_count[note] != 0u) {
    return;
  }

  for (size_t i = 0; i < FM_SYNTH_VOICE_COUNT; ++i) {
    FmVoice *voice = &g_voices[i];

    if (voice->active && voice->note == note) {
      fm_synth_release_voice(voice);
    }
  }
}

void fm_synth_all_notes_off(void) {
  memset(g_note_hold_count, 0, sizeof(g_note_hold_count));

  for (size_t i = 0; i < FM_SYNTH_VOICE_COUNT; ++i) {
    if (g_voices[i].active) {
      fm_synth_release_voice(&g_voices[i]);
    }
  }
}

void fm_synth_panic(void) {
  memset(g_voices, 0, sizeof(g_voices));
  memset(g_note_hold_count, 0, sizeof(g_note_hold_count));
}

void fm_synth_get_stats(FmSynthStats *stats) {
  if (stats == NULL) {
    return;
  }

  stats->voice_steal_count = g_voice_steal_count;
  stats->same_note_retrigger_count = g_same_note_retrigger_count;
  stats->output_clip_count = g_output_clip_count;
  stats->reverb_render_us = g_reverb_render_us;
  stats->active_voice_count = fm_synth_active_voice_count();
  stats->peak_active_voice_count = g_peak_active_voice_count;
}

void __not_in_flash_func(fm_synth_render_block)(int16_t *buffer, size_t samples) {
  if (buffer == NULL) {
    return;
  }

  // Allocate temporary stereo buffer for reverb processing
  // We process in blocks for efficiency
  int16_t stereo_left[FM_DX7_BLOCK_SAMPLES];
  int16_t stereo_right[FM_DX7_BLOCK_SAMPLES];
  
  // Convert wet level (0-255) to Q15 (0-0x7FFF)
  int16_t wet_q15 = (int16_t)(((int32_t)g_reverb_wet * REVERB_Q15_ONE) / 255);
  
  for (size_t block_start = 0; block_start < samples;
       block_start += FM_DX7_BLOCK_SAMPLES) {
    int32_t mix_block[FM_DX7_BLOCK_SAMPLES];
    bool mix_initialized = false;
    uint8_t active_voice_count = 0u;
    uint16_t target_mix_gain_q8;
    const size_t remaining = samples - block_start;
    const size_t copy_count = remaining < FM_DX7_BLOCK_SAMPLES
                                ? remaining
                                : FM_DX7_BLOCK_SAMPLES;

    for (size_t voice_index = 0; voice_index < FM_SYNTH_VOICE_COUNT; ++voice_index) {
      if (g_voices[voice_index].active) {
        ++active_voice_count;
      }
    }

    if (active_voice_count > g_peak_active_voice_count) {
      g_peak_active_voice_count = active_voice_count;
    }

    target_mix_gain_q8 = active_voice_count == 0u
                             ? 0u
                             : (uint16_t)(((uint16_t)g_voice_level *
                                           (uint16_t)k_voice_normalization_q8
                                               [active_voice_count]) >>
                                          8);

    for (size_t voice_index = 0; voice_index < FM_SYNTH_VOICE_COUNT; ++voice_index) {
      FmVoice *voice = &g_voices[voice_index];
      const FmPatch *patch = voice->patch;
      FmDx7OpParams params[FM_SYNTH_OPERATOR_COUNT];
      int32_t voice_block[FM_DX7_BLOCK_SAMPLES];

      if (!voice->active) {
        continue;
      }

      if (patch == NULL) {
        patch = g_current_patch;
      }

      for (size_t op = 0; op < FM_SYNTH_OPERATOR_COUNT; ++op) {
        params[op].phase = voice->op[op].phase;
        params[op].freq = voice->op[op].phase_step;
        params[op].gain[0] = voice->op[op].gain_q24;
        params[op].gain[1] =
            fm_dx7_exp2_lookup(fm_dx7_env_get_sample(&voice->op[op].env, &patch->op[op]) -
                              (14 * (1 << 24)));
      }

      fm_dx7_core_compute(voice_block, params, patch->algorithm,
                          voice->feedback_buffer, fm_synth_feedback_shift(patch));

      if (!mix_initialized) {
        for (size_t i = 0; i < copy_count; ++i) {
          mix_block[i] = voice_block[i];
        }
        mix_initialized = true;
      } else {
        for (size_t i = 0; i < copy_count; ++i) {
          mix_block[i] += voice_block[i];
        }
      }

      for (size_t op = 0; op < FM_SYNTH_OPERATOR_COUNT; ++op) {
        voice->op[op].phase = params[op].phase;
        voice->op[op].gain_q24 = params[op].gain[1];
      }

      if (fm_synth_voice_is_finished(voice)) {
        memset(voice, 0, sizeof(*voice));
      }
    }

    // If no voice contributed, downmix from silence.
    if (!mix_initialized) {
      memset(mix_block, 0, copy_count * sizeof(int32_t));
    }

    // Convert to stereo (center mono)
    for (size_t i = 0; i < copy_count; ++i) {
      const int32_t gain_q24 =
          ((int32_t)g_mix_gain_q8 << 16) +
          (copy_count > 1u
               ? (((int32_t)target_mix_gain_q8 - (int32_t)g_mix_gain_q8) << 16) *
                     (int32_t)i /
                     (int32_t)(copy_count - 1u)
               : 0);
      const int32_t sample =
          (int32_t)((((int64_t)mix_block[i] * (int64_t)gain_q24) >> 24) *
                    (int64_t)g_master_level >>
                    (FM_DX7_Q24_TO_S16_SHIFT + FM_MIX_SHIFT));

      if (sample > 32767 || sample < -32768) {
        ++g_output_clip_count;
      }

      int16_t dry_sample = clamp_s16(sample);
      stereo_left[i] = dry_sample;   // Dry left = center
      stereo_right[i] = dry_sample;  // Dry right = center
    }

    // Apply reverb if wet amount > 0 and reverb is enabled (timed for perf reporting)
    if (wet_q15 > 0 && reverb_is_enabled()) {
      const uint32_t reverb_start_us = time_us_32();
      reverb_process_stereo(stereo_left, stereo_right,
                           stereo_left, stereo_right,
                           (int)copy_count, wet_q15);
      g_reverb_render_us += time_us_32() - reverb_start_us;
    }

    // Pack interleaved stereo output
    for (size_t i = 0; i < copy_count; ++i) {
      // block_start is a frame index; multiply by 2 to get the interleaved position
      buffer[(block_start + i) * 2] = stereo_left[i];
      buffer[(block_start + i) * 2 + 1] = stereo_right[i];
    }

    g_mix_gain_q8 = target_mix_gain_q8;
  }
}

#include "audio_output.h"
#include "audio_i2s_pcm5102.pio.h"
#include "hardware/clocks.h"
#include "pico/critical_section.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico_gpio.h"

#define AUDIO_OUTPUT_BUFFER_COUNT 4u

static const uint k_i2s_data_pin = AUDIO_SDATA;
static const uint k_i2s_clock_pin_base = AUDIO_BCLK;
static uint g_dma_channel;
static uint g_pio_offset;
static AudioOutputLevel g_audio_level = AUDIO_OUTPUT_LEVEL_HEADPHONES;
static volatile uint8_t g_active_buffer;
static volatile uint8_t g_ready_mask;
static volatile uint8_t g_filling_mask;
static volatile uint32_t g_dma_starvation_count;
static critical_section_t g_buffer_state_lock;
static volatile AudioOutputRenderRequestCallback g_render_request_callback;
static int16_t g_mix_buffers[AUDIO_OUTPUT_BUFFER_COUNT][AUDIO_OUTPUT_BUFFER_SAMPLES];
static uint32_t g_dma_buffers[AUDIO_OUTPUT_BUFFER_COUNT][AUDIO_OUTPUT_STEREO_FRAME_COUNT];

static void audio_output_fill_silence(uint slot) {
  // Fill with silence: both L and R samples set to 0
  for (size_t i = 0; i < AUDIO_OUTPUT_STEREO_FRAME_COUNT; ++i) {
    g_mix_buffers[slot][i * 2] = 0;
    g_mix_buffers[slot][i * 2 + 1] = 0;
    g_dma_buffers[slot][i] = 0;
  }
}

static void audio_output_pack_buffer(uint slot) {
  // Handle interleaved stereo: pack L and R samples into 32-bit words
  for (size_t i = 0; i < AUDIO_OUTPUT_STEREO_FRAME_COUNT; ++i) {
    uint16_t left = (uint16_t)g_mix_buffers[slot][i * 2];
    uint16_t right = (uint16_t)g_mix_buffers[slot][i * 2 + 1];
    g_dma_buffers[slot][i] = ((uint32_t)right << 16u) | (uint32_t)left;
  }
}

static int audio_output_find_slot(const int16_t *buffer) {
  for (uint i = 0; i < AUDIO_OUTPUT_BUFFER_COUNT; ++i) {
    if (buffer == g_mix_buffers[i]) {
      return (int)i;
    }
  }
  return -1;
}

static int audio_output_take_ready_slot_locked(void) {
  const uint8_t ready_mask = g_ready_mask;

  for (uint i = 0; i < AUDIO_OUTPUT_BUFFER_COUNT; ++i) {
    const uint8_t slot_mask = (uint8_t)(1u << i);
    if (ready_mask & slot_mask) {
      g_ready_mask &= (uint8_t)~slot_mask;
      return (int)i;
    }
  }

  return -1;
}

static void audio_output_dma_irq_handler(void) {
  const uint32_t dma_mask = 1u << g_dma_channel;
  uint completed_slot;
  int next_slot;
  AudioOutputRenderRequestCallback render_request_callback;

  if ((dma_hw->ints0 & dma_mask) == 0u) {
    return;
  }

  dma_hw->ints0 = dma_mask;

  critical_section_enter_blocking(&g_buffer_state_lock);
  completed_slot = g_active_buffer;
  next_slot = audio_output_take_ready_slot_locked();

  if (next_slot >= 0) {
    g_active_buffer = (uint8_t)next_slot;
  } else {
    ++g_dma_starvation_count;
    g_active_buffer = (uint8_t)completed_slot;
  }
  render_request_callback = g_render_request_callback;
  critical_section_exit(&g_buffer_state_lock);

  if (next_slot >= 0) {
    dma_channel_transfer_from_buffer_now(g_dma_channel, g_dma_buffers[next_slot],
                                         AUDIO_OUTPUT_STEREO_FRAME_COUNT);
  } else {
    audio_output_fill_silence(completed_slot);
    dma_channel_transfer_from_buffer_now(g_dma_channel,
                                         g_dma_buffers[completed_slot],
                                         AUDIO_OUTPUT_STEREO_FRAME_COUNT);
  }

  if (render_request_callback != NULL) {
    render_request_callback();
  }
}

void audio_output_init(void) {
  uint i2s_offset;
  uint32_t divider;
  dma_channel_config dma_config;

  gpio_set_function(k_i2s_data_pin, GPIO_FUNC_PIO0);
  gpio_set_function(k_i2s_clock_pin_base, GPIO_FUNC_PIO0);
  gpio_set_function(k_i2s_clock_pin_base + 1u, GPIO_FUNC_PIO0);

  pio_sm_claim(AUDIO_PIO, AUDIO_SM);

  i2s_offset = pio_add_program(AUDIO_PIO, &audio_i2s_pcm5102_program);
  g_pio_offset = i2s_offset;
  audio_i2s_pcm5102_program_init(AUDIO_PIO, AUDIO_SM, i2s_offset,
                                 k_i2s_data_pin, k_i2s_clock_pin_base);
  // Apply the default output level by patching the SET Y immediates in PIO
  // instruction memory. Safe to do before the SM is enabled.
  audio_output_set_level(g_audio_level);

  /* With the picoTracker's 220.5 MHz system clock this produces the exact
   * 78.125 PIO divider required for 44.1 kHz stereo output. */
  divider = clock_get_hz(clk_sys) * 2u / AUDIO_OUTPUT_SAMPLE_RATE_HZ;
  pio_sm_set_clkdiv_int_frac(AUDIO_PIO, AUDIO_SM, divider >> 8u,
                             divider & 0xffu);

  pio_sm_set_enabled(AUDIO_PIO, AUDIO_SM, true);

  for (uint i = 0; i < AUDIO_OUTPUT_BUFFER_COUNT; ++i) {
    audio_output_fill_silence(i);
  }

  critical_section_init(&g_buffer_state_lock);

  g_dma_channel = (uint)dma_claim_unused_channel(true);
  dma_config = dma_channel_get_default_config(g_dma_channel);
  channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
  channel_config_set_read_increment(&dma_config, true);
  channel_config_set_write_increment(&dma_config, false);
  channel_config_set_dreq(&dma_config, pio_get_dreq(AUDIO_PIO, AUDIO_SM, true));

  dma_channel_configure(g_dma_channel, &dma_config, &AUDIO_PIO->txf[AUDIO_SM],
                        g_dma_buffers[0], AUDIO_OUTPUT_STEREO_FRAME_COUNT, false);

  g_active_buffer = 0u;
  g_ready_mask = 0u;
  g_filling_mask = 0u;
  g_dma_starvation_count = 0u;
  g_render_request_callback = NULL;

  irq_set_exclusive_handler(DMA_IRQ_0, audio_output_dma_irq_handler);
  dma_channel_set_irq0_enabled(g_dma_channel, true);
  irq_set_enabled(DMA_IRQ_0, true);
  dma_channel_start(g_dma_channel);
}

size_t audio_output_buffer_count(void) { return AUDIO_OUTPUT_BUFFER_COUNT; }

size_t audio_output_buffer_samples(void) { return AUDIO_OUTPUT_BUFFER_SAMPLES; }

bool audio_output_try_acquire_buffer(int16_t **buffer) {
  uint8_t unavailable_mask;

  if (buffer == NULL) {
    return false;
  }

  critical_section_enter_blocking(&g_buffer_state_lock);
  unavailable_mask = (uint8_t)(g_ready_mask | g_filling_mask | (1u << g_active_buffer));

  for (uint i = 0; i < AUDIO_OUTPUT_BUFFER_COUNT; ++i) {
    const uint8_t slot_mask = (uint8_t)(1u << i);
    if ((unavailable_mask & slot_mask) == 0u) {
      g_filling_mask |= slot_mask;
      critical_section_exit(&g_buffer_state_lock);
      *buffer = g_mix_buffers[i];
      return true;
    }
  }

  critical_section_exit(&g_buffer_state_lock);
  return false;
}

void audio_output_submit_buffer(int16_t *buffer) {
  const int slot = audio_output_find_slot(buffer);

  if (slot < 0) {
    return;
  }

  audio_output_pack_buffer((uint)slot);

  critical_section_enter_blocking(&g_buffer_state_lock);
  g_filling_mask &= (uint8_t)~(1u << slot);
  g_ready_mask |= (uint8_t)(1u << slot);
  critical_section_exit(&g_buffer_state_lock);
}

void audio_output_get_stats(AudioOutputStats *stats) {
  if (stats == NULL) {
    return;
  }

  critical_section_enter_blocking(&g_buffer_state_lock);
  stats->dma_starvation_count = g_dma_starvation_count;
  critical_section_exit(&g_buffer_state_lock);
}

void audio_output_set_render_request_callback(
    AudioOutputRenderRequestCallback callback) {
  critical_section_enter_blocking(&g_buffer_state_lock);
  g_render_request_callback = callback;
  critical_section_exit(&g_buffer_state_lock);
}

// SET Y, value with sideset bits — encoded for the I2S PIO program's specific
// sideset values at instruction indices 3, 9, 15, 21. See
// audio_i2s_pcm5102.pio: indices 3 and 21 use side 0b11; indices 9 and 15 use
// side 0b01. Layout: 111(SET) | side(2) | delay(3) | dst=Y(010) | value(5).
static inline uint16_t set_y_side11(uint8_t value) {
  return (uint16_t)(0xf840u | (value & 0x1fu));
}
static inline uint16_t set_y_side01(uint8_t value) {
  return (uint16_t)(0xe840u | (value & 0x1fu));
}

void audio_output_set_level(AudioOutputLevel level) {
  uint8_t offset_count;
  uint8_t backfill_count;

  switch (level) {
  case AUDIO_OUTPUT_LEVEL_LINE:
    offset_count = 1u;   // less MSB sign-extension = louder
    backfill_count = 12u;
    break;
  case AUDIO_OUTPUT_LEVEL_HEADPHONES:
  default:
    offset_count = 3u;
    backfill_count = 10u;
    break;
  }

  // Patch the four SET Y immediates in PIO instruction memory. Writes are
  // single-word and the SM will pick up the new immediate the next time it
  // executes that PC — at worst one frame transition of mismatched padding.
  AUDIO_PIO->instr_mem[g_pio_offset + 3u]  = set_y_side11(backfill_count);
  AUDIO_PIO->instr_mem[g_pio_offset + 9u]  = set_y_side01(offset_count);
  AUDIO_PIO->instr_mem[g_pio_offset + 15u] = set_y_side01(backfill_count);
  AUDIO_PIO->instr_mem[g_pio_offset + 21u] = set_y_side11(offset_count);

  g_audio_level = level;
}

AudioOutputLevel audio_output_get_level(void) { return g_audio_level; }

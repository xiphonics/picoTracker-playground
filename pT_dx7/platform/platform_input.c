#include "platform_input.h"

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico_gpio.h"

_Static_assert(INPUT_LEFT == 8, "picoTracker button scan expects GPIO8..16");
_Static_assert(INPUT_PLAY == 16, "picoTracker button scan expects GPIO8..16");

enum {
  PLATFORM_INPUT_DEBOUNCE_US = 15000u,
};

void platform_input_init_buttons(void) {
  const uint buttons[] = {INPUT_LEFT, INPUT_DOWN, INPUT_RIGHT, INPUT_UP,
                          INPUT_ALT,  INPUT_EDIT, INPUT_ENTER, INPUT_NAV,
                          INPUT_PLAY};

  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
    gpio_init(buttons[i]);
    gpio_set_dir(buttons[i], GPIO_IN);
    gpio_pull_up(buttons[i]);
  }
}

uint16_t platform_input_scan_keys(void) {
  const uint32_t button_mask = (uint32_t)0x1FFu << INPUT_LEFT;
  static uint16_t last_raw_keys = 0u;
  static uint16_t debounced_keys = 0u;
  static uint32_t last_change_us = 0u;
  const uint16_t raw_keys =
      (uint16_t)((~gpio_get_all() & button_mask) >> INPUT_LEFT);
  const uint32_t now_us = time_us_32();

  if (raw_keys != last_raw_keys) {
    last_raw_keys = raw_keys;
    last_change_us = now_us;
  }

  if (debounced_keys != last_raw_keys &&
      (uint32_t)(now_us - last_change_us) >= PLATFORM_INPUT_DEBOUNCE_US) {
    debounced_keys = last_raw_keys;
  }

  return debounced_keys;
}

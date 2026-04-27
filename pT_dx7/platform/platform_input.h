#ifndef DX_2350_PLATFORM_INPUT_H
#define DX_2350_PLATFORM_INPUT_H

#include <stdint.h>

void platform_input_init_buttons(void);
uint16_t platform_input_scan_keys(void);

#endif

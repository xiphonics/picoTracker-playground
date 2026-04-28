//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2021-2022 Graham Sanderson
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Main program, simply calls D_DoomMain high level loop.
//

#include "config.h"

#include <stdlib.h>
#include <stdio.h>

#if !LIB_PICO_STDLIB
#include "SDL.h"
#else
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/sem.h"
#include "pico/multicore.h"
#if PICO_ON_DEVICE
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#endif
#endif
#if USE_PICO_NET
#include "piconet.h"
#endif
#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#if PICO_RP2350
#include "hardware/structs/qmi.h"
#endif

//
// D_DoomMain()
// Not a globally visible function, just included for source reference,
// calls all startup code, parses command line options.
//

void D_DoomMain (void);

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
#if defined(PICO_AUDIO_I2S_DATA_PIN) && defined(PICO_AUDIO_I2S_CLOCK_PIN_BASE)
bi_decl(bi_3pins_with_names(PICO_AUDIO_I2S_DATA_PIN, "I2S DIN", PICO_AUDIO_I2S_CLOCK_PIN_BASE, "I2S BCK", PICO_AUDIO_I2S_CLOCK_PIN_BASE+1, "I2S LRCK"));
#endif
#endif

int main(int argc, char **argv)
{
    // save arguments
#if !NO_USE_ARGS
    myargc = argc;
    myargv = argv;
#endif
#if PICO_ON_DEVICE
#if PICO_RP2350
    uint clkdiv = 3;
    uint rxdelay = 2;
    hw_write_masked(
            &qmi_hw->m[0].timing,
            ((clkdiv << QMI_M0_TIMING_CLKDIV_LSB) & QMI_M0_TIMING_CLKDIV_BITS) |
            ((rxdelay << QMI_M0_TIMING_RXDELAY_LSB) & QMI_M0_TIMING_RXDELAY_BITS),
            QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS
    );
#endif
    // picoTracker bring-up: avoid forcing a high core voltage while testing
    // conservative clocks, and don't apply Pico board SMPS assumptions.
#if !PICOTRACKER
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    busy_wait_us(1000);
#endif
    // todo pause? is this the cause of the cold start issue?
#if PICOTRACKER
    // picoTracker uses the SDK runtime clock init from SYS_CLK_HZ.
#else
    set_sys_clock_khz(270000, true);
#endif
#if !USE_PICO_NET
    // debug ?
//    gpio_debug_pins_init();
#endif
#ifdef PICO_SMPS_MODE_PIN
#if !PICOTRACKER
    gpio_init(PICO_SMPS_MODE_PIN);
    gpio_set_dir(PICO_SMPS_MODE_PIN, GPIO_OUT);
    gpio_put(PICO_SMPS_MODE_PIN, 1);
#endif
#endif
#endif
#if LIB_PICO_STDIO
#if !PICOTRACKER
    stdio_init_all();
#endif
#endif
#if PICO_BUILD
    I_Init();
#endif
#if USE_PICO_NET
    // do init early to set pulls
    piconet_init();
#endif
//!
    // Print the program version and exit.
    //
    if (M_ParmExists("-version") || M_ParmExists("--version")) {
        puts(PACKAGE_STRING);
        exit(0);
    }

#if !NO_USE_ARGS
    M_FindResponseFile();
#endif

    #ifdef SDL_HINT_NO_SIGNAL_HANDLERS
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    #endif

    // start doom
    D_DoomMain ();

    return 0;
}

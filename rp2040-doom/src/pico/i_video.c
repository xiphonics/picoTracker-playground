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
//	DOOM graphics stuff for Pico.
//

#if PICODOOM_RENDER_NEWHOPE
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <doom/r_data.h>
#include "doom/f_wipe.h"
#include "pico.h"

#include "config.h"
#include "d_loop.h"
#include "deh_str.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_diskicon.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#if PICOTRACKER
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#else
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "video_doom.pio.h"
#endif
#include "pico/multicore.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "picodoom.h"
#include "image_decoder.h"
#if PICO_ON_DEVICE
#include "hardware/dma.h"
#include "hardware/structs/xip_ctrl.h"
#endif

#define YELLOW_SUBMARINE 0
#if PICOTRACKER
// picoTracker ST7789 LCD on SPI1
#define LCD_SPI        spi1
#define LCD_CS_PIN     20
#define LCD_DC_PIN     21
#define LCD_RST_PIN    22
#define LCD_BL_PIN     23
#define LCD_SCK_PIN    26
#define LCD_MOSI_PIN   27
#define LCD_WIDTH      320
#define LCD_HEIGHT     240
#define LCD_LETTERBOX  ((LCD_HEIGHT - SCREENHEIGHT) / 2)   // 20 rows top & bottom

// ST7789 expects big-endian RGB565; store palette byte-swapped so 8-bit DMA
// sends bytes in the correct order (high byte first).
#undef PICO_SCANVIDEO_PIXEL_FROM_RGB8
#define PICO_SCANVIDEO_PIXEL_FROM_RGB8(r, g, b) \
    __builtin_bswap16((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static int lcd_dma_channel;
// Double-buffered line scratch: while DMA sends one line the CPU converts the next
static uint32_t __aligned(4) lcd_line_buf[2][SCREENWIDTH / 2];

#define PICOTRACKER_LCD_TEST_PATTERN 0
#define PICOTRACKER_LCD_DEBUG_RAW_FB 0

#define SUPPORT_TEXT 0
#else
#define SUPPORT_TEXT 1
#endif
#if SUPPORT_TEXT
typedef struct __packed {
    const char * const name;
    const uint8_t * const data;
    const uint8_t w;
    const uint8_t h;
} txt_font_t;
#define TXT_SCREEN_W 80
#include "fonts/normal.h"

static uint16_t ega_colors[] = {
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0x00, 0x00),         // 0: Black
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0x00, 0xa8),         // 1: Blue
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0xa8, 0x00),         // 2: Green
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0xa8, 0xa8),         // 3: Cyan
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xa8, 0x00, 0x00),         // 4: Red
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xa8, 0x00, 0xa8),         // 5: Magenta
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xa8, 0x54, 0x00),         // 6: Brown
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xa8, 0xa8, 0xa8),         // 7: Grey
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x54, 0x54, 0x54),         // 8: Dark grey
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x54, 0x54, 0xfe),         // 9: Bright blue
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x54, 0xfe, 0x54),         // 10: Bright green
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x54, 0xfe, 0xfe),         // 11: Bright cyan
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xfe, 0x54, 0x54),         // 12: Bright red
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xfe, 0x54, 0xfe),         // 13: Bright magenta
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xfe, 0xfe, 0x54),         // 14: Yellow
    PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xfe, 0xfe, 0xfe),         // 15: Bright white
};
#endif

// todo temproarly turned this off because it causes a seeming bug in scanvideo (perhaps only with the new callback stuff) where the last repeated scanline of a pixel line is freed while shown
//  note it may just be that this happens anyway, but usually we are writing slower than the beam?
#define USE_INTERP PICO_ON_DEVICE
#if USE_INTERP
#include "hardware/interp.h"
#endif

CU_REGISTER_DEBUG_PINS(scanline_copy)
//CU_SELECT_DEBUG_PINS(scanline_copy)

static const patch_t *stbar;

volatile uint8_t interp_in_use;

// display has been set up?

static boolean initialized = false;

boolean screenvisible = true;

//int vga_porch_flash = false;

//static int startup_delay = 1000;

// The screen buffer; this is modified to draw things to the screen
//pixel_t *I_VideoBuffer = NULL;
// Gamma correction level to use

boolean screensaver_mode = false;

isb_int8_t usegamma = 0;

// Joystick/gamepad hysteresis
unsigned int joywait = 0;

pixel_t *I_VideoBuffer; // todo can't have this

uint8_t __aligned(4) frame_buffer[2][SCREENWIDTH*MAIN_VIEWHEIGHT];
static uint16_t palette[256];
static uint16_t __scratch_x("shared_pal") shared_pal[NUM_SHARED_PALETTES][16];
static int8_t next_pal=-1;

semaphore_t render_frame_ready, display_frame_freed;
semaphore_t core1_launch;

#if !PICOTRACKER
#define USE_1280x1024x60 1

#if SUPPORT_TEXT
uint8_t *text_screen_data;
static uint32_t *text_scanline_buffer_start;
static uint8_t *text_screen_cpy;
static uint8_t *text_font_cpy;
#endif

#if USE_1280x1024x60
//static uint32_t missing_scanline_data[] = {
//        video_doom_offset_raw_1p | (0 << 16u),
//        video_doom_offset_end_of_scanline_skip_ALIGN
//};

static uint32_t missing_scanline_data[] =
        {
#if YELLOW_SUBMARINE
                video_doom_offset_color_run | (PICO_SCANVIDEO_PIXEL_FROM_RGB8(255,255,0) << 16u),
                120 | (video_doom_offset_raw_1p << 16u),
#endif
                0u | (video_doom_offset_end_of_scanline_ALIGN << 16u)
        };

#if PICO_ON_DEVICE
bool video_doom_adapt_for_mode(const struct scanvideo_pio_program *program, const struct scanvideo_mode *mode,
                               struct scanvideo_scanline_buffer *missing_scanvideo_scanline_buffer, uint16_t *modifiable_instructions);
pio_sm_config video_doom_configure_pio(pio_hw_t *pio, uint sm, uint offset);
#endif
#define VIDEO_DOOM_PROGRAM_NAME "doom"
const struct scanvideo_pio_program video_doom = {
#if PICO_ON_DEVICE
        .program = &video_doom_program,
        .adapt_for_mode = video_doom_adapt_for_mode,
        .configure_pio = video_doom_configure_pio,
#else
        .id = VIDEO_DOOM_PROGRAM_NAME
#endif
};

const scanvideo_timing_t vga_timing_1280x1000_60_default = // same as 1280x1024_60 standard just with some 12 blank lines at the top and bottom
        {
                .clock_freq = 108000000,

                .h_active = 1280,
                .v_active = 1024 - 24,

                .h_front_porch = 48,
                .h_pulse = 112,
                .h_total = 1688,
                .h_sync_polarity = 0,

                .v_front_porch = 1 + 24 - 12, // center our slightly short screen
                .v_pulse = 3,
                .v_total = 1066,
                .v_sync_polarity = 0,
        };

const scanvideo_timing_t vga_timing_640x1000_60_default = // same as 1280x1024_60 standard just with some 12 blank lines at the top and bottom
        {
                .clock_freq = 108000000 / 2,

#if PICO_ON_DEVICE
                .h_active = 1280 / 2,
#else
                .h_active = 1280,
#endif
                .v_active = 1024 - 24,

                .h_front_porch = 48 / 2,
                .h_pulse = 112 / 2,
                .h_total = 1688 / 2,
                .h_sync_polarity = 0,

                .v_front_porch = 1 + 24 - 12, // center our slightly short screen
                .v_pulse = 3,
                .v_total = 1066,
                .v_sync_polarity = 0,
        };

const scanvideo_mode_t vga_mode_320x200 =
        {
                .default_timing = &vga_timing_640x1000_60_default,
                .pio_program = &video_doom,
#if PICO_ON_DEVICE
                .width = 320,
#else
                .width = 640,
#endif
                .height = 200,
                .xscale = 2,
                .yscale = 5,
        };
#define VGA_MODE vga_mode_320x200
#elif USE_320x240x60
#define VGA_MODE vga_mode_320x240_60
#else
const scanvideo_mode_t vga_mode_320x200_60 =
        {
                .default_timing = &vga_timing_1280x1024_60_default,
                .pio_program = &video_24mhz_composable,
                .width = 320,
                .height = 204,
                .xscale = 4,
                .yscale = 5,
        };

#define VGA_MODE vga_mode_320x200_60
#endif
#endif // !PICOTRACKER

#if USE_INTERP
static interp_hw_save_t interp0_save, interp1_save;
static boolean interp_updated;
static boolean need_save;

static inline void interp_save_static(interp_hw_t *interp, interp_hw_save_t *saver) {
    saver->accum[0] = interp->accum[0];
    saver->accum[1] = interp->accum[1];
    saver->base[0] = interp->base[0];
    saver->base[1] = interp->base[1];
    saver->base[2] = interp->base[2];
    saver->ctrl[0] = interp->ctrl[0];
    saver->ctrl[1] = interp->ctrl[1];
}

static inline void interp_restore_static(interp_hw_t *interp, interp_hw_save_t *saver) {
    interp->accum[0] = saver->accum[0];
    interp->accum[1] = saver->accum[1];
    interp->base[0] = saver->base[0];
    interp->base[1] = saver->base[1];
    interp->base[2] = saver->base[2];
    interp->ctrl[0] = saver->ctrl[0];
    interp->ctrl[1] = saver->ctrl[1];
}
#endif

void I_ShutdownGraphics(void)
{
}

//
// I_StartFrame
//
void I_StartFrame (void)
{
    // er?
}

//
// Set the window title
//

void I_SetWindowTitle(const char *title)
{
//    window_title = title;
}

//
// I_SetPalette
//
void I_SetPaletteNum(int doompalette)
{
    next_pal = doompalette;
}

//
// I_FinishUpdate
//
void I_FinishUpdate (void)
{
}

uint8_t display_frame_index;
uint8_t display_overlay_index;
uint8_t display_video_type;

typedef void (*scanline_func)(uint32_t *dest, int scanline);

static void scanline_func_none(uint32_t *dest, int scanline);
static void scanline_func_double(uint32_t *dest, int scanline);
static void scanline_func_single(uint32_t *dest, int scanline);
static void scanline_func_wipe(uint32_t *dest, int scanline);

scanline_func scanline_funcs[] = {
        scanline_func_none,     // VIDEO_TYPE_NONE
        NULL,                   // VIDEO_TYPE_TEXT
        scanline_func_single,   // VIDEO_TYPE_SAVING
        scanline_func_double,   // VIDEO_TYPE_DOUBLE
        scanline_func_single,   // VIDEO_TYPE_SINGLE
        scanline_func_wipe,     // VIDEO_TYPE_WIPE
};

uint8_t *wipe_yoffsets; // position of start of y in each column
int16_t *wipe_yoffsets_raw;
uint32_t *wipe_linelookup; // offset of each line from start of screenbuffer (can be negative for FB 1 to FB 0)
uint8_t next_video_type;
uint8_t next_frame_index; // todo combine with video type?
uint8_t next_overlay_index;
#if !DEMO1_ONLY
uint8_t *next_video_scroll;
uint8_t *video_scroll;
#endif
volatile uint8_t wipe_min;
#if !PICOTRACKER
uint32_t *saved_scanline_buffer_ptrs[PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT];
#endif

#pragma GCC push_options
#if PICO_ON_DEVICE
#pragma GCC optimize("O3")
#endif

static inline void palette_convert_scanline(uint32_t *dest, const uint8_t *src) {
#if USE_INTERP
    if (interp_updated != 1) {
                if (need_save) {
                    interp_save_static(interp0, &interp0_save);
                    interp_save_static(interp1, &interp1_save);
                }
                interp_config c = interp_default_config();
                interp_config_set_shift(&c, 0);
                interp_config_set_mask(&c, 0, 7);
                interp_set_config(interp0, 0, &c);
                interp_config_set_shift(&c, 16);
                interp_set_config(interp1, 0, &c);
                interp_config_set_shift(&c, 8);
                interp_config_set_cross_input(&c, true);
                interp_set_config(interp0, 1, &c);
                interp_config_set_shift(&c, 24);
                interp_set_config(interp1, 1, &c);
                uint32_t palette_div2 = ((uintptr_t)palette) >> 1;
                interp0->base[0] = palette_div2;
                interp0->base[1] = palette_div2;
                interp1->base[0] = palette_div2;
                interp1->base[1] = palette_div2;
                interp_updated = 1;
            }
            extern void palette8to16(uint32_t *dest, const uint8_t *src, uint words);
            palette8to16(dest, src, SCREENWIDTH);
//            dest[4] = (255-scanline) * 0x2000;
            dest += SCREENWIDTH / 2;
//            dest[-4] = (255-scanline) * 0x10001;
#else
    for (int i = 0; i < SCREENWIDTH; i += 2) {
        uint32_t val = palette[*src++];
        val |= (palette[*src++]) << 16;
        *dest++ = val;
    }
#endif
}
static void scanline_func_none(uint32_t *dest, int scanline) {
    memset(dest, 0, SCREENWIDTH * 2);
}

#if SUPPORT_TEXT
void check_text_buffer(scanvideo_scanline_buffer_t *buffer) {
#if PICO_ON_DEVICE
    if (buffer->data < text_scanline_buffer_start || buffer->data >= text_scanline_buffer_start + TEXT_SCANLINE_BUFFER_TOTAL_WORDS) {
        // is an original scanvideo allocated buffer, we need to use a larger one
        int i;
        for(i=0;i<PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT;i++) {
            if (!saved_scanline_buffer_ptrs[i]) break;
        }
        assert(i<PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT);
        saved_scanline_buffer_ptrs[i] = buffer->data;
        buffer->data = text_scanline_buffer_start + i * TEXT_SCANLINE_BUFFER_WORDS;
    }
#endif
}

static void finish_text_buffer(scanvideo_scanline_buffer_t *buffer) {
    uint16_t * p = (uint16_t *)buffer->data;
    p[0] = video_doom_offset_raw_run_half;
    p[1] = p[2];
    p[2] = SCREENWIDTH*2 - 3;
    buffer->data[SCREENWIDTH + 1] = video_doom_offset_raw_1p;
    buffer->data[SCREENWIDTH + 2] = video_doom_offset_end_of_scanline_skip_ALIGN;
    buffer->data_used = SCREENWIDTH + 3;
}

static void __not_in_flash_func(render_text_mode_half_scanline)(scanvideo_scanline_buffer_t *buffer, const uint8_t *text_data, int yoffset) {
    uint16_t * p = (uint16_t *)(buffer->data + 1);
//    memset(buffer->data + 1, 0, 1280);
//    uint x = scanline * 2 + yoffset;
//    buffer->data[1 + (x/2)] = x&1 ? 0xffff0000 : 0xffff;
#if 1
    uint blink = scanvideo_frame_number(buffer->scanline_id) & 16;
    // not going to change so just hard code
//    assert(normal_font.w == 8);
//    assert(normal_font.h == 16);
    const uint8_t *font_base = text_font_cpy + yoffset;
    for(uint i=0;i<80;i++) {
        uint fg = text_data[1] & 0xf;
        uint bg = (text_data[1] >> 4) & 0xf;
        if (bg & 0x8) {
            bg &= ~0x8;
            // blinking
            if (blink) fg = bg;
        }
        // probably user error but this wasn't working correctly with the inline asm on the stack
        static uint16_t colors[2];
        colors[0] = ega_colors[bg];
        colors[1] = ega_colors[fg];
        uint bits8 = font_base[text_data[0] * 16];
#if PICO_ON_DEVICE
        // todo use interpolator?
        uint tmp1, tmp2, tmp3;
        __asm__ volatile (
            ".syntax unified\n"

            "movs %[r_tmp3], #2\n"
            "lsls %[r_tmp1],%[r_bits8],#1\n"
            "ands %[r_tmp1],%[r_tmp3]\n"
            "ldrh %[r_tmp1],[%[r_colors],%[r_tmp1]]\n"

            "movs %[r_tmp2],%[r_bits8]\n"
            "ands %[r_tmp2],%[r_tmp3]\n"
            "ldrh %[r_tmp2],[%[r_colors],%[r_tmp2]]\n"

            "lsls %[r_tmp2], #16\n"
            "orrs %[r_tmp1], %[r_tmp2]\n"
            "stmia %[r_p]!, {%[r_tmp1]}\n"

            "lsrs %[r_tmp1],%[r_bits8],#1\n"
            "ands %[r_tmp1],%[r_tmp3]\n"
            "ldrh %[r_tmp1],[%[r_colors],%[r_tmp1]]\n"

            "lsrs %[r_tmp2],%[r_bits8],#2\n"
            "ands %[r_tmp2],%[r_tmp3]\n"
            "ldrh %[r_tmp2],[%[r_colors],%[r_tmp2]]\n"

            "lsls %[r_tmp2], #16\n"
            "orrs %[r_tmp1], %[r_tmp2]\n"
            "stmia %[r_p]!, {%[r_tmp1]}\n"

            "lsrs %[r_tmp1],%[r_bits8],#3\n"
            "ands %[r_tmp1],%[r_tmp3]\n"
            "ldrh %[r_tmp1],[%[r_colors],%[r_tmp1]]\n"

            "lsrs %[r_tmp2],%[r_bits8],#4\n"
            "ands %[r_tmp2],%[r_tmp3]\n"
            "ldrh %[r_tmp2],[%[r_colors],%[r_tmp2]]\n"

            "lsls %[r_tmp2], #16\n"
            "orrs %[r_tmp1], %[r_tmp2]\n"
            "stmia %[r_p]!, {%[r_tmp1]}\n"

            "lsrs %[r_tmp1],%[r_bits8],#5\n"
            "ands %[r_tmp1],%[r_tmp3]\n"
            "ldrh %[r_tmp1],[%[r_colors],%[r_tmp1]]\n"

            "lsrs %[r_tmp2],%[r_bits8],#6\n"
            "ands %[r_tmp2],%[r_tmp3]\n"
            "ldrh %[r_tmp2],[%[r_colors],%[r_tmp2]]\n"

            "lsls %[r_tmp2], #16\n"
            "orrs %[r_tmp1], %[r_tmp2]\n"
            "stmia %[r_p]!, {%[r_tmp1]}\n"

        : [ r_p] "+l" (p),
              [ r_tmp1] "=&l" (tmp1),
              [ r_tmp2] "=&l" (tmp2),
              [ r_tmp3] "=&l" (tmp3)

            : [ r_bits8] "l" (bits8),
              [ r_colors] "l" (colors)
            :
        );
#else
        p[0] = colors[bits8&1];
        p[1] = colors[(bits8>>1)&1];
        p[2] = colors[(bits8>>2)&1];
        p[3] = colors[(bits8>>3)&1];
        p[4] = colors[(bits8>>4)&1];
        p[5] = colors[(bits8>>5)&1];
        p[6] = colors[(bits8>>6)&1];
        p[7] = colors[(bits8>>7)&1];
        p+=8;
#endif
        text_data+=2;
    }
#endif
}

static void __noinline render_text_mode_scanline(scanvideo_scanline_buffer_t *buffer, int scanline) {
#if 1
    const uint8_t *text_data = text_screen_data;
    assert(text_data);
    text_data += TXT_SCREEN_W * 2 * (scanline/8);
    check_text_buffer(buffer);
    render_text_mode_half_scanline(buffer, text_data, (scanline & 7u)*2 );
    finish_text_buffer(buffer);
    if (buffer->link) {
        buffer->link_after = 2;
        buffer->link->link_after = 0;
        check_text_buffer(buffer->link);
        render_text_mode_half_scanline(buffer->link, text_data, (scanline & 7u)*2 + 1);
        finish_text_buffer(buffer->link);
    }
#else
    uint16_t *p = (uint16_t *)buffer->data;
    p[0] = video_doom_offset_raw_run;
    p[1] = p[2];
    p[2] = SCREENWIDTH - 3;
    memset(buffer->data+1, 0x1f * ((scanline + 8) / 8), SCREENWIDTH * 2);
    if (buffer->link) {
        buffer->link_after = 2;
        scanvideo_scanline_buffer_t *buffer2  = buffer->link;
        memset(buffer2->data+1, 0xf1 * ((scanline + 8) / 8), SCREENWIDTH * 2);
        p = (uint16_t *)buffer2->data;
        p[0] = video_doom_offset_raw_run;
        p[1] = p[2];
        p[2] = SCREENWIDTH - 3;
        buffer2->data[SCREENWIDTH / 2 + 1] = video_doom_offset_raw_1p;
        buffer2->data[SCREENWIDTH / 2 + 2] = video_doom_offset_end_of_scanline_skip_ALIGN;
        buffer2->data_used = SCREENWIDTH / 2 + 3;
    }
    buffer->data[SCREENWIDTH / 2 + 1] = video_doom_offset_raw_1p;
    buffer->data[SCREENWIDTH / 2 + 2] = video_doom_offset_end_of_scanline_skip_ALIGN;
    buffer->data_used = SCREENWIDTH / 2 + 3;
#endif
}
#endif

static void __scratch_x("scanlines") scanline_func_double(uint32_t *dest, int scanline) {
    if (scanline < MAIN_VIEWHEIGHT) {
        const uint8_t *src = frame_buffer[display_frame_index] + scanline * SCREENWIDTH;
//        if (scanline == 100) {
//            printf("SL %d %p\n", display_frame_index, &frame_buffer[display_frame_index]);
//        }
        palette_convert_scanline(dest, src);
    } else {
        // we expect everything to be overdrawn by statusbar so we do nothing
    }
}

static void __not_in_flash_func(scanline_func_single)(uint32_t *dest, int scanline) {
    uint8_t *src;
    if (scanline < MAIN_VIEWHEIGHT) {
        src = frame_buffer[display_frame_index] + scanline * SCREENWIDTH;
    } else {
        src = frame_buffer[display_frame_index^1] + (scanline - 32) * SCREENWIDTH;
    }
#if !DEMO1_ONLY
    if (video_scroll) {
        for(int i=SCREENWIDTH-1;i>0;i--) {
            src[i] = src[i-1];
        }
        src[0] = video_scroll[scanline];
    }
#endif
    palette_convert_scanline(dest, src);
}

static void scanline_func_wipe(uint32_t *dest, int scanline) {
    const uint8_t *src;
#if 0
    if (scanline < MAIN_VIEWHEIGHT) {
        src = frame_buffer[display_frame_index^1] + scanline * SCREENWIDTH;
    } else {
        src = frame_buffer[display_frame_index] + (scanline - 32) * SCREENWIDTH;
    }
    palette_convert_scanline(dest, src);
    return;
#endif
    if (scanline < MAIN_VIEWHEIGHT) {
        src = frame_buffer[display_frame_index];
    } else {
        src = frame_buffer[display_frame_index^1] - 32 * SCREENWIDTH;
    }
    assert(wipe_yoffsets && wipe_linelookup);
    uint16_t *d = (uint16_t *)dest;
    src += scanline * SCREENWIDTH;
    for (int i = 0; i < SCREENWIDTH; i++) {
        int rel = scanline - wipe_yoffsets[i];
        if (rel < 0) {
            d[i] = palette[src[i]];
        } else {
            const uint8_t *flip;
#if PICO_ON_DEVICE
            flip = (const uint8_t *)wipe_linelookup[rel];
#else
            flip = &frame_buffer[0][0] + wipe_linelookup[rel];
#endif
            // todo better protection here
            if (flip >= &frame_buffer[0][0] && flip < &frame_buffer[0][0] + 2 * SCREENWIDTH * MAIN_VIEWHEIGHT) {
                d[i] = palette[flip[i]];
            }
        }
    }
}

static inline uint draw_vpatch(uint16_t *dest, patch_t *patch, vpatchlist_t *vp, uint off) {
    int repeat = vp->entry.repeat;
    dest += vp->entry.x;
    int w = vpatch_width(patch);
    const uint8_t *data0 = vpatch_data(patch);
    const uint8_t *data = data0 + off;
    if (!vpatch_has_shared_palette(patch)) {
        const uint8_t *pal = vpatch_palette(patch);
        switch (vpatch_type(patch)) {
            case vp4_runs: {
                uint16_t *p = dest;
                uint16_t *pend = dest + w;
                uint8_t gap;
                while (0xff != (gap = *data++)) {
                    p += gap;
                    int len = *data++;
                    for (int i = 1; i < len; i += 2) {
                        uint v = *data++;
                        *p++ = palette[pal[v & 0xf]];
                        *p++ = palette[pal[v >> 4]];
                    }
                    if (len & 1) {
                        *p++ = palette[pal[(*data++) & 0xf]];
                    }
                    assert(p <= pend);
                    if (p == pend) break;
                }
                break;
            }
            case vp4_alpha: {
                uint16_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = palette[pal[v & 0xf]];
                    if (v >> 4) p[1] = palette[pal[v >> 4]];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = palette[pal[v & 0xf]];
                }
                break;
            }
            case vp4_solid: {
                uint16_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    p[0] = palette[pal[v & 0xf]];
                    p[1] = palette[pal[v >> 4]];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    p[0] = palette[pal[v & 0xf]];
                }
                break;
            }
            case vp6_runs: {
                uint16_t *p = dest;
                uint16_t *pend = dest + w;
                uint8_t gap;
                while (0xff != (gap = *data++)) {
                    p += gap;
                    int len = *data++;
                    for (int i = 3; i < len; i += 4) {
                        uint v = *data++;
                        v |= (*data++) << 8;
                        v |= (*data++) << 16;
                        *p++ = palette[pal[v & 0x3f]];
                        *p++ = palette[pal[(v >> 6) & 0x3f]];
                        *p++ = palette[pal[(v >> 12) & 0x3f]];
                        *p++ = palette[pal[(v >> 18) & 0x3f]];
                    }
                    len &= 3;
                    if (len--) {
                        uint v = *data++;
                        *p++ = palette[pal[v & 0x3f]];
                        if (len--) {
                            v >>= 6;
                            v |= (*data++) << 2;
                            *p++ = palette[pal[v & 0x3f]];
                            if (len--) {
                                v >>= 6;
                                v |= (*data++) << 4;
                                *p++ = palette[pal[v & 0x3f]];
                                assert(!len);
                            }
                        }
                    }
                    assert(p <= pend);
                    if (p == pend) break;
                }
                break;
            }
            case vp8_runs: {
                uint16_t *p = dest;
                uint16_t *pend = dest + w;
                uint8_t gap;
                while (0xff != (gap = *data++)) {
                    p += gap;
                    int len = *data++;
                    for (int i = 0; i < len; i++) {
                        *p++ = palette[pal[*data++]];
                    }
                    assert(p <= pend);
                    if (p == pend) break;
                }
                break;
            }
            case vp_border: {
                dest[0] = palette[*data++];
                uint16_t col = palette[*data++];
                for (int i = 1; i < w - 1; i++) dest[i] = col;
                dest[w-1] = palette[*data++];
                break;
            }
            default:
                assert(false);
                break;
        }
    } else {
        uint sp = vpatch_shared_palette(patch);
        uint16_t *pal16 = shared_pal[sp];
        assert(sp < NUM_SHARED_PALETTES);
        switch (vpatch_type(patch)) {
            case vp4_solid: {
#if PICO_ON_DEVICE
                if (patch == stbar) {
                    static const uint8_t *cached_data;
#if PICO_RP2040
                    static uint32_t __scratch_x("data_cache") data_cache[41];
#else
                    // short of scratch space on RP2350 for some reason, so lets put this in main RAM
                    static uint32_t data_cache[41];
#endif
                    int i = 0;
                    uint32_t *d = (uint32_t *) dest;
#define DMA_CHANNEL 11
                    if (cached_data == data) {
                        const uint8_t *source = (const uint8_t *) data_cache;
                        // we need to correct for the misalignment of data, because the XIP copy ignores the low 2 bits...
                        // the raw bitmap data is always misaligned by 3 (the size of the header in the case of stbar)
                        source += 3;
                        for (; source < (const uint8_t *) dma_hw->ch[DMA_CHANNEL].al1_write_addr; source++) {
                            uint32_t val = pal16[source[0] & 0xf];
                            val |= (pal16[source[0] >> 4]) << 16;
                            *d++ = val;
                        }
                        source -= 3;
                        i = (source - (const uint8_t *) data_cache);
                    }
                    if (true) {
                        //                        once = true;
                        xip_ctrl_hw->stream_ctr = 0;
                        // workaround yucky bug
#if !PICO_RP2350
                        (void) *(io_rw_32 *) XIP_NOCACHE_NOALLOC_BASE;
                        xip_ctrl_hw->stream_fifo;
#endif
                        dma_channel_abort(DMA_CHANNEL);
                        dma_channel_config c = dma_channel_get_default_config(DMA_CHANNEL);
                        channel_config_set_read_increment(&c, false);
                        channel_config_set_write_increment(&c, true);
                        channel_config_set_dreq(&c, DREQ_XIP_STREAM);
#if !PICO_RP2350
                        dma_channel_set_read_addr(DMA_CHANNEL, (void *) XIP_AUX_BASE, false);
#else
                        dma_channel_set_read_addr(DMA_CHANNEL, &xip_ctrl_hw->stream_fifo, false);
#endif
                        dma_channel_set_config(DMA_CHANNEL, &c, false);
                        cached_data = data + SCREENWIDTH / 2;
                        xip_ctrl_hw->stream_addr = (uintptr_t) cached_data;
                        xip_ctrl_hw->stream_ctr = 41;
                        __compiler_memory_barrier();
                        dma_channel_transfer_to_buffer_now(DMA_CHANNEL, data_cache, 41);
                    }
                    for (; i < SCREENWIDTH / 2; i++) {
                        uint32_t val = pal16[data[i] & 0xf];
                        val |= (pal16[data[i] >> 4]) << 16;
                        *d++ = val;
                    }
                    data += SCREENWIDTH / 2;
                    break; // early break from switch
                }
#endif
                if (((uintptr_t)dest)&3) {
                    uint16_t *p = dest;
                    for (int i = 0; i < w / 2; i++) {
                        uint v = *data++;
                        p[0] = pal16[v & 0xf];
                        p[1] = pal16[v >> 4];
                        p += 2;
                    }
                } else {
                    uint32_t *wide = (uint32_t *) dest;
                    for (int i = 0; i < w / 2; i++) {
                        uint v = *data++;
                        wide[i] = pal16[v & 0xf] | (pal16[v >> 4] << 16);
                    }
                }
                if (w & 1) {
                    uint v = *data++;
                    dest[w-1] = pal16[v & 0xf];
                }
                break;
            }
            case vp4_alpha: {
                uint16_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = pal16[v & 0xf];
                    if (v >> 4) p[1] = pal16[v >> 4];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = pal16[v & 0xf];
                }
                break;
            }
            default:
                assert(false);
        }
    }
    if (repeat) {
        // we need them to be solid... which they are, but if not you'll just get some visual funk
        //assert(vpatch_type(patch) == vp4_solid);
        if (vp->entry.patch_handle == VPATCH_M_THERMM) w--; // hackity hack
        for(int i=0;i<repeat*w;i++) {
            dest[w+i] = dest[i];
        }
    }
    return data - data0;
}

// this is not in flash as quite large and only once per frame
void __noinline new_frame_init_overlays_palette_and_wipe() {
    // re-initialize our overlay drawing
    if (display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS) {
        memset(vpatchlists->vpatch_next, 0, sizeof(vpatchlists->vpatch_next));
        memset(vpatchlists->vpatch_starters, 0, sizeof(vpatchlists->vpatch_starters));
        memset(vpatchlists->vpatch_doff, 0, sizeof(vpatchlists->vpatch_doff));
        vpatchlist_t *overlays = vpatchlists->overlays[display_overlay_index];
        // do it in reverse so our linked lists are in ascending order
        for (int i = overlays->header.size - 1; i > 0; i--) {
            assert(overlays[i].entry.y < count_of(vpatchlists->vpatch_starters));
            vpatchlists->vpatch_next[i] = vpatchlists->vpatch_starters[overlays[i].entry.y];
            vpatchlists->vpatch_starters[overlays[i].entry.y] = i;
        }
        if (next_pal != -1) {
            static const uint8_t *playpal;
            static bool calculate_palettes;
            if (!playpal) {
                lumpindex_t l = W_GetNumForName("PLAYPAL");
                playpal = W_CacheLumpNum(l, PU_STATIC);
                calculate_palettes = W_LumpLength(l) == 768;
            }
            if (!calculate_palettes || !next_pal) {
                const uint8_t *doompalette = playpal + next_pal * 768;
                for (int i = 0; i < 256; i++) {
                    int r = *doompalette++;
                    int g = *doompalette++;
                    int b = *doompalette++;
                    if (usegamma) {
                        r = gammatable[usegamma-1][r];
                        g = gammatable[usegamma-1][g];
                        b = gammatable[usegamma-1][b];
                    }
                    palette[i] = PICO_SCANVIDEO_PIXEL_FROM_RGB8(r, g, b);
                }
            } else {
                int mul, r0, g0, b0;
                if (next_pal < 9) {
                    mul = next_pal * 65536 / 9;
                    r0 = 255; g0 = b0 = 0;
                } else if (next_pal < 13) {
                    mul = (next_pal - 8) * 65536 / 8;
                    r0 = 215; g0 = 186; b0 = 69;
                } else {
                    mul = 65536 / 8;
                    r0 = b0 = 0; g0 = 256;
                }
                const uint8_t *doompalette = playpal;
                for (int i = 0; i < 256; i++) {
                    int r = *doompalette++;
                    int g = *doompalette++;
                    int b = *doompalette++;
                    r += ((r0 - r) * mul) >> 16;
                    g += ((g0 - g) * mul) >> 16;
                    b += ((b0 - b) * mul) >> 16;
                    palette[i] = PICO_SCANVIDEO_PIXEL_FROM_RGB8(r, g, b);
                }
            }
            next_pal = -1;
            assert(vpatch_type(stbar) == vp4_solid); // no transparent, no runs, 4 bpp
            for (int i = 0; i < NUM_SHARED_PALETTES; i++) {
                patch_t *patch = resolve_vpatch_handle(vpatch_for_shared_palette[i]);
                assert(vpatch_colorcount(patch) <= 16);
                assert(vpatch_has_shared_palette(patch));
                for (int j = 0; j < 16; j++) {
                    shared_pal[i][j] = palette[vpatch_palette(patch)[j]];
                }
            }
        }
        if (display_video_type == VIDEO_TYPE_WIPE) {
//            printf("WIPEMIN %d\n", wipe_min);
            if (wipe_min <= 200) {
                bool regular = display_overlay_index; // just happens to toggle every frame
                int new_wipe_min = 200;
                for (int i = 0; i < SCREENWIDTH; i++) {
                    int v;
                    if (wipe_yoffsets_raw[i] < 0) {
                        if (regular) {
                            wipe_yoffsets_raw[i]++;
                        }
                        v = 0;
                    } else {
                        int dy = (wipe_yoffsets_raw[i] < 16) ? (1 + wipe_yoffsets_raw[i] + regular) / 2 : 4;
                        if (wipe_yoffsets_raw[i] + dy > 200) {
                            v = 200;
                        } else {
                            wipe_yoffsets_raw[i] += dy;
                            v = wipe_yoffsets_raw[i];
                        }
                    }
                    wipe_yoffsets[i] = v;
                    if (v < new_wipe_min) new_wipe_min = v;
                }
                assert(new_wipe_min >= wipe_min);
                wipe_min = new_wipe_min;
            }
        }
    }
}

// this method moved out of scratchx because we didn't have quite enough space for core1 stack
void __no_inline_not_in_flash_func(new_frame_stuff)() {
    // this part of the per frame code is in RAM as it is needed during save
    if (sem_available(&render_frame_ready)) {
        sem_acquire_blocking(&render_frame_ready);
        display_video_type = next_video_type;
        display_frame_index = next_frame_index;
        display_overlay_index = next_overlay_index;
#if !DEMO1_ONLY
        video_scroll = next_video_scroll; // todo does this waste too much space
#endif
        sem_release(&display_frame_freed);
    } else {
#if !DEMO1_ONLY
        video_scroll = NULL;
#endif
    }
    if (display_video_type != VIDEO_TYPE_SAVING) {
        // this stuff is large (so in flash) and not needed in save move
        new_frame_init_overlays_palette_and_wipe();
    }
}

#if !PICOTRACKER
void __scratch_x("scanlines") fill_scanlines() {
#if SUPPORT_TEXT
    struct scanvideo_scanline_buffer *buffer = scanvideo_begin_scanline_generation_linked(display_video_type == VIDEO_TYPE_TEXT ? 2 : 1, false);
#else
    struct scanvideo_scanline_buffer *buffer = scanvideo_begin_scanline_generation(false);
#endif
#if USE_INTERP
    need_save = interp_in_use;
    interp_updated = 0;
#endif

    while (buffer) {
        static int8_t last_frame_number = -1;
        int frame = scanvideo_frame_number(buffer->scanline_id);
        int scanline = scanvideo_scanline_number(buffer->scanline_id);
        if ((int8_t) frame != last_frame_number) {
            last_frame_number = frame;
            new_frame_stuff();
        }

        DEBUG_PINS_SET(scanline_copy, 1);
        if (display_video_type != VIDEO_TYPE_TEXT) {
            // we don't have text mode -> normal transition yet, but we may for network game, so leaving this here - we would need to put the buffer pointers back
            assert (buffer->data < text_scanline_buffer_start || buffer->data >= text_scanline_buffer_start + TEXT_SCANLINE_BUFFER_TOTAL_WORDS);
            scanline_funcs[display_video_type](buffer->data+1, scanline);
            if (display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS) {
                assert(scanline < count_of(vpatchlists->vpatch_starters));
                int prev = 0;
                for (int vp = vpatchlists->vpatch_starters[scanline]; vp;) {
                    int next = vpatchlists->vpatch_next[vp];
                    while (vpatchlists->vpatch_next[prev] && vpatchlists->vpatch_next[prev] < vp) {
                        prev = vpatchlists->vpatch_next[prev];
                    }
                    assert(prev != vp);
                    assert(vpatchlists->vpatch_next[prev] != vp);
                    vpatchlists->vpatch_next[vp] = vpatchlists->vpatch_next[prev];
                    vpatchlists->vpatch_next[prev] = vp;
                    prev = vp;
                    vp = next;
                }
                vpatchlist_t *overlays = vpatchlists->overlays[display_overlay_index];
                prev = 0;
                for (int vp = vpatchlists->vpatch_next[prev]; vp; vp = vpatchlists->vpatch_next[prev]) {
                    patch_t *patch = resolve_vpatch_handle(overlays[vp].entry.patch_handle);
                    int yoff = scanline - overlays[vp].entry.y;
                    if (yoff < vpatch_height(patch)) {
                        vpatchlists->vpatch_doff[vp] = draw_vpatch((uint16_t*)(buffer->data + 1), patch, &overlays[vp],
                                                                   vpatchlists->vpatch_doff[vp]);
                        prev = vp;
                    } else {
                        vpatchlists->vpatch_next[prev] = vpatchlists->vpatch_next[vp];
                    }
                }
            }
            uint16_t *p = (uint16_t *) buffer->data;
            p[0] = video_doom_offset_raw_run;
            p[1] = p[2];
            p[2] = SCREENWIDTH - 3;
            buffer->data[SCREENWIDTH / 2 + 1] = video_doom_offset_raw_1p;
            buffer->data[SCREENWIDTH / 2 + 2] = video_doom_offset_end_of_scanline_skip_ALIGN;
            buffer->data_used = SCREENWIDTH / 2 + 3;
            DEBUG_PINS_CLR(scanline_copy, 1);
        } else {
#if SUPPORT_TEXT
            render_text_mode_scanline(buffer, scanline);
#else
            memset(buffer->data + 1, 0, SCREENWIDTH * 2);
            p[0] = video_doom_offset_raw_run;
            p[1] = p[2];
            p[2] = SCREENWIDTH - 3;
            buffer->data[SCREENWIDTH / 2 + 1] = video_doom_offset_raw_1p;
            buffer->data[SCREENWIDTH / 2 + 2] = video_doom_offset_end_of_scanline_skip_ALIGN;
            buffer->data_used = SCREENWIDTH / 2 + 3;
#endif
        }
        scanvideo_end_scanline_generation(buffer);
#if SUPPORT_TEXT
        buffer = scanvideo_begin_scanline_generation_linked(display_video_type == VIDEO_TYPE_TEXT ? 2 : 1, false);
#else
        buffer = scanvideo_begin_scanline_generation(false);
#endif
    }
#if USE_INTERP
    if (interp_updated && need_save) {
        interp_restore_static(interp0, &interp0_save);
        interp_restore_static(interp1, &interp1_save);
    }
#endif
}

#if PICO_ON_DEVICE
#define LOW_PRIO_IRQ 31
#include "hardware/irq.h"

static void __not_in_flash_func(free_buffer_callback)() {
//    irq_set_pending(LOW_PRIO_IRQ);
    // ^ is in flash by default
#if !PICO_RP2350
    *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ISPR_OFFSET)) = 1u << LOW_PRIO_IRQ;
#else
    nvic_hw->ispr[LOW_PRIO_IRQ / 32] = 1 << (LOW_PRIO_IRQ % 32);
#endif
}
#endif

//static semaphore_t init_sem;
static void core1() {
#if !PICO_ON_DEVICE
    void simulate_video_pio_video_doom(const uint32_t *dma_data, uint32_t dma_data_size,
                                       uint16_t *pixel_buffer, int32_t max_pixels, int32_t expected_width, bool overlay);
    scanvideo_set_simulate_scanvideo_pio_fn(VIDEO_DOOM_PROGRAM_NAME, simulate_video_pio_video_doom);
#endif
    scanvideo_setup(&VGA_MODE);
//    sem_release(&init_sem);
#if PICO_ON_DEVICE
    irq_set_exclusive_handler(LOW_PRIO_IRQ, fill_scanlines);
    irq_set_enabled(LOW_PRIO_IRQ, true);
    scanvideo_set_scanline_release_fn(free_buffer_callback);
#endif
    scanvideo_timing_enable(true);
#if PICO_ON_DEVICE
    irq_set_pending(LOW_PRIO_IRQ);
#endif
    sem_release(&core1_launch);
    while (true) {
        pd_core1_loop();
#if PICO_ON_DEVICE
        tight_loop_contents();
#else
        fill_scanlines();
#endif
    }
}
#endif // !PICOTRACKER
#pragma GCC pop_options

// ============================================================
// picoTracker ST7789 display driver
// ============================================================
#if PICOTRACKER

static void lcd_cmd(uint8_t cmd) {
    gpio_put(LCD_DC_PIN, 0);
    gpio_put(LCD_CS_PIN, 0);
    spi_write_blocking(LCD_SPI, &cmd, 1);
    gpio_put(LCD_CS_PIN, 1);
}

static void lcd_data(const uint8_t *data, size_t len) {
    gpio_put(LCD_DC_PIN, 1);
    gpio_put(LCD_CS_PIN, 0);
    spi_write_blocking(LCD_SPI, data, len);
    gpio_put(LCD_CS_PIN, 1);
}

static void lcd_data_byte(uint8_t b) { lcd_data(&b, 1); }

static void fill_lcd_test_pattern_line(uint16_t *dest, int y) {
    static const uint16_t colors[] = {
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xff, 0x00, 0x00),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xff, 0x80, 0x00),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xff, 0xff, 0x00),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0xff, 0x00),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0x80, 0xff),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0x00, 0xff),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x80, 0x00, 0xff),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xff, 0xff, 0xff),
    };
    int band_height = MAX(1, LCD_HEIGHT / (int)count_of(colors));
    int color_index = MIN((int)count_of(colors) - 1, y / band_height);

    for (int x = 0; x < SCREENWIDTH; ++x) {
        uint16_t color = colors[color_index];
        if ((x / 20) & 1) {
            color ^= PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x20, 0x20, 0x20);
        }
        dest[x] = color;
    }
}

static void fill_lcd_raw_framebuffer_line(uint16_t *dest, int scanline) {
    static const uint16_t mode_colors[] = {
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x20, 0x20, 0x20),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xff, 0xff, 0x00),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0xff, 0xff),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0xff, 0x00),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0x00, 0x80, 0xff),
            PICO_SCANVIDEO_PIXEL_FROM_RGB8(0xff, 0x00, 0xff),
    };

    if (scanline < 12) {
        uint16_t left = mode_colors[display_video_type < count_of(mode_colors) ? display_video_type : 0];
        uint16_t right = mode_colors[next_video_type < count_of(mode_colors) ? next_video_type : 0];
        for (int x = 0; x < SCREENWIDTH; ++x) {
            dest[x] = x < (SCREENWIDTH / 2) ? left : right;
        }
        return;
    }

    if (scanline < (LCD_HEIGHT / 2)) {
        int y = (scanline - 12) * MAIN_VIEWHEIGHT / ((LCD_HEIGHT / 2) - 12);
        const uint8_t *src = frame_buffer[0] + y * SCREENWIDTH;
        for (int x = 0; x < SCREENWIDTH; ++x) {
            uint8_t v = src[x];
            dest[x] = PICO_SCANVIDEO_PIXEL_FROM_RGB8(v, v, v);
        }
        return;
    }

    int y = (scanline - (LCD_HEIGHT / 2)) * MAIN_VIEWHEIGHT / (LCD_HEIGHT - (LCD_HEIGHT / 2));
    const uint8_t *src = frame_buffer[1] + y * SCREENWIDTH;
    for (int x = 0; x < SCREENWIDTH; ++x) {
        uint8_t v = src[x];
        dest[x] = PICO_SCANVIDEO_PIXEL_FROM_RGB8(v, v, v);
    }
}

static void lcd_init_hw(void) {
    // 48 MHz SPI (clk_peri = 96 MHz, min prescaler = 2)
    spi_init(LCD_SPI, 48000000);
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LCD_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);

    gpio_init(LCD_CS_PIN);  gpio_set_dir(LCD_CS_PIN,  GPIO_OUT); gpio_put(LCD_CS_PIN,  1);
    gpio_init(LCD_DC_PIN);  gpio_set_dir(LCD_DC_PIN,  GPIO_OUT); gpio_put(LCD_DC_PIN,  1);
    gpio_init(LCD_RST_PIN); gpio_set_dir(LCD_RST_PIN, GPIO_OUT); gpio_put(LCD_RST_PIN, 1);
    gpio_init(LCD_BL_PIN);  gpio_set_dir(LCD_BL_PIN,  GPIO_OUT); gpio_put(LCD_BL_PIN,  0);

    // Hardware reset
    gpio_put(LCD_RST_PIN, 0); sleep_ms(10);
    gpio_put(LCD_RST_PIN, 1); sleep_ms(120);

    // Software reset
    lcd_cmd(0x01); sleep_ms(150);

    // Gamma set
    lcd_cmd(0x26); lcd_data_byte(0x01);

    // Positive gamma
    lcd_cmd(0xE0);
    static const uint8_t pgamma[] = {0xD0,0x00,0x02,0x07,0x0A,0x28,0x32,0x44,0x42,0x06,0x0E,0x12,0x14,0x17};
    lcd_data(pgamma, sizeof(pgamma));

    // Negative gamma
    lcd_cmd(0xE1);
    static const uint8_t ngamma[] = {0xD0,0x00,0x02,0x07,0x0A,0x28,0x31,0x54,0x47,0x0E,0x1C,0x17,0x1B,0x1E};
    lcd_data(ngamma, sizeof(ngamma));

    // Landscape mode needs row/column exchange on the ST7789.
    lcd_cmd(0x36); lcd_data_byte(0xA0);

    // Display inversion ON (required for correct colours on ST7789)
    lcd_cmd(0x21);

    // Pixel format: 16-bit RGB565
    lcd_cmd(0x3A); lcd_data_byte(0x55);

    // Frame rate: 75 Hz
    lcd_cmd(0xB1); lcd_data_byte(0x00); lcd_data_byte(0x09);

    // Sleep out then display on
    lcd_cmd(0x11); sleep_ms(120);
    lcd_cmd(0x29);

    // In landscape mode the address window is 320x240.
    lcd_cmd(0x2A);
    static const uint8_t col[] = {0x00,0x00,0x01,0x3F};
    lcd_data(col, sizeof(col));

    // Page address 0-239
    lcd_cmd(0x2B);
    static const uint8_t page[] = {0x00,0x00,0x00,0xEF};
    lcd_data(page, sizeof(page));

    // Clear the panel before turning on the backlight.
    {
        lcd_cmd(0x2C);
        gpio_put(LCD_DC_PIN, 1);
        gpio_put(LCD_CS_PIN, 0);
        static const uint8_t black2[2] = {0, 0};
        for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
            spi_write_blocking(LCD_SPI, black2, 2);
        }
        gpio_put(LCD_CS_PIN, 1);
    }

    // Set up SPI DMA channel
    lcd_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(lcd_dma_channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(lcd_dma_channel, &c,
                          &spi_get_hw(LCD_SPI)->dr,
                          NULL, 0, false);

    // Backlight on
    gpio_put(LCD_BL_PIN, 1);
}

// Render overlays (status bar patches etc.) onto the line buffer for one scanline.
static inline void render_overlays_for_scanline(uint16_t *dest, int scanline) {
    if (display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS) {
        int prev = 0;
        for (int vp = vpatchlists->vpatch_starters[scanline]; vp;) {
            int next = vpatchlists->vpatch_next[vp];
            while (vpatchlists->vpatch_next[prev] && vpatchlists->vpatch_next[prev] < vp) {
                prev = vpatchlists->vpatch_next[prev];
            }
            vpatchlists->vpatch_next[vp] = vpatchlists->vpatch_next[prev];
            vpatchlists->vpatch_next[prev] = vp;
            prev = vp;
            vp = next;
        }
        vpatchlist_t *overlays = vpatchlists->overlays[display_overlay_index];
        prev = 0;
        for (int vp = vpatchlists->vpatch_next[prev]; vp; vp = vpatchlists->vpatch_next[prev]) {
            patch_t *patch = resolve_vpatch_handle(overlays[vp].entry.patch_handle);
            int yoff = scanline - overlays[vp].entry.y;
            if (yoff < vpatch_height(patch)) {
                vpatchlists->vpatch_doff[vp] = draw_vpatch(dest, patch, &overlays[vp],
                                                           vpatchlists->vpatch_doff[vp]);
                prev = vp;
            } else {
                vpatchlists->vpatch_next[prev] = vpatchlists->vpatch_next[vp];
            }
        }
    }
}

// Blit a fully-rendered game frame to the ST7789 via SPI DMA.
// Double-buffered: while DMA sends line N the CPU converts line N+1.
static void __scratch_x("lcd_blit") blit_frame_to_lcd(void) {
#if USE_INTERP
    need_save = interp_in_use;
    interp_updated = 0;
#endif

    // Issue RAMWR and hold CS/DC for the full frame transfer
    lcd_cmd(0x2C);
    gpio_put(LCD_DC_PIN, 1);
    gpio_put(LCD_CS_PIN, 0);

    int cur = 0;
#if PICOTRACKER_LCD_TEST_PATTERN
    int nxt = 1 - cur;
#endif

#if PICOTRACKER_LCD_TEST_PATTERN
    for (int y = 0; y < LCD_HEIGHT; ++y) {
        fill_lcd_test_pattern_line((uint16_t *) lcd_line_buf[nxt], y);
        dma_channel_wait_for_finish_blocking(lcd_dma_channel);
        cur = nxt;
        dma_channel_set_read_addr(lcd_dma_channel, lcd_line_buf[cur], false);
        dma_channel_set_trans_count(lcd_dma_channel, SCREENWIDTH * 2, true);
        nxt = 1 - cur;
    }
#elif PICOTRACKER_LCD_DEBUG_RAW_FB
    memset(lcd_line_buf[cur], 0, SCREENWIDTH * 2);
    for (int y = 0; y < LCD_LETTERBOX; y++) {
        dma_channel_wait_for_finish_blocking(lcd_dma_channel);
        dma_channel_set_read_addr(lcd_dma_channel, lcd_line_buf[cur], false);
        dma_channel_set_trans_count(lcd_dma_channel, SCREENWIDTH * 2, true);
    }

    int nxt = 1 - cur;
    fill_lcd_raw_framebuffer_line((uint16_t *) lcd_line_buf[nxt], 0);

    for (int y = 0; y < SCREENHEIGHT; y++) {
        dma_channel_wait_for_finish_blocking(lcd_dma_channel);
        cur = nxt;
        dma_channel_set_read_addr(lcd_dma_channel, lcd_line_buf[cur], false);
        dma_channel_set_trans_count(lcd_dma_channel, SCREENWIDTH * 2, true);

        nxt = 1 - cur;
        if (y + 1 < SCREENHEIGHT) {
            fill_lcd_raw_framebuffer_line((uint16_t *) lcd_line_buf[nxt], y + 1);
        }
    }

    memset(lcd_line_buf[0], 0, SCREENWIDTH * 2);
    for (int y = 0; y < LCD_LETTERBOX; y++) {
        dma_channel_wait_for_finish_blocking(lcd_dma_channel);
        dma_channel_set_read_addr(lcd_dma_channel, lcd_line_buf[0], false);
        dma_channel_set_trans_count(lcd_dma_channel, SCREENWIDTH * 2, true);
    }
#else
    // --- Top letterbox (black) ---
    memset(lcd_line_buf[cur], 0, SCREENWIDTH * 2);
    for (int y = 0; y < LCD_LETTERBOX; y++) {
        dma_channel_wait_for_finish_blocking(lcd_dma_channel);
        dma_channel_set_read_addr(lcd_dma_channel, lcd_line_buf[cur], false);
        dma_channel_set_trans_count(lcd_dma_channel, SCREENWIDTH * 2, true);
    }

    // Pre-convert line 0 into the other buffer while DMA finishes last black line
    int nxt = 1 - cur;
    scanline_funcs[display_video_type](lcd_line_buf[nxt], 0);
    render_overlays_for_scanline((uint16_t *)lcd_line_buf[nxt], 0);

    // --- Game scanlines ---
    for (int y = 0; y < SCREENHEIGHT; y++) {
        dma_channel_wait_for_finish_blocking(lcd_dma_channel);
        cur = nxt;
        dma_channel_set_read_addr(lcd_dma_channel, lcd_line_buf[cur], false);
        dma_channel_set_trans_count(lcd_dma_channel, SCREENWIDTH * 2, true);

        nxt = 1 - cur;
        if (y + 1 < SCREENHEIGHT) {
            scanline_funcs[display_video_type](lcd_line_buf[nxt], y + 1);
            render_overlays_for_scanline((uint16_t *)lcd_line_buf[nxt], y + 1);
        }
    }

    // --- Bottom letterbox (black) ---
    memset(lcd_line_buf[0], 0, SCREENWIDTH * 2);
    for (int y = 0; y < LCD_LETTERBOX; y++) {
        dma_channel_wait_for_finish_blocking(lcd_dma_channel);
        dma_channel_set_read_addr(lcd_dma_channel, lcd_line_buf[0], false);
        dma_channel_set_trans_count(lcd_dma_channel, SCREENWIDTH * 2, true);
    }
#endif

    dma_channel_wait_for_finish_blocking(lcd_dma_channel);
    gpio_put(LCD_CS_PIN, 1);

#if USE_INTERP
    if (interp_updated && need_save) {
        interp_restore_static(interp0, &interp0_save);
        interp_restore_static(interp1, &interp1_save);
    }
#endif
}

static void core1_picotracker(void) {
    lcd_init_hw();
    sem_release(&core1_launch);

    while (true) {
        pd_core1_loop();

        sem_acquire_blocking(&render_frame_ready);
        display_video_type    = next_video_type;
        display_frame_index   = next_frame_index;
        display_overlay_index = next_overlay_index;
#if !DEMO1_ONLY
        video_scroll = next_video_scroll;
#endif
        sem_release(&display_frame_freed);

        if (display_video_type != VIDEO_TYPE_SAVING) {
            new_frame_init_overlays_palette_and_wipe();
        }
        blit_frame_to_lcd();
    }
}

#endif // PICOTRACKER

#if PICO_RP2350 && !PICOTRACKER
#include "hardware/structs/accessctrl.h"
#endif
void I_InitGraphics(void)
{
    stbar = resolve_vpatch_handle(VPATCH_STBAR);
    sem_init(&render_frame_ready, 0, 2);
    sem_init(&display_frame_freed, 1, 2);
    sem_init(&core1_launch, 0, 1);
    pd_init();
#if PICOTRACKER
    multicore_launch_core1(core1_picotracker);
#else
    multicore_launch_core1(core1);
#endif
    // wait for core1 launch as it may do malloc and we have no mutex around that
    sem_acquire_blocking(&core1_launch);
#if USE_ZONE_FOR_MALLOC
    disallow_core1_malloc = true;
#endif
#if PICO_RP2350 && !PICOTRACKER
    hw_set_bits(&accessctrl_hw->xip_ctrl, ACCESSCTRL_PASSWORD_BITS | 0xff);
#endif
    initialized = true;
}

// Bind all variables controlling video options into the configuration
// file system.
void I_BindVideoVariables(void)
{
//    M_BindIntVariable("use_mouse",                 &usemouse);
//    M_BindIntVariable("fullscreen",                &fullscreen);
//    M_BindIntVariable("video_display",             &video_display);
//    M_BindIntVariable("aspect_ratio_correct",      &aspect_ratio_correct);
//    M_BindIntVariable("integer_scaling",           &integer_scaling);
//    M_BindIntVariable("vga_porch_flash",           &vga_porch_flash);
//    M_BindIntVariable("startup_delay",             &startup_delay);
//    M_BindIntVariable("fullscreen_width",          &fullscreen_width);
//    M_BindIntVariable("fullscreen_height",         &fullscreen_height);
//    M_BindIntVariable("force_software_renderer",   &force_software_renderer);
//    M_BindIntVariable("max_scaling_buffer_pixels", &max_scaling_buffer_pixels);
//    M_BindIntVariable("window_width",              &window_width);
//    M_BindIntVariable("window_height",             &window_height);
//    M_BindIntVariable("grabmouse",                 &grabmouse);
//    M_BindStringVariable("video_driver",           &video_driver);
//    M_BindStringVariable("window_position",        &window_position);
//    M_BindIntVariable("usegamma",                  &usegamma);
//    M_BindIntVariable("png_screenshots",           &png_screenshots);
}

//
// I_StartTic
//
void I_StartTic (void)
{
    if (!initialized)
    {
        return;
    }

    I_GetEvent();
//
//    if (usemouse && !nomouse && window_focused)
//    {
//        I_ReadMouse();
//    }
//
//    if (joywait < I_GetTime())
//    {
//        I_UpdateJoystick();
//    }
}


//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
    // what is this?
}

int I_GetPaletteIndex(int r, int g, int b)
{
    return 0;
}

#if !NO_USE_ENDDOOM
void I_Endoom(byte *endoom_data) {
#if SUPPORT_TEXT
    uint32_t size;
    uint8_t *wa = pd_get_work_area(&size);
    assert(size >=TEXT_SCANLINE_BUFFER_TOTAL_WORDS * 4 + 80*25*2 + 4096);
    text_screen_cpy = wa;
    text_font_cpy = text_screen_cpy + 80 * 25 * 2;
    text_scanline_buffer_start = (uint32_t *) (text_font_cpy + 4096);
#if 0
    static_assert(sizeof(normal_font_data) == 4096, "");
    memcpy(text_font_cpy, normal_font_data, sizeof(normal_font_data));
    memcpy(text_screen_cpy, endoom_data, 80 * 25 * 2);
#else
    static_assert(TEXT_SCANLINE_BUFFER_TOTAL_WORDS * 4 > 1024 + 512, "");
    uint8_t *tmp_buf = (uint8_t *)text_scanline_buffer_start;
    uint16_t *decoder = (uint16_t *)(tmp_buf + 512);
    th_bit_input bi;
    th_bit_input_init(&bi, normal_font_data_z);
    decode_data(text_font_cpy, 4096, &bi, decoder, 512, tmp_buf, 512);
    th_bit_input_init(&bi, endoom_data);
    // text
    decode_data(text_screen_cpy, 80*25, &bi, decoder, 512, tmp_buf, 512);
    // attr
    decode_data(text_screen_cpy+80*25, 80*25, &bi, decoder, 512, tmp_buf, 512);
    static_assert(TEXT_SCANLINE_BUFFER_TOTAL_WORDS * 4 > 80*25*2, "");
    // re-interlace the text & attr
    memcpy(tmp_buf, text_screen_cpy, 80*25*2);
    for(int i=0;i<80*25;i++) {
        text_screen_cpy[i*2] = tmp_buf[i];
        text_screen_cpy[i*2+1] = tmp_buf[80*25 + i];
    }
#endif
    text_screen_data = text_screen_cpy;
#endif
}
#endif

void I_GraphicsCheckCommandLine(void)
{
//    int i;
//
//    //!
//    // @category video
//    // @vanilla
//    //
//    // Disable blitting the screen.
//    //
//
//    noblit = M_CheckParm ("-noblit");
//
//    //!
//    // @category video
//    //
//    // Don't grab the mouse when running in windowed mode.
//    //
//
//    nograbmouse_override = M_ParmExists("-nograbmouse");
//
//    // default to fullscreen mode, allow override with command line
//    // nofullscreen because we love prboom
//
//    //!
//    // @category video
//    //
//    // Run in a window.
//    //
//
//    if (M_CheckParm("-window") || M_CheckParm("-nofullscreen"))
//    {
//        fullscreen = false;
//    }
//
//    //!
//    // @category video
//    //
//    // Run in fullscreen mode.
//    //
//
//    if (M_CheckParm("-fullscreen"))
//    {
//        fullscreen = true;
//    }
//
//    //!
//    // @category video
//    //
//    // Disable the mouse.
//    //
//
//    nomouse = M_CheckParm("-nomouse") > 0;
//
//    //!
//    // @category video
//    // @arg <x>
//    //
//    // Specify the screen width, in pixels. Implies -window.
//    //
//
//    i = M_CheckParmWithArgs("-width", 1);
//
//    if (i > 0)
//    {
//        window_width = atoi(myargv[i + 1]);
//        fullscreen = false;
//    }
//
//    //!
//    // @category video
//    // @arg <y>
//    //
//    // Specify the screen height, in pixels. Implies -window.
//    //
//
//    i = M_CheckParmWithArgs("-height", 1);
//
//    if (i > 0)
//    {
//        window_height = atoi(myargv[i + 1]);
//        fullscreen = false;
//    }
//
//    //!
//    // @category video
//    // @arg <WxY>
//    //
//    // Specify the dimensions of the window. Implies -window.
//    //
//
//    i = M_CheckParmWithArgs("-geometry", 1);
//
//    if (i > 0)
//    {
//        int w, h, s;
//
//        s = sscanf(myargv[i + 1], "%ix%i", &w, &h);
//        if (s == 2)
//        {
//            window_width = w;
//            window_height = h;
//            fullscreen = false;
//        }
//    }
//
//    //!
//    // @category video
//    //
//    // Don't scale up the screen. Implies -window.
//    //
//
//    if (M_CheckParm("-1"))
//    {
//        SetScaleFactor(1);
//    }
//
//    //!
//    // @category video
//    //
//    // Double up the screen to 2x its normal size. Implies -window.
//    //
//
//    if (M_CheckParm("-2"))
//    {
//        SetScaleFactor(2);
//    }
//
//    //!
//    // @category video
//    //
//    // Double up the screen to 3x its normal size. Implies -window.
//    //
//
//    if (M_CheckParm("-3"))
//    {
//        SetScaleFactor(3);
//    }
}

// Check if we have been invoked as a screensaver by xscreensaver.

void I_CheckIsScreensaver(void)
{
}

void I_DisplayFPSDots(boolean dots_on)
{
}

#if PICO_ON_DEVICE
#if !PICOTRACKER
bool video_doom_adapt_for_mode(const struct scanvideo_pio_program *program, const struct scanvideo_mode *mode,
                               struct scanvideo_scanline_buffer *missing_scanvideo_scanline_buffer, uint16_t *modifiable_instructions) {
    missing_scanvideo_scanline_buffer->data = missing_scanline_data;
    missing_scanvideo_scanline_buffer->data_used = missing_scanvideo_scanline_buffer->data_max = sizeof(missing_scanline_data) / 4;
    return true;
}

pio_sm_config video_doom_configure_pio(pio_hw_t *pio, uint sm, uint offset) {
    pio_sm_config config = video_24mhz_composable_default_program_get_default_config(offset);
    scanvideo_default_configure_pio(pio, sm, offset, &config, false);
    return config;
}
#endif
#else
void simulate_video_pio_video_doom(const uint32_t *dma_data, uint32_t dma_data_size,
                                   uint16_t *pixel_buffer, int32_t max_pixels, int32_t expected_width, bool overlay) {
    const uint16_t *it = (uint16_t *) dma_data;
    assert(!(3u & (uintptr_t) dma_data));
    const uint16_t *const __unused dma_data_end = (uint16_t *) (dma_data + dma_data_size);
    const uint16_t *const pixels_end = (uint16_t *) (pixel_buffer + max_pixels);
    uint16_t *pixels = pixel_buffer;
    bool __unused ok = false;
    bool done = false;
    bool __unused last_was_black = true; // in case no pixels
    const uint16_t display_enable_bit = PICO_SCANVIDEO_ALPHA_MASK; // for now
    do {
        uint16_t cmd = *it++;
        switch (cmd) {
            case video_doom_offset_nop_raw:
                break;
            case video_doom_offset_end_of_scanline_skip_ALIGN:
                it++;
                // fall thru
            case video_doom_offset_end_of_scanline_ALIGN:
                done = ok = true;
                break;
            case video_doom_offset_raw_run_half: {
                assert(pixels < pixels_end);
                uint16_t c = *it++;
                if (!overlay || (c & display_enable_bit))
                    *pixels++ = c;
                else
                    pixels++;
                uint16_t len = *it++;
                for (int i = 0; i < len + 2; i++) {
                    assert(pixels < pixels_end);
                    c = *it++;
                    if (!overlay || (c & display_enable_bit))
                        *pixels++ = c;
                    else
                        pixels++;
                }
                last_was_black = !c;
                break;
            }
            case video_doom_offset_raw_1p_half: {
                uint16_t c;
                if (pixels == pixels_end) {
                    c = *it++;
                    assert(!c); // must end with black
                } else {
                    assert(pixels < pixels_end);
                    c = *it++;
                    if (!overlay || (c & display_enable_bit))
                        *pixels++ = c;
                    else
                        pixels++;
                }
                last_was_black = !c;
                break;
            }
            case video_doom_offset_raw_run: {
                assert(pixels < pixels_end);
                uint16_t c = *it++;
                if (!overlay || (c & display_enable_bit))
                    *pixels++ = c, *pixels++ = c;
                else
                    pixels+=2;
                uint16_t len = *it++;
                for (int i = 0; i < len + 2; i++) {
                    assert(pixels < pixels_end);
                    c = *it++;
                    if (!overlay || (c & display_enable_bit))
                        *pixels++ = c, *pixels++ = c;
                    else
                        pixels+=2;
                }
                last_was_black = !c;
                break;
            }
            case video_doom_offset_raw_1p: {
                uint16_t c;
                if (pixels == pixels_end) {
                    c = *it++;
                    assert(!c); // must end with black
                } else {
                    assert(pixels < pixels_end);
                    c = *it++;
                    if (!overlay || (c & display_enable_bit))
                        *pixels++ = c, *pixels++ = c;
                    else
                        pixels += 2;
                }
                last_was_black = !c;
                break;
            }
            default:
                assert(cmd < 32);
                assert(false);
                done = true;
        }
    } while (!done);
    assert(ok);
    assert(it == dma_data_end);
    assert(!(3u & (uintptr_t) (it))); // should end on dword boundary
#if 0
    // should probably have this back ignored for now because of overlays which don't bother
    if (!overlay) {
        assert(!expected_width || pixels == pixel_buffer +
                                            expected_width); // with the correct number of pixels (one more because we stick a black pixel on the end)
    }
#else
    if (expected_width && pixels < pixel_buffer + expected_width) {
        // black out rest of line
        if (!overlay) memset(pixels, 0, (expected_width - (pixels - pixel_buffer)) * sizeof(uint16_t));
    }
#endif
    assert(last_was_black);
}
#endif

#endif

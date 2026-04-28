/*
 * Copyright (c) 20222 Graham Sanderson
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "pico.h"

#include <string.h>

#if PICO_ON_DEVICE
static inline char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return c;
}

int strnicmp(const char *a, const char *b, size_t len) {
    int diff = 0;
    for(uint i=0; i<len && a[i]; i++) {
        diff = to_lower(a[i]) - to_lower(b[i]);
        if (diff) break;
    }
    return diff;
}

int stricmp(const char *a, const char *b) {
    return strnicmp(a, b, strlen(a));
}

#if PICOTRACKER
volatile uint32_t __attribute__((used)) picotracker_fault_sp;
volatile uint32_t __attribute__((used)) picotracker_fault_pc;
volatile uint32_t __attribute__((used)) picotracker_fault_lr;
volatile uint32_t __attribute__((used)) picotracker_fault_xpsr;
volatile uint32_t __attribute__((used)) picotracker_fault_exc_return;

void __attribute__((noinline, used)) picotracker_hardfault_capture(uint32_t *stack, uint32_t exc_return) {
    picotracker_fault_sp = (uint32_t)stack;
    picotracker_fault_pc = stack[6];
    picotracker_fault_lr = stack[5];
    picotracker_fault_xpsr = stack[7];
    picotracker_fault_exc_return = exc_return;
    __asm volatile("bkpt #0");
    while (1) {
        __asm volatile("wfi");
    }
}

void __attribute__((naked, used)) isr_hardfault(void) {
    __asm volatile(
            "movs r2, #4\n"
            "mov r1, lr\n"
            "tst r1, r2\n"
            "beq 1f\n"
            "mrs r0, psp\n"
            "b 2f\n"
            "1:\n"
            "mrs r0, msp\n"
            "2:\n"
            "b picotracker_hardfault_capture\n"
    );
}
#endif

#endif

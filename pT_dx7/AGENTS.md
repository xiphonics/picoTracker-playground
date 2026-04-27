# Repository Guidelines

## Project Structure & Module Organization
`src/` contains the firmware entry point (`main.c`), MIDI transport, USB descriptors, and the generated DX7 patch header (`dx7_patches.h`). 
`audio/` has FM synth, codec, audio output, PIO programs 
`display/` contains the LCD driver and startup UI code
`platform/` covers board-specific clocks and input handling

Root-level build files live in `CMakeLists.txt`.

## Build, Test, and Development Commands
Configure against the Pico SDK:

```sh
PICO_SDK_PATH=/path/to/pico-sdk cmake -S . -B build -DPICO_BOARD=pico
cmake --build build -j
```

The main output is `build/pT_dx7.uf2`.

## Coding Style & Naming Conventions
Match the existing style in each language: `static` helpers for file-local logic and `snake_case` for functions and variables (`battery_monitor_sample`). Macros and compile-time flags use uppercase. No formatter config is checked in, so keep includes grouped and changes mechanically minimal.
Use #define for consts instead of hardcoding magic numberor string literals.

## Testing Guidelines
No automated unit-test suite. Every change should at least pass a clean CMake build and a hardware smoke test on picoTracker hardware.

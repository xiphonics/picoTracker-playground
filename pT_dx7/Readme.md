# pT_dx7

Standalone DX7 firmware for the original RP2040-based picoTracker hardware.

## Build

```sh
cd ..
PICO_SDK_PATH=/path/to/pico-sdk cmake -S pT_dx7 -B pT_dx7/build -DPICO_BOARD=pico
cmake --build pT_dx7/build -j
```

The build produces `pT_dx7/build/pT_dx7.uf2`.

## Flash

1. Hold `BOOTSEL` while connecting the picoTracker pico over USB.
2. Copy `pT_dx7.uf2` onto the mounted `RPI-RP2` drive.
3. Reflash the normal picoTracker UF2 to return to stock firmware.

## Controls

The UI uses a unified cursor-based navigation. Use the arrow keys to select a parameter and modify its value.

- **UP / DOWN**: Move the cursor (`>`) between parameters (Patch, Bank, Reverb, etc.).
- **LEFT / RIGHT**: Change the value of the selected parameter.
- **EDIT + LEFT / RIGHT**: Adjust level and reverb parameters in larger steps (±16 instead of ±1).
- **PLAY**: Play a fixed `C4` preview note while held.
- **ENTER**: Panic button (instantly kills all active voices).

### UI Parameters

| Parameter | Description |
|---|---|
| **Patch** | Cycles through the 32 patches in the currently loaded ROM bank. |
| **Bank** | Cycles through the DX7 SysEx bank files (.syx) found in `/dx7patches` on the SD card. |
| **MASTER** | Overall output volume. |
| **VOICE** | Internal synth engine gain. Use this to balance patch volume before the master stage. |
| **REV WET** | Reverb mix level. 0 is dry, 255 is maximum wet signal. |
| **REV FB** | **Reverb Feedback**. Controls the decay time. Higher values create a longer, more sustaining tail. Capped at 0.85 to prevent runaway oscillation. |
| **REV DMP** | **Reverb Damping**. Controls high-frequency absorption in the feedback loop. Higher values result in a darker, warmer reverb tail. |
| **REV GAIN** | **Reverb Input Gain**. Controls the headroom into the reverb network (OFF, 0dB, -6dB, -12dB, -18dB). **Setting this to OFF completely disables the reverb DSP**, reclaiming CPU performance for maximum polyphony. |
| **OUT** | **Output Level toggle**. Switch between **HP** (safe level for headphones) and **LINE** (full-scale line-level output). |

The LCD shows the current patch, master level, voice level, preview note,
preview gate state, USB MIDI state, and battery status.

Incoming MIDI program change messages update the currently selected patch.

## MIDI Implementation

### Transports

| Transport | Details |
|---|---|
| **USB MIDI** | Device mode — connects to a host |
| **UART (TRS)** | Standard 5-pin MIDI IN |

### Received (MIDI IN) Messages

| Message | Channel | Behavior |
|---|---|---|
| **Note On** | All (1–16) | Triggers a voice. Velocity 0 is treated as Note Off. |
| **Note Off** | All (1–16) | Releases the voice. |
| **Program Change** | All (1–16) | Selects a patch (wrapped to the number of patches in the loaded bank). |
| **CC #0** (Bank Select MSB) | All (1–16) | Combined with CC #32 to select a ROM bank file from `/dx7patches`. |
| **CC #32** (Bank Select LSB) | All (1–16) | Combined with CC #0 to select a ROM bank file (0–16383). |
| **CC #7** (Volume) | All (1–16) | Sets the master output level (0–127 → 0–255). |
| **CC #11** (Expression) | All (1–16) | Sets the internal voice gain (0–127 → 0–255). |
| **CC #120** (All Notes Off) | Omni | Kills all active voices (panic). |
| **CC #123** (All Sound Off) | Omni | Kills all active voices (panic). |
| **MIDI Stop** | Omni | Stops playback and kills all voices. |
| **MIDI Reset** (0xFF) | Omni | Stops playback and kills all voices. |

### Notes

- **Running status** is supported for all standard short messages.
- **SysEx** (0xF0 … 0xF7) is parsed and queued but not dispatched as a MIDI IN message. SysEx files are loaded from the SD card (`/dx7patches/*.syx`).
- All other MIDI messages (MTC, Song Position, Tune Request, Active Sensing, etc.) are silently ignored.
- When USB MIDI is disconnected or reconnected, all voices are automatically killed to prevent stuck notes.

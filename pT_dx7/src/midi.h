#ifndef MIDI_H
#define MIDI_H

#include <stdbool.h>
#include <stdint.h>

typedef union {
  uint8_t b[3];
  uint8_t status;
  struct {
    uint8_t status;
    uint8_t note;
    uint8_t velocity;
  } note;
  struct {
    uint8_t status;
    uint8_t control;
    uint8_t value;
  } cc;
  struct {
    uint8_t status;
    uint8_t program;
  } program;
} MidiMessage;

enum {
  MIDI_NOTEOFF = 0x80,
  MIDI_NOTEON = 0x90,
  MIDI_CONTROLCHANGE = 0xb0,
  MIDI_PROGRAMCHANGE = 0xc0,
  MIDI_AFTERTOUCH = 0xd0,
  MIDI_PITCHWHEEL = 0xe0,
  MIDI_SYSEX = 0xf0,
  MIDI_CLOCK = 0xf8,
  MIDI_START = 0xfa,
  MIDI_CONTINUE = 0xfb,
  MIDI_STOP = 0xfc,
  MIDI_ACTIVESENSING = 0xfe,
  MIDI_RESET = 0xff,
};

typedef struct {
  uint32_t rx_messages;
  uint32_t tx_messages;
  uint32_t tx_sysex;
  uint32_t rx_parse_errors;
  uint32_t tx_drop_unmounted;
  uint32_t tx_drop_short_write;
  uint32_t tx_drop_queue_full;
} MidiStats;

static inline int midi_type(MidiMessage msg) {
  return msg.status >= 0xf0 ? msg.status : (msg.status & 0xf0);
}

static inline int midi_channel(MidiMessage msg) {
  return msg.status & 0x0f;
}

void midi_init(void);
void midi_service(void);
bool midi_receive(MidiMessage *out);
bool midi_ready(void);
bool midi_send(MidiMessage msg);
bool midi_send_sysex(const uint8_t *data, int len);
void midi_get_stats(MidiStats *out);

#endif

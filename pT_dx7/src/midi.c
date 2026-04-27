#include "midi.h"

#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico_gpio.h"
#include "tusb.h"

#ifndef DX_MIDI_DEBUG_LOG
#define DX_MIDI_DEBUG_LOG 0
#endif

#ifndef DX_MIDI_HOST_RX_DEBUG_LOG
#define DX_MIDI_HOST_RX_DEBUG_LOG 0
#endif

#ifndef USB_MIDI_HOST
#define USB_MIDI_HOST 0
#endif

#ifndef TINYUSB_HAS_MIDI_HOST
#define TINYUSB_HAS_MIDI_HOST 0
#endif

#if !USB_MIDI_HOST
typedef struct {
  uint8_t bytes[3];
  uint8_t len;
} MidiTxEvent;

typedef struct {
  uint16_t offset;
  uint16_t len;
  uint16_t sent;
} MidiSysExDesc;
#endif

typedef struct {
  MidiStats stats;
  uint8_t running_status;
  uint8_t data[2];
  uint8_t data_index;
  uint8_t expected_data;
  bool initialized;
  bool usb_initialized;
  bool last_mounted;
  bool uart_initialized;
  volatile uint8_t uart_rx_queue[128];
  volatile uint16_t uart_rx_head;
  volatile uint16_t uart_rx_tail;

  MidiMessage rx_queue[128];
  uint16_t rx_head;
  uint16_t rx_tail;
  uint16_t rx_count;

#if USB_MIDI_HOST
  bool host_device_mounted;
  bool host_midi_mounted;
  bool host_rx_pending;
  uint8_t host_device_addr;
  uint8_t host_midi_idx;
#else
  MidiTxEvent tx_queue[128];
  uint16_t tx_head;
  uint16_t tx_tail;
  uint16_t tx_count;

  MidiSysExDesc sysex_queue[8];
  uint8_t sysex_pool[4096];
  uint16_t sysex_head;
  uint16_t sysex_tail;
  uint16_t sysex_used;
  uint8_t sysex_q_head;
  uint8_t sysex_q_tail;
  uint8_t sysex_q_count;
#endif
} MidiRuntime;

static MidiRuntime g_midi;

static void midi_uart_init(void);
static void midi_uart_drain_rx(void);
void __isr __time_critical_func(midi_uart_irq_handler)(void);

static uint8_t midi_status_length(uint8_t status) {
  if (status >= 0xf8) {
    return 1;
  }
  if (status >= 0xf0) {
    switch (status) {
    case 0xf1:
    case 0xf3:
      return 2;
    case 0xf2:
      return 3;
    case 0xf6:
      return 1;
    default:
      return 1;
    }
  }

  switch (status & 0xf0) {
  case MIDI_PROGRAMCHANGE:
  case MIDI_AFTERTOUCH:
    return 2;
  case MIDI_NOTEOFF:
  case MIDI_NOTEON:
  case MIDI_CONTROLCHANGE:
  case MIDI_PITCHWHEEL:
    return 3;
  default:
    return 0;
  }
}

static bool midi_rx_queue_push(MidiMessage msg) {
  if (g_midi.rx_count >=
      (uint16_t)(sizeof(g_midi.rx_queue) / sizeof(g_midi.rx_queue[0]))) {
    g_midi.stats.rx_parse_errors++;
    return false;
  }
  g_midi.rx_queue[g_midi.rx_head] = msg;
  g_midi.rx_head = (uint16_t)(
      (g_midi.rx_head + 1u) % (sizeof(g_midi.rx_queue) / sizeof(g_midi.rx_queue[0])));
  g_midi.rx_count++;
  g_midi.stats.rx_messages++;
  return true;
}

static void midi_feed_byte(uint8_t value) {
  if (value & 0x80u) {
    if (value == MIDI_SYSEX) {
      g_midi.running_status = 0;
      g_midi.data_index = 0;
      g_midi.expected_data = 0;
      return;
    }

    {
      uint8_t len = midi_status_length(value);
      if (len == 0) {
        g_midi.stats.rx_parse_errors++;
        return;
      }

      if (len == 1) {
        MidiMessage msg = {.status = value};
        (void)midi_rx_queue_push(msg);
        if (value < 0xf0) {
          g_midi.running_status = value;
        }
        return;
      }

      g_midi.running_status = value;
      g_midi.data_index = 0;
      g_midi.expected_data = (uint8_t)(len - 1u);
      return;
    }
  }

  if (g_midi.running_status == 0 || g_midi.expected_data == 0) {
    g_midi.stats.rx_parse_errors++;
    return;
  }

  g_midi.data[g_midi.data_index++] = value;
  if (g_midi.data_index >= g_midi.expected_data) {
    MidiMessage msg = {.status = g_midi.running_status};
    msg.b[1] = g_midi.data[0];
    msg.b[2] = g_midi.expected_data > 1 ? g_midi.data[1] : 0;
    (void)midi_rx_queue_push(msg);
    g_midi.data_index = 0;
  }
}

void __isr __time_critical_func(midi_uart_irq_handler)(void) {
  uint32_t status = uart_get_hw(MIDI_UART)->mis;

  if ((status & (UART_UARTMIS_RXMIS_BITS | UART_UARTMIS_RTMIS_BITS)) == 0u) {
    return;
  }

  while (uart_is_readable(MIDI_UART)) {
    const uint8_t value = uart_getc(MIDI_UART);
    const uint16_t next_head = (uint16_t)((g_midi.uart_rx_head + 1u) %
                                          (sizeof(g_midi.uart_rx_queue) /
                                           sizeof(g_midi.uart_rx_queue[0])));

    if (next_head == g_midi.uart_rx_tail) {
      g_midi.stats.rx_parse_errors++;
      continue;
    }

    g_midi.uart_rx_queue[g_midi.uart_rx_head] = value;
    g_midi.uart_rx_head = next_head;
  }
}

static void midi_uart_init(void) {
  int actual_baud;

  gpio_set_function(MIDI_OUT_PIN, GPIO_FUNC_UART);
  gpio_set_function(MIDI_IN_PIN, GPIO_FUNC_UART);

  actual_baud = uart_init(MIDI_UART, MIDI_BAUD_RATE);
  uart_set_hw_flow(MIDI_UART, false, false);
  uart_set_fifo_enabled(MIDI_UART, false);
  uart_set_format(MIDI_UART, 8, 1, UART_PARITY_NONE);
  uart_set_translate_crlf(MIDI_UART, false);

  irq_set_exclusive_handler(MIDI_UART_IRQ, midi_uart_irq_handler);
  irq_set_enabled(MIDI_UART_IRQ, true);
  uart_set_irq_enables(MIDI_UART, true, false);

  g_midi.uart_initialized = true;
  printf("UART MIDI ready: uart%d GPIO%d/%d @ %d baud\n",
         MIDI_UART == uart0 ? 0 : 1, MIDI_OUT_PIN, MIDI_IN_PIN, actual_baud);
}

static void midi_uart_drain_rx(void) {
  while (g_midi.uart_initialized) {
    uint32_t interrupt_state;
    uint8_t value;

    interrupt_state = save_and_disable_interrupts();
    if (g_midi.uart_rx_tail == g_midi.uart_rx_head) {
      restore_interrupts(interrupt_state);
      return;
    }

    value = g_midi.uart_rx_queue[g_midi.uart_rx_tail];
    g_midi.uart_rx_tail = (uint16_t)((g_midi.uart_rx_tail + 1u) %
                                     (sizeof(g_midi.uart_rx_queue) /
                                      sizeof(g_midi.uart_rx_queue[0])));
    restore_interrupts(interrupt_state);

    midi_feed_byte(value);
  }
}

#if !USB_MIDI_HOST
static int midi_usb_packet_data_len(uint8_t cin) {
  switch (cin) {
  case 0x2:
  case 0x6:
  case 0xc:
  case 0xd:
    return 2;
  case 0x5:
  case 0xf:
    return 1;
  case 0x4:
  case 0x7:
  case 0x8:
  case 0x9:
  case 0xa:
  case 0xb:
  case 0xe:
    return 3;
  default:
    return 0;
  }
}

static bool midi_tx_queue_push(const uint8_t *data, uint8_t len) {
  MidiTxEvent *ev;

  if (data == NULL || len == 0u || len > 3u) {
    return false;
  }
  if (g_midi.tx_count >=
      (uint16_t)(sizeof(g_midi.tx_queue) / sizeof(g_midi.tx_queue[0]))) {
    g_midi.stats.tx_drop_queue_full++;
    return false;
  }

  ev = &g_midi.tx_queue[g_midi.tx_head];
  memcpy(ev->bytes, data, len);
  ev->len = len;
  g_midi.tx_head = (uint16_t)(
      (g_midi.tx_head + 1u) % (sizeof(g_midi.tx_queue) / sizeof(g_midi.tx_queue[0])));
  g_midi.tx_count++;
  return true;
}

static bool midi_sysex_pool_write(const uint8_t *data, uint16_t len,
                                  uint16_t *offset_out) {
  uint16_t offset;
  uint16_t first;

  if (data == NULL || offset_out == NULL || len == 0u ||
      len > (uint16_t)sizeof(g_midi.sysex_pool)) {
    return false;
  }
  if (g_midi.sysex_q_count >=
          (uint8_t)(sizeof(g_midi.sysex_queue) / sizeof(g_midi.sysex_queue[0])) ||
      (uint32_t)g_midi.sysex_used + len > sizeof(g_midi.sysex_pool)) {
    g_midi.stats.tx_drop_queue_full++;
    return false;
  }

  offset = g_midi.sysex_head;
  first = len;
  if ((uint32_t)offset + len > sizeof(g_midi.sysex_pool)) {
    first = (uint16_t)(sizeof(g_midi.sysex_pool) - offset);
  }
  memcpy(&g_midi.sysex_pool[offset], data, first);
  if (first < len) {
    memcpy(g_midi.sysex_pool, data + first, len - first);
  }

  g_midi.sysex_head = (uint16_t)((offset + len) % sizeof(g_midi.sysex_pool));
  g_midi.sysex_used = (uint16_t)(g_midi.sysex_used + len);
  *offset_out = offset;
  return true;
}

static bool midi_sysex_queue_push(const uint8_t *data, uint16_t len) {
  uint16_t offset = 0u;
  MidiSysExDesc *desc;

  if (!midi_sysex_pool_write(data, len, &offset)) {
    return false;
  }

  desc = &g_midi.sysex_queue[g_midi.sysex_q_head];
  desc->offset = offset;
  desc->len = len;
  desc->sent = 0u;
  g_midi.sysex_q_head = (uint8_t)((g_midi.sysex_q_head + 1u) %
                                  (sizeof(g_midi.sysex_queue) /
                                   sizeof(g_midi.sysex_queue[0])));
  g_midi.sysex_q_count++;
  return true;
}

static void midi_sysex_queue_pop_complete(void) {
  MidiSysExDesc *desc;

  if (g_midi.sysex_q_count == 0u) {
    return;
  }

  desc = &g_midi.sysex_queue[g_midi.sysex_q_tail];
  g_midi.sysex_tail =
      (uint16_t)((desc->offset + desc->len) % sizeof(g_midi.sysex_pool));
  g_midi.sysex_used = (uint16_t)(g_midi.sysex_used - desc->len);
  g_midi.sysex_q_tail = (uint8_t)((g_midi.sysex_q_tail + 1u) %
                                  (sizeof(g_midi.sysex_queue) /
                                   sizeof(g_midi.sysex_queue[0])));
  g_midi.sysex_q_count--;
}

static uint8_t midi_tx_cin_for_short_message(const MidiTxEvent *ev) {
  uint8_t status;

  if (ev == NULL || ev->len == 0u) {
    return 0u;
  }

  status = ev->bytes[0];
  if (status < 0xf0u) {
    switch (status & 0xf0u) {
    case MIDI_NOTEOFF:
    case MIDI_NOTEON:
    case MIDI_CONTROLCHANGE:
    case MIDI_PITCHWHEEL:
      return (uint8_t)(status >> 4);
    case MIDI_PROGRAMCHANGE:
      return 0x0cu;
    case MIDI_AFTERTOUCH:
      return 0x0du;
    default:
      return 0u;
    }
  }

  switch (status) {
  case 0xf1:
  case 0xf3:
    return 0x02u;
  case 0xf2:
    return 0x03u;
  case 0xf6:
    return 0x05u;
  case 0xf8:
  case 0xfa:
  case 0xfb:
  case 0xfc:
  case 0xfe:
  case 0xff:
    return 0x0fu;
  default:
    return ev->len == 1u ? 0x0fu : 0u;
  }
}

static bool midi_write_short_event(const MidiTxEvent *ev) {
  uint8_t packet[4] = {0};
  uint8_t cin;

  if (ev == NULL || ev->len == 0u || ev->len > 3u) {
    return false;
  }

  cin = midi_tx_cin_for_short_message(ev);
  if (cin == 0u) {
    return false;
  }

  packet[0] = cin;
  memcpy(&packet[1], ev->bytes, ev->len);
  return tud_midi_packet_write(packet);
}

static uint8_t midi_sysex_peek_byte(uint16_t offset, uint16_t advance) {
  uint16_t index = (uint16_t)((offset + advance) % sizeof(g_midi.sysex_pool));
  return g_midi.sysex_pool[index];
}

static uint8_t midi_sysex_packet_len(const MidiSysExDesc *desc) {
  uint16_t remaining;

  if (desc == NULL || desc->sent >= desc->len) {
    return 0u;
  }

  remaining = (uint16_t)(desc->len - desc->sent);
  return remaining >= 3u ? 3u : (uint8_t)remaining;
}

static uint8_t midi_sysex_packet_cin(const MidiSysExDesc *desc,
                                     uint8_t packet_len) {
  uint8_t last_byte;

  if (desc == NULL || packet_len == 0u) {
    return 0u;
  }

  last_byte = midi_sysex_peek_byte(
      desc->offset, (uint16_t)(desc->sent + packet_len - 1u));
  if (last_byte == 0xf7u) {
    return (uint8_t)(0x04u + packet_len);
  }

  return 0x04u;
}

static bool midi_write_sysex_packet(const MidiSysExDesc *desc,
                                    uint8_t *sent_out) {
  uint8_t packet[4] = {0};
  uint8_t packet_len;
  uint8_t cin;

  if (desc == NULL || sent_out == NULL) {
    return false;
  }

  packet_len = midi_sysex_packet_len(desc);
  cin = midi_sysex_packet_cin(desc, packet_len);
  if (packet_len == 0u || cin == 0u) {
    return false;
  }

  packet[0] = cin;
  for (uint8_t i = 0; i < packet_len; ++i) {
    packet[1 + i] =
        midi_sysex_peek_byte(desc->offset, (uint16_t)(desc->sent + i));
  }

  if (!tud_midi_packet_write(packet)) {
    return false;
  }

  *sent_out = packet_len;
  return true;
}

static void midi_drain_tx(void) {
  uint32_t budget_packets = 128u;
  uint32_t budget_msgs = 24u;

  if (!tud_midi_mounted()) {
    return;
  }

  while (budget_msgs > 0u && budget_packets > 0u) {
    if (g_midi.tx_count > 0u) {
      MidiTxEvent *ev = &g_midi.tx_queue[g_midi.tx_tail];
      if (!midi_write_short_event(ev)) {
        g_midi.stats.tx_drop_short_write++;
        break;
      }
      budget_packets--;
      budget_msgs--;
      g_midi.tx_tail = (uint16_t)((g_midi.tx_tail + 1u) %
                                  (sizeof(g_midi.tx_queue) /
                                   sizeof(g_midi.tx_queue[0])));
      g_midi.tx_count--;
      g_midi.stats.tx_messages++;
      continue;
    }

    if (g_midi.sysex_q_count > 0u) {
      MidiSysExDesc *desc = &g_midi.sysex_queue[g_midi.sysex_q_tail];
      uint8_t sent = 0u;
      if (!midi_write_sysex_packet(desc, &sent)) {
        g_midi.stats.tx_drop_short_write++;
        break;
      }
      desc->sent = (uint16_t)(desc->sent + sent);
      budget_packets--;
      budget_msgs--;
      if (desc->sent >= desc->len) {
        midi_sysex_queue_pop_complete();
        g_midi.stats.tx_sysex++;
      }
      continue;
    }

    break;
  }
}
#endif

static void midi_log_mount_state(bool mounted) {
#if DX_MIDI_DEBUG_LOG
  printf("MIDI USB mounted=%d\n", mounted ? 1 : 0);
#else
  (void)mounted;
#endif
}

#if USB_MIDI_HOST
static void midi_host_log_stream(uint8_t idx, uint8_t cable_num,
                                 const uint8_t *buffer, uint32_t len) {
#if DX_MIDI_HOST_RX_DEBUG_LOG
  if (buffer == NULL || len == 0u) {
    return;
  }

  printf("USB MIDI host rx: idx=%u cable=%u len=%lu data=", idx, cable_num,
         (unsigned long)len);
  for (uint32_t i = 0; i < len; ++i) {
    printf("%02X", buffer[i]);
    if ((i + 1u) < len) {
      putchar(' ');
    }
  }
  printf("\n");
#else
  (void)idx;
  (void)cable_num;
  (void)buffer;
  (void)len;
#endif
}

static void midi_feed_bytes(const uint8_t *buffer, uint32_t len) {
  if (buffer == NULL) {
    return;
  }

  for (uint32_t i = 0; i < len; ++i) {
    midi_feed_byte(buffer[i]);
  }
}

static bool midi_host_drain_rx(uint8_t idx, uint32_t budget_reads) {
#if TINYUSB_HAS_MIDI_HOST
  uint8_t buffer[48];

  for (uint32_t i = 0; i < budget_reads && tuh_midi_mounted(idx); ++i) {
    uint8_t cable_num = 0u;
    uint32_t bytes_read =
        tuh_midi_stream_read(idx, &cable_num, buffer, sizeof(buffer));
    if (bytes_read == 0u) {
      return false;
    }

    midi_host_log_stream(idx, cable_num, buffer, bytes_read);
    midi_feed_bytes(buffer, bytes_read);
  }
  return tuh_midi_mounted(idx);
#else
  (void)idx;
  (void)budget_reads;
  return false;
#endif
}

void tuh_mount_cb(uint8_t dev_addr) {
  g_midi.host_device_mounted = true;
  g_midi.host_device_addr = dev_addr;
  printf("USB host device mounted: addr=%u\n", dev_addr);
}

void tuh_umount_cb(uint8_t dev_addr) {
  if (g_midi.host_device_addr == dev_addr) {
    g_midi.host_device_mounted = false;
    g_midi.host_device_addr = 0u;
  }
  printf("USB host device unmounted: addr=%u\n", dev_addr);
}

#if TINYUSB_HAS_MIDI_HOST
void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount_cb_data) {
  g_midi.host_midi_idx = idx;
  g_midi.host_midi_mounted = true;
  g_midi.host_rx_pending = false;
  printf(
      "USB MIDI host mounted: idx=%u addr=%u itf=%u rx_cables=%u tx_cables=%u\n",
      idx, mount_cb_data->daddr, mount_cb_data->bInterfaceNumber,
      mount_cb_data->rx_cable_count, mount_cb_data->tx_cable_count);
}

void tuh_midi_umount_cb(uint8_t idx) {
  if (g_midi.host_midi_idx == idx) {
    g_midi.host_midi_mounted = false;
    g_midi.host_rx_pending = false;
  }
  printf("USB MIDI host unmounted: idx=%u\n", idx);
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes) {
  if (xferred_bytes == 0u || !g_midi.host_midi_mounted ||
      g_midi.host_midi_idx != idx) {
    return;
  }
  g_midi.host_rx_pending = true;
}

void tuh_midi_tx_cb(uint8_t idx, uint32_t xferred_bytes) {
  (void)idx;
  (void)xferred_bytes;
}
#endif
#endif

void midi_init(void) {
  memset(&g_midi, 0, sizeof(g_midi));
  board_init();
  midi_uart_init();
  g_midi.initialized = g_midi.uart_initialized;
#if USB_MIDI_HOST
  const tusb_rhport_init_t host_init = {
      .role = TUSB_ROLE_HOST,
      .speed = TUSB_SPEED_AUTO,
  };
  if (tusb_init(BOARD_TUH_RHPORT, &host_init)) {
    if (board_init_after_tusb) {
      board_init_after_tusb();
    }
    g_midi.usb_initialized = true;
  }
#else
  if (tusb_init()) {
    g_midi.usb_initialized = true;
  }
#endif
}

void midi_service(void) {
#if USB_MIDI_HOST
  bool mounted;
#else
  uint8_t packet[4];
  uint32_t budget_packets = 64u;
  bool mounted;
#endif

  if (!g_midi.initialized) {
    return;
  }

  midi_uart_drain_rx();

#if USB_MIDI_HOST
  const uint32_t host_rx_budget_reads = 4u;
  if (g_midi.usb_initialized) {
    tuh_task();
#if TINYUSB_HAS_MIDI_HOST
    if (g_midi.host_midi_mounted && g_midi.host_rx_pending) {
      g_midi.host_rx_pending =
          midi_host_drain_rx(g_midi.host_midi_idx, host_rx_budget_reads);
    }
    mounted = g_midi.host_midi_mounted;
#else
    mounted = false;
#endif
  } else {
    mounted = false;
  }
#else
  if (g_midi.usb_initialized) {
    tud_task();
    mounted = tud_midi_mounted();
    while (budget_packets-- > 0u && tud_midi_packet_read(packet)) {
      int len = midi_usb_packet_data_len((uint8_t)(packet[0] & 0x0fu));
      for (int i = 0; i < len; ++i) {
        midi_feed_byte(packet[1 + i]);
      }
    }

    midi_drain_tx();
  } else {
    mounted = false;
  }
#endif

  if (mounted != g_midi.last_mounted) {
    g_midi.last_mounted = mounted;
    midi_log_mount_state(mounted);
  }
}

bool midi_receive(MidiMessage *out) {
  if (out == NULL || g_midi.rx_count == 0u) {
    return false;
  }
  *out = g_midi.rx_queue[g_midi.rx_tail];
  g_midi.rx_tail = (uint16_t)(
      (g_midi.rx_tail + 1u) % (sizeof(g_midi.rx_queue) / sizeof(g_midi.rx_queue[0])));
  g_midi.rx_count--;
  return true;
}

bool midi_ready(void) {
#if USB_MIDI_HOST
#if TINYUSB_HAS_MIDI_HOST
  return g_midi.initialized && g_midi.host_midi_mounted;
#else
  return false;
#endif
#else
  return g_midi.initialized && g_midi.usb_initialized && tud_midi_mounted();
#endif
}

bool midi_send(MidiMessage msg) {
  uint8_t len = midi_status_length(msg.status);
  if (len == 0u || len > 3u || !g_midi.initialized) {
    return false;
  }
#if USB_MIDI_HOST
  g_midi.stats.tx_drop_unmounted++;
  return false;
#else
  if (!g_midi.usb_initialized || !tud_midi_mounted()) {
    g_midi.stats.tx_drop_unmounted++;
  }
  return midi_tx_queue_push(msg.b, len);
#endif
}

bool midi_send_sysex(const uint8_t *data, int len) {
  if (data == NULL || len <= 0 || !g_midi.initialized) {
    return false;
  }
#if USB_MIDI_HOST
  g_midi.stats.tx_drop_unmounted++;
  (void)len;
  return false;
#else
  if (!g_midi.usb_initialized || !tud_midi_mounted()) {
    g_midi.stats.tx_drop_unmounted++;
  }
  if (len > 0xffff) {
    g_midi.stats.tx_drop_queue_full++;
    return false;
  }
  return midi_sysex_queue_push(data, (uint16_t)len);
#endif
}

void midi_get_stats(MidiStats *out) {
  if (out == NULL) {
    return;
  }
  *out = g_midi.stats;
}

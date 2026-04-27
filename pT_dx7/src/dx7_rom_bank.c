#include "dx7_rom_bank.h"

#include <stdio.h>
#include <string.h>

#include "dx7_patches.h"

enum {
  DX7_BULK_HEADER_SIZE = 6u,
  DX7_BULK_TRAILER_SIZE = 2u,
  DX7_PATCH_NAME_OFFSET = 118u,
  DX7_PATCH_NAME_SIZE = 10u,
  DX7_BULK_PAYLOAD_SIZE =
      DX7_ROM_BANK_PATCH_COUNT * DX7_ROM_BANK_PATCH_SIZE,
  DX7_BULK_TOTAL_SIZE =
      DX7_BULK_HEADER_SIZE + DX7_BULK_PAYLOAD_SIZE + DX7_BULK_TRAILER_SIZE,
};

static const char *const k_builtin_rom_name = "built-in";
static uint8_t g_active_bank[DX7_ROM_BANK_PATCH_COUNT][DX7_ROM_BANK_PATCH_SIZE];
static bool g_ram_bank_loaded = false;
static char g_active_name[DX7_ROM_BANK_NAME_MAX];
static char g_last_error[96];

_Static_assert(DX7_NUM_PATCHES > 0, "built-in DX7 bank must contain a patch");

static void dx7_rom_bank_set_last_error(const char *message) {
  if (message == NULL) {
    g_last_error[0] = '\0';
    return;
  }

  snprintf(g_last_error, sizeof(g_last_error), "%s", message);
}

static bool dx7_rom_bank_copy_string(char *buffer, size_t buffer_size,
                                     const char *text) {
  size_t text_length;

  if (buffer == NULL || buffer_size == 0u || text == NULL) {
    return false;
  }

  text_length = strlen(text);
  if (text_length >= buffer_size) {
    return false;
  }

  memcpy(buffer, text, text_length + 1u);
  return true;
}

static uint8_t dx7_rom_bank_checksum(const uint8_t *payload,
                                     size_t payload_size) {
  uint32_t sum = 0u;

  if (payload == NULL) {
    return 0u;
  }

  for (size_t i = 0; i < payload_size; ++i) {
    sum += payload[i];
  }

  return (uint8_t)((uint8_t)(0u - (uint8_t)sum) & 0x7Fu);
}

void dx7_rom_bank_init(void) {
  g_ram_bank_loaded = false;
  (void)dx7_rom_bank_copy_string(g_active_name, sizeof(g_active_name),
                                 k_builtin_rom_name);
  dx7_rom_bank_set_last_error(NULL);
}

size_t dx7_rom_bank_patch_count(void) {
  return g_ram_bank_loaded ? DX7_ROM_BANK_PATCH_COUNT : DX7_NUM_PATCHES;
}

const uint8_t *dx7_rom_bank_patch_data(uint8_t patch_index) {
  if (g_ram_bank_loaded) {
    if (patch_index >= DX7_ROM_BANK_PATCH_COUNT) {
      return NULL;
    }
    return g_active_bank[patch_index];
  }

  if (patch_index >= DX7_NUM_PATCHES) {
    return NULL;
  }

  return dx7_patches[patch_index];
}

void dx7_rom_bank_patch_name_copy(uint8_t patch_index, char *buffer,
                                  size_t buffer_size) {
  const uint8_t *patch_data;
  size_t name_length = DX7_PATCH_NAME_SIZE;

  if (buffer == NULL || buffer_size == 0u) {
    return;
  }

  buffer[0] = '\0';
  patch_data = dx7_rom_bank_patch_data(patch_index);
  if (patch_data == NULL) {
    return;
  }

  while (name_length > 0u &&
         patch_data[DX7_PATCH_NAME_OFFSET + name_length - 1u] == ' ') {
    --name_length;
  }

  if (name_length >= buffer_size) {
    name_length = buffer_size - 1u;
  }

  for (size_t i = 0; i < name_length; ++i) {
    buffer[i] = (char)patch_data[DX7_PATCH_NAME_OFFSET + i];
  }
  buffer[name_length] = '\0';
}

const char *dx7_rom_bank_active_name(void) {
  return g_active_name[0] != '\0' ? g_active_name : k_builtin_rom_name;
}

bool dx7_rom_bank_load_sysex(const uint8_t *data, size_t size,
                             const char *source_name) {
  const uint8_t *payload;
  uint8_t checksum;

  if (data == NULL) {
    dx7_rom_bank_set_last_error("unsupported SysEx format: null data");
    return false;
  }

  if (size != DX7_BULK_TOTAL_SIZE) {
    if (size > DX7_BULK_TOTAL_SIZE) {
      dx7_rom_bank_set_last_error("unsupported SysEx format: unexpected size");
    } else {
      dx7_rom_bank_set_last_error("unsupported SysEx format: truncated file");
    }
    return false;
  }

  if (data[0] != 0xF0u || data[1] != 0x43u || (data[2] & 0xF0u) != 0x00u ||
      data[3] != 0x09u || data[4] != 0x20u || data[5] != 0x00u ||
      data[size - 1u] != 0xF7u) {
    dx7_rom_bank_set_last_error("unsupported SysEx format");
    return false;
  }

  payload = &data[DX7_BULK_HEADER_SIZE];
  checksum = dx7_rom_bank_checksum(payload, DX7_BULK_PAYLOAD_SIZE);
  if (data[size - 2u] != checksum) {
    dx7_rom_bank_set_last_error("bad checksum");
    return false;
  }

  memcpy(g_active_bank, payload, DX7_BULK_PAYLOAD_SIZE);
  g_ram_bank_loaded = true;
  if (source_name == NULL || source_name[0] == '\0' ||
      !dx7_rom_bank_copy_string(g_active_name, sizeof(g_active_name),
                                source_name)) {
    (void)dx7_rom_bank_copy_string(g_active_name, sizeof(g_active_name),
                                   "sd rom");
  }
  dx7_rom_bank_set_last_error(NULL);
  return true;
}

bool dx7_rom_bank_last_error(char *buffer, size_t buffer_size) {
  if (g_last_error[0] == '\0') {
    if (buffer != NULL && buffer_size > 0u) {
      buffer[0] = '\0';
    }
    return false;
  }

  return dx7_rom_bank_copy_string(buffer, buffer_size, g_last_error);
}

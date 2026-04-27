#ifndef DX7_ROM_BANK_H
#define DX7_ROM_BANK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  DX7_ROM_BANK_PATCH_COUNT = 32,
  DX7_ROM_BANK_PATCH_SIZE = 128,
  DX7_ROM_BANK_NAME_MAX = 64,
};

void dx7_rom_bank_init(void);
size_t dx7_rom_bank_patch_count(void);
const uint8_t *dx7_rom_bank_patch_data(uint8_t patch_index);
void dx7_rom_bank_patch_name_copy(uint8_t patch_index, char *buffer,
                                  size_t buffer_size);
const char *dx7_rom_bank_active_name(void);
bool dx7_rom_bank_load_sysex(const uint8_t *data, size_t size,
                             const char *source_name);
bool dx7_rom_bank_last_error(char *buffer, size_t buffer_size);

#endif

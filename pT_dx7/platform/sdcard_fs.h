#ifndef SDCARD_FS_H
#define SDCARD_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  DX7_ROM_FILE_MAX = 128,
  DX7_ROM_NAME_MAX = 64,
};

bool sdcard_fs_init(void);
bool sdcard_fs_refresh_rom_dir(void);
size_t sdcard_fs_rom_count(void);
bool sdcard_fs_get_rom_name(size_t index, char *buffer, size_t buffer_size);
bool sdcard_fs_read_rom_file(size_t index, uint8_t *buffer, size_t capacity,
                             size_t *bytes_read);
bool sdcard_fs_last_error(char *buffer, size_t buffer_size);
bool sdcard_fs_is_mounted(void);
bool sdcard_fs_rom_dir_found(void);

#ifdef __cplusplus
}
#endif

#endif

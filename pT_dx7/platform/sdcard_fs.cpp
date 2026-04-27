#include "sdcard_fs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SdFat.h"
#include "pico/stdlib.h"
#include "sdcard/sdcard.h"

namespace {

constexpr const char *kRomDirectory = "/dx7patches";

SdFs g_sd;
bool g_mount_attempted = false;
bool g_mounted = false;
bool g_rom_dir_found = false;
char g_last_error[96];
size_t g_rom_file_count = 0u;
char g_rom_names[DX7_ROM_FILE_MAX][DX7_ROM_NAME_MAX];

void set_last_error(const char *message) {
  if (message == nullptr) {
    g_last_error[0] = '\0';
    return;
  }

  snprintf(g_last_error, sizeof(g_last_error), "%s", message);
}

template <typename... Args>
void set_last_errorf(const char *format, Args... args) {
  snprintf(g_last_error, sizeof(g_last_error), format, args...);
}

int compare_case_insensitive_char(char lhs, char rhs) {
  const int lhs_folded = tolower((unsigned char)lhs);
  const int rhs_folded = tolower((unsigned char)rhs);
  if (lhs_folded != rhs_folded) {
    return lhs_folded - rhs_folded;
  }
  return (unsigned char)lhs - (unsigned char)rhs;
}

int compare_rom_names(const void *lhs, const void *rhs) {
  const char *lhs_name = static_cast<const char *>(lhs);
  const char *rhs_name = static_cast<const char *>(rhs);

  while (*lhs_name != '\0' && *rhs_name != '\0') {
    const int diff = compare_case_insensitive_char(*lhs_name, *rhs_name);
    if (diff != 0) {
      return diff;
    }
    ++lhs_name;
    ++rhs_name;
  }

  return compare_case_insensitive_char(*lhs_name, *rhs_name);
}

bool has_sysex_extension(const char *name) {
  size_t length;

  if (name == nullptr) {
    return false;
  }

  length = strlen(name);
  if (length < 4u) {
    return false;
  }

  return tolower((unsigned char)name[length - 4u]) == '.' &&
         tolower((unsigned char)name[length - 3u]) == 's' &&
         tolower((unsigned char)name[length - 2u]) == 'y' &&
         tolower((unsigned char)name[length - 1u]) == 'x';
}

bool copy_string(char *dst, size_t dst_size, const char *src) {
  const size_t src_len = strlen(src);

  if (dst == nullptr || dst_size == 0u) {
    return false;
  }

  if (src_len >= dst_size) {
    return false;
  }

  memcpy(dst, src, src_len + 1u);
  return true;
}

bool build_rom_path(size_t index, char *buffer, size_t buffer_size) {
  if (index >= g_rom_file_count || buffer == nullptr || buffer_size == 0u) {
    return false;
  }

  const int written = snprintf(buffer, buffer_size, "%s/%s", kRomDirectory,
                               g_rom_names[index]);
  return written > 0 && (size_t)written < buffer_size;
}

}  // namespace

uint32_t millis(void) { return to_ms_since_boot(get_absolute_time()); }

uint32_t micros(void) { return time_us_32(); }

extern "C" bool sdcard_fs_init(void) {
  if (g_mount_attempted) {
    return g_mounted;
  }

  g_mount_attempted = true;
  g_rom_dir_found = false;
  g_rom_file_count = 0u;
  set_last_error(nullptr);

  if (!g_sd.begin(SD_CONFIG)) {
    const SdCard *card = g_sd.card();
    if (card != nullptr && card->errorCode() != 0u) {
      set_last_errorf("mount failed (sd error %u)",
                      (unsigned int)card->errorCode());
    } else {
      set_last_error("mount failed");
    }
    return false;
  }

  g_mounted = true;
  return true;
}

extern "C" bool sdcard_fs_refresh_rom_dir(void) {
  FsFile dir;
  FsFile entry;

  g_rom_file_count = 0u;
  g_rom_dir_found = false;

  if (!sdcard_fs_init()) {
    return false;
  }

  dir = g_sd.open(kRomDirectory, O_RDONLY);
  if (!dir || !dir.isDir()) {
    set_last_error("/dx7patches missing");
    return false;
  }

  g_rom_dir_found = true;
  set_last_error(nullptr);

  while (entry.openNext(&dir, O_RDONLY)) {
    char name[DX7_ROM_NAME_MAX];

    if (entry.isDir()) {
      entry.close();
      continue;
    }

    if (entry.getName(name, sizeof(name)) == 0u || !has_sysex_extension(name)) {
      entry.close();
      continue;
    }

    if (g_rom_file_count < DX7_ROM_FILE_MAX) {
      (void)copy_string(g_rom_names[g_rom_file_count], sizeof(g_rom_names[0]),
                        name);
      ++g_rom_file_count;
    }

    entry.close();
  }

  dir.close();

  qsort(g_rom_names, g_rom_file_count, sizeof(g_rom_names[0]),
        compare_rom_names);

  return true;
}

extern "C" size_t sdcard_fs_rom_count(void) { return g_rom_file_count; }

extern "C" bool sdcard_fs_get_rom_name(size_t index, char *buffer,
                                        size_t buffer_size) {
  if (index >= g_rom_file_count) {
    set_last_error("rom index out of range");
    return false;
  }

  if (!copy_string(buffer, buffer_size, g_rom_names[index])) {
    set_last_error("rom name buffer too small");
    return false;
  }

  return true;
}

extern "C" bool sdcard_fs_read_rom_file(size_t index, uint8_t *buffer,
                                         size_t capacity, size_t *bytes_read) {
  FsFile file;
  char path[sizeof("/dx7patches/") + DX7_ROM_NAME_MAX];
  uint64_t file_size;
  int32_t read_size;

  if (bytes_read != nullptr) {
    *bytes_read = 0u;
  }

  if (buffer == nullptr) {
    set_last_error("null rom read buffer");
    return false;
  }

  if (index >= g_rom_file_count) {
    set_last_error("rom index out of range");
    return false;
  }

  if (!g_mounted) {
    set_last_error("sd card not mounted");
    return false;
  }

  if (!build_rom_path(index, path, sizeof(path))) {
    set_last_error("rom path build failed");
    return false;
  }

  file = g_sd.open(path, O_RDONLY);
  if (!file || file.isDir()) {
    set_last_error("failed to open rom file");
    return false;
  }

  file_size = file.fileSize();
  if (file_size > capacity) {
    set_last_error("rom file exceeds buffer capacity");
    file.close();
    return false;
  }

  read_size = file.read(buffer, (size_t)file_size);
  file.close();

  if (read_size < 0 || (uint64_t)read_size != file_size) {
    set_last_error("rom file read failed");
    return false;
  }

  if (bytes_read != nullptr) {
    *bytes_read = (size_t)read_size;
  }

  return true;
}

extern "C" bool sdcard_fs_last_error(char *buffer, size_t buffer_size) {
  if (g_last_error[0] == '\0') {
    if (buffer != nullptr && buffer_size > 0u) {
      buffer[0] = '\0';
    }
    return false;
  }

  return copy_string(buffer, buffer_size, g_last_error);
}

extern "C" bool sdcard_fs_is_mounted(void) { return g_mounted; }

extern "C" bool sdcard_fs_rom_dir_found(void) { return g_rom_dir_found; }

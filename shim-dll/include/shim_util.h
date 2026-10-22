#ifndef SHIM_UTIL_H_
#define SHIM_UTIL_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

/* Builds <game dir>\name, or a bare relative name if the exe path is unavailable. */
static inline void shim_game_path(char path[MAX_PATH], const char* name) {
  DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
  char* separator = length == 0 || length >= MAX_PATH ? NULL : strrchr(path, '\\');
  if (separator == NULL) {
    path[0] = '\0';
  } else {
    separator[1] = '\0';
  }
  strcat_s(path, MAX_PATH, name);
}

static inline bool write_memory(void* address, const void* data, size_t size) {
  DWORD old_protect;
  if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
  memcpy(address, data, size);
  FlushInstructionCache(GetCurrentProcess(), address, size);
  DWORD ignored;
  return VirtualProtect(address, size, old_protect, &ignored) != FALSE;
}

/* Overwrites replaced_length bytes with a JMP to target, padding with NOPs. */
static inline bool install_jump(uint8_t* address, size_t replaced_length,
                                const void* target) {
  uint8_t patch[16];
  if (replaced_length < 5 || replaced_length > sizeof(patch)) return false;
  memset(patch, 0x90, replaced_length);
  patch[0] = 0xE9;
  int32_t displacement = (int32_t)((const uint8_t*)target - (address + 5));
  memcpy(patch + 1, &displacement, sizeof(displacement));
  return write_memory(address, patch, replaced_length);
}

#endif /* SHIM_UTIL_H_ */

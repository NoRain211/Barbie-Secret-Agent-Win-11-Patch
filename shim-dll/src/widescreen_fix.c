/*
 * Native widescreen patch for Secret Agent Barbie.
 *
 * Patch signatures and rendering behavior are based on AlphaYellow's
 * SecretAgentBarbieWidescreenFix v1.4, used under the MIT License. See
 * THIRD_PARTY_NOTICES.md for attribution and license text.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "shim_log.h"
#include "widescreen_fix.h"

#define ORIGINAL_WIDTH 640
#define ORIGINAL_HEIGHT 480
#define ORIGINAL_ASPECT (4.0f / 3.0f)
#define CONFIG_NAME "SecretAgentBarbieWidescreenFix.ini"
#define EXTRA_RESOLUTION_RVA 0x000E0E98u
#define MENU_RESOLUTION_RVA 0x000F47BDu
#define FULLSCREEN_BITMAP_RVA 0x00061F30u

static const size_t k_splash_call_rvas[] = {0x000F490Cu, 0x000F4930u};

typedef struct {
  uint8_t* address;
  size_t length;
} pattern_match_t;

typedef struct {
  uint8_t* resolution[7];
  uint8_t* aspect[3];
  uint8_t* fov[2];
  uint8_t* menu_width_guard;
  uint8_t* extra_resolution;
  uint8_t* menu_resolution;
  uint8_t* splash_calls[2];
  uint8_t* fullscreen_bitmap;
} patch_sites_t;

static const char* const k_resolution_patterns[] = {
    "68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 ?? E8 ?? ?? ?? ?? E8",
    "68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 ?? 6A ?? E8 ?? ?? ?? ?? 8B 4C 24",
    "68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? A0",
    "C7 46 ?? ?? ?? ?? ?? 89 46 ?? 89 46 ?? 8A 46 ?? C7 46 ?? ?? ?? ?? ?? 24 ?? 88 5E",
    "C7 46 ?? ?? ?? ?? ?? 89 46 ?? 89 46 ?? 8A 46 ?? C7 46 ?? ?? ?? ?? ?? 24 ?? C7 46",
    "68 ?? ?? ?? ?? 6A ?? E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 6A ?? 6A ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 6A ?? E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 6A ?? 6A ?? 68 ?? ?? ?? ?? 6A ?? 6A ?? E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 6A ?? 6A ?? 68 ?? ?? ?? ?? 6A ?? 6A ?? E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 6A",
    "68 ?? ?? ?? ?? 8B D9 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4",
};

static const char* const k_aspect_patterns[] = {
    "68 ?? ?? ?? ?? 68 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? D9 5C 24",
    "68 ?? ?? ?? ?? 68 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? D8 0D",
    "68 ?? ?? ?? ?? 68 ?? ?? ?? ?? C7 84 24 ?? ?? ?? ?? ?? ?? ?? ?? E8",
};

static const char* const k_fov_patterns[] = {
    "D9 44 24 ?? D8 0D ?? ?? ?? ?? 8B 44 24 ?? 8B 54 24",
    "D9 81 ?? ?? ?? ?? 51 D9 1C 24 E8 ?? ?? ?? ?? 8B 4C 24",
};

static LONG g_init_state = 0;
static int g_output_width = ORIGINAL_WIDTH;
static int g_output_height = ORIGINAL_HEIGHT;
static float g_aspect_scale = 1.0f;
static float g_fov_factor = 1.0f;
static float g_gameplay_original_fov = 0.0f;
static float g_gameplay_modified_fov = 0.0f;
static bool g_has_gameplay_fov = false;
__attribute__((used)) static void* g_overall_multiplier_address = NULL;
__attribute__((used)) static void* g_overall_return_address = NULL;
__attribute__((used)) static void* g_gameplay_return_address = NULL;

static bool parse_pattern(const char* text, uint8_t* bytes, uint8_t* mask,
                          size_t capacity, size_t* length) {
  size_t count = 0;
  while (*text != '\0') {
    while (*text == ' ') ++text;
    if (*text == '\0') break;
    if (count == capacity) return false;

    if (text[0] == '?' && text[1] == '?') {
      bytes[count] = 0;
      mask[count] = 0;
      text += 2;
    } else {
      char token[3] = {text[0], text[1], '\0'};
      char* end = NULL;
      unsigned long value = strtoul(token, &end, 16);
      if (end != token + 2 || value > UINT8_MAX) return false;
      bytes[count] = (uint8_t)value;
      mask[count] = UINT8_MAX;
      text += 2;
    }
    ++count;
  }
  *length = count;
  return count > 0;
}

static pattern_match_t find_unique_pattern(uint8_t* base, size_t size,
                                           const char* pattern) {
  uint8_t bytes[128];
  uint8_t mask[128];
  size_t length = 0;
  pattern_match_t result = {NULL, 0};

  if (!parse_pattern(pattern, bytes, mask, sizeof(bytes), &length) || length > size) {
    return result;
  }

  for (size_t offset = 0; offset <= size - length; ++offset) {
    bool matches = true;
    for (size_t i = 0; i < length; ++i) {
      if (mask[i] != 0 && base[offset + i] != bytes[i]) {
        matches = false;
        break;
      }
    }
    if (!matches) continue;
    if (result.address != NULL) {
      result.address = NULL;
      result.length = 0;
      return result;
    }
    result.address = base + offset;
    result.length = length;
  }
  return result;
}

static bool get_image_bounds(void* image, size_t supplied_size,
                             uint8_t** base, size_t* size) {
  if (image == NULL) return false;

  uint8_t* bytes = (uint8_t*)image;
  IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)bytes;
  if (supplied_size < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
      dos->e_lfanew <= 0 || (size_t)dos->e_lfanew > supplied_size - sizeof(IMAGE_NT_HEADERS32)) {
    return false;
  }

  IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(bytes + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
      nt->OptionalHeader.SizeOfImage == 0 || nt->OptionalHeader.SizeOfImage > supplied_size) {
    return false;
  }

  *base = bytes;
  *size = nt->OptionalHeader.SizeOfImage;
  return true;
}

static uint32_t read_u32(const void* address) {
  uint32_t value;
  memcpy(&value, address, sizeof(value));
  return value;
}

static bool find_patch_sites(uint8_t* base, size_t size, patch_sites_t* sites) {
  memset(sites, 0, sizeof(*sites));

  for (size_t i = 0; i < sizeof(k_resolution_patterns) / sizeof(k_resolution_patterns[0]); ++i) {
    sites->resolution[i] = find_unique_pattern(base, size, k_resolution_patterns[i]).address;
    if (sites->resolution[i] == NULL) return false;
  }
  for (size_t i = 0; i < sizeof(k_aspect_patterns) / sizeof(k_aspect_patterns[0]); ++i) {
    sites->aspect[i] = find_unique_pattern(base, size, k_aspect_patterns[i]).address;
    if (sites->aspect[i] == NULL) return false;
  }
  for (size_t i = 0; i < sizeof(k_fov_patterns) / sizeof(k_fov_patterns[0]); ++i) {
    sites->fov[i] = find_unique_pattern(base, size, k_fov_patterns[i]).address;
    if (sites->fov[i] == NULL) return false;
  }

  if (EXTRA_RESOLUTION_RVA + 24 > size) return false;
  sites->extra_resolution = base + EXTRA_RESOLUTION_RVA;
  if (MENU_RESOLUTION_RVA + 12 > size) return false;
  sites->menu_resolution = base + MENU_RESOLUTION_RVA;
  const uint8_t fullscreen_signature[] = {
      0x83, 0xEC, 0x20, 0x53, 0x56, 0x8B, 0xF1, 0x33, 0xDB, 0x57};
  if (FULLSCREEN_BITMAP_RVA + sizeof(fullscreen_signature) > size ||
      memcmp(base + FULLSCREEN_BITMAP_RVA, fullscreen_signature,
             sizeof(fullscreen_signature)) != 0) return false;
  sites->fullscreen_bitmap = base + FULLSCREEN_BITMAP_RVA;
  const uint8_t splash_signatures[2][5] = {
      {0x8B, 0x01, 0xFF, 0x50, 0x60},
      {0x8B, 0x11, 0xFF, 0x52, 0x60}};
  for (size_t i = 0; i < 2; ++i) {
    if (k_splash_call_rvas[i] + 5 > size ||
        memcmp(base + k_splash_call_rvas[i], splash_signatures[i], 5) != 0) {
      return false;
    }
    sites->splash_calls[i] = base + k_splash_call_rvas[i];
  }

  const size_t width_offsets[] = {6, 6, 6, 3, 3, 1, 8};
  const size_t height_offsets[] = {1, 1, 1, 19, 19, 29, 1};
  for (size_t i = 0; i < 7; ++i) {
    if (read_u32(sites->resolution[i] + width_offsets[i]) != ORIGINAL_WIDTH ||
        read_u32(sites->resolution[i] + height_offsets[i]) != ORIGINAL_HEIGHT) {
      return false;
    }
  }
  if (sites->resolution[0] < base + 14 ||
      sites->resolution[0][-14] != 0x81 ||
      sites->resolution[0][-13] != 0xFE) {
    return false;
  }
  sites->menu_width_guard = sites->resolution[0] - 12;
  if (read_u32(sites->menu_width_guard) != ORIGINAL_WIDTH) return false;
  if (read_u32(sites->extra_resolution + 1) != ORIGINAL_WIDTH ||
      read_u32(sites->extra_resolution + 20) != ORIGINAL_HEIGHT) {
    return false;
  }
  if (read_u32(sites->menu_resolution + 1) != ORIGINAL_HEIGHT ||
      read_u32(sites->menu_resolution + 8) != ORIGINAL_WIDTH) {
    return false;
  }
  if (sites->fov[0][3] != 4 || read_u32(sites->fov[1] + 2) != 0xB0) {
    return false;
  }
  return true;
}

static bool write_memory(void* address, const void* data, size_t size) {
  DWORD old_protect;
  if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
  memcpy(address, data, size);
  FlushInstructionCache(GetCurrentProcess(), address, size);
  DWORD ignored;
  return VirtualProtect(address, size, old_protect, &ignored) != FALSE;
}

static bool write_u32(void* address, uint32_t value) {
  return write_memory(address, &value, sizeof(value));
}

static bool write_float(void* address, float value) {
  return write_memory(address, &value, sizeof(value));
}

static bool install_jump(uint8_t* address, size_t replaced_length, const void* target) {
  if (replaced_length < 5) return false;
  uint8_t patch[16];
  if (replaced_length > sizeof(patch)) return false;
  memset(patch, 0x90, replaced_length);
  patch[0] = 0xE9;
  int32_t displacement = (int32_t)((const uint8_t*)target - (address + 5));
  memcpy(patch + 1, &displacement, sizeof(displacement));
  return write_memory(address, patch, replaced_length);
}

__attribute__((used, noinline)) static float adjust_overall_fov(float current) {
  const float pi = 3.14159265358979323846f;
  float half_radians = current * (0.5f * pi / 180.0f);
  return 2.0f * atanf(tanf(half_radians) * g_aspect_scale) * (180.0f / pi);
}

__attribute__((used, noinline)) static float adjust_gameplay_fov(float current) {
  if (!g_has_gameplay_fov || fabsf(current - g_gameplay_modified_fov) > 0.001f) {
    g_gameplay_original_fov = current;
    g_has_gameplay_fov = true;
  }
  g_gameplay_modified_fov = g_gameplay_original_fov * g_fov_factor;
  return g_gameplay_modified_fov;
}

__attribute__((naked, used)) static void overall_fov_hook(void) {
  __asm__ __volatile__(
      "pushfl\n\t"
      "pushal\n\t"
      "pushl 40(%%esp)\n\t"
      "call _adjust_overall_fov\n\t"
      "addl $4, %%esp\n\t"
      "movl _g_overall_multiplier_address, %%eax\n\t"
      "fmuls (%%eax)\n\t"
      "popal\n\t"
      "popfl\n\t"
      "jmp *_g_overall_return_address\n\t"
      ::: "eax");
}

__attribute__((naked, used)) static void gameplay_fov_hook(void) {
  __asm__ __volatile__(
      "pushfl\n\t"
      "pushal\n\t"
      "movl 24(%%esp), %%eax\n\t"
      "pushl 176(%%eax)\n\t"
      "call _adjust_gameplay_fov\n\t"
      "addl $4, %%esp\n\t"
      "popal\n\t"
      "popfl\n\t"
      "jmp *_g_gameplay_return_address\n\t"
      ::: "eax");
}

static void config_path(char path[MAX_PATH]) {
  DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    strcpy_s(path, MAX_PATH, CONFIG_NAME);
    return;
  }
  char* separator = strrchr(path, '\\');
  if (separator != NULL) *(separator + 1) = '\0';
  strcat_s(path, MAX_PATH, CONFIG_NAME);
}

static float read_profile_float(const char* path, const char* section,
                                const char* key, float fallback) {
  char text[64];
  GetPrivateProfileStringA(section, key, "", text, sizeof(text), path);
  if (text[0] == '\0') return fallback;
  char* end = NULL;
  float value = strtof(text, &end);
  return end == text || !isfinite(value) ? fallback : value;
}

static bool read_profile_bool(const char* path, const char* section,
                              const char* key, bool fallback) {
  char text[16];
  GetPrivateProfileStringA(section, key, fallback ? "true" : "false",
                           text, sizeof(text), path);
  return _stricmp(text, "true") == 0 || strcmp(text, "1") == 0 ||
         _stricmp(text, "yes") == 0 || _stricmp(text, "on") == 0;
}

static bool load_settings(int* width, int* height) {
  char path[MAX_PATH];
  config_path(path);
  if (!read_profile_bool(path, "Fix", "Enabled", true)) {
    shim_log("Widescreen: disabled in %s", CONFIG_NAME);
    return false;
  }

  *width = GetPrivateProfileIntA("Settings", "Width", 0, path);
  *height = GetPrivateProfileIntA("Settings", "Height", 0, path);
  g_fov_factor = read_profile_float(path, "Settings", "FOVFactor", 1.0f);
  if (g_fov_factor < 0.1f || g_fov_factor > 5.0f) g_fov_factor = 1.0f;

  if (*width <= 0 || *height <= 0) {
    DEVMODEA mode = {.dmSize = sizeof(mode)};
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode)) {
      *width = (int)mode.dmPelsWidth;
      *height = (int)mode.dmPelsHeight;
    } else {
      *width = GetSystemMetrics(SM_CXSCREEN);
      *height = GetSystemMetrics(SM_CYSCREEN);
    }
  }

  return *width >= ORIGINAL_WIDTH && *height >= ORIGINAL_HEIGHT &&
         *width <= 16384 && *height <= 16384;
}

static bool apply_resolution_and_aspect(const patch_sites_t* sites,
                                        int width, int height) {
  const size_t width_offsets[] = {6, 6, 6, 3, 3, 1, 8};
  const size_t height_offsets[] = {1, 1, 1, 19, 19, 29, 1};
  for (size_t i = 0; i < 7; ++i) {
    if (!write_u32(sites->resolution[i] + width_offsets[i], (uint32_t)width) ||
        !write_u32(sites->resolution[i] + height_offsets[i], (uint32_t)height)) {
      return false;
    }
  }
  if (!write_u32(sites->menu_width_guard, (uint32_t)width)) return false;
  if (!write_u32(sites->extra_resolution + 1, (uint32_t)width) ||
      !write_u32(sites->extra_resolution + 20, (uint32_t)height)) {
    return false;
  }

  /* The post-intro splash uses DirectDraw BltFast, outside the menu renderer.
     Reuse the engine's full-screen Blt for its initial frame and redraw loop. */
  for (size_t i = 0; i < 2; ++i) {
    uint8_t call[5] = {0xE8};
    int32_t relative = (int32_t)(sites->fullscreen_bitmap -
                                 (sites->splash_calls[i] + sizeof(call)));
    memcpy(call + 1, &relative, sizeof(relative));
    if (!write_memory(sites->splash_calls[i], call, sizeof(call))) return false;
  }

  float aspect = (float)width / (float)height;
  g_aspect_scale = aspect / ORIGINAL_ASPECT;
  return write_float(sites->aspect[0] + 1, 0.9710000157f * g_aspect_scale) &&
         write_float(sites->aspect[1] + 1, aspect) &&
         write_float(sites->aspect[2] + 1, aspect);
}

static bool apply_fov_hooks(const patch_sites_t* sites) {
  g_overall_multiplier_address = (void*)(uintptr_t)read_u32(sites->fov[0] + 6);
  g_overall_return_address = sites->fov[0] + 10;
  g_gameplay_return_address = sites->fov[1] + 6;

  return install_jump(sites->fov[0], 10, overall_fov_hook) &&
         install_jump(sites->fov[1], 6, gameplay_fov_hook);
}

bool widescreen_fix_init(void) {
  LONG state = InterlockedCompareExchange(&g_init_state, 1, 0);
  if (state != 0) return state == 2;

  int width;
  int height;
  if (!load_settings(&width, &height)) {
    InterlockedExchange(&g_init_state, 3);
    return false;
  }

  uint8_t* base;
  size_t size;
  HMODULE image = GetModuleHandleA(NULL);
  IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
  size_t mapped_size = 0;
  if (image != NULL && dos->e_magic == IMAGE_DOS_SIGNATURE) {
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((uint8_t*)image + dos->e_lfanew);
    if (nt->Signature == IMAGE_NT_SIGNATURE) mapped_size = nt->OptionalHeader.SizeOfImage;
  }

  patch_sites_t sites;
  bool success = get_image_bounds(image, mapped_size, &base, &size) &&
                 find_patch_sites(base, size, &sites) &&
                 apply_resolution_and_aspect(&sites, width, height) &&
                 apply_fov_hooks(&sites);

  if (success) {
    g_output_width = width;
    g_output_height = height;
    shim_log("Widescreen: native %dx%d enabled (aspect %.4f, FOV factor %.3f)",
             width, height, (double)width / (double)height, (double)g_fov_factor);
    InterlockedExchange(&g_init_state, 2);
  } else {
    shim_log("Widescreen: executable signatures did not match; patch skipped");
    InterlockedExchange(&g_init_state, 3);
  }
  return success;
}

bool widescreen_fix_get_resolution(int* width, int* height) {
  if (width == NULL || height == NULL ||
      InterlockedCompareExchange(&g_init_state, 0, 0) != 2) {
    return false;
  }
  *width = g_output_width;
  *height = g_output_height;
  return true;
}

#ifdef WIDESCREEN_TEST
bool widescreen_fix_verify_image(const void* image, size_t image_size) {
  uint8_t* base;
  size_t size;
  patch_sites_t sites;
  return get_image_bounds((void*)image, image_size, &base, &size) &&
         find_patch_sites(base, size, &sites);
}

bool widescreen_fix_test_enabled(const char* path) {
  return read_profile_bool(path, "Fix", "Enabled", false);
}

bool widescreen_fix_test_apply_resolution(void* image, size_t image_size,
                                          int width, int height) {
  uint8_t* base;
  size_t size;
  patch_sites_t sites;
  return get_image_bounds(image, image_size, &base, &size) &&
         find_patch_sites(base, size, &sites) &&
         apply_resolution_and_aspect(&sites, width, height);
}
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "widescreen_fix.h"

FILE* g_log_file = NULL;
bool g_log_suspended = false;

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s SecretAgent.exe\n", argv[0]);
    return 2;
  }

  const char* ini_path = "build/tests/widescreen_probe.ini";
  FILE* ini = fopen(ini_path, "wb");
  if (ini == NULL) return 6;
  fputs("[Fix]\nEnabled=true\n", ini);
  fclose(ini);
  bool enabled = widescreen_fix_test_enabled(ini_path);
  DeleteFileA(ini_path);
  if (!enabled) {
    fprintf(stderr, "widescreen Enabled=true parsing failed\n");
    return 7;
  }

  HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return 3;
  HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL);
  if (mapping == NULL) {
    CloseHandle(file);
    return 4;
  }
  void* image = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (image == NULL) {
    CloseHandle(mapping);
    CloseHandle(file);
    return 5;
  }

  IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
  IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((unsigned char*)image + dos->e_lfanew);
  bool valid = widescreen_fix_verify_image(image, nt->OptionalHeader.SizeOfImage);

  unsigned char* writable = VirtualAlloc(NULL, nt->OptionalHeader.SizeOfImage,
                                         MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
  if (writable == NULL) valid = false;
  if (writable != NULL) {
    memcpy(writable, image, nt->OptionalHeader.SizeOfImage);
    const size_t splash_signatures[] = {0x000F490C, 0x000F4930, 0x00061F30};
    for (size_t i = 0; i < 3; ++i) {
      writable[splash_signatures[i]] ^= 1;
      if (widescreen_fix_verify_image(writable, nt->OptionalHeader.SizeOfImage)) {
        fprintf(stderr, "unexpected splash signature was accepted\n");
        valid = false;
      }
      writable[splash_signatures[i]] ^= 1;
    }
    valid = valid && widescreen_fix_test_apply_resolution(
                         writable, nt->OptionalHeader.SizeOfImage, 2560, 1440);
    /* Both the first splash frame and its three-second redraw loop must use
       the engine's full-screen bitmap blit, not its native-size draw. */
    const size_t splash_calls[] = {0x000F490C, 0x000F4930};
    for (size_t i = 0; i < 2; ++i) {
      int32_t relative;
      memcpy(&relative, writable + splash_calls[i] + 1, sizeof(relative));
      if (writable[splash_calls[i]] != 0xE8 ||
          (int32_t)(splash_calls[i] + 5) + relative != 0x00061F30) {
        fprintf(stderr, "splash frame %u still uses native-size drawing\n",
                (unsigned)i);
        valid = false;
      }
    }
    uint32_t output_width;
    uint32_t output_height;
    uint32_t ui_width;
    uint32_t ui_height;
    uint32_t menu_width_guard;
    memcpy(&output_width, writable + 0x000E0E99, sizeof(output_width));
    memcpy(&output_height, writable + 0x000E0EAC, sizeof(output_height));
    memcpy(&ui_height, writable + 0x000F47BE, sizeof(ui_height));
    memcpy(&ui_width, writable + 0x000F47C5, sizeof(ui_width));
    memcpy(&menu_width_guard, writable + 0x0002677E,
           sizeof(menu_width_guard));
    if (output_width != 2560 || output_height != 1440 ||
        ui_width != 2560 || ui_height != 1440 ||
        menu_width_guard != 2560) {
      fprintf(stderr,
              "resolution patch failed: output=%lux%lu ui=%lux%lu "
              "menu guard=%lu\n",
              (unsigned long)output_width, (unsigned long)output_height,
              (unsigned long)ui_width, (unsigned long)ui_height,
              (unsigned long)menu_width_guard);
      valid = false;
    }
    VirtualFree(writable, 0, MEM_RELEASE);
  }

  UnmapViewOfFile(image);
  CloseHandle(mapping);
  CloseHandle(file);
  if (!valid) {
    fprintf(stderr, "SecretAgent.exe widescreen signatures did not match\n");
    return 1;
  }
  puts("SecretAgent.exe widescreen signatures verified");
  return 0;
}

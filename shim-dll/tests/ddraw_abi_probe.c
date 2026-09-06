#include <stdio.h>
#include <windows.h>

typedef BOOL (WINAPI* enum_callback_a)(GUID*, LPSTR, LPSTR, LPVOID);
typedef HRESULT (WINAPI* direct_draw_enumerate_a)(enum_callback_a, LPVOID);

static BOOL WINAPI count_device(GUID* guid, LPSTR description, LPSTR name,
                                LPVOID context) {
  (void)guid;
  (void)description;
  (void)name;
  ++*(int*)context;
  return TRUE;
}

int main(int argc, char** argv) {
  const char* shim_path = argc > 1 ? argv[1] : "ddraw.dll";
  HMODULE shim = LoadLibraryA(shim_path);
  if (shim == NULL) {
    fprintf(stderr, "LoadLibraryA(%s) failed: %lu\n", shim_path, GetLastError());
    return 1;
  }

  direct_draw_enumerate_a enumerate =
      (direct_draw_enumerate_a)GetProcAddress(shim, "DirectDrawEnumerateA");
  if (enumerate == NULL) {
    fprintf(stderr, "DirectDrawEnumerateA export missing\n");
    FreeLibrary(shim);
    return 1;
  }

  int device_count = 0;
  HRESULT result = enumerate(count_device, &device_count);
  if (FAILED(result)) {
    fprintf(stderr, "DirectDrawEnumerateA failed: 0x%08lx\n", (unsigned long)result);
    FreeLibrary(shim);
    return 1;
  }

  printf("ddraw ABI probe passed (%d device callbacks)\n", device_count);
  FreeLibrary(shim);
  return 0;
}

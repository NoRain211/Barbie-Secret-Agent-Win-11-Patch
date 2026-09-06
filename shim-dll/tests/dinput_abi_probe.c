#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef struct {
  DWORD Data1;
  WORD Data2;
  WORD Data3;
  BYTE Data4[8];
} probe_guid;

typedef HRESULT (WINAPI* direct_input_create_a)(HINSTANCE, DWORD, void**, void*);
typedef HRESULT (STDMETHODCALLTYPE* query_interface)(void*, const probe_guid*, void**);
typedef ULONG (STDMETHODCALLTYPE* release_interface)(void*);
typedef BOOL (CALLBACK* enum_devices_callback)(const void*, void*);
typedef HRESULT (STDMETHODCALLTYPE* enum_devices)(void*, DWORD, enum_devices_callback,
                                                  void*, DWORD);

static const probe_guid iid_direct_input_a = {
    0x89521360, 0xAA8A, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const probe_guid iid_direct_input_7a = {
    0x9A4CB684, 0x236D, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};

static BOOL CALLBACK count_device(const void* device, void* context) {
  (void)device;
  ++*(int*)context;
  return TRUE;
}

int main(int argc, char** argv) {
  const char* shim_path = argc > 1 ? argv[1] : "dinput.dll";
  HMODULE shim = LoadLibraryA(shim_path);
  if (shim == NULL) {
    fprintf(stderr, "LoadLibraryA(%s) failed: %lu\n", shim_path, GetLastError());
    return 1;
  }

  direct_input_create_a create =
      (direct_input_create_a)GetProcAddress(shim, "DirectInputCreateA");
  if (create == NULL) {
    fprintf(stderr, "DirectInputCreateA export missing\n");
    FreeLibrary(shim);
    return 1;
  }

  void* direct_input = NULL;
  HRESULT result = create(GetModuleHandleA(NULL), 0x0700, &direct_input, NULL);
  if (FAILED(result) || direct_input == NULL) {
    fprintf(stderr, "DirectInputCreateA failed: 0x%08lx\n", (unsigned long)result);
    FreeLibrary(shim);
    return 1;
  }

  void** vtable = *(void***)direct_input;
  query_interface query = (query_interface)vtable[0];
  release_interface release = (release_interface)vtable[2];
  enum_devices enumerate = (enum_devices)vtable[4];

  void* queried = NULL;
  result = query(direct_input, &iid_direct_input_a, &queried);
  if (FAILED(result) || queried != direct_input) {
    fprintf(stderr, "IDirectInputA QueryInterface failed: 0x%08lx result=%p\n",
            (unsigned long)result, queried);
    release(direct_input);
    FreeLibrary(shim);
    return 1;
  }
  release(queried);

  queried = NULL;
  result = query(direct_input, &iid_direct_input_7a, &queried);
  if (FAILED(result) || queried != direct_input) {
    fprintf(stderr, "IDirectInput7A upgrade lost wrapper identity: 0x%08lx result=%p\n",
            (unsigned long)result, queried);
    release(direct_input);
    FreeLibrary(shim);
    return 1;
  }
  release(queried);

  int device_count = 0;
  result = enumerate(direct_input, 4, count_device, &device_count, 1);
  if (FAILED(result)) {
    fprintf(stderr, "EnumDevices failed: 0x%08lx\n", (unsigned long)result);
    release(direct_input);
    FreeLibrary(shim);
    return 1;
  }

  printf("dinput ABI probe passed (%d joystick callbacks)\n", device_count);
  release(direct_input);
  FreeLibrary(shim);
  return 0;
}

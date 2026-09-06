#define COBJMACROS
#include <d3d.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "menu_fix.h"
#include "shim_log.h"

#define ORIGINAL_WIDTH 640
#define ORIGINAL_HEIGHT 480
#define MENU_UPDATE_RVA 0x000C7430
#define MENU_RENDER_RVA 0x000C7740
#define CURSOR_GETTER_RVA 0x000D2440
#define CURSOR_RENDER_RVA 0x000D26D0
#define OUTFIT_CURSOR_RETURN_RVA 0x0000D366
#define CURSOR_MENU_BOUNDS_RVA 0x000D25B0
#define CURSOR_BOUNDS_RVA 0x005ABC7C
#define D3D_DEVICE_RVA 0x0017F09C
#define DEVICE_READY_CALL_RVA 0x00086081
#define DEVICE_READY_TARGET_RVA 0x00086830
#define CURSOR_OBJECT_RVA 0x005ABC6C
#define CAMERA_ARRAY_RVA 0x0017CEA0
#define CAMERA_CONFIG_RVA 0x0003E180
#define VIEWER_UPDATE_RVA 0x00005470
#define MENU_UPDATE_SLOT 23
#define MENU_RENDER_SLOT 24
#define DRAW_PRIMITIVE_SLOT 25
#define DRAW_INDEXED_PRIMITIVE_SLOT 26
#define CURSOR_DRAW_RETURN_RVA 0x0011B0C9
#define LOAD_MENU_INDEX 3
#define MAIN_MENU_INDEX 4
#define NEW_MENU_INDEX 5
#define VIEWER_MENU_INDEX 7
#define VIEWER_MODEL_ZOOM 3.0f
#define TOOL_OVERLAY_INDEX -2
#define OUTFIT_CAMERA_DISTANCE_RVA 0x0000C44C
#define COORDINATE_POLICY_NONE 0
#define COORDINATE_POLICY_LOGICAL 1
#define COORDINATE_POLICY_CENTER 2
#define COORDINATE_POLICY_LEFT_BOTTOM 3
#define COORDINATE_POLICY_RIGHT_TOP 4
#define COORDINATE_POLICY_AUTO 5
#define GUI_CONTAINER_VTABLE_RVA 0x0014BE8C

typedef void (__attribute__((thiscall)) *menu_render_fn)(void* self);
typedef void (__attribute__((thiscall)) *menu_update_fn)(void* self);
typedef void (__attribute__((thiscall)) *device_ready_fn)(void* self);
typedef void (__attribute__((thiscall)) *camera_config_fn)(
    void* self, float fov, float aspect, float near_plane, float far_plane);
typedef HRESULT (WINAPI *draw_primitive_fn)(
    IDirect3DDevice7*, D3DPRIMITIVETYPE, DWORD, void*, DWORD, DWORD);
typedef HRESULT (WINAPI *draw_indexed_primitive_fn)(
    IDirect3DDevice7*, D3DPRIMITIVETYPE, DWORD, void*, DWORD, WORD*, DWORD,
    DWORD);

typedef struct menu_slot {
  uintptr_t vtable_rva;
  uintptr_t expected_render_rva;
  const char* name;
  LONG coordinate_policy;
} menu_slot_t;

static const menu_slot_t k_menu_slots[] = {
    {0x00145294, 0x000C7740, "saLoadScreen", COORDINATE_POLICY_LOGICAL},
    {0x00145324, 0x000C7740, "saMenu", COORDINATE_POLICY_LOGICAL},
    {0x001453B4, 0x000030D0, "saMenuCredits", COORDINATE_POLICY_LOGICAL},
    {0x0014543C, 0x000030D0, "saMenuLoad", COORDINATE_POLICY_CENTER},
    {0x001454DC, 0x000030D0, "saMenuMain", COORDINATE_POLICY_LOGICAL},
    {0x00145584, 0x000030D0, "saMenuNew", COORDINATE_POLICY_CENTER},
    {0x0014560C, 0x000030D0, "saMenuOptions", COORDINATE_POLICY_CENTER},
    {0x001456A4, 0x00005650, "saMenuViewer", COORDINATE_POLICY_LOGICAL},
    {0x00145BB4, 0x0000D310, "saHudOutfit", COORDINATE_POLICY_CENTER},
    {0x00145B24, 0x000030D0, "saHudInventory", COORDINATE_POLICY_LEFT_BOTTOM},
    {0x00145C3C, 0x000C7740, "saHudPause", COORDINATE_POLICY_CENTER},
    {0x00145CC4, 0x000030D0, "saHudPDA", COORDINATE_POLICY_CENTER},
    {0x00145DCC, 0x0000E990, "saHudRadar", COORDINATE_POLICY_RIGHT_TOP},
};
static const uint8_t k_menu_render_signature[] = {
    0x56, 0x57, 0x8B, 0xF1, 0x33, 0xFF,
};
static const uint8_t k_menu_update_signature[] = {
    0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t k_cursor_getter_signature[] = {
    0x8B, 0x0D, 0x6C, 0xBC, 0x9A, 0x00,
};
static const uint8_t k_cursor_bounds_signature[] = {
    0x83, 0xEC, 0x10, 0x8D, 0x44, 0x24, 0x04,
};

typedef struct cursor_point {
  float x;
  float y;
  float z;
} cursor_point_t;

typedef struct colored_tl_vertex {
  float x;
  float y;
  float z;
  float rhw;
  DWORD color;
} colored_tl_vertex_t;

static volatile LONG g_menu_render_depth = 0;
static volatile LONG g_menu_update_depth = 0;
static volatile LONG g_render_policy = COORDINATE_POLICY_NONE;
static volatile LONG g_update_policy = COORDINATE_POLICY_NONE;
static bool g_drawing_cursor = false;
static LONG g_init_state = 0;
static LONG g_device_hook_logged = 0;
static LONG g_logged_menu_mask = 0;
static LONG g_logged_dynamic_menu = 0;
static LONG g_logged_native_cursor_mask = 0;
static LONG g_cursor_draw_logs = 0;
#ifdef MENU_FIX_DIAGNOSTICS
static LONG g_cursor_state_logged = 0;
#endif
static LONG g_cursor_texture_synced = 0;
static LONG g_seen_known_menu = 0;
static LONG g_viewer_camera_logged = 0;
static LONG g_clear_logged_mask = 0;
static LONG g_main_menu_texture_dump_mask = 0;
static LONG g_main_menu_override_attempt_mask = 0;
static LONG g_main_menu_override_loaded_mask = 0;
static volatile LONG g_active_menu_index = -1;
static volatile LONG g_update_menu_index = -1;
static unsigned g_active_primitive_index = 0;
static LONG g_trace_menu = -1;
static unsigned g_trace_primitives = 0;
static DWORD g_trace_fvf = 0;
static DWORD g_trace_vertices = 0;
static cursor_point_t g_logical_cursor_position;
static float g_viewer_base_fov = 0.0f;
static float g_scale_x = 1.0f;
static float g_scale_y = 1.0f;
static int g_output_width = ORIGINAL_WIDTH;
static int g_output_height = ORIGINAL_HEIGHT;
static uint8_t* g_image_base = NULL;
static menu_update_fn g_original_menu_update = NULL;
static menu_render_fn g_original_menu_render = NULL;
static menu_update_fn g_original_viewer_update = NULL;
static menu_render_fn g_original_viewer_render = NULL;
static device_ready_fn g_original_device_ready = NULL;
static draw_primitive_fn g_original_draw_primitive = NULL;
static draw_indexed_primitive_fn g_original_draw_indexed_primitive = NULL;
__attribute__((used)) static const int g_original_ui_width = ORIGINAL_WIDTH;
__attribute__((used)) static void* g_outfit_distance_return = NULL;

static uint8_t expand_masked_channel(uint32_t pixel, uint32_t mask) {
  if (mask == 0) return 0;

  unsigned shift = 0;
  while (((mask >> shift) & 1u) == 0u) ++shift;
  uint32_t value_mask = mask >> shift;
  uint32_t value = (pixel & mask) >> shift;
  return (uint8_t)((value * 255u + value_mask / 2u) / value_mask);
}

static uint32_t pack_masked_channel(uint8_t channel, uint32_t mask) {
  if (mask == 0) return 0;

  unsigned shift = 0;
  while (((mask >> shift) & 1u) == 0u) ++shift;
  uint32_t value_mask = mask >> shift;
  uint32_t value = ((uint32_t)channel * value_mask + 127u) / 255u;
  return (value << shift) & mask;
}

static bool dump_rgb_texture(LPDIRECTDRAWSURFACE7 texture,
                             unsigned tile_index) {
  if (texture == NULL || tile_index >= 6) return false;

  DDSURFACEDESC2 desc;
  memset(&desc, 0, sizeof(desc));
  desc.dwSize = sizeof(desc);
  if (FAILED(IDirectDrawSurface7_GetSurfaceDesc(texture, &desc)) ||
      desc.dwWidth == 0 || desc.dwHeight == 0 ||
      (desc.ddpfPixelFormat.dwFlags & DDPF_RGB) == 0 ||
      desc.ddpfPixelFormat.dwRGBBitCount == 0 ||
      desc.ddpfPixelFormat.dwRGBBitCount > 32) {
    return false;
  }

  DDSURFACEDESC2 locked;
  memset(&locked, 0, sizeof(locked));
  locked.dwSize = sizeof(locked);
  HRESULT lock_result = IDirectDrawSurface7_Lock(
      texture, NULL, &locked, DDLOCK_READONLY | DDLOCK_WAIT, NULL);
  if (FAILED(lock_result) || locked.lpSurface == NULL) return false;

  DWORD row_bytes = desc.dwWidth * 4;
  if (desc.dwHeight > SIZE_MAX / row_bytes) {
    IDirectDrawSurface7_Unlock(texture, NULL);
    return false;
  }
  size_t pixel_bytes = (size_t)row_bytes * desc.dwHeight;
  uint8_t* output = (uint8_t*)malloc(pixel_bytes);
  if (output == NULL) {
    IDirectDrawSurface7_Unlock(texture, NULL);
    return false;
  }

  const DDPIXELFORMAT* format = &desc.ddpfPixelFormat;
  size_t source_bytes = (format->dwRGBBitCount + 7) / 8;
  size_t source_pitch =
      locked.lPitch < 0 ? (size_t)-locked.lPitch : (size_t)locked.lPitch;
  for (DWORD y = 0; y < desc.dwHeight; ++y) {
    const uint8_t* source =
        (const uint8_t*)locked.lpSurface + (size_t)y * source_pitch;
    uint8_t* destination = output + (size_t)y * row_bytes;
    for (DWORD x = 0; x < desc.dwWidth; ++x) {
      uint32_t pixel = 0;
      memcpy(&pixel, source + (size_t)x * source_bytes, source_bytes);
      destination[x * 4 + 0] =
          expand_masked_channel(pixel, format->dwBBitMask);
      destination[x * 4 + 1] =
          expand_masked_channel(pixel, format->dwGBitMask);
      destination[x * 4 + 2] =
          expand_masked_channel(pixel, format->dwRBitMask);
      destination[x * 4 + 3] = 0xff;
    }
  }
  IDirectDrawSurface7_Unlock(texture, NULL);

  CreateDirectoryA("HD_UI", NULL);
  CreateDirectoryA("HD_UI\\Extracted", NULL);
  char path[MAX_PATH];
  wsprintfA(path, "HD_UI\\Extracted\\main_menu_tile_%u.bmp", tile_index);

  BITMAPFILEHEADER file_header;
  BITMAPINFOHEADER info_header;
  memset(&file_header, 0, sizeof(file_header));
  memset(&info_header, 0, sizeof(info_header));
  file_header.bfType = 0x4d42;
  file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
  file_header.bfSize = file_header.bfOffBits + (DWORD)pixel_bytes;
  info_header.biSize = sizeof(info_header);
  info_header.biWidth = (LONG)desc.dwWidth;
  info_header.biHeight = -(LONG)desc.dwHeight;
  info_header.biPlanes = 1;
  info_header.biBitCount = 32;
  info_header.biCompression = BI_RGB;
  info_header.biSizeImage = (DWORD)pixel_bytes;

  bool ok = false;
  HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ok = WriteFile(file, &file_header, sizeof(file_header), &written, NULL) &&
         written == sizeof(file_header) &&
         WriteFile(file, &info_header, sizeof(info_header), &written, NULL) &&
         written == sizeof(info_header) &&
         WriteFile(file, output, (DWORD)pixel_bytes, &written, NULL) &&
         written == pixel_bytes;
    CloseHandle(file);
  }
  free(output);

  shim_log("HD UI: extracted main-menu tile %u (%lux%lu, %lu bpp) to %s",
           tile_index, (unsigned long)desc.dwWidth,
           (unsigned long)desc.dwHeight,
           (unsigned long)format->dwRGBBitCount, ok ? path : "write failed");
  return ok;
}

static bool load_bmp_into_texture(LPDIRECTDRAWSURFACE7 texture,
                                  const char* path) {
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return false;

  BITMAPFILEHEADER file_header;
  BITMAPINFOHEADER info_header;
  DWORD read = 0;
  bool header_ok =
      ReadFile(file, &file_header, sizeof(file_header), &read, NULL) &&
      read == sizeof(file_header) &&
      ReadFile(file, &info_header, sizeof(info_header), &read, NULL) &&
      read == sizeof(info_header) && file_header.bfType == 0x4d42 &&
      info_header.biSize >= sizeof(info_header) && info_header.biWidth == 256 &&
      (info_header.biHeight == 256 || info_header.biHeight == -256) &&
      info_header.biPlanes == 1 &&
      (info_header.biBitCount == 24 || info_header.biBitCount == 32) &&
      info_header.biCompression == BI_RGB;
  if (!header_ok) {
    CloseHandle(file);
    return false;
  }

  DWORD source_row_bytes =
      ((DWORD)info_header.biWidth * info_header.biBitCount + 31u) / 32u * 4u;
  DWORD source_bytes = source_row_bytes * 256u;
  uint8_t* source = (uint8_t*)malloc(source_bytes);
  if (source == NULL) {
    CloseHandle(file);
    return false;
  }
  SetFilePointer(file, (LONG)file_header.bfOffBits, NULL, FILE_BEGIN);
  bool read_ok = ReadFile(file, source, source_bytes, &read, NULL) &&
                 read == source_bytes;
  CloseHandle(file);
  if (!read_ok) {
    free(source);
    return false;
  }

  DDSURFACEDESC2 desc;
  memset(&desc, 0, sizeof(desc));
  desc.dwSize = sizeof(desc);
  if (FAILED(IDirectDrawSurface7_GetSurfaceDesc(texture, &desc)) ||
      desc.dwWidth != 256 || desc.dwHeight != 256 ||
      (desc.ddpfPixelFormat.dwFlags & DDPF_RGB) == 0 ||
      desc.ddpfPixelFormat.dwRGBBitCount == 0 ||
      desc.ddpfPixelFormat.dwRGBBitCount > 32) {
    free(source);
    return false;
  }

  DDSURFACEDESC2 locked;
  memset(&locked, 0, sizeof(locked));
  locked.dwSize = sizeof(locked);
  HRESULT lock_result =
      IDirectDrawSurface7_Lock(texture, NULL, &locked, DDLOCK_WAIT, NULL);
  if (FAILED(lock_result) || locked.lpSurface == NULL || locked.lPitch <= 0) {
    free(source);
    return false;
  }

  const DDPIXELFORMAT* format = &desc.ddpfPixelFormat;
  size_t destination_bytes = (format->dwRGBBitCount + 7) / 8;
  size_t source_pixel_bytes = info_header.biBitCount / 8;
  for (DWORD y = 0; y < 256; ++y) {
    DWORD source_y = info_header.biHeight > 0 ? 255u - y : y;
    const uint8_t* source_row = source + (size_t)source_y * source_row_bytes;
    uint8_t* destination_row =
        (uint8_t*)locked.lpSurface + (size_t)y * locked.lPitch;
    for (DWORD x = 0; x < 256; ++x) {
      const uint8_t* source_pixel = source_row + (size_t)x * source_pixel_bytes;
      uint32_t pixel =
          pack_masked_channel(source_pixel[2], format->dwRBitMask) |
          pack_masked_channel(source_pixel[1], format->dwGBitMask) |
          pack_masked_channel(source_pixel[0], format->dwBBitMask) |
          pack_masked_channel(source_pixel_bytes == 4 ? source_pixel[3] : 255,
                              format->dwRGBAlphaBitMask);
      memcpy(destination_row + (size_t)x * destination_bytes, &pixel,
             destination_bytes);
    }
  }
  HRESULT unlock_result = IDirectDrawSurface7_Unlock(texture, NULL);
  free(source);
  return SUCCEEDED(unlock_result);
}

static void apply_main_menu_texture_override(IDirect3DDevice7* device,
                                             unsigned primitive_index) {
  if (primitive_index >= 6) return;
  LONG bit = 1L << primitive_index;
  if ((InterlockedOr(&g_main_menu_override_attempt_mask, bit) & bit) != 0) {
    return;
  }

  LPDIRECTDRAWSURFACE7 texture = NULL;
  HRESULT texture_result = IDirect3DDevice7_GetTexture(device, 0, &texture);
  char path[MAX_PATH];
  wsprintfA(path, "HD_UI\\Override\\main_menu_tile_%u.bmp",
            primitive_index);
  bool loaded = SUCCEEDED(texture_result) && texture != NULL &&
                load_bmp_into_texture(texture, path);
  if (texture != NULL) IDirectDrawSurface7_Release(texture);
  if (loaded) InterlockedOr(&g_main_menu_override_loaded_mask, bit);
  shim_log("HD UI: main-menu override tile %u %s (%s)", primitive_index,
           loaded ? "loaded" : "not loaded", path);
}

static void dump_main_menu_texture(IDirect3DDevice7* device,
                                   unsigned primitive_index) {
  if (primitive_index >= 6) return;
  LONG bit = 1L << primitive_index;
  if ((InterlockedOr(&g_main_menu_texture_dump_mask, bit) & bit) != 0) return;

  LPDIRECTDRAWSURFACE7 texture = NULL;
  if (FAILED(IDirect3DDevice7_GetTexture(device, 0, &texture)) ||
      texture == NULL) {
    InterlockedAnd(&g_main_menu_texture_dump_mask, ~bit);
    return;
  }
  if (!dump_rgb_texture(texture, primitive_index)) {
    InterlockedAnd(&g_main_menu_texture_dump_mask, ~bit);
  }
  IDirectDrawSurface7_Release(texture);
}

static bool write_memory(void* destination, const void* source, size_t size) {
  DWORD old_protect;
  if (!VirtualProtect(destination, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
    return false;
  }
  memcpy(destination, source, size);
  FlushInstructionCache(GetCurrentProcess(), destination, size);
  DWORD ignored;
  VirtualProtect(destination, size, old_protect, &ignored);
  return true;
}

static size_t menu_index_for_vtable(uintptr_t vtable_rva) {
  for (size_t i = 0; i < sizeof(k_menu_slots) / sizeof(k_menu_slots[0]); ++i) {
    if (k_menu_slots[i].vtable_rva == vtable_rva) return i;
  }
  return SIZE_MAX;
}

static size_t transformed_vertex_stride(DWORD fvf) {
  if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) return 0;

  size_t stride = 4 * sizeof(float);
  if (fvf & D3DFVF_NORMAL) stride += 3 * sizeof(float);
  if (fvf & D3DFVF_DIFFUSE) stride += sizeof(DWORD);
  if (fvf & D3DFVF_SPECULAR) stride += sizeof(DWORD);
  stride += ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) *
            2 * sizeof(float);
  return stride;
}

static bool transformed_vertex_bounds(DWORD fvf, const void* vertices,
                                      DWORD vertex_count, float* min_x,
                                      float* min_y, float* max_x,
                                      float* max_y) {
  size_t stride = transformed_vertex_stride(fvf);
  if (stride == 0 || vertices == NULL || vertex_count == 0) return false;

  const float* first = (const float*)vertices;
  if (first[0] != first[0] || first[1] != first[1]) return false;
  *min_x = *max_x = first[0];
  *min_y = *max_y = first[1];
  for (DWORD i = 1; i < vertex_count; ++i) {
    const float* position =
        (const float*)((const uint8_t*)vertices + i * stride);
    if (position[0] != position[0] || position[1] != position[1]) return false;
    if (position[0] < *min_x) *min_x = position[0];
    if (position[0] > *max_x) *max_x = position[0];
    if (position[1] < *min_y) *min_y = position[1];
    if (position[1] > *max_y) *max_y = position[1];
  }
  return true;
}

static uintptr_t return_address_rva(void* return_address) {
  if (g_image_base == NULL || (uint8_t*)return_address < g_image_base) return 0;
  return (uintptr_t)((uint8_t*)return_address - g_image_base);
}

#ifdef MENU_FIX_TEST
static void scale_coordinates(int width, int height, float x, float y,
                              float* scaled_x, float* scaled_y) {
  *scaled_x = x * (float)width / ORIGINAL_WIDTH;
  *scaled_y = y * (float)height / ORIGINAL_HEIGHT;
}
#endif

static void scale_centered_coordinates(int width, int height, float x, float y,
                                       float* scaled_x, float* scaled_y) {
  float scale = (float)height / ORIGINAL_HEIGHT;
  float offset_x = ((float)width - ORIGINAL_WIDTH * scale) * 0.5f;
  *scaled_x = offset_x + x * scale;
  *scaled_y = y * scale;
}

#ifdef MENU_FIX_TEST
static void unscale_coordinates(int width, int height, float x, float y,
                                float* logical_x, float* logical_y) {
  *logical_x = x * ORIGINAL_WIDTH / (float)width;
  *logical_y = y * ORIGINAL_HEIGHT / (float)height;
}
#endif

static void unscale_centered_coordinates(int width, int height, float x,
                                         float y, float* logical_x,
                                         float* logical_y) {
  float scale = (float)height / ORIGINAL_HEIGHT;
  float offset_x = ((float)width - ORIGINAL_WIDTH * scale) * 0.5f;
  *logical_x = (x - offset_x) / scale;
  *logical_y = y / scale;
}

static void scale_anchored_coordinates(int width, int height, float anchor_x,
                                       float anchor_y, float x, float y,
                                       float* scaled_x, float* scaled_y) {
  float scale = fminf((float)width / ORIGINAL_WIDTH,
                      (float)height / ORIGINAL_HEIGHT);
  *scaled_x = anchor_x + (x - anchor_x) * scale;
  *scaled_y = anchor_y + (y - anchor_y) * scale;
}

static void unscale_anchored_coordinates(int width, int height,
                                         float anchor_x, float anchor_y,
                                         float x, float y, float* logical_x,
                                         float* logical_y) {
  float scale = fminf((float)width / ORIGINAL_WIDTH,
                      (float)height / ORIGINAL_HEIGHT);
  *logical_x = anchor_x + (x - anchor_x) / scale;
  *logical_y = anchor_y + (y - anchor_y) / scale;
}

static bool gui_name_is(const void* object, const char* name) {
  const uint8_t* bytes = (const uint8_t*)object;
  const char* text = *(const char* const*)(bytes + 0x40);
  size_t length = *(const uint32_t*)(bytes + 0x44);
  return text != NULL && length == strlen(name) &&
         memcmp(text, name, length) == 0;
}

static bool is_tool_overlay(const void* object, uintptr_t vtable_rva) {
  if (vtable_rva != GUI_CONTAINER_VTABLE_RVA ||
      !gui_name_is(object, "Camera")) return false;
  /* Both camera.def and grapple.def use Camera with six named backdrop tiles. */
  const uint8_t* bytes = (const uint8_t*)object;
  void* const* begin = *(void* const* const*)(bytes + 0xA0);
  void* const* end = *(void* const* const*)(bytes + 0xA4);
  if (begin == NULL || end - begin < 6) return false;
  for (unsigned i = 0; i < 6; ++i) {
    char name[] = "Back1";
    name[4] += (char)i;
    if (!gui_name_is(begin[i], name)) return false;
  }
  return true;
}

static bool menu_uses_native_coordinates(size_t menu_index) {
  return menu_index != SIZE_MAX &&
         k_menu_slots[menu_index].coordinate_policy != COORDINATE_POLICY_LOGICAL;
}

static bool menu_clears_background(size_t menu_index) {
  return menu_index == MAIN_MENU_INDEX || menu_index == VIEWER_MENU_INDEX;
}

static bool should_clear_menu_background(size_t menu_index, LONG depth) {
  (void)depth;
  return menu_clears_background(menu_index);
}

static LONG menu_coordinate_policy(size_t menu_index, uintptr_t vtable_rva) {
  if (menu_index != SIZE_MAX) return k_menu_slots[menu_index].coordinate_policy;
  return vtable_rva == GUI_CONTAINER_VTABLE_RVA ? COORDINATE_POLICY_AUTO
                                                 : COORDINATE_POLICY_NONE;
}

static LONG menu_object_coordinate_policy(const void* object,
                                          uintptr_t vtable_rva) {
  /* Question dialogs are centered native layouts, including guiQuesExit.def. */
  if (vtable_rva == GUI_CONTAINER_VTABLE_RVA &&
      gui_name_is(object, "Question")) return COORDINATE_POLICY_CENTER;
  return menu_coordinate_policy(menu_index_for_vtable(vtable_rva), vtable_rva);
}

static cursor_point_t* cursor_position_getter_hook(void) {
  void* cursor = *(void**)(g_image_base + CURSOR_OBJECT_RVA);
  if (cursor == NULL) return NULL;

  cursor_point_t* physical = (cursor_point_t*)((uint8_t*)cursor + 0x0C);
  LONG policy = InterlockedCompareExchange(&g_update_policy, 0, 0);
  if (InterlockedCompareExchange(&g_menu_update_depth, 0, 0) <= 0 ||
      policy == COORDINATE_POLICY_NONE || policy == COORDINATE_POLICY_AUTO ||
      g_scale_x <= 0.0f || g_scale_y <= 0.0f) {
    return physical;
  }

  g_logical_cursor_position = *physical;
  if (policy == COORDINATE_POLICY_LOGICAL) {
    unscale_centered_coordinates(
        g_output_width, g_output_height, physical->x, physical->y,
        &g_logical_cursor_position.x, &g_logical_cursor_position.y);
  } else {
    float anchor_x = policy == COORDINATE_POLICY_RIGHT_TOP
                         ? (float)g_output_width
                         : policy == COORDINATE_POLICY_CENTER
                               ? (float)g_output_width * 0.5f
                               : 0.0f;
    float anchor_y = policy == COORDINATE_POLICY_LEFT_BOTTOM
                         ? (float)g_output_height
                         : policy == COORDINATE_POLICY_CENTER
                               ? (float)g_output_height * 0.5f
                               : 0.0f;
    unscale_anchored_coordinates(
        g_output_width, g_output_height, anchor_x, anchor_y, physical->x,
        physical->y, &g_logical_cursor_position.x,
        &g_logical_cursor_position.y);
  }
  return &g_logical_cursor_position;
}

static void __attribute__((thiscall)) menu_update_hook(void* self) {
  void** vtable = *(void***)self;
  uintptr_t vtable_rva = (uintptr_t)((uint8_t*)vtable - g_image_base);
  size_t menu_index = menu_index_for_vtable(vtable_rva);
  LONG policy = menu_object_coordinate_policy(self, vtable_rva);
  LONG previous_policy =
      InterlockedCompareExchange(&g_update_policy, 0, 0);
  if (policy == COORDINATE_POLICY_AUTO &&
      previous_policy != COORDINATE_POLICY_NONE) policy = previous_policy;
  LONG previous_menu_index =
      InterlockedExchange(&g_update_menu_index, (LONG)menu_index);
  InterlockedIncrement(&g_menu_update_depth);
  if (policy != COORDINATE_POLICY_NONE) {
    InterlockedExchange(&g_update_policy, policy);
  }
  if (menu_index != SIZE_MAX && menu_uses_native_coordinates(menu_index)) {
    LONG menu_bit = 1L << menu_index;
    if ((InterlockedOr(&g_logged_native_cursor_mask, menu_bit) & menu_bit) ==
        0) {
      shim_log("Menu input: %s uses inverse anchored coordinates",
               k_menu_slots[menu_index].name);
    }
  }
  g_original_menu_update(self);
  if (policy != COORDINATE_POLICY_NONE) {
    InterlockedExchange(&g_update_policy, previous_policy);
  }
  InterlockedExchange(&g_update_menu_index, previous_menu_index);
  InterlockedDecrement(&g_menu_update_depth);
}

static void choose_auto_coordinate_anchor(int width, int height, float min_x,
                                          float min_y, float max_x,
                                          float max_y, float* anchor_x,
                                          float* anchor_y) {
  /* ponytail: fallback for unnamed HUD containers; use layout anchors if a
     future layout has primitives spanning the centered canvas boundary. */
  float canvas_left = ((float)width - ORIGINAL_WIDTH) * 0.5f;
  float canvas_top = ((float)height - ORIGINAL_HEIGHT) * 0.5f;
  float canvas_right = canvas_left + ORIGINAL_WIDTH;
  float canvas_bottom = canvas_top + ORIGINAL_HEIGHT;
  *anchor_x = max_x <= canvas_left
                  ? 0.0f
                  : min_x >= canvas_right ? (float)width
                                           : (float)width * 0.5f;
  *anchor_y = max_y <= canvas_top
                  ? 0.0f
                  : min_y >= canvas_bottom ? (float)height
                                            : (float)height * 0.5f;
}

static void auto_coordinate_anchor(DWORD fvf, const void* vertices,
                                   DWORD vertex_count, float* anchor_x,
                                   float* anchor_y) {
  float min_x;
  float min_y;
  float max_x;
  float max_y;
  if (!transformed_vertex_bounds(fvf, vertices, vertex_count, &min_x, &min_y,
                                 &max_x, &max_y)) {
    *anchor_x = (float)g_output_width * 0.5f;
    *anchor_y = (float)g_output_height * 0.5f;
    return;
  }
  choose_auto_coordinate_anchor(g_output_width, g_output_height, min_x, min_y,
                                max_x, max_y, anchor_x, anchor_y);
}

static void* scaled_vertex_copy(DWORD fvf, const void* vertices,
                                DWORD vertex_count, LONG policy,
                                bool stretch_background) {
  size_t stride = transformed_vertex_stride(fvf);
  if (stride == 0 || vertices == NULL || vertex_count == 0 ||
      vertex_count > SIZE_MAX / stride) {
    return NULL;
  }

  size_t size = stride * vertex_count;
  uint8_t* copy = (uint8_t*)malloc(size);
  if (copy == NULL) return NULL;
  memcpy(copy, vertices, size);

  float anchor_x = 0.0f;
  float anchor_y = 0.0f;
  if (policy == COORDINATE_POLICY_CENTER) {
    anchor_x = (float)g_output_width * 0.5f;
    anchor_y = (float)g_output_height * 0.5f;
  } else if (policy == COORDINATE_POLICY_LEFT_BOTTOM) {
    anchor_y = (float)g_output_height;
  } else if (policy == COORDINATE_POLICY_RIGHT_TOP) {
    anchor_x = (float)g_output_width;
  } else if (policy == COORDINATE_POLICY_AUTO) {
    auto_coordinate_anchor(fvf, vertices, vertex_count, &anchor_x, &anchor_y);
  }

  for (DWORD i = 0; i < vertex_count; ++i) {
    float* position = (float*)(copy + i * stride);
    if (stretch_background) {
      position[0] = anchor_x + (position[0] - anchor_x) * g_scale_x;
      position[1] = anchor_y + (position[1] - anchor_y) * g_scale_y;
    } else if (policy == COORDINATE_POLICY_LOGICAL) {
      scale_centered_coordinates(g_output_width, g_output_height, position[0],
                                 position[1], &position[0], &position[1]);
    } else if (policy == COORDINATE_POLICY_NONE) {
      position[0] *= g_scale_x;
      position[1] *= g_scale_y;
    } else {
      scale_anchored_coordinates(g_output_width, g_output_height, anchor_x,
                                 anchor_y, position[0], position[1],
                                 &position[0], &position[1]);
    }
  }
  return copy;
}

static void trace_primitive(DWORD fvf, const void* vertices,
                            DWORD vertex_count, void* return_address,
                            bool scaled) {
  if (g_trace_menu < 0) return;
  if (g_trace_primitives++ == 0) {
    g_trace_fvf = fvf;
    g_trace_vertices = vertex_count;
  }
  float min_x;
  float min_y;
  float max_x;
  float max_y;
  if (transformed_vertex_bounds(fvf, vertices, vertex_count, &min_x, &min_y,
                                &max_x, &max_y)) {
    shim_log("Menu primitive: %s caller=0x%08lx fvf=0x%08lx vertices=%lu "
             "bounds=(%.1f,%.1f)-(%.1f,%.1f) scaled=%u",
             k_menu_slots[g_trace_menu].name,
             (unsigned long)return_address_rva(return_address),
             (unsigned long)fvf, (unsigned long)vertex_count, min_x, min_y,
             max_x, max_y, scaled ? 1u : 0u);
  }
}

#ifdef MENU_FIX_DIAGNOSTICS
static void dump_cursor_texture(const DDSURFACEDESC2* desc,
                                const DDSURFACEDESC2* locked) {
  if (desc->ddpfPixelFormat.dwRGBBitCount != 16 || locked->lpSurface == NULL ||
      desc->dwWidth == 0 || desc->dwHeight == 0) {
    return;
  }

  DWORD output_width = desc->dwWidth * 2;
  DWORD row_size = (output_width * 3 + 3) & ~3u;
  if (desc->dwHeight > SIZE_MAX / row_size) return;
  size_t pixel_bytes = (size_t)row_size * desc->dwHeight;
  uint8_t* output = (uint8_t*)calloc(1, pixel_bytes);
  if (output == NULL) return;

  size_t source_pitch =
      locked->lPitch < 0 ? (size_t)-locked->lPitch : (size_t)locked->lPitch;
  for (DWORD y = 0; y < desc->dwHeight; ++y) {
    const uint16_t* source =
        (const uint16_t*)((const uint8_t*)locked->lpSurface + y * source_pitch);
    uint8_t* destination = output + y * row_size;
    for (DWORD x = 0; x < desc->dwWidth; ++x) {
      uint16_t pixel = source[x];
      uint8_t alpha = (uint8_t)(((pixel & desc->ddpfPixelFormat.dwRGBAlphaBitMask) >>
                                 12) *
                                17);
      uint8_t red = (uint8_t)(((pixel & desc->ddpfPixelFormat.dwRBitMask) >> 8) *
                              17);
      uint8_t green =
          (uint8_t)(((pixel & desc->ddpfPixelFormat.dwGBitMask) >> 4) * 17);
      uint8_t blue =
          (uint8_t)((pixel & desc->ddpfPixelFormat.dwBBitMask) * 17);
      destination[x * 3 + 0] = blue;
      destination[x * 3 + 1] = green;
      destination[x * 3 + 2] = red;
      size_t alpha_offset = (size_t)(x + desc->dwWidth) * 3;
      destination[alpha_offset + 0] = alpha;
      destination[alpha_offset + 1] = alpha;
      destination[alpha_offset + 2] = alpha;
    }
  }

  BITMAPFILEHEADER file_header;
  BITMAPINFOHEADER info_header;
  memset(&file_header, 0, sizeof(file_header));
  memset(&info_header, 0, sizeof(info_header));
  file_header.bfType = 0x4d42;
  file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
  file_header.bfSize = file_header.bfOffBits + (DWORD)pixel_bytes;
  info_header.biSize = sizeof(info_header);
  info_header.biWidth = (LONG)output_width;
  info_header.biHeight = -(LONG)desc->dwHeight;
  info_header.biPlanes = 1;
  info_header.biBitCount = 24;
  info_header.biCompression = BI_RGB;
  info_header.biSizeImage = (DWORD)pixel_bytes;

  HANDLE file = CreateFileA("cursor_texture_dump.bmp", GENERIC_WRITE,
                            FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written;
    BOOL ok = WriteFile(file, &file_header, sizeof(file_header), &written, NULL) &&
              written == sizeof(file_header) &&
              WriteFile(file, &info_header, sizeof(info_header), &written, NULL) &&
              written == sizeof(info_header) &&
              WriteFile(file, output, (DWORD)pixel_bytes, &written, NULL) &&
              written == pixel_bytes;
    shim_log("Cursor texture dump: %s", ok ? "cursor_texture_dump.bmp"
                                             : "write failed");
    CloseHandle(file);
  }
  free(output);
}

static void log_cursor_draw_state(IDirect3DDevice7* device, DWORD fvf,
                                  const void* vertices, DWORD vertex_count,
                                  LPDIRECTDRAWSURFACE7 texture,
                                  const DDSURFACEDESC2* desc) {
  if (InterlockedCompareExchange(&g_cursor_state_logged, 1, 0) != 0) return;

  DWORD alpha_test = 0xffffffffu;
  DWORD source_blend = 0xffffffffu;
  DWORD destination_blend = 0xffffffffu;
  DWORD alpha_blend = 0xffffffffu;
  DWORD color_key = 0xffffffffu;
  DWORD color_op = 0xffffffffu;
  DWORD color_arg1 = 0xffffffffu;
  DWORD color_arg2 = 0xffffffffu;
  DWORD alpha_op = 0xffffffffu;
  DWORD alpha_arg1 = 0xffffffffu;
  DWORD alpha_arg2 = 0xffffffffu;
  IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_ALPHATESTENABLE,
                                  &alpha_test);
  IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_SRCBLEND,
                                  &source_blend);
  IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_DESTBLEND,
                                  &destination_blend);
  IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_ALPHABLENDENABLE,
                                  &alpha_blend);
  IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_COLORKEYENABLE,
                                  &color_key);
  IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_COLOROP, &color_op);
  IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_COLORARG1,
                                        &color_arg1);
  IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_COLORARG2,
                                        &color_arg2);
  IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_ALPHAOP, &alpha_op);
  IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_ALPHAARG1,
                                        &alpha_arg1);
  IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_ALPHAARG2,
                                        &alpha_arg2);
  shim_log("Cursor state: atest=%lu blend=%lu src=%lu dst=%lu ckey=%lu "
           "color=(%lu,%lu,%lu) alpha=(%lu,%lu,%lu)",
           (unsigned long)alpha_test, (unsigned long)alpha_blend,
           (unsigned long)source_blend, (unsigned long)destination_blend,
           (unsigned long)color_key, (unsigned long)color_op,
           (unsigned long)color_arg1, (unsigned long)color_arg2,
           (unsigned long)alpha_op, (unsigned long)alpha_arg1,
           (unsigned long)alpha_arg2);

  size_t stride = transformed_vertex_stride(fvf);
  if (stride != 0 && vertices != NULL && vertex_count != 0) {
    shim_log("Cursor vertices: fvf=0x%08lx count=%lu stride=%lu",
             (unsigned long)fvf, (unsigned long)vertex_count,
             (unsigned long)stride);
    for (DWORD i = 0; i < vertex_count; ++i) {
      const uint8_t* vertex = (const uint8_t*)vertices + i * stride;
      const float* position = (const float*)vertex;
      size_t color_offset = 4 * sizeof(float);
      if (fvf & D3DFVF_NORMAL) color_offset += 3 * sizeof(float);
      DWORD diffuse = 0xffffffffu;
      DWORD specular = 0xffffffffu;
      if (fvf & D3DFVF_DIFFUSE) {
        memcpy(&diffuse, vertex + color_offset, sizeof(diffuse));
        color_offset += sizeof(DWORD);
      }
      if (fvf & D3DFVF_SPECULAR) {
        memcpy(&specular, vertex + color_offset, sizeof(specular));
        color_offset += sizeof(DWORD);
      }
      float u = 0.0f;
      float v = 0.0f;
      if ((fvf & D3DFVF_TEXCOUNT_MASK) != 0) {
        memcpy(&u, vertex + color_offset, sizeof(u));
        memcpy(&v, vertex + color_offset + sizeof(u), sizeof(v));
      }
      shim_log("Cursor vertex %lu: pos=(%.1f,%.1f,%.3f,%.3f) "
               "diffuse=0x%08lx specular=0x%08lx uv=(%.3f,%.3f)",
               (unsigned long)i, position[0], position[1], position[2],
               position[3], (unsigned long)diffuse, (unsigned long)specular,
               u, v);
    }
  }

  if (texture == NULL || desc == NULL) return;
  const DDPIXELFORMAT* format = &desc->ddpfPixelFormat;
  shim_log("Cursor texture: caps=0x%08lx flags=0x%08lx fourcc=0x%08lx "
           "bpp=%lu masks=%08lx/%08lx/%08lx/%08lx mipmaps=%lu pitch=%ld",
           (unsigned long)desc->ddsCaps.dwCaps, (unsigned long)format->dwFlags,
           (unsigned long)format->dwFourCC, (unsigned long)format->dwRGBBitCount,
           (unsigned long)format->dwRBitMask,
           (unsigned long)format->dwGBitMask,
           (unsigned long)format->dwBBitMask,
           (unsigned long)format->dwRGBAlphaBitMask,
           (unsigned long)desc->dwMipMapCount, (long)desc->lPitch);

  DDCOLORKEY source_key;
  memset(&source_key, 0, sizeof(source_key));
  HRESULT key_result =
      IDirectDrawSurface7_GetColorKey(texture, DDCKEY_SRCBLT, &source_key);
  shim_log("Cursor color key: result=0x%08lx low=0x%08lx high=0x%08lx",
           (unsigned long)key_result, (unsigned long)source_key.dwColorSpaceLowValue,
           (unsigned long)source_key.dwColorSpaceHighValue);

  DDSURFACEDESC2 locked;
  memset(&locked, 0, sizeof(locked));
  locked.dwSize = sizeof(locked);
  HRESULT lock_result = IDirectDrawSurface7_Lock(
      texture, NULL, &locked, DDLOCK_READONLY | DDLOCK_WAIT, NULL);
  uint32_t hash = 2166136261u;
  unsigned first[4] = {0, 0, 0, 0};
  if (SUCCEEDED(lock_result) && locked.lpSurface != NULL) {
    const uint8_t* pixels = (const uint8_t*)locked.lpSurface;
    size_t pitch = locked.lPitch < 0 ? (size_t)-locked.lPitch
                                    : (size_t)locked.lPitch;
    size_t bytes_per_pixel = (format->dwRGBBitCount + 7) / 8;
    size_t row_bytes = (size_t)desc->dwWidth * bytes_per_pixel;
    if (row_bytes == 0 || row_bytes > pitch) row_bytes = pitch;
    for (unsigned i = 0; i < 4 && i < row_bytes; ++i) first[i] = pixels[i];
    for (DWORD y = 0; y < desc->dwHeight; ++y) {
      const uint8_t* row = pixels + y * pitch;
      for (size_t x = 0; x < row_bytes; ++x) {
        hash ^= row[x];
        hash *= 16777619u;
      }
    }
    dump_cursor_texture(desc, &locked);
    IDirectDrawSurface7_Unlock(texture, NULL);
  }
  shim_log("Cursor pixels: lock=0x%08lx pitch=%ld hash=0x%08lx "
           "first=%02x%02x%02x%02x",
           (unsigned long)lock_result, (long)locked.lPitch,
           (unsigned long)hash, first[0], first[1], first[2], first[3]);
}
#endif

static bool cursor_texture_needs_sync(const DDSURFACEDESC2* desc) {
  if (desc == NULL) return false;
  const DDPIXELFORMAT* format = &desc->ddpfPixelFormat;
  return desc->dwWidth == 32 && desc->dwHeight == 32 &&
         (desc->ddsCaps.dwCaps & (DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY)) ==
             (DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY) &&
         (format->dwFlags & (DDPF_RGB | DDPF_ALPHAPIXELS)) ==
             (DDPF_RGB | DDPF_ALPHAPIXELS) &&
         format->dwRGBBitCount == 16 && format->dwRBitMask == 0x00000f00 &&
         format->dwGBitMask == 0x000000f0 && format->dwBBitMask == 0x0000000f &&
         format->dwRGBAlphaBitMask == 0x0000f000;
}

static void synchronize_cursor_texture(LPDIRECTDRAWSURFACE7 texture,
                                       const DDSURFACEDESC2* desc) {
  if (texture == NULL || !cursor_texture_needs_sync(desc) ||
      InterlockedCompareExchange(&g_cursor_texture_synced, 1, 0) != 0) {
    return;
  }

  DDSURFACEDESC2 locked;
  memset(&locked, 0, sizeof(locked));
  locked.dwSize = sizeof(locked);
  HRESULT lock_result = IDirectDrawSurface7_Lock(
      texture, NULL, &locked, DDLOCK_READONLY | DDLOCK_WAIT, NULL);
  HRESULT unlock_result = E_FAIL;
  if (SUCCEEDED(lock_result)) {
    unlock_result = IDirectDrawSurface7_Unlock(texture, NULL);
  }
  shim_log("Cursor fix: synchronized ARGB4444 texture "
           "(lock=0x%08lx unlock=0x%08lx)",
           (unsigned long)lock_result, (unsigned long)unlock_result);
}

static bool untextured_cursor_candidate(IDirect3DDevice7* device, DWORD fvf,
                                        const void* vertices,
                                        DWORD vertex_count,
                                        void* return_address, float* left,
                                        float* top, float* z, float* rhw) {
  if (InterlockedCompareExchange(&g_render_policy, 0, 0) !=
          COORDINATE_POLICY_NONE ||
      InterlockedCompareExchange(&g_seen_known_menu, 0, 0) == 0 ||
      return_address_rva(return_address) != CURSOR_DRAW_RETURN_RVA) {
    return false;
  }

  float min_x;
  float min_y;
  float max_x;
  float max_y;
  if (!transformed_vertex_bounds(fvf, vertices, vertex_count, &min_x, &min_y,
                                 &max_x, &max_y) ||
      max_x - min_x < 31.0f || max_x - min_x > 33.0f ||
      max_y - min_y < 31.0f || max_y - min_y > 33.0f) {
    return false;
  }

  LPDIRECTDRAWSURFACE7 texture = NULL;
  HRESULT texture_result = IDirect3DDevice7_GetTexture(device, 0, &texture);
  DDSURFACEDESC2 desc;
  memset(&desc, 0, sizeof(desc));
  desc.dwSize = sizeof(desc);
  HRESULT desc_result = texture == NULL
                            ? E_POINTER
                            : IDirectDrawSurface7_GetSurfaceDesc(texture,
                                                                 &desc);
  LONG log_index = InterlockedIncrement(&g_cursor_draw_logs);
  if (log_index <= 8) {
    shim_log("Cursor candidate: caller=0x%08lx bounds=(%.1f,%.1f)-(%.1f,%.1f) "
             "texture=%p get=0x%08lx desc=0x%08lx size=%lux%lu",
             (unsigned long)return_address_rva(return_address), min_x, min_y,
             max_x, max_y, texture, (unsigned long)texture_result,
             (unsigned long)desc_result, (unsigned long)desc.dwWidth,
              (unsigned long)desc.dwHeight);
  }
  synchronize_cursor_texture(texture, &desc);
  if (texture != NULL) IDirectDrawSurface7_Release(texture);
  if (FAILED(texture_result) || texture != NULL) return false;

  const float* first = (const float*)vertices;
  *left = min_x;
  *top = min_y;
  *z = first[2];
  *rhw = first[3];
  return true;
}

static HRESULT draw_fallback_cursor(IDirect3DDevice7* device, float left,
                                    float top, float z, float rhw,
                                    DWORD flags) {
  static const float k_shape[][2] = {
      {0, 0}, {0, 24}, {7, 18},
      {0, 0}, {7, 18}, {18, 18},
      {7, 18}, {12, 31}, {18, 28},
  };
  colored_tl_vertex_t vertices[sizeof(k_shape) / sizeof(k_shape[0])];
  for (size_t i = 0; i < sizeof(vertices) / sizeof(vertices[0]); ++i) {
    vertices[i].x = left + k_shape[i][0];
    vertices[i].y = top + k_shape[i][1];
    vertices[i].z = z;
    vertices[i].rhw = rhw;
    vertices[i].color = 0xFFFF00A8u;
  }
  return g_original_draw_primitive(
      device, D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, vertices,
      (DWORD)(sizeof(vertices) / sizeof(vertices[0])), flags);
}

static bool should_scale_primitive(DWORD fvf, const void* vertices,
                                   DWORD vertex_count) {
  (void)vertices;
  (void)vertex_count;
  if (g_drawing_cursor) return false;
  if (transformed_vertex_stride(fvf) == 0) return false;
  LONG policy = InterlockedCompareExchange(&g_render_policy, 0, 0);
  return policy != COORDINATE_POLICY_NONE ||
         (policy == COORDINATE_POLICY_NONE &&
          InterlockedCompareExchange(&g_seen_known_menu, 0, 0) == 0);
}

static HRESULT WINAPI menu_draw_primitive(
    IDirect3DDevice7* device, D3DPRIMITIVETYPE primitive_type, DWORD fvf,
    void* vertices, DWORD vertex_count, DWORD flags) {
  if (transformed_vertex_stride(fvf) == 0) {
    return g_original_draw_primitive(device, primitive_type, fvf, vertices,
                                     vertex_count, flags);
  }

  LONG active_menu = InterlockedCompareExchange(&g_active_menu_index, 0, 0);
  unsigned primitive_index = g_active_primitive_index++;
  bool main_background =
      active_menu == MAIN_MENU_INDEX && primitive_index < 6;
  if (main_background) {
    dump_main_menu_texture(device, primitive_index);
    apply_main_menu_texture_override(device, primitive_index);
  }
  bool scale = should_scale_primitive(fvf, vertices, vertex_count);
  bool stretch_background = main_background ||
      (active_menu == TOOL_OVERLAY_INDEX && primitive_index < 6);
  float cursor_left;
  float cursor_top;
  float cursor_z;
  float cursor_rhw;
  if (!scale && untextured_cursor_candidate(
                    device, fvf, vertices, vertex_count,
                    __builtin_return_address(0), &cursor_left, &cursor_top,
                    &cursor_z, &cursor_rhw)) {
    return draw_fallback_cursor(device, cursor_left, cursor_top, cursor_z,
                                cursor_rhw, flags);
  }
  trace_primitive(fvf, vertices, vertex_count, __builtin_return_address(0),
                  scale);
  if (!scale) {
    return g_original_draw_primitive(device, primitive_type, fvf, vertices,
                                     vertex_count, flags);
  }
  LONG policy = InterlockedCompareExchange(&g_render_policy, 0, 0);
  void* scaled = scaled_vertex_copy(fvf, vertices, vertex_count, policy,
                                    stretch_background);
  HRESULT result = g_original_draw_primitive(
      device, primitive_type, fvf, scaled != NULL ? scaled : vertices,
      vertex_count, flags);
  free(scaled);
  return result;
}

static HRESULT WINAPI menu_draw_indexed_primitive(
    IDirect3DDevice7* device, D3DPRIMITIVETYPE primitive_type, DWORD fvf,
    void* vertices, DWORD vertex_count, WORD* indices, DWORD index_count,
    DWORD flags) {
  if (transformed_vertex_stride(fvf) == 0) {
    return g_original_draw_indexed_primitive(
        device, primitive_type, fvf, vertices, vertex_count, indices,
        index_count, flags);
  }

  LONG active_menu = InterlockedCompareExchange(&g_active_menu_index, 0, 0);
  unsigned primitive_index = g_active_primitive_index++;
  bool main_background =
      active_menu == MAIN_MENU_INDEX && primitive_index < 6;
  if (main_background) {
    dump_main_menu_texture(device, primitive_index);
    apply_main_menu_texture_override(device, primitive_index);
  }
  bool scale = should_scale_primitive(fvf, vertices, vertex_count);
  bool stretch_background = main_background ||
      (active_menu == TOOL_OVERLAY_INDEX && primitive_index < 6);
  float cursor_left;
  float cursor_top;
  float cursor_z;
  float cursor_rhw;
  if (!scale && untextured_cursor_candidate(
                    device, fvf, vertices, vertex_count,
                    __builtin_return_address(0), &cursor_left, &cursor_top,
                    &cursor_z, &cursor_rhw)) {
    return draw_fallback_cursor(device, cursor_left, cursor_top, cursor_z,
                                cursor_rhw, flags);
  }
  trace_primitive(fvf, vertices, vertex_count, __builtin_return_address(0),
                  scale);
  if (!scale) {
    return g_original_draw_indexed_primitive(
        device, primitive_type, fvf, vertices, vertex_count, indices,
        index_count, flags);
  }
  LONG policy = InterlockedCompareExchange(&g_render_policy, 0, 0);
  void* scaled = scaled_vertex_copy(fvf, vertices, vertex_count, policy,
                                    stretch_background);
  HRESULT result = g_original_draw_indexed_primitive(
      device, primitive_type, fvf, scaled != NULL ? scaled : vertices,
      vertex_count, indices, index_count, flags);
  free(scaled);
  return result;
}

static float zoomed_fov(float fov, float factor) {
  const float radians_per_degree = 3.14159265358979323846f / 180.0f;
  return 2.0f * atanf(tanf(fov * 0.5f * radians_per_degree) / factor) /
         radians_per_degree;
}

static bool ensure_device_hooks(void) {
  IDirect3DDevice7* device =
      *(IDirect3DDevice7**)(g_image_base + D3D_DEVICE_RVA);
  if (device == NULL) return false;

  void** vtable = *(void***)device;
  if (vtable[DRAW_PRIMITIVE_SLOT] == (void*)menu_draw_primitive &&
      vtable[DRAW_INDEXED_PRIMITIVE_SLOT] ==
          (void*)menu_draw_indexed_primitive) {
    return true;
  }

  draw_primitive_fn draw_primitive =
      (draw_primitive_fn)vtable[DRAW_PRIMITIVE_SLOT];
  draw_indexed_primitive_fn draw_indexed_primitive =
      (draw_indexed_primitive_fn)vtable[DRAW_INDEXED_PRIMITIVE_SLOT];
  g_original_draw_primitive = draw_primitive;
  g_original_draw_indexed_primitive = draw_indexed_primitive;
  void* primitive_hook = (void*)menu_draw_primitive;
  void* indexed_hook = (void*)menu_draw_indexed_primitive;
  if (!write_memory(&vtable[DRAW_PRIMITIVE_SLOT], &primitive_hook,
                    sizeof(primitive_hook)) ||
      !write_memory(&vtable[DRAW_INDEXED_PRIMITIVE_SLOT], &indexed_hook,
                    sizeof(indexed_hook))) {
    write_memory(&vtable[DRAW_PRIMITIVE_SLOT], &draw_primitive,
                 sizeof(draw_primitive));
    write_memory(&vtable[DRAW_INDEXED_PRIMITIVE_SLOT],
                 &draw_indexed_primitive, sizeof(draw_indexed_primitive));
    return false;
  }

  if (InterlockedCompareExchange(&g_device_hook_logged, 1, 0) == 0) {
    shim_log("Menu fix: hooked pre-transformed Direct3D primitives");
  }
  return true;
}

static void render_cursor_overlay(void* cursor, IDirect3DDevice7* device,
                                  LONG menu_depth, uintptr_t caller_rva) {
  /* saHudOutfit draws it once inside the HUD; the frame draws it again last. */
  if (cursor == NULL || menu_depth > 0 ||
      caller_rva == OUTFIT_CURSOR_RETURN_RVA) return;
  DWORD z_enabled = 0;
  DWORD z_write = 0;
  bool saved_depth = device != NULL &&
      SUCCEEDED(IDirect3DDevice7_GetRenderState(
          device, D3DRENDERSTATE_ZENABLE, &z_enabled)) &&
      SUCCEEDED(IDirect3DDevice7_GetRenderState(
          device, D3DRENDERSTATE_ZWRITEENABLE, &z_write));
  if (saved_depth) {
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, FALSE);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZWRITEENABLE, FALSE);
  }
  bool previous_cursor = g_drawing_cursor;
  LONG previous_policy = InterlockedExchange(&g_render_policy,
                                             COORDINATE_POLICY_NONE);
  g_drawing_cursor = true;
  void** vtable = *(void***)cursor;
  ((menu_render_fn)vtable[2])(cursor);
  g_drawing_cursor = previous_cursor;
  InterlockedExchange(&g_render_policy, previous_policy);
  if (saved_depth) {
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, z_enabled);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZWRITEENABLE, z_write);
  }
}

static void cursor_render_hook(void) {
  render_cursor_overlay(
      *(void**)(g_image_base + CURSOR_OBJECT_RVA),
      *(IDirect3DDevice7**)(g_image_base + D3D_DEVICE_RVA),
      InterlockedCompareExchange(&g_menu_render_depth, 0, 0),
      return_address_rva(__builtin_return_address(0)));
}

static void __attribute__((thiscall)) device_ready_hook(void* self) {
  g_original_device_ready(self);
  if (!ensure_device_hooks()) {
    shim_log("Menu fix: Direct3D device hook installation failed");
  }
}

static bool configure_viewer_camera(bool zoom) {
  float* camera =
      *(float**)(g_image_base + CAMERA_ARRAY_RVA + 3 * sizeof(void*));
  if (camera == NULL) return false;

  float fov = *(float*)((uint8_t*)camera + 0xb0);
  float aspect = *(float*)((uint8_t*)camera + 0xb4);
  float near_plane = *(float*)((uint8_t*)camera + 0xb8);
  float far_plane = *(float*)((uint8_t*)camera + 0xbc);
  if (zoom) {
    g_viewer_base_fov = fov;
  }
  else if (g_viewer_base_fov <= 0.0f) {
    return false;
  }
  float configured_fov =
      zoom ? zoomed_fov(fov, VIEWER_MODEL_ZOOM) : g_viewer_base_fov;
  camera_config_fn configure_camera =
      (camera_config_fn)(g_image_base + CAMERA_CONFIG_RVA);
  configure_camera(camera, configured_fov, aspect, near_plane, far_plane);
  if (zoom &&
      InterlockedCompareExchange(&g_viewer_camera_logged, 1, 0) == 0) {
    shim_log("Viewer camera: fov %.3f -> %.3f aspect=%.3f near=%.3f far=%.3f",
             fov, configured_fov, aspect, near_plane, far_plane);
  }
  return true;
}

static void __attribute__((thiscall)) viewer_update_hook(void* self) {
  g_original_viewer_update(self);
  configure_viewer_camera(true);
}

static void __attribute__((thiscall)) viewer_render_hook(void* self) {
  g_original_viewer_render(self);
  configure_viewer_camera(false);
}

static void clear_menu_background(size_t menu_index) {
  if (!menu_clears_background(menu_index)) return;

  IDirect3DDevice7* device =
      *(IDirect3DDevice7**)(g_image_base + D3D_DEVICE_RVA);
  if (device == NULL) return;

  HRESULT result = IDirect3DDevice7_Clear(
      device, 0, NULL, D3DCLEAR_TARGET, 0x00000000u, 1.0f, 0);
  LONG menu_bit = 1L << menu_index;
  if ((InterlockedOr(&g_clear_logged_mask, menu_bit) & menu_bit) == 0) {
    shim_log("Menu fix: cleared %s background (result=0x%08lx)",
             k_menu_slots[menu_index].name, (unsigned long)result);
  }
}

static void __attribute__((thiscall)) menu_render_hook(void* self) {
  void** vtable = *(void***)self;
  uintptr_t vtable_rva = (uintptr_t)((uint8_t*)vtable - g_image_base);
  size_t menu_index = menu_index_for_vtable(vtable_rva);
  if (!ensure_device_hooks()) {
    g_original_menu_render(self);
    return;
  }

  LONG policy = menu_object_coordinate_policy(self, vtable_rva);
  bool tool_overlay = is_tool_overlay(self, vtable_rva);
  if (tool_overlay) policy = COORDINATE_POLICY_CENTER;
  LONG previous_policy =
      InterlockedCompareExchange(&g_render_policy, 0, 0);
  if (policy == COORDINATE_POLICY_AUTO &&
      previous_policy != COORDINATE_POLICY_NONE) policy = previous_policy;
  LONG previous_active_menu =
      InterlockedExchange(&g_active_menu_index,
                          tool_overlay ? TOOL_OVERLAY_INDEX : (LONG)menu_index);
  unsigned previous_primitive_index = g_active_primitive_index;
  g_active_primitive_index = 0;
  LONG depth = InterlockedIncrement(&g_menu_render_depth);
  if (policy != COORDINATE_POLICY_NONE) {
    InterlockedExchange(&g_seen_known_menu, 1);
    InterlockedExchange(&g_render_policy, policy);
  }

  if (should_clear_menu_background(menu_index, depth)) {
    clear_menu_background(menu_index);
  }

  if (depth == 1 && menu_index == SIZE_MAX &&
      InterlockedCompareExchange(&g_logged_dynamic_menu, 1, 0) == 0) {
    shim_log("Menu fix: scaling additional menu class at vtable RVA 0x%08lx",
             (unsigned long)vtable_rva);
  }

  LONG menu_bit = menu_index == SIZE_MAX ? 0 : 1L << menu_index;
  bool trace = menu_index != SIZE_MAX && g_trace_menu < 0 &&
               (g_logged_menu_mask & menu_bit) == 0;
  if (trace) {
    g_trace_menu = (LONG)menu_index;
    g_trace_primitives = 0;
    g_trace_fvf = 0;
    g_trace_vertices = 0;
  }
  g_original_menu_render(self);
  if (policy != COORDINATE_POLICY_NONE) {
    InterlockedExchange(&g_render_policy, previous_policy);
  }
  g_active_primitive_index = previous_primitive_index;
  InterlockedExchange(&g_active_menu_index, previous_active_menu);
  InterlockedDecrement(&g_menu_render_depth);
  if (trace) {
    shim_log("Menu trace: %s primitives=%u first_fvf=0x%08lx vertices=%lu",
             k_menu_slots[menu_index].name, g_trace_primitives,
             (unsigned long)g_trace_fvf, (unsigned long)g_trace_vertices);
    g_trace_menu = -1;
    InterlockedOr(&g_logged_menu_mask, menu_bit);
  }
}

static bool install_cursor_getter_hook(void) {
  uint8_t* site = g_image_base + CURSOR_GETTER_RVA;
  if (memcmp(site, k_cursor_getter_signature,
             sizeof(k_cursor_getter_signature)) != 0) {
    return false;
  }

  uint8_t patch[sizeof(k_cursor_getter_signature)] = {0xE9};
  int32_t relative =
      (int32_t)((uint8_t*)cursor_position_getter_hook - (site + 5));
  memcpy(patch + 1, &relative, sizeof(relative));
  patch[5] = 0x90;
  return write_memory(site, patch, sizeof(patch));
}

static void screen_cursor_bounds(int width, int height, int* bounds) {
  bounds[0] = width;
  bounds[1] = height;
  bounds[2] = 0;
  bounds[3] = 0;
}

static void cursor_menu_bounds_hook(void* menu) {
  (void)menu;
  /* Menu rectangles are still unscaled; pointer motion is in output pixels. */
  screen_cursor_bounds(g_output_width, g_output_height,
                       (int*)(g_image_base + CURSOR_BOUNDS_RVA));
}

static bool install_cursor_bounds_hook(void) {
  uint8_t* site = g_image_base + CURSOR_MENU_BOUNDS_RVA;
  if (memcmp(site, k_cursor_bounds_signature,
             sizeof(k_cursor_bounds_signature)) != 0) return false;
  uint8_t patch[sizeof(k_cursor_bounds_signature)] = {0xE9};
  int32_t relative = (int32_t)((uint8_t*)cursor_menu_bounds_hook - (site + 5));
  memcpy(patch + 1, &relative, sizeof(relative));
  memset(patch + 5, 0x90, sizeof(patch) - 5);
  return write_memory(site, patch, sizeof(patch));
}

static bool install_cursor_render_hook(void) {
  static const uint8_t signature[] = {
      0x8B, 0x0D, 0x6C, 0xBC, 0x9A, 0x00, 0x8B, 0x01, 0xFF, 0x60, 0x08,
  };
  uint8_t* site = g_image_base + CURSOR_RENDER_RVA;
  if (memcmp(site, signature, sizeof(signature)) != 0) return false;
  uint8_t patch[sizeof(signature)] = {0xE9};
  int32_t relative = (int32_t)((uint8_t*)cursor_render_hook - (site + 5));
  memcpy(patch + 1, &relative, sizeof(relative));
  memset(patch + 5, 0x90, sizeof(patch) - 5);
  return write_memory(site, patch, sizeof(patch));
}

__attribute__((naked, used)) static void outfit_distance_hook(void) {
  /* Replace only the width used for camera distance, not the native viewport. */
  __asm__ __volatile__(
      "fildl _g_original_ui_width\n\t"
      "pushl $0x43960000\n\t"
      "jmp *_g_outfit_distance_return\n\t");
}

static bool install_outfit_distance_hook(void) {
  static const uint8_t signature[] = {
      0xDB, 0x44, 0x24, 0x00, 0x68, 0x00, 0x00, 0x96, 0x43,
  };
  uint8_t* site = g_image_base + OUTFIT_CAMERA_DISTANCE_RVA;
  if (memcmp(site, signature, sizeof(signature)) != 0) return false;
  uint8_t patch[sizeof(signature)] = {0xE9};
  int32_t relative = (int32_t)((uint8_t*)outfit_distance_hook - (site + 5));
  memcpy(patch + 1, &relative, sizeof(relative));
  memset(patch + 5, 0x90, sizeof(patch) - 5);
  g_outfit_distance_return = site + sizeof(signature);
  return write_memory(site, patch, sizeof(patch));
}

static bool install_device_ready_hook(void) {
  uint8_t* call_site = g_image_base + DEVICE_READY_CALL_RVA;
  if (call_site[0] != 0xE8) return false;

  int32_t original_relative;
  memcpy(&original_relative, call_site + 1, sizeof(original_relative));
  uint8_t* original_target = call_site + 5 + original_relative;
  if (original_target != g_image_base + DEVICE_READY_TARGET_RVA) return false;

  g_original_device_ready = (device_ready_fn)original_target;
  uint8_t patch[5] = {0xE8};
  int32_t hook_relative =
      (int32_t)((uint8_t*)device_ready_hook - (call_site + 5));
  memcpy(patch + 1, &hook_relative, sizeof(hook_relative));
  return write_memory(call_site, patch, sizeof(patch));
}

static bool install_viewer_camera_hooks(void) {
  void** viewer_vtable =
      (void**)(g_image_base + k_menu_slots[VIEWER_MENU_INDEX].vtable_rva);
  g_original_viewer_update = (menu_update_fn)viewer_vtable[MENU_UPDATE_SLOT];
  g_original_viewer_render = (menu_render_fn)viewer_vtable[MENU_RENDER_SLOT];
  if ((uint8_t*)g_original_viewer_update != g_image_base + VIEWER_UPDATE_RVA) {
    return false;
  }

  void* update_hook = (void*)viewer_update_hook;
  void* render_hook = (void*)viewer_render_hook;
  if (!write_memory(&viewer_vtable[MENU_UPDATE_SLOT], &update_hook,
                    sizeof(update_hook))) {
    return false;
  }
  if (!write_memory(&viewer_vtable[MENU_RENDER_SLOT], &render_hook,
                    sizeof(render_hook))) {
    write_memory(&viewer_vtable[MENU_UPDATE_SLOT], &g_original_viewer_update,
                 sizeof(g_original_viewer_update));
    return false;
  }
  return true;
}

bool menu_fix_init(int width, int height) {
  LONG state = InterlockedCompareExchange(&g_init_state, 1, 0);
  if (state != 0) return state == 2;
  if (width <= ORIGINAL_WIDTH && height <= ORIGINAL_HEIGHT) {
    InterlockedExchange(&g_init_state, 2);
    return true;
  }

  uint8_t* base = (uint8_t*)GetModuleHandleA(NULL);
  if (base == NULL) {
    InterlockedExchange(&g_init_state, 3);
    return false;
  }
  g_image_base = base;

  uint8_t* update_site = base + MENU_UPDATE_RVA;
  uint8_t* render_site = base + MENU_RENDER_RVA;
  if (memcmp(render_site, k_menu_render_signature,
             sizeof(k_menu_render_signature)) != 0 ||
      memcmp(update_site, k_menu_update_signature,
             sizeof(k_menu_update_signature)) != 0) {
    shim_log("Menu fix: shared update/render signature did not match; patch skipped");
    InterlockedExchange(&g_init_state, 3);
    return false;
  }

  for (size_t i = 0; i < sizeof(k_menu_slots) / sizeof(k_menu_slots[0]); ++i) {
    void** vtable = (void**)(base + k_menu_slots[i].vtable_rva);
    if ((uint8_t*)vtable[MENU_RENDER_SLOT] !=
        base + k_menu_slots[i].expected_render_rva) {
      shim_log("Menu fix: render signature %u did not match; patch skipped",
               (unsigned)i);
      InterlockedExchange(&g_init_state, 3);
      return false;
    }
  }

  const size_t trampoline_size = sizeof(k_menu_render_signature) + 5;
  uint8_t* trampoline = (uint8_t*)VirtualAlloc(
      NULL, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (trampoline == NULL) {
    shim_log("Menu fix: failed to allocate shared render trampoline");
    InterlockedExchange(&g_init_state, 3);
    return false;
  }
  memcpy(trampoline, render_site, sizeof(k_menu_render_signature));
  trampoline[sizeof(k_menu_render_signature)] = 0xE9;
  int32_t trampoline_relative =
      (int32_t)((render_site + sizeof(k_menu_render_signature)) -
                (trampoline + trampoline_size));
  memcpy(trampoline + sizeof(k_menu_render_signature) + 1,
         &trampoline_relative, sizeof(trampoline_relative));
  FlushInstructionCache(GetCurrentProcess(), trampoline, trampoline_size);
  g_original_menu_render = (menu_render_fn)trampoline;

  const size_t update_trampoline_size = sizeof(k_menu_update_signature) + 5;
  uint8_t* update_trampoline = (uint8_t*)VirtualAlloc(
      NULL, update_trampoline_size, MEM_COMMIT | MEM_RESERVE,
      PAGE_EXECUTE_READWRITE);
  if (update_trampoline == NULL) {
    shim_log("Menu fix: failed to allocate shared update trampoline");
    InterlockedExchange(&g_init_state, 3);
    return false;
  }
  memcpy(update_trampoline, update_site, sizeof(k_menu_update_signature));
  update_trampoline[sizeof(k_menu_update_signature)] = 0xE9;
  int32_t update_trampoline_relative =
      (int32_t)((update_site + sizeof(k_menu_update_signature)) -
                (update_trampoline + update_trampoline_size));
  memcpy(update_trampoline + sizeof(k_menu_update_signature) + 1,
         &update_trampoline_relative, sizeof(update_trampoline_relative));
  FlushInstructionCache(GetCurrentProcess(), update_trampoline,
                        update_trampoline_size);
  g_original_menu_update = (menu_update_fn)update_trampoline;

  uint8_t render_patch[sizeof(k_menu_render_signature)] = {0xE9};
  int32_t render_relative =
      (int32_t)((uint8_t*)menu_render_hook - (render_site + 5));
  memcpy(render_patch + 1, &render_relative, sizeof(render_relative));
  render_patch[5] = 0x90;
  if (!write_memory(render_site, render_patch, sizeof(render_patch))) {
    shim_log("Menu fix: failed to patch shared renderer");
    InterlockedExchange(&g_init_state, 3);
    return false;
  }

  uint8_t update_patch[sizeof(k_menu_update_signature)] = {0xE9};
  int32_t update_relative =
      (int32_t)((uint8_t*)menu_update_hook - (update_site + 5));
  memcpy(update_patch + 1, &update_relative, sizeof(update_relative));
  update_patch[5] = 0x90;
  if (!write_memory(update_site, update_patch, sizeof(update_patch))) {
    write_memory(render_site, k_menu_render_signature,
                 sizeof(k_menu_render_signature));
    shim_log("Menu fix: failed to patch shared updater");
    InterlockedExchange(&g_init_state, 3);
    return false;
  }

  if (!install_cursor_getter_hook()) {
    write_memory(render_site, k_menu_render_signature,
                 sizeof(k_menu_render_signature));
    write_memory(update_site, k_menu_update_signature,
                 sizeof(k_menu_update_signature));
    shim_log("Menu fix: cursor getter signature did not match; patch skipped");
    InterlockedExchange(&g_init_state, 3);
    return false;
  }

  g_scale_x = (float)width / ORIGINAL_WIDTH;
  g_scale_y = (float)height / ORIGINAL_HEIGHT;
  g_output_width = width;
  g_output_height = height;
  if (!install_cursor_bounds_hook()) {
    shim_log("Menu fix: cursor bounds hook installation failed");
  }
  if (!install_cursor_render_hook()) {
    shim_log("Menu fix: cursor render signature did not match");
  }
  if (!install_outfit_distance_hook()) {
    shim_log("Menu fix: outfit camera distance signature did not match");
  }
  if (!install_device_ready_hook()) {
    shim_log("Menu fix: early Direct3D hook signature did not match");
  }
  if (!install_viewer_camera_hooks()) {
    shim_log("Menu fix: viewer camera hook installation failed");
  }
  shim_log("Menu fix: proportional UI and full-screen pointer bounds at %dx%d",
           width, height);
  InterlockedExchange(&g_init_state, 2);
  return true;
}

#ifdef MENU_FIX_TEST
LONG menu_fix_test_object_policy(const void* object, uintptr_t vtable_rva) {
  return menu_object_coordinate_policy(object, vtable_rva);
}

void menu_fix_test_render_cursor(void* cursor, void* device, LONG depth,
                                 uintptr_t caller_rva) {
  render_cursor_overlay(cursor, (IDirect3DDevice7*)device, depth, caller_rva);
}

bool menu_fix_test_scales_cursor(void) {
  return should_scale_primitive(D3DFVF_TLVERTEX, NULL, 0);
}

void menu_fix_test_cursor_bounds(int width, int height, int* bounds) {
  screen_cursor_bounds(width, height, bounds);
}

size_t menu_fix_test_vertex_stride(DWORD fvf) {
  return transformed_vertex_stride(fvf);
}

void menu_fix_test_scale(int width, int height, float x, float y,
                         float* scaled_x, float* scaled_y) {
  scale_coordinates(width, height, x, y, scaled_x, scaled_y);
}

void menu_fix_test_unscale(int width, int height, float x, float y,
                           float* logical_x, float* logical_y) {
  unscale_coordinates(width, height, x, y, logical_x, logical_y);
}

void menu_fix_test_scale_centered(int width, int height, float x, float y,
                                  float* scaled_x, float* scaled_y) {
  scale_centered_coordinates(width, height, x, y, scaled_x, scaled_y);
}

void menu_fix_test_unscale_centered(int width, int height, float x, float y,
                                    float* logical_x, float* logical_y) {
  unscale_centered_coordinates(width, height, x, y, logical_x, logical_y);
}

void menu_fix_test_scale_anchored(int width, int height, float anchor_x,
                                  float anchor_y, float x, float y,
                                  float* scaled_x, float* scaled_y) {
  scale_anchored_coordinates(width, height, anchor_x, anchor_y, x, y,
                             scaled_x, scaled_y);
}

void menu_fix_test_unscale_anchored(int width, int height, float anchor_x,
                                    float anchor_y, float x, float y,
                                    float* logical_x, float* logical_y) {
  unscale_anchored_coordinates(width, height, anchor_x, anchor_y, x, y,
                               logical_x, logical_y);
}

void menu_fix_test_scale_auto(int width, int height, float min_x, float min_y,
                              float max_x, float max_y, float x, float y,
                              float* scaled_x, float* scaled_y) {
  float anchor_x;
  float anchor_y;
  choose_auto_coordinate_anchor(width, height, min_x, min_y, max_x, max_y,
                                &anchor_x, &anchor_y);
  scale_anchored_coordinates(width, height, anchor_x, anchor_y, x, y,
                             scaled_x, scaled_y);
}

bool menu_fix_test_vtable(uintptr_t vtable_rva) {
  return menu_index_for_vtable(vtable_rva) != SIZE_MAX;
}

bool menu_fix_test_uses_native_cursor(uintptr_t vtable_rva) {
  return menu_uses_native_coordinates(menu_index_for_vtable(vtable_rva));
}

bool menu_fix_test_scales_vtable(uintptr_t vtable_rva) {
  size_t menu_index = menu_index_for_vtable(vtable_rva);
  return menu_coordinate_policy(menu_index, vtable_rva) !=
         COORDINATE_POLICY_NONE;
}

bool menu_fix_test_clears_background(uintptr_t vtable_rva) {
  return menu_clears_background(menu_index_for_vtable(vtable_rva));
}

bool menu_fix_test_should_clear_background(uintptr_t vtable_rva, LONG depth) {
  return should_clear_menu_background(menu_index_for_vtable(vtable_rva),
                                      depth);
}

bool menu_fix_test_cursor_texture_needs_sync(const DDSURFACEDESC2* desc) {
  return cursor_texture_needs_sync(desc);
}

float menu_fix_test_zoomed_fov(float fov, float factor) {
  return zoomed_fov(fov, factor);
}
#endif

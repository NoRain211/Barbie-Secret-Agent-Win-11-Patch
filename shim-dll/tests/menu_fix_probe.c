#include <d3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "menu_fix.h"

static int expect_close(float actual, float expected, const char* label) {
  if (fabsf(actual - expected) < 0.01f) return 0;
  fprintf(stderr, "%s %.3f; want %.3f\n", label, actual, expected);
  return 1;
}

static DWORD cursor_z_enabled = TRUE;
static DWORD cursor_z_write = TRUE;
static int cursor_draws;
static int cursor_failed;

static HRESULT WINAPI cursor_get_state(IDirect3DDevice7* device,
                                       D3DRENDERSTATETYPE state, DWORD* value) {
  (void)device;
  *value = state == D3DRENDERSTATE_ZENABLE ? cursor_z_enabled : cursor_z_write;
  return S_OK;
}

static HRESULT WINAPI cursor_set_state(IDirect3DDevice7* device,
                                       D3DRENDERSTATETYPE state, DWORD value) {
  (void)device;
  if (state == D3DRENDERSTATE_ZENABLE) cursor_z_enabled = value;
  else cursor_z_write = value;
  return S_OK;
}

static void __attribute__((thiscall)) cursor_draw(void* self) {
  (void)self;
  ++cursor_draws;
  if (cursor_z_enabled || cursor_z_write || menu_fix_test_scales_cursor()) {
    cursor_failed = 1;
  }
}

int main(void) {
  struct {
    uint8_t padding[0x40];
    const char* name;
    uint32_t length;
  } question = {{0}, "Question", 8};
  /* guiQuesExit.def is a generic centered container, not a saMenu subclass. */
  if (menu_fix_test_object_policy(&question, 0x0014BE8C) != 2) {
    fprintf(stderr, "exit dialog must use centered render AND input coordinates\n");
    return 1;
  }
  question.name = "Camera";
  question.length = 6;
  if (menu_fix_test_object_policy(&question, 0x0014BE8C) != 5) {
    fprintf(stderr, "unrelated generic containers must keep their policy\n");
    return 1;
  }
  IDirect3DDevice7Vtbl device_methods = {0};
  device_methods.GetRenderState = cursor_get_state;
  device_methods.SetRenderState = cursor_set_state;
  IDirect3DDevice7 device = {&device_methods};
  void* cursor_methods[] = {NULL, NULL, (void*)cursor_draw};
  void** cursor = cursor_methods;
  menu_fix_test_render_cursor(&cursor, &device, 1, 0x0000D366);
  menu_fix_test_render_cursor(&cursor, &device, 0, 0x0000D366);
  menu_fix_test_render_cursor(&cursor, &device, 0, 0x00028112);
  if (cursor_draws != 1 || cursor_failed ||
      cursor_z_enabled != TRUE || cursor_z_write != TRUE) {
    fprintf(stderr, "cursor must draw once, unscaled, above UI, restoring depth\n");
    return 1;
  }
  const int resolutions[][2] = {{640, 480}, {1920, 1080}, {2560, 1440}};
  for (size_t i = 0; i < sizeof(resolutions) / sizeof(resolutions[0]); ++i) {
    float center_x = resolutions[i][0] * 0.5f;
    float center_y = resolutions[i][1] * 0.5f;
    /* Centers of the actual 64x64 Back/Ok buttons in guiQuesExit.def. */
    const float button_x[] = {283.0f + 32.0f, 335.0f + 32.0f};
    for (size_t button = 0; button < 2; ++button) {
      float raw_x = center_x - 256.0f + button_x[button];
      float raw_y = center_y - 128.0f + 162.0f + 32.0f;
      float x, y;
      menu_fix_test_scale_anchored(resolutions[i][0], resolutions[i][1],
                                   center_x, center_y, raw_x, raw_y, &x, &y);
      menu_fix_test_unscale_anchored(resolutions[i][0], resolutions[i][1],
                                     center_x, center_y, x, y, &x, &y);
      if (expect_close(x, raw_x, "exit button click x") ||
          expect_close(y, raw_y, "exit button click y")) return 1;
    }
    int bounds[] = {512, 512, 1024, 464};
    menu_fix_test_cursor_bounds(resolutions[i][0], resolutions[i][1], bounds);
    if (bounds[0] != resolutions[i][0] || bounds[1] != resolutions[i][1] ||
        bounds[2] != 0 || bounds[3] != 0) {
      fprintf(stderr, "cursor is confined to the unscaled menu rectangle\n");
      return 1;
    }
  }
  float x;
  float y;
  menu_fix_test_scale(2560, 1440, 0.0f, 0.0f, &x, &y);
  if (expect_close(x, 0.0f, "left") || expect_close(y, 0.0f, "top")) {
    return 1;
  }
  menu_fix_test_scale(1920, 1080, 640.0f, 480.0f, &x, &y);
  if (expect_close(x, 1920.0f, "right") ||
      expect_close(y, 1080.0f, "bottom")) {
    return 1;
  }
  menu_fix_test_unscale(2560, 1440, 0.0f, 0.0f, &x, &y);
  if (expect_close(x, 0.0f, "logical left") ||
      expect_close(y, 0.0f, "logical top")) {
    return 1;
  }
  menu_fix_test_unscale(2560, 1440, 2560.0f, 1440.0f, &x, &y);
  if (expect_close(x, 640.0f, "logical right") ||
      expect_close(y, 480.0f, "logical bottom")) {
    return 1;
  }

  menu_fix_test_scale_centered(2560, 1440, 0.0f, 0.0f, &x, &y);
  if (expect_close(x, 320.0f, "centered left") ||
      expect_close(y, 0.0f, "centered top")) {
    return 1;
  }
  menu_fix_test_scale_centered(2560, 1440, 640.0f, 480.0f, &x, &y);
  if (expect_close(x, 2240.0f, "centered right") ||
      expect_close(y, 1440.0f, "centered bottom")) {
    return 1;
  }
  menu_fix_test_unscale_centered(2560, 1440, 320.0f, 0.0f, &x, &y);
  if (expect_close(x, 0.0f, "centered logical left") ||
      expect_close(y, 0.0f, "centered logical top")) {
    return 1;
  }
  menu_fix_test_unscale_centered(2560, 1440, 2240.0f, 1440.0f, &x, &y);
  if (expect_close(x, 640.0f, "centered logical right") ||
      expect_close(y, 480.0f, "centered logical bottom")) {
    return 1;
  }

  menu_fix_test_scale_anchored(2560, 1440, 1280.0f, 720.0f,
                               1024.0f, 464.0f, &x, &y);
  if (expect_close(x, 512.0f, "native centered left") ||
      expect_close(y, -48.0f, "native centered top")) {
    return 1;
  }
  menu_fix_test_unscale_anchored(2560, 1440, 1280.0f, 720.0f,
                                 512.0f, -48.0f, &x, &y);
  if (expect_close(x, 1024.0f, "native logical left") ||
      expect_close(y, 464.0f, "native logical top")) {
    return 1;
  }
  menu_fix_test_scale_anchored(2560, 1440, 0.0f, 1440.0f,
                               64.0f, 1376.0f, &x, &y);
  if (expect_close(x, 192.0f, "left anchored") ||
      expect_close(y, 1248.0f, "bottom anchored")) {
    return 1;
  }
  menu_fix_test_scale_anchored(2560, 1440, 2560.0f, 0.0f,
                               2496.0f, 64.0f, &x, &y);
  if (expect_close(x, 2368.0f, "right anchored") ||
      expect_close(y, 192.0f, "top anchored")) {
    return 1;
  }
  menu_fix_test_scale_auto(2560, 1440, 960.0f, 480.0f, 1216.0f, 736.0f,
                           960.0f, 480.0f, &x, &y);
  if (expect_close(x, 320.0f, "auto centered left") ||
      expect_close(y, 0.0f, "auto centered top")) {
    return 1;
  }

  if (menu_fix_test_vertex_stride(D3DFVF_TLVERTEX) != 32 ||
      menu_fix_test_vertex_stride(D3DFVF_VERTEX) != 0) {
    fprintf(stderr, "transformed vertex stride classification failed\n");
    return 1;
  }

  if (!menu_fix_test_vtable(0x001454DC) ||
      !menu_fix_test_vtable(0x001456A4) ||
      !menu_fix_test_vtable(0x00145294) ||
      !menu_fix_test_vtable(0x00145BB4) ||
      !menu_fix_test_vtable(0x00145B24) ||
      !menu_fix_test_vtable(0x00145C3C) ||
      !menu_fix_test_vtable(0x00145CC4) ||
      !menu_fix_test_vtable(0x00145DCC) ||
      menu_fix_test_vtable(0x00145C14)) {
    fprintf(stderr, "menu vtable classification failed\n");
    return 1;
  }

  if (!menu_fix_test_uses_native_cursor(0x0014543C) ||
      !menu_fix_test_uses_native_cursor(0x00145584) ||
      !menu_fix_test_uses_native_cursor(0x0014560C) ||
      !menu_fix_test_uses_native_cursor(0x00145BB4) ||
      !menu_fix_test_uses_native_cursor(0x00145B24) ||
      !menu_fix_test_uses_native_cursor(0x00145C3C) ||
      !menu_fix_test_uses_native_cursor(0x00145CC4) ||
      !menu_fix_test_uses_native_cursor(0x00145DCC) ||
      menu_fix_test_uses_native_cursor(0x001454DC) ||
      menu_fix_test_uses_native_cursor(0x001456A4)) {
    fprintf(stderr, "menu cursor coordinate classification failed\n");
    return 1;
  }

  if (!menu_fix_test_scales_vtable(0x0014BE8C) ||
      !menu_fix_test_scales_vtable(0x00145BB4) ||
      menu_fix_test_scales_vtable(0x00145C14)) {
    fprintf(stderr, "menu render policy classification failed\n");
    return 1;
  }

  if (!menu_fix_test_clears_background(0x001454DC) ||
      !menu_fix_test_clears_background(0x001456A4) ||
      menu_fix_test_clears_background(0x0014543C) ||
      menu_fix_test_clears_background(0x00145584)) {
    fprintf(stderr, "menu background-clear classification failed\n");
    return 1;
  }

  if (!menu_fix_test_should_clear_background(0x001454DC, 2) ||
      !menu_fix_test_should_clear_background(0x001456A4, 2) ||
      menu_fix_test_should_clear_background(0x0014BE8C, 1)) {
    fprintf(stderr, "nested menu background-clear policy failed\n");
    return 1;
  }

  DDSURFACEDESC2 cursor_desc;
  memset(&cursor_desc, 0, sizeof(cursor_desc));
  cursor_desc.dwSize = sizeof(cursor_desc);
  cursor_desc.dwWidth = 32;
  cursor_desc.dwHeight = 32;
  cursor_desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY;
  cursor_desc.ddpfPixelFormat.dwSize = sizeof(cursor_desc.ddpfPixelFormat);
  cursor_desc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
  cursor_desc.ddpfPixelFormat.dwRGBBitCount = 16;
  cursor_desc.ddpfPixelFormat.dwRBitMask = 0x00000f00;
  cursor_desc.ddpfPixelFormat.dwGBitMask = 0x000000f0;
  cursor_desc.ddpfPixelFormat.dwBBitMask = 0x0000000f;
  cursor_desc.ddpfPixelFormat.dwRGBAlphaBitMask = 0x0000f000;
  if (!menu_fix_test_cursor_texture_needs_sync(&cursor_desc)) {
    fprintf(stderr, "cursor texture sync classification failed\n");
    return 1;
  }
  cursor_desc.dwWidth = 64;
  if (menu_fix_test_cursor_texture_needs_sync(&cursor_desc)) {
    fprintf(stderr, "non-cursor texture was classified for sync\n");
    return 1;
  }

  if (expect_close(menu_fix_test_zoomed_fov(35.0f, 3.0f), 11.9995f,
                   "viewer fov")) {
    return 1;
  }

  puts("menu fix tests passed");
  return 0;
}

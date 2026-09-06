#ifndef MENU_FIX_H_
#define MENU_FIX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>
#include <ddraw.h>

bool menu_fix_init(int width, int height);

#ifdef MENU_FIX_TEST
LONG menu_fix_test_object_policy(const void* object, uintptr_t vtable_rva);
void menu_fix_test_render_cursor(void* cursor, void* device, LONG depth,
                                 uintptr_t caller_rva);
bool menu_fix_test_scales_cursor(void);
void menu_fix_test_cursor_bounds(int width, int height, int* bounds);
size_t menu_fix_test_vertex_stride(DWORD fvf);
void menu_fix_test_scale(int width, int height, float x, float y,
                         float* scaled_x, float* scaled_y);
void menu_fix_test_unscale(int width, int height, float x, float y,
                           float* logical_x, float* logical_y);
void menu_fix_test_scale_centered(int width, int height, float x, float y,
                                  float* scaled_x, float* scaled_y);
void menu_fix_test_unscale_centered(int width, int height, float x, float y,
                                    float* logical_x, float* logical_y);
void menu_fix_test_scale_anchored(int width, int height, float anchor_x,
                                  float anchor_y, float x, float y,
                                  float* scaled_x, float* scaled_y);
void menu_fix_test_unscale_anchored(int width, int height, float anchor_x,
                                    float anchor_y, float x, float y,
                                    float* logical_x, float* logical_y);
void menu_fix_test_scale_auto(int width, int height, float min_x, float min_y,
                              float max_x, float max_y, float x, float y,
                              float* scaled_x, float* scaled_y);
bool menu_fix_test_vtable(uintptr_t vtable_rva);
bool menu_fix_test_uses_native_cursor(uintptr_t vtable_rva);
bool menu_fix_test_scales_vtable(uintptr_t vtable_rva);
bool menu_fix_test_clears_background(uintptr_t vtable_rva);
bool menu_fix_test_should_clear_background(uintptr_t vtable_rva, LONG depth);
bool menu_fix_test_cursor_texture_needs_sync(const DDSURFACEDESC2* desc);
float menu_fix_test_zoomed_fov(float fov, float factor);
#endif

#endif /* MENU_FIX_H_ */

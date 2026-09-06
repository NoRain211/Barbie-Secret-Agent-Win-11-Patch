#ifndef WIDESCREEN_FIX_H_
#define WIDESCREEN_FIX_H_

#include <stdbool.h>
#include <stddef.h>

bool widescreen_fix_init(void);
bool widescreen_fix_get_resolution(int* width, int* height);

#ifdef WIDESCREEN_TEST
bool widescreen_fix_test_skip_intro(void* image, size_t image_size, bool enabled);
bool widescreen_fix_verify_image(const void* image, size_t image_size);
bool widescreen_fix_test_enabled(const char* path);
bool widescreen_fix_test_apply_resolution(void* image, size_t image_size,
                                          int width, int height);
#endif

#endif /* WIDESCREEN_FIX_H_ */

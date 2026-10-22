#include <stdbool.h>
#include <windows.h>
#include <ddraw.h>

#include "menu_fix.h"
#include "shim_log.h"
#include "shim_util.h"
#include "widescreen_fix.h"

#define DDRAW7_SET_COOP_LEVEL_SLOT 20
#define DDRAW7_SET_DISPLAY_MODE_SLOT 21

typedef HRESULT (WINAPI* PFN_DirectDrawCreateEx)(LPGUID, LPVOID*, REFIID, IUnknown*);
typedef HRESULT (WINAPI* PFN_SetCooperativeLevel)(IDirectDraw7*, HWND, DWORD);
typedef HRESULT (WINAPI* PFN_SetDisplayMode)(IDirectDraw7*, DWORD, DWORD, DWORD, DWORD,
                                             DWORD);

/* Backend state owned by main.c. */
extern FARPROC g_fn_DirectDrawCreateEx;
extern bool g_using_dgvoodoo;
bool ensure_exports_loaded(void);

static HWND g_game_hwnd = NULL;
static PFN_SetCooperativeLevel g_original_set_cooperative_level = NULL;
static PFN_SetDisplayMode g_original_set_display_mode = NULL;

static HRESULT WINAPI hooked_SetCooperativeLevel(IDirectDraw7* self, HWND hwnd,
                                                 DWORD flags) {
  if (hwnd != NULL) g_game_hwnd = hwnd;
  HRESULT result = g_original_set_cooperative_level(self, hwnd, flags);
  shim_log("SetCooperativeLevel: flags=0x%08lx result=0x%08lx", flags,
           (unsigned long)result);
  return result;
}

/* dgVoodoo2 scales the original 640x480 mode itself; a 2x window keeps it readable. */
static void enlarge_original_window(void) {
  RECT rc = {0, 0, 1280, 960};
  DWORD style = (DWORD)GetWindowLongA(g_game_hwnd, GWL_STYLE);
  DWORD exstyle = (DWORD)GetWindowLongA(g_game_hwnd, GWL_EXSTYLE);
  AdjustWindowRectEx(&rc, style, FALSE, exstyle);
  int w = rc.right - rc.left;
  int h = rc.bottom - rc.top;
  MONITORINFO monitor = {.cbSize = sizeof(monitor)};
  const RECT* work = &monitor.rcWork;
  if (GetMonitorInfoA(MonitorFromWindow(g_game_hwnd, MONITOR_DEFAULTTONEAREST), &monitor) &&
      w <= work->right - work->left && h <= work->bottom - work->top) {
    SetWindowPos(g_game_hwnd, NULL, work->left + (work->right - work->left - w) / 2,
                 work->top + (work->bottom - work->top - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    shim_log("SetDisplayMode: resized window to 1280x960 centered");
  } else {
    shim_log("SetDisplayMode: 1280x960 window does not fit the monitor; size kept");
  }
}

static HRESULT WINAPI hooked_SetDisplayMode(IDirectDraw7* self, DWORD width, DWORD height,
                                            DWORD bpp, DWORD refresh, DWORD flags) {
  HRESULT result = g_original_set_display_mode(self, width, height, bpp, refresh, flags);
  shim_log("SetDisplayMode: %lux%lu x%lubpp refresh=%lu flags=0x%lx result=0x%08lx",
           width, height, bpp, refresh, flags, (unsigned long)result);
  if (SUCCEEDED(result) && g_game_hwnd != NULL && width == 640 && height == 480 &&
      bpp == 16) {
    enlarge_original_window();
  }
  return result;
}

/* dgVoodoo2 shares one IDirectDraw7 vtable, so it is patched once in place. */
static bool hook_ddraw7(IDirectDraw7* ddraw) {
  void** vtable = *(void***)ddraw;
  void* coop_hook = (void*)hooked_SetCooperativeLevel;
  void* mode_hook = (void*)hooked_SetDisplayMode;
  if (vtable[DDRAW7_SET_COOP_LEVEL_SLOT] == coop_hook &&
      vtable[DDRAW7_SET_DISPLAY_MODE_SLOT] == mode_hook) {
    return true;
  }
  g_original_set_cooperative_level =
      (PFN_SetCooperativeLevel)vtable[DDRAW7_SET_COOP_LEVEL_SLOT];
  g_original_set_display_mode = (PFN_SetDisplayMode)vtable[DDRAW7_SET_DISPLAY_MODE_SLOT];
  return write_memory(&vtable[DDRAW7_SET_COOP_LEVEL_SLOT], &coop_hook, sizeof(coop_hook)) &&
         write_memory(&vtable[DDRAW7_SET_DISPLAY_MODE_SLOT], &mode_hook, sizeof(mode_hook));
}

__declspec(dllexport) HRESULT WINAPI DirectDrawCreateEx(LPGUID lpGUID, LPVOID* lplpDD,
                                                       REFIID riid, IUnknown* pUnkOuter) {
  shim_log("DirectDrawCreateEx: called");
  if (widescreen_fix_init()) {
    int width;
    int height;
    if (widescreen_fix_get_resolution(&width, &height)) {
      menu_fix_init(width, height);
    }
  }
  if (!ensure_exports_loaded()) {
    shim_log("DirectDrawCreateEx: FAILED to load a DirectDraw backend");
    return E_FAIL;
  }

  HRESULT result =
      ((PFN_DirectDrawCreateEx)g_fn_DirectDrawCreateEx)(lpGUID, lplpDD, riid, pUnkOuter);
  shim_log("DirectDrawCreateEx: real result=0x%08lx", (unsigned long)result);
  if (SUCCEEDED(result) && g_using_dgvoodoo && lplpDD != NULL && *lplpDD != NULL &&
      IsEqualIID(riid, &IID_IDirectDraw7) && !hook_ddraw7((IDirectDraw7*)*lplpDD)) {
    shim_log("DirectDrawCreateEx: IDirectDraw7 hook installation failed");
  }
  return result;
}
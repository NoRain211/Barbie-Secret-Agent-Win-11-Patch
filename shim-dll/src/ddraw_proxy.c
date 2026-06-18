#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <ddraw.h>

#include "vtable_offsets.h"
#include "shim_log.h"

typedef HRESULT (WINAPI *PFN_DirectDrawCreateEx)(LPGUID, LPVOID*, REFIID, IUnknown*);
typedef void (WINAPI* PFN_SetAppCompatData)(DWORD, DWORD);

#define DDRAW7_VTBL_METHODS 30
#define DDRAW7_QI_INDEX 0
#define DDRAW7_ADDREF_INDEX 1
#define DDRAW7_RELEASE_INDEX 2
#define DDRAW7_COMPACT_INDEX 3
#define DDRAW7_CREATE_CLIPPER_INDEX 4
#define DDRAW7_CREATE_PALETTE_INDEX 5
#define DDRAW7_CREATE_SURFACE_INDEX (DDRAW7_VTBL_CREATE_SURFACE / sizeof(void*))
#define DDRAW7_DUPLICATE_SURFACE_INDEX 7
#define DDRAW7_ENUM_DISPLAY_MODES_INDEX 8
#define DDRAW7_ENUM_SURFACES_INDEX 9
#define DDRAW7_FLIP_TO_GDI_INDEX 10
#define DDRAW7_GET_CAPS_INDEX 11
#define DDRAW7_GET_DISPLAY_MODE_INDEX 12
#define DDRAW7_GET_FOURCC_CODES_INDEX 13
#define DDRAW7_GET_GDI_SURFACE_INDEX 14
#define DDRAW7_GET_MONITOR_FREQUENCY_INDEX 15
#define DDRAW7_GET_SCANLINE_INDEX 16
#define DDRAW7_GET_VERTICAL_BLANK_STATUS_INDEX 17
#define DDRAW7_INITIALIZE_INDEX 18
#define DDRAW7_RESTORE_DISPLAY_MODE_INDEX 19
#define DDRAW7_SET_COOP_LEVEL_INDEX (DDRAW7_VTBL_SET_COOP_LEVEL / sizeof(void*))
#define DDRAW7_SET_DISPLAY_MODE_INDEX (DDRAW7_VTBL_SET_DISPLAY_MODE / sizeof(void*))
#define DDRAW7_WAIT_FOR_VERTICAL_BLANK_INDEX 22
#define DDRAW7_GET_AVAILABLE_VIDMEM_INDEX 23
#define DDRAW7_GET_SURFACE_FROM_DC_INDEX 24
#define DDRAW7_RESTORE_ALL_SURFACES_INDEX 25
#define DDRAW7_TEST_COOP_LEVEL_INDEX 26
#define DDRAW7_GET_DEVICE_IDENTIFIER_INDEX 27
#define DDRAW7_START_MODE_TEST_INDEX 28
#define DDRAW7_EVALUATE_MODE_INDEX 29

typedef struct ProxyIDirectDraw7 {
  void** lpVtbl;
  IDirectDraw7* real_object;
  void** real_vtable;
  void** proxy_vtable;
  bool coop_downgraded;
  HWND game_hwnd;
} ProxyIDirectDraw7;

static HMODULE g_real_ddraw_module = NULL;
static PFN_DirectDrawCreateEx g_fn_DirectDrawCreateEx = NULL;
static PFN_SetAppCompatData g_fn_SetAppCompatData = NULL;
static bool g_using_dgvoodoo_chain = false;

static bool load_real_ddraw_module(void);
static void* create_proxy_ddraw7(IDirectDraw7* real_object);
static bool try_adjust_surface_desc(const LPDDSURFACEDESC2 surface_desc,
                                   DDSURFACEDESC2* adjusted_desc);
static bool is_target_ddraw_iid(REFIID riid);
static PFN_SetAppCompatData resolve_SetAppCompatData(HMODULE module);

static HRESULT WINAPI proxy_QueryInterface(ProxyIDirectDraw7* this_ptr, REFIID riid,
                                          LPVOID* ppvObj);
static ULONG WINAPI proxy_AddRef(ProxyIDirectDraw7* this_ptr);
static ULONG WINAPI proxy_Release(ProxyIDirectDraw7* this_ptr);
static HRESULT WINAPI proxy_Compact(ProxyIDirectDraw7* this_ptr);
static HRESULT WINAPI proxy_CreateClipper(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                         LPDIRECTDRAWCLIPPER* lplpDDClipper,
                                         IUnknown* pUnkOuter);
static HRESULT WINAPI proxy_CreatePalette(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                         LPPALETTEENTRY lpDDColorTable,
                                         LPDIRECTDRAWPALETTE* lpColorTable,
                                         IUnknown* pUnkOuter);
static HRESULT WINAPI proxy_CreateSurface(ProxyIDirectDraw7* this_ptr,
                                        LPDDSURFACEDESC2 lpDDSurfaceDesc,
                                        LPDIRECTDRAWSURFACE7* lplpDDSurface,
                                        IUnknown* pUnkOuter);
static HRESULT WINAPI proxy_DuplicateSurface(ProxyIDirectDraw7* this_ptr,
                                            LPDIRECTDRAWSURFACE7 lpDDSurface,
                                            LPDIRECTDRAWSURFACE7* lplpDupDDSurface);
static HRESULT WINAPI proxy_EnumDisplayModes(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                            LPDDSURFACEDESC2 lpDDSurfaceDesc,
                                            LPVOID lpContext,
                                            LPDDENUMMODESCALLBACK2 lpEnumModesCallback);
static HRESULT WINAPI proxy_EnumSurfaces(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                        LPDDSURFACEDESC2 lpDDSurfaceDesc, LPVOID lpContext,
                                        LPDDENUMSURFACESCALLBACK7 lpEnumSurfacesCallback);
static HRESULT WINAPI proxy_FlipToGDISurface(ProxyIDirectDraw7* this_ptr);
static HRESULT WINAPI proxy_GetCaps(ProxyIDirectDraw7* this_ptr, LPDDCAPS lpDDSCaps,
                                   LPDDCAPS lpDDSCaps2);
static HRESULT WINAPI proxy_GetDisplayMode(ProxyIDirectDraw7* this_ptr,
                                          LPDDSURFACEDESC2 lpDDSurfaceDesc);
static HRESULT WINAPI proxy_GetFourCCCodes(ProxyIDirectDraw7* this_ptr, LPDWORD lpNumCodes,
                                          LPDWORD lpPixelFormat);
static HRESULT WINAPI proxy_GetGDISurface(ProxyIDirectDraw7* this_ptr,
                                         LPDIRECTDRAWSURFACE7* lplpGDISurface);
static HRESULT WINAPI proxy_GetMonitorFrequency(ProxyIDirectDraw7* this_ptr, LPDWORD lpdwFrequency);
static HRESULT WINAPI proxy_GetScanLine(ProxyIDirectDraw7* this_ptr, LPDWORD lpdwScanLine);
static HRESULT WINAPI proxy_GetVerticalBlankStatus(ProxyIDirectDraw7* this_ptr, LPBOOL lpbsInVBlank);
static HRESULT WINAPI proxy_Initialize(ProxyIDirectDraw7* this_ptr, LPGUID lpGUID);
static HRESULT WINAPI proxy_RestoreDisplayMode(ProxyIDirectDraw7* this_ptr);
static HRESULT WINAPI proxy_SetCooperativeLevel(ProxyIDirectDraw7* this_ptr,
                                               HWND hWnd, DWORD dwFlags);
static HRESULT WINAPI proxy_SetDisplayMode(ProxyIDirectDraw7* this_ptr, DWORD dwWidth,
                                          DWORD dwHeight, DWORD dwBPP, DWORD dwRefreshRate,
                                          DWORD dwFlags);
static HRESULT WINAPI proxy_WaitForVerticalBlank(ProxyIDirectDraw7* this_ptr,
                                                DWORD dwFlags, HANDLE hEvent);
static HRESULT WINAPI proxy_GetAvailableVidMem(ProxyIDirectDraw7* this_ptr, LPDDSCAPS2 lpDDSCaps,
                                              LPDWORD lpdwTotal, LPDWORD lpdwFree);
static HRESULT WINAPI proxy_GetSurfaceFromDC(ProxyIDirectDraw7* this_ptr, HDC hdc,
                                            LPDIRECTDRAWSURFACE7* lpDDSurface);
static HRESULT WINAPI proxy_RestoreAllSurfaces(ProxyIDirectDraw7* this_ptr);
static HRESULT WINAPI proxy_TestCooperativeLevel(ProxyIDirectDraw7* this_ptr);
static HRESULT WINAPI proxy_GetDeviceIdentifier(ProxyIDirectDraw7* this_ptr,
                                               LPDDDEVICEIDENTIFIER2 lpdddi, DWORD dwFlags);
static HRESULT WINAPI proxy_StartModeTest(ProxyIDirectDraw7* this_ptr, LPSIZE lpModes,
                                         DWORD dwNumEntries, DWORD dwFlags);
static HRESULT WINAPI proxy_EvaluateMode(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                        DWORD* lpdwTimeout);

static bool load_real_ddraw_module(void) {
  if (g_real_ddraw_module != NULL) {
    return true;
  }

  WCHAR dll_path[MAX_PATH];
  WCHAR exe_path[MAX_PATH];
  WCHAR* separator = NULL;

  if (GetModuleFileNameW(NULL, exe_path, MAX_PATH) > 0) {
    separator = wcsrchr(exe_path, L'\\');
    if (separator != NULL) {
      *(separator + 1) = 0;
      if (wcscpy_s(dll_path, MAX_PATH, exe_path) == 0 &&
          wcscat_s(dll_path, MAX_PATH, L"dgVoodoo_ddraw.dll") == 0) {
        g_real_ddraw_module = LoadLibraryW(dll_path);
        if (g_real_ddraw_module != NULL) {
          g_fn_DirectDrawCreateEx = (PFN_DirectDrawCreateEx)GetProcAddress(g_real_ddraw_module,
                                                                         "DirectDrawCreateEx");
          if (g_fn_DirectDrawCreateEx != NULL) {
            g_using_dgvoodoo_chain = true;
            if (g_fn_SetAppCompatData == NULL) {
              g_fn_SetAppCompatData = resolve_SetAppCompatData(g_real_ddraw_module);
            }
            return true;
          }
          FreeLibrary(g_real_ddraw_module);
          g_real_ddraw_module = NULL;
          g_using_dgvoodoo_chain = false;
        }
      }
    }
  }

  UINT path_len = GetSystemWindowsDirectoryW(dll_path, MAX_PATH);
  if (path_len == 0 || path_len + 20 >= MAX_PATH) {
    return false;
  }

  if (wcscat_s(dll_path, MAX_PATH, L"\\SysWOW64\\ddraw.dll") != 0) {
    return false;
  }

  g_real_ddraw_module = LoadLibraryW(dll_path);
  if (g_real_ddraw_module == NULL) {
    g_real_ddraw_module = LoadLibraryW(L"C:\\Windows\\System32\\ddraw.dll");
    if (g_real_ddraw_module == NULL) {
      return false;
    }
  }
  g_using_dgvoodoo_chain = false;

  g_fn_DirectDrawCreateEx = (PFN_DirectDrawCreateEx)GetProcAddress(g_real_ddraw_module,
                                                                 "DirectDrawCreateEx");
  if (g_fn_SetAppCompatData == NULL) {
    g_fn_SetAppCompatData = resolve_SetAppCompatData(g_real_ddraw_module);
  }
  return g_fn_DirectDrawCreateEx != NULL;
}

static PFN_SetAppCompatData resolve_SetAppCompatData(HMODULE module) {
  union {
    FARPROC raw;
    PFN_SetAppCompatData typed;
  } proc;

  proc.raw = GetProcAddress(module, "SetAppCompatData");
  return proc.typed;
}

static bool is_target_ddraw_iid(REFIID riid) {
  return IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectDraw) ||
         IsEqualIID(riid, &IID_IDirectDraw2) || IsEqualIID(riid, &IID_IDirectDraw4) ||
         IsEqualIID(riid, &IID_IDirectDraw7);
}

static bool try_adjust_surface_desc(const LPDDSURFACEDESC2 surface_desc,
                                   DDSURFACEDESC2* adjusted_desc) {
  if (g_using_dgvoodoo_chain) {
    return false;
  }

  if (surface_desc == NULL || adjusted_desc == NULL || surface_desc->dwSize != sizeof(DDSURFACEDESC2)) {
    return false;
  }

  *adjusted_desc = *surface_desc;
  bool changed = false;

  if ((adjusted_desc->dwFlags & DDSD_CAPS) == 0) {
    return false;
  }

  if ((adjusted_desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0 &&
      (adjusted_desc->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) != 0) {
    adjusted_desc->ddsCaps.dwCaps &= ~DDSCAPS_SYSTEMMEMORY;
    changed = true;
  }

  if ((adjusted_desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) != 0 &&
      (adjusted_desc->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) == 0) {
    adjusted_desc->ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
    adjusted_desc->ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
    changed = true;
  }

  return changed;
}

static void* create_proxy_ddraw7(IDirectDraw7* real_object) {
  if (real_object == NULL) {
    return NULL;
  }

  void** real_vtable = *(void***)real_object;
  void** proxy_vtable = (void**)calloc(DDRAW7_VTBL_METHODS, sizeof(void*));
  if (proxy_vtable == NULL) {
    return NULL;
  }

  ProxyIDirectDraw7* proxy = (ProxyIDirectDraw7*)calloc(1, sizeof(ProxyIDirectDraw7));
  if (proxy == NULL) {
    free(proxy_vtable);
    return NULL;
  }

  proxy->real_object = real_object;
  proxy->real_vtable = real_vtable;
  proxy->proxy_vtable = proxy_vtable;
  proxy->lpVtbl = proxy_vtable;

  proxy_vtable[DDRAW7_QI_INDEX] = (void*)proxy_QueryInterface;
  proxy_vtable[DDRAW7_ADDREF_INDEX] = (void*)proxy_AddRef;
  proxy_vtable[DDRAW7_RELEASE_INDEX] = (void*)proxy_Release;
  proxy_vtable[DDRAW7_COMPACT_INDEX] = (void*)proxy_Compact;
  proxy_vtable[DDRAW7_CREATE_CLIPPER_INDEX] = (void*)proxy_CreateClipper;
  proxy_vtable[DDRAW7_CREATE_PALETTE_INDEX] = (void*)proxy_CreatePalette;
  proxy_vtable[DDRAW7_CREATE_SURFACE_INDEX] = (void*)proxy_CreateSurface;
  proxy_vtable[DDRAW7_DUPLICATE_SURFACE_INDEX] = (void*)proxy_DuplicateSurface;
  proxy_vtable[DDRAW7_ENUM_DISPLAY_MODES_INDEX] = (void*)proxy_EnumDisplayModes;
  proxy_vtable[DDRAW7_ENUM_SURFACES_INDEX] = (void*)proxy_EnumSurfaces;
  proxy_vtable[DDRAW7_FLIP_TO_GDI_INDEX] = (void*)proxy_FlipToGDISurface;
  proxy_vtable[DDRAW7_GET_CAPS_INDEX] = (void*)proxy_GetCaps;
  proxy_vtable[DDRAW7_GET_DISPLAY_MODE_INDEX] = (void*)proxy_GetDisplayMode;
  proxy_vtable[DDRAW7_GET_FOURCC_CODES_INDEX] = (void*)proxy_GetFourCCCodes;
  proxy_vtable[DDRAW7_GET_GDI_SURFACE_INDEX] = (void*)proxy_GetGDISurface;
  proxy_vtable[DDRAW7_GET_MONITOR_FREQUENCY_INDEX] = (void*)proxy_GetMonitorFrequency;
  proxy_vtable[DDRAW7_GET_SCANLINE_INDEX] = (void*)proxy_GetScanLine;
  proxy_vtable[DDRAW7_GET_VERTICAL_BLANK_STATUS_INDEX] = (void*)proxy_GetVerticalBlankStatus;
  proxy_vtable[DDRAW7_INITIALIZE_INDEX] = (void*)proxy_Initialize;
  proxy_vtable[DDRAW7_RESTORE_DISPLAY_MODE_INDEX] = (void*)proxy_RestoreDisplayMode;
  proxy_vtable[DDRAW7_SET_COOP_LEVEL_INDEX] = (void*)proxy_SetCooperativeLevel;
  proxy_vtable[DDRAW7_SET_DISPLAY_MODE_INDEX] = (void*)proxy_SetDisplayMode;
  proxy_vtable[DDRAW7_WAIT_FOR_VERTICAL_BLANK_INDEX] = (void*)proxy_WaitForVerticalBlank;
  proxy_vtable[DDRAW7_GET_AVAILABLE_VIDMEM_INDEX] = (void*)proxy_GetAvailableVidMem;
  proxy_vtable[DDRAW7_GET_SURFACE_FROM_DC_INDEX] = (void*)proxy_GetSurfaceFromDC;
  proxy_vtable[DDRAW7_RESTORE_ALL_SURFACES_INDEX] = (void*)proxy_RestoreAllSurfaces;
  proxy_vtable[DDRAW7_TEST_COOP_LEVEL_INDEX] = (void*)proxy_TestCooperativeLevel;
  proxy_vtable[DDRAW7_GET_DEVICE_IDENTIFIER_INDEX] = (void*)proxy_GetDeviceIdentifier;
  proxy_vtable[DDRAW7_START_MODE_TEST_INDEX] = (void*)proxy_StartModeTest;
  proxy_vtable[DDRAW7_EVALUATE_MODE_INDEX] = (void*)proxy_EvaluateMode;

  return proxy;
}

static HRESULT WINAPI proxy_QueryInterface(ProxyIDirectDraw7* this_ptr, REFIID riid,
                                          LPVOID* ppvObj) {
  if (this_ptr == NULL || ppvObj == NULL) {
    return E_INVALIDARG;
  }

  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectDraw7)) {
    *ppvObj = this_ptr;
    proxy_AddRef(this_ptr);
    return S_OK;
  }

  {
    typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, REFIID, LPVOID*);
    Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_QI_INDEX];
    return fn(this_ptr->real_object, riid, ppvObj);
  }
}

static ULONG WINAPI proxy_AddRef(ProxyIDirectDraw7* this_ptr) {
  typedef ULONG (WINAPI* Fn)(IDirectDraw7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_ADDREF_INDEX];
  return fn(this_ptr->real_object);
}

static ULONG WINAPI proxy_Release(ProxyIDirectDraw7* this_ptr) {
  typedef ULONG (WINAPI* Fn)(IDirectDraw7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_RELEASE_INDEX];
  ULONG refs = fn(this_ptr->real_object);

  if (refs == 0) {
    free(this_ptr->proxy_vtable);
    free(this_ptr);
  }
  return refs;
}

static HRESULT WINAPI proxy_Compact(ProxyIDirectDraw7* this_ptr) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_COMPACT_INDEX];
  return fn(this_ptr->real_object);
}

static HRESULT WINAPI proxy_CreateClipper(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                         LPDIRECTDRAWCLIPPER* lplpDDClipper,
                                         IUnknown* pUnkOuter) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_CREATE_CLIPPER_INDEX];
  return fn(this_ptr->real_object, dwFlags, lplpDDClipper, pUnkOuter);
}

static HRESULT WINAPI proxy_CreatePalette(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                         LPPALETTEENTRY lpDDColorTable,
                                         LPDIRECTDRAWPALETTE* lpColorTable,
                                         IUnknown* pUnkOuter) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_CREATE_PALETTE_INDEX];
  return fn(this_ptr->real_object, dwFlags, lpDDColorTable, lpColorTable, pUnkOuter);
}

static HRESULT WINAPI proxy_CreateSurface(ProxyIDirectDraw7* this_ptr,
                                        LPDDSURFACEDESC2 lpDDSurfaceDesc,
                                        LPDIRECTDRAWSURFACE7* lplpDDSurface,
                                        IUnknown* pUnkOuter) {
  DDSURFACEDESC2 adjusted_desc;
  LPDDSURFACEDESC2 surface_desc = lpDDSurfaceDesc;

  DWORD caps = 0;
  if (lpDDSurfaceDesc && (lpDDSurfaceDesc->dwFlags & DDSD_CAPS))
    caps = lpDDSurfaceDesc->ddsCaps.dwCaps;
  shim_log("CreateSurface: caps=0x%08lx (primary=%d zbuf=%d sysmem=%d)",
           caps, !!(caps & DDSCAPS_PRIMARYSURFACE), !!(caps & DDSCAPS_ZBUFFER),
           !!(caps & DDSCAPS_SYSTEMMEMORY));

  if (try_adjust_surface_desc(lpDDSurfaceDesc, &adjusted_desc)) {
    shim_log("CreateSurface: ADJUSTED caps 0x%08lx -> 0x%08lx",
             caps, adjusted_desc.ddsCaps.dwCaps);
    surface_desc = &adjusted_desc;
  }

  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE7*,
                              IUnknown*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_CREATE_SURFACE_INDEX];
  HRESULT hr = fn(this_ptr->real_object, surface_desc, lplpDDSurface, pUnkOuter);
  shim_log("CreateSurface: result=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT WINAPI proxy_DuplicateSurface(ProxyIDirectDraw7* this_ptr,
                                            LPDIRECTDRAWSURFACE7 lpDDSurface,
                                            LPDIRECTDRAWSURFACE7* lplpDupDDSurface) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_DUPLICATE_SURFACE_INDEX];
  return fn(this_ptr->real_object, lpDDSurface, lplpDupDDSurface);
}

static HRESULT WINAPI proxy_EnumDisplayModes(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                            LPDDSURFACEDESC2 lpDDSurfaceDesc,
                                            LPVOID lpContext,
                                            LPDDENUMMODESCALLBACK2 lpEnumModesCallback) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, DWORD, LPDDSURFACEDESC2, LPVOID,
                              LPDDENUMMODESCALLBACK2);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_ENUM_DISPLAY_MODES_INDEX];
  return fn(this_ptr->real_object, dwFlags, lpDDSurfaceDesc, lpContext, lpEnumModesCallback);
}

static HRESULT WINAPI proxy_EnumSurfaces(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                        LPDDSURFACEDESC2 lpDDSurfaceDesc, LPVOID lpContext,
                                        LPDDENUMSURFACESCALLBACK7 lpEnumSurfacesCallback) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, DWORD, LPDDSURFACEDESC2, LPVOID,
                              LPDDENUMSURFACESCALLBACK7);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_ENUM_SURFACES_INDEX];
  return fn(this_ptr->real_object, dwFlags, lpDDSurfaceDesc, lpContext,
            lpEnumSurfacesCallback);
}

static HRESULT WINAPI proxy_FlipToGDISurface(ProxyIDirectDraw7* this_ptr) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_FLIP_TO_GDI_INDEX];
  return fn(this_ptr->real_object);
}

static HRESULT WINAPI proxy_GetCaps(ProxyIDirectDraw7* this_ptr, LPDDCAPS lpDDSCaps,
                                   LPDDCAPS lpDDSCaps2) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDDCAPS, LPDDCAPS);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_CAPS_INDEX];
  return fn(this_ptr->real_object, lpDDSCaps, lpDDSCaps2);
}

static HRESULT WINAPI proxy_GetDisplayMode(ProxyIDirectDraw7* this_ptr,
                                          LPDDSURFACEDESC2 lpDDSurfaceDesc) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDDSURFACEDESC2);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_DISPLAY_MODE_INDEX];
  return fn(this_ptr->real_object, lpDDSurfaceDesc);
}

static HRESULT WINAPI proxy_GetFourCCCodes(ProxyIDirectDraw7* this_ptr, LPDWORD lpNumCodes,
                                          LPDWORD lpPixelFormat) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDWORD, LPDWORD);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_FOURCC_CODES_INDEX];
  return fn(this_ptr->real_object, lpNumCodes, lpPixelFormat);
}

static HRESULT WINAPI proxy_GetGDISurface(ProxyIDirectDraw7* this_ptr,
                                         LPDIRECTDRAWSURFACE7* lplpGDISurface) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDIRECTDRAWSURFACE7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_GDI_SURFACE_INDEX];
  return fn(this_ptr->real_object, lplpGDISurface);
}

static HRESULT WINAPI proxy_GetMonitorFrequency(ProxyIDirectDraw7* this_ptr, LPDWORD lpdwFrequency) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDWORD);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_MONITOR_FREQUENCY_INDEX];
  return fn(this_ptr->real_object, lpdwFrequency);
}

static HRESULT WINAPI proxy_GetScanLine(ProxyIDirectDraw7* this_ptr, LPDWORD lpdwScanLine) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDWORD);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_SCANLINE_INDEX];
  return fn(this_ptr->real_object, lpdwScanLine);
}

static HRESULT WINAPI proxy_GetVerticalBlankStatus(ProxyIDirectDraw7* this_ptr, LPBOOL lpbsInVBlank) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPBOOL);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_VERTICAL_BLANK_STATUS_INDEX];
  return fn(this_ptr->real_object, lpbsInVBlank);
}

static HRESULT WINAPI proxy_Initialize(ProxyIDirectDraw7* this_ptr, LPGUID lpGUID) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPGUID);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_INITIALIZE_INDEX];
  return fn(this_ptr->real_object, lpGUID);
}

static HRESULT WINAPI proxy_RestoreDisplayMode(ProxyIDirectDraw7* this_ptr) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_RESTORE_DISPLAY_MODE_INDEX];
  return fn(this_ptr->real_object);
}

static HRESULT WINAPI proxy_SetCooperativeLevel(ProxyIDirectDraw7* this_ptr,
                                               HWND hWnd, DWORD dwFlags) {
  this_ptr->coop_downgraded = false;
  if (hWnd) this_ptr->game_hwnd = hWnd;
  if ((dwFlags & (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) ==
      (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) {
    if (g_fn_SetAppCompatData != NULL) {
      g_fn_SetAppCompatData(12, 0);
      shim_log("SetCooperativeLevel: SetAppCompatData(12, 0) called for fullscreen");
    }
  }

  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, HWND, DWORD);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_SET_COOP_LEVEL_INDEX];
  HRESULT hr = fn(this_ptr->real_object, hWnd, dwFlags);
  shim_log("SetCooperativeLevel: result=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT WINAPI proxy_SetDisplayMode(ProxyIDirectDraw7* this_ptr, DWORD dwWidth,
                                          DWORD dwHeight, DWORD dwBPP, DWORD dwRefreshRate,
                                          DWORD dwFlags) {
  shim_log("SetDisplayMode: %lux%lu x%lubpp refresh=%lu flags=0x%lx coop_downgraded=%d",
           dwWidth, dwHeight, dwBPP, dwRefreshRate, dwFlags, this_ptr->coop_downgraded);
  if (this_ptr->coop_downgraded) {
    shim_log("SetDisplayMode: SKIPPED (coop downgraded), returning DD_OK");
    return DD_OK;
  }

  if (dwBPP == 16 && !g_using_dgvoodoo_chain) {
    shim_log("SetDisplayMode: forcing compatibility bpp 32 for 16bpp request (%lux%lux%lubpp)",
             dwWidth, dwHeight, dwBPP);
    dwBPP = 32;
  }

  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, DWORD, DWORD, DWORD, DWORD, DWORD);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_SET_DISPLAY_MODE_INDEX];

  HRESULT result = fn(this_ptr->real_object, dwWidth, dwHeight, dwBPP, dwRefreshRate, dwFlags);
  shim_log("SetDisplayMode: result=0x%08lx", (unsigned long)result);

  /* After dgVoodoo2 sets the mode, resize the window to 2x for consistent sizing.
     dgVoodoo2 handles scaling internally — we just make the window larger. */
  if (SUCCEEDED(result) && g_using_dgvoodoo_chain && this_ptr->game_hwnd &&
      dwWidth == 640 && dwHeight == 480 && dwBPP == 16) {
    RECT rc = {0, 0, 1280, 960};
    DWORD style = (DWORD)GetWindowLongA(this_ptr->game_hwnd, GWL_STYLE);
    DWORD exstyle = (DWORD)GetWindowLongA(this_ptr->game_hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, style, FALSE, exstyle);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(this_ptr->game_hwnd, NULL, (sx - w) / 2, (sy - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    shim_log("SetDisplayMode: resized window to 1280x960 centered");
  }

  if (FAILED(result) && (dwWidth != 640 || dwHeight != 480 || dwBPP != 16)) {
    shim_log("SetDisplayMode: retrying 640x480x16");
    result = fn(this_ptr->real_object, 640, 480, 16, dwRefreshRate, dwFlags);
    shim_log("SetDisplayMode: fallback result=0x%08lx", (unsigned long)result);
  }

  return result;
}

static HRESULT WINAPI proxy_WaitForVerticalBlank(ProxyIDirectDraw7* this_ptr,
                                                DWORD dwFlags, HANDLE hEvent) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, DWORD, HANDLE);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_WAIT_FOR_VERTICAL_BLANK_INDEX];
  return fn(this_ptr->real_object, dwFlags, hEvent);
}

static HRESULT WINAPI proxy_GetAvailableVidMem(ProxyIDirectDraw7* this_ptr, LPDDSCAPS2 lpDDSCaps,
                                              LPDWORD lpdwTotal, LPDWORD lpdwFree) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDDSCAPS2, LPDWORD, LPDWORD);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_AVAILABLE_VIDMEM_INDEX];
  return fn(this_ptr->real_object, lpDDSCaps, lpdwTotal, lpdwFree);
}

static HRESULT WINAPI proxy_GetSurfaceFromDC(ProxyIDirectDraw7* this_ptr, HDC hdc,
                                            LPDIRECTDRAWSURFACE7* lpDDSurface) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, HDC, LPDIRECTDRAWSURFACE7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_SURFACE_FROM_DC_INDEX];
  return fn(this_ptr->real_object, hdc, lpDDSurface);
}

static HRESULT WINAPI proxy_RestoreAllSurfaces(ProxyIDirectDraw7* this_ptr) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_RESTORE_ALL_SURFACES_INDEX];
  return fn(this_ptr->real_object);
}

static HRESULT WINAPI proxy_TestCooperativeLevel(ProxyIDirectDraw7* this_ptr) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_TEST_COOP_LEVEL_INDEX];
  return fn(this_ptr->real_object);
}

static HRESULT WINAPI proxy_GetDeviceIdentifier(ProxyIDirectDraw7* this_ptr,
                                               LPDDDEVICEIDENTIFIER2 lpdddi, DWORD dwFlags) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPDDDEVICEIDENTIFIER2, DWORD);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_GET_DEVICE_IDENTIFIER_INDEX];
  return fn(this_ptr->real_object, lpdddi, dwFlags);
}

static HRESULT WINAPI proxy_StartModeTest(ProxyIDirectDraw7* this_ptr, LPSIZE lpModes,
                                         DWORD dwNumEntries, DWORD dwFlags) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, LPSIZE, DWORD, DWORD);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_START_MODE_TEST_INDEX];
  return fn(this_ptr->real_object, lpModes, dwNumEntries, dwFlags);
}

static HRESULT WINAPI proxy_EvaluateMode(ProxyIDirectDraw7* this_ptr, DWORD dwFlags,
                                        DWORD* lpdwTimeout) {
  typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, DWORD, DWORD*);
  Fn fn = (Fn)this_ptr->real_vtable[DDRAW7_EVALUATE_MODE_INDEX];
  return fn(this_ptr->real_object, dwFlags, lpdwTimeout);
}

__declspec(dllexport) HRESULT WINAPI DirectDrawCreateEx(LPGUID lpGUID, LPVOID* lplpDD,
                                                       REFIID riid, IUnknown* pUnkOuter) {
  shim_log("DirectDrawCreateEx: called");
  if (!load_real_ddraw_module()) {
    shim_log("DirectDrawCreateEx: FAILED to load real ddraw.dll");
    return E_FAIL;
  }

  HRESULT result = g_fn_DirectDrawCreateEx(lpGUID, lplpDD, riid, pUnkOuter);
  shim_log("DirectDrawCreateEx: real result=0x%08lx", (unsigned long)result);
  if (FAILED(result) || lplpDD == NULL || *lplpDD == NULL) {
    return result;
  }

  if (!is_target_ddraw_iid(riid)) {
    shim_log("DirectDrawCreateEx: non-target IID, returning unwrapped");
    return result;
  }

  ProxyIDirectDraw7* proxy = (ProxyIDirectDraw7*)create_proxy_ddraw7((IDirectDraw7*)*lplpDD);
  if (proxy != NULL) {
    *lplpDD = proxy;
    shim_log("DirectDrawCreateEx: wrapped in proxy at %p", (void*)proxy);
  } else {
    shim_log("DirectDrawCreateEx: proxy creation failed, returning unwrapped");
  }

  return result;
}

#ifndef SHIM_DLL_VTABLE_OFFSETS_H_
#define SHIM_DLL_VTABLE_OFFSETS_H_

// IDirectDraw7 vtable offsets (byte offsets into the vtable).
#define DDRAW7_VTBL_CREATE_SURFACE 0x18
#define DDRAW7_VTBL_SET_COOP_LEVEL 0x50
#define DDRAW7_VTBL_SET_DISPLAY_MODE 0x54

// IDirectDrawSurface7 vtable offsets (byte offsets into the vtable).
#define DDSURFACE7_VTBL_BLT 0x14
#define DDSURFACE7_VTBL_FLIP 0x2C

#endif  // SHIM_DLL_VTABLE_OFFSETS_H_

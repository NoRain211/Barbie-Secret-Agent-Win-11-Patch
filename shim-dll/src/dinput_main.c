/*
 * dinput.dll proxy — XInput gamepad support for Secret Agent Barbie
 *
 * Forwards keyboard/mouse DInput calls to real system dinput.dll.
 * Adds Xbox controller as a virtual DI7 joystick via XInput.
 *
 * Build: gcc -m32 -shared -DINITGUID -o dinput.dll src/dinput_main.c
 *        -Iinclude -lole32 -luuid dinput.def -Wl,--enable-stdcall-fixup
 */

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── Logging (debug builds only) ──────────────────────────────────── */

#if defined(SHIM_DEBUG) || defined(DEBUG)
static FILE* g_log_file = NULL;
static void di_log_init(void) {
    if (g_log_file) return;
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* s = strrchr(path, '\\');
    if (s) *(s + 1) = '\0';
    strcat(path, "dinput_proxy.log");
    g_log_file = fopen(path, "w");
}
static void di_log(const char* fmt, ...) {
    va_list a;
    if (!g_log_file) di_log_init();
    if (!g_log_file) return;
    DWORD t = GetTickCount();
    fprintf(g_log_file, "[%lu.%03lu] ", t / 1000, t % 1000);
    va_start(a, fmt);
    vfprintf(g_log_file, fmt, a);
    va_end(a);
    fprintf(g_log_file, "\n");
    fflush(g_log_file);
}
static void di_log_close(void) {
    if (g_log_file) { fclose(g_log_file); g_log_file = NULL; }
}
#else
#define di_log_init()
#define di_log(...)
#define di_log_close()
#endif

/* ── DInput / XInput types (avoid SDK headers) ────────────────────── */

typedef struct { unsigned long Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } MY_GUID;
typedef MY_GUID MY_IID;
typedef long HRESULT_T;

#define DI_OK                   0
#define DIERR_INVALIDPARAM      ((HRESULT_T)0x80070057)
#define DIERR_NOTINITIALIZED    ((HRESULT_T)0x80070015)
#define DIERR_NOTACQUIRED       ((HRESULT_T)0x8007001E)
#define DIERR_NOTFOUND          ((HRESULT_T)0x80070002)
#define DIENUM_CONTINUE         1
#define DIENUM_STOP             0
#define DIDEVTYPE_JOYSTICK      4
#define DIEDFL_ATTACHEDONLY     0x01
#define DIEDFL_FORCEFEEDBACK    0x100
#define DISCL_EXCLUSIVE         0x01
#define DISCL_NONEXCLUSIVE      0x02
#define DISCL_FOREGROUND        0x04
#define DISCL_BACKGROUND        0x08

/* IDirectInput7A vtable indices */
#define DI_VTBL_QI              0
#define DI_VTBL_ADDREF          1
#define DI_VTBL_RELEASE         2
#define DI_VTBL_CREATEDEVICE    3
#define DI_VTBL_ENUMDEVICES     4
#define DI_VTBL_GETDEVSTATUS    5
#define DI_VTBL_RUNCONTROLPANEL 6
#define DI_VTBL_INITIALIZE      7
#define DI_VTBL_FINDDEVICE      8
#define DI_VTBL_CREATEDEVICEEX  9
#define DI_VTBL_COUNT           10

/* DIDEVICEINSTANCEA structure (DI5+ version, dwSize = 0x240) */
#pragma pack(push, 1)
typedef struct {
    DWORD dwSize;
    MY_GUID guidInstance;
    MY_GUID guidProduct;
    DWORD dwDevType;
    char tszInstanceName[260];
    char tszProductName[260];
    MY_GUID guidFFDriver;
} DIDEVICEINSTANCEA;
#pragma pack(pop)

/* DIDATAFORMAT */
typedef struct {
    DWORD dwSize;
    DWORD dwObjSize;
    DWORD dwFlags;
    DWORD dwDataSize;
    DWORD dwNumObjs;
    void* rgodf;
} DIDATAFORMAT;

/* DIJOYSTATE (80 bytes — what the game expects) */
typedef struct {
    LONG lX;
    LONG lY;
    LONG lZ;
    LONG lRx;
    LONG lRy;
    LONG lRz;
    LONG rglSlider[2];
    DWORD rgdwPOV[4];
    BYTE rgbButtons[32];
} DIJOYSTATE;

/* DIDEVCAPS */
typedef struct {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwDevType;
    DWORD dwAxes;
    DWORD dwButtons;
    DWORD dwPOVs;
    DWORD dwFFSamplePeriod;
    DWORD dwFFMinTimeResolution;
    DWORD dwFirmwareRevision;
    DWORD dwHardwareRevision;
    DWORD dwFFDriverVersion;
} DIDEVCAPS;

/* XInput types */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger;
                 SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef DWORD (WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI* PFN_XInputGetCapabilities)(DWORD, DWORD, void*);

/* XInput button masks */
#define XBTN_DPAD_UP    0x0001
#define XBTN_DPAD_DOWN  0x0002
#define XBTN_DPAD_LEFT  0x0004
#define XBTN_DPAD_RIGHT 0x0008
#define XBTN_START      0x0010
#define XBTN_BACK       0x0020
#define XBTN_LTHUMB     0x0040
#define XBTN_RTHUMB     0x0080
#define XBTN_LSHOULDER  0x0100
#define XBTN_RSHOULDER  0x0200
#define XBTN_A          0x1000
#define XBTN_B          0x2000
#define XBTN_X          0x4000
#define XBTN_Y          0x8000

/* Deadzone */
#define THUMB_DEADZONE  7849
#define TRIGGER_DEADZONE 30

/* Axis range config (used by XInputDevice and xinput_read_joystate) */
typedef struct { LONG range_min, range_max; DWORD deadzone_pct; } AxisConfig;
#define NUM_AXES 6

/* DInput callback types */
typedef BOOL (CALLBACK* LPDIENUMDEVICESCALLBACKA)(const DIDEVICEINSTANCEA*, void*);

/* DIDEVICEOBJECTINSTANCEA (DI5+ version) for EnumObjects */
typedef struct {
    DWORD dwSize;
    MY_GUID guidType;
    DWORD dwOfs;
    DWORD dwType;
    DWORD dwFlags;
    char tszName[260];
    DWORD dwFFMaxForce;
    DWORD dwFFForceResolution;
    WORD wCollectionNumber;
    WORD wDesignatorIndex;
    WORD wUsagePage;
    WORD wUsage;
    DWORD dwDimension;
    WORD wExponent;
    WORD wReportId;
} DIDEVICEOBJECTINSTANCEA;

typedef BOOL (CALLBACK* LPDIENUMDEVICEOBJECTSCALLBACKA)(const DIDEVICEOBJECTINSTANCEA*, void*);

/* Object type GUIDs */
static const MY_GUID GUID_XAxis  = {0xA36D02E0,0xC9F3,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const MY_GUID GUID_YAxis  = {0xA36D02E1,0xC9F3,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const MY_GUID GUID_ZAxis  = {0xA36D02E2,0xC9F3,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const MY_GUID GUID_RxAxis = {0xA36D02F4,0xC9F3,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const MY_GUID GUID_RyAxis = {0xA36D02F5,0xC9F3,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const MY_GUID GUID_RzAxis = {0xA36D02E3,0xC9F3,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const MY_GUID GUID_POV    = {0xA36D02F2,0xC9F3,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const MY_GUID GUID_Button = {0xA36D02F0,0xC9F3,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};

/* DIDFT flags */
#define DIDFT_ABSAXIS    0x02
#define DIDFT_PSHBUTTON  0x04
#define DIDFT_POV_       0x10  /* avoid conflict with POV hat value macros */
#define DIDFT_MAKEINSTANCE(n) ((DWORD)(n) << 8)

/* ── Synthetic GUID for our XInput device ─────────────────────────── */

static const MY_GUID GUID_XInputPad = {
    0x584E5055, 0x5400, 0x0001,
    { 0x53, 0x41, 0x42, 0x41, 0x52, 0x42, 0x49, 0x45 }  /* "SABARBIE" */
};

/* IID_IDirectInputA through IID_IDirectInput7A (for QI) */
static const MY_GUID IID_IDirectInputA  = { 0x89521360, 0xAA8A, 0x11CF, {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00} };
static const MY_GUID IID_IDirectInput2A = { 0x5944E662, 0xAA8A, 0x11CF, {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00} };
static const MY_GUID IID_IDirectInput7A = { 0x9A4CB684, 0x236D, 0x11D3, {0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE} };
static const MY_GUID MY_IID_IUnknown    = { 0x00000000, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46} };

/* ── Globals ──────────────────────────────────────────────────────── */

static HMODULE g_real_dinput = NULL;
static HMODULE g_xinput = NULL;
static PFN_XInputGetState g_XInputGetState = NULL;

typedef HRESULT_T (WINAPI* PFN_DirectInputCreateA)(HINSTANCE, DWORD, void**, void*);
typedef HRESULT_T (WINAPI* PFN_DirectInputCreateW)(HINSTANCE, DWORD, void**, void*);
typedef HRESULT_T (WINAPI* PFN_DirectInputCreateEx)(HINSTANCE, DWORD, const MY_IID*, void**, void*);
static PFN_DirectInputCreateA g_real_DirectInputCreateA = NULL;
static PFN_DirectInputCreateW g_real_DirectInputCreateW = NULL;
static PFN_DirectInputCreateEx g_real_DirectInputCreateEx = NULL;

static int guid_eq(const MY_GUID* a, const MY_GUID* b) {
    return memcmp(a, b, sizeof(MY_GUID)) == 0;
}

/* ── Apply deadzone to XInput thumbstick ──────────────────────────── */

static SHORT apply_deadzone(SHORT val, SHORT dz) {
    if (val > dz) return (SHORT)(((val - dz) * 32767L) / (32767 - dz));
    if (val < -dz) return (SHORT)(((val + dz) * 32767L) / (32767 - dz));
    return 0;
}

/* ── XInput → DIJOYSTATE mapping ──────────────────────────────────── */

/* Scale a normalized value (-32768..32767) to the configured axis range */
static LONG scale_axis(LONG raw, LONG rmin, LONG rmax) {
    /* raw is -32768..32767, scale to rmin..rmax */
    /* center = (rmin+rmax)/2, half_range = (rmax-rmin)/2 */
    LONG center = (rmin + rmax) / 2;
    LONG half = (rmax - rmin) / 2;
    return center + (LONG)((int64_t)raw * half / 32767);
}

/* Scale a positive value (0..255) to the configured axis range */
static LONG scale_trigger(BYTE raw, LONG rmin, LONG rmax) {
    return rmin + (LONG)((int64_t)raw * (rmax - rmin) / 255);
}

/* Inject mouse movement from a stick via SendInput */
#define MOUSE_SENSITIVITY 15  /* pixels per poll at full deflection */

static void inject_mouse_from_stick(SHORT raw_x, SHORT raw_y) {
    SHORT dx = apply_deadzone(raw_x, THUMB_DEADZONE);
    SHORT dy = apply_deadzone(raw_y, THUMB_DEADZONE);
    if (dx == 0 && dy == 0) return;

    int mx = (int)dx * MOUSE_SENSITIVITY / 32767;
    int my = -(int)dy * MOUSE_SENSITIVITY / 32767;  /* invert Y: stick up → mouse up */
    if (mx == 0 && my == 0) return;

    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = mx;
    inp.mi.dy = my;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &inp, sizeof(INPUT));
}

static BOOL xinput_read_joystate(DWORD user_idx, const AxisConfig axes[NUM_AXES], DIJOYSTATE* js) {
    if (!g_XInputGetState) return FALSE;
    XINPUT_STATE xs;
    DWORD res = g_XInputGetState(user_idx, &xs);
    if (res != 0) return FALSE;

    memset(js, 0, sizeof(*js));
    XINPUT_GAMEPAD* gp = &xs.Gamepad;
    WORD btn = gp->wButtons;

    /* Right stick → mouse cursor injection (camera/look) */
    inject_mouse_from_stick(gp->sThumbRX, gp->sThumbRY);

    /* Left stick → movement axes (lX/lY) — analog */
    SHORT ls_x = apply_deadzone(gp->sThumbLX, THUMB_DEADZONE);
    SHORT ls_y = apply_deadzone(gp->sThumbLY, THUMB_DEADZONE);

    /* D-pad → movement axes (lX/lY) — digital override */
    int dpad_up    = !!(btn & XBTN_DPAD_UP);
    int dpad_down  = !!(btn & XBTN_DPAD_DOWN);
    int dpad_left  = !!(btn & XBTN_DPAD_LEFT);
    int dpad_right = !!(btn & XBTN_DPAD_RIGHT);

    /* D-pad overrides left stick if any d-pad direction is pressed */
    if (dpad_up || dpad_down || dpad_left || dpad_right) {
        LONG cx = (axes[0].range_min + axes[0].range_max) / 2;
        LONG cy = (axes[1].range_min + axes[1].range_max) / 2;
        js->lX = dpad_right ? axes[0].range_max : dpad_left ? axes[0].range_min : cx;
        js->lY = dpad_down  ? axes[1].range_max : dpad_up   ? axes[1].range_min : cy;
    } else {
        /* Left stick analog movement */
        js->lX = scale_axis(ls_x, axes[0].range_min, axes[0].range_max);
        js->lY = scale_axis((SHORT)-ls_y, axes[1].range_min, axes[1].range_max);
    }

    /* Right stick also on lRx/lRy for any game code that reads those */
    js->lRx = scale_axis(apply_deadzone(gp->sThumbRX, THUMB_DEADZONE), axes[3].range_min, axes[3].range_max);
    js->lRy = scale_axis(-apply_deadzone(gp->sThumbRY, THUMB_DEADZONE), axes[4].range_min, axes[4].range_max);

    /* Triggers → Z axes */
    BYTE lt = gp->bLeftTrigger > TRIGGER_DEADZONE ? gp->bLeftTrigger : 0;
    BYTE rt = gp->bRightTrigger > TRIGGER_DEADZONE ? gp->bRightTrigger : 0;
    js->lZ  = scale_trigger(lt, axes[2].range_min, axes[2].range_max);
    js->lRz = scale_trigger(rt, axes[5].range_min, axes[5].range_max);

    /* D-pad → POV hat (also keep for games that read POV) */
    if      (dpad_up && dpad_right)   js->rgdwPOV[0] = 4500;
    else if (dpad_right && dpad_down) js->rgdwPOV[0] = 13500;
    else if (dpad_down && dpad_left)  js->rgdwPOV[0] = 22500;
    else if (dpad_left && dpad_up)    js->rgdwPOV[0] = 31500;
    else if (dpad_up)                 js->rgdwPOV[0] = 0;
    else if (dpad_right)              js->rgdwPOV[0] = 9000;
    else if (dpad_down)               js->rgdwPOV[0] = 18000;
    else if (dpad_left)               js->rgdwPOV[0] = 27000;
    else                              js->rgdwPOV[0] = (DWORD)-1;
    js->rgdwPOV[1] = js->rgdwPOV[2] = js->rgdwPOV[3] = (DWORD)-1;

    /* Buttons (0x80 = pressed, 0x00 = released) */
    js->rgbButtons[0]  = (btn & XBTN_A)         ? 0x80 : 0;
    js->rgbButtons[1]  = (btn & XBTN_B)         ? 0x80 : 0;
    js->rgbButtons[2]  = (btn & XBTN_X)         ? 0x80 : 0;
    js->rgbButtons[3]  = (btn & XBTN_Y)         ? 0x80 : 0;
    js->rgbButtons[4]  = (btn & XBTN_LSHOULDER) ? 0x80 : 0;
    js->rgbButtons[5]  = (btn & XBTN_RSHOULDER) ? 0x80 : 0;
    js->rgbButtons[6]  = (btn & XBTN_BACK)      ? 0x80 : 0;
    js->rgbButtons[7]  = (btn & XBTN_START)     ? 0x80 : 0;
    js->rgbButtons[8]  = (btn & XBTN_LTHUMB)    ? 0x80 : 0;
    js->rgbButtons[9]  = (btn & XBTN_RTHUMB)    ? 0x80 : 0;

    /* ── Keyboard/mouse injection from Xbox buttons ─────────────── */
    /* Track previous state for edge detection */
    static WORD prev_btn = 0;
    static BYTE prev_rt = 0;

    /* Helper: inject a keyboard scan code press/release */
    #define INJECT_KEY(scan, pressed) do { \
        INPUT _inp; memset(&_inp, 0, sizeof(_inp)); \
        _inp.type = INPUT_KEYBOARD; \
        _inp.ki.wScan = (scan); \
        _inp.ki.dwFlags = KEYEVENTF_SCANCODE | ((pressed) ? 0 : KEYEVENTF_KEYUP); \
        SendInput(1, &_inp, sizeof(INPUT)); \
    } while(0)

    /* Helper: inject mouse button press/release */
    #define INJECT_MOUSE_BTN(down_flag, up_flag, pressed) do { \
        INPUT _inp; memset(&_inp, 0, sizeof(_inp)); \
        _inp.type = INPUT_MOUSE; \
        _inp.mi.dwFlags = (pressed) ? (down_flag) : (up_flag); \
        SendInput(1, &_inp, sizeof(INPUT)); \
    } while(0)

    /* A button → Space (jump) — scan code 0x39 */
    if ((btn & XBTN_A) && !(prev_btn & XBTN_A))
        INJECT_KEY(0x39, 1);
    else if (!(btn & XBTN_A) && (prev_btn & XBTN_A))
        INJECT_KEY(0x39, 0);

    /* B button → Left mouse click */
    if ((btn & XBTN_B) && !(prev_btn & XBTN_B))
        INJECT_MOUSE_BTN(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 1);
    else if (!(btn & XBTN_B) && (prev_btn & XBTN_B))
        INJECT_MOUSE_BTN(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0);

    /* X button → Left mouse click (action/interact) */
    if ((btn & XBTN_X) && !(prev_btn & XBTN_X))
        INJECT_MOUSE_BTN(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 1);
    else if (!(btn & XBTN_X) && (prev_btn & XBTN_X))
        INJECT_MOUSE_BTN(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0);

    /* Right trigger → Left mouse click (alternate action) */
    BYTE rt_pressed = rt > 128;
    BYTE prev_rt_pressed = prev_rt > 128;
    if (rt_pressed && !prev_rt_pressed)
        INJECT_MOUSE_BTN(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 1);
    else if (!rt_pressed && prev_rt_pressed)
        INJECT_MOUSE_BTN(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0);

    /* Start → Escape (menu) — scan code 0x01 */
    if ((btn & XBTN_START) && !(prev_btn & XBTN_START))
        INJECT_KEY(0x01, 1);
    else if (!(btn & XBTN_START) && (prev_btn & XBTN_START))
        INJECT_KEY(0x01, 0);

    prev_btn = btn;
    prev_rt = rt;

    #undef INJECT_KEY
    #undef INJECT_MOUSE_BTN

    return TRUE;
}

/* ══════════════════════════════════════════════════════════════════════
 * IDirectInputDevice7A wrapper — XInput-backed virtual joystick
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct XInputDevice XInputDevice;

/* COM vtable — 29 methods for IDirectInputDevice7A */
typedef struct {
    HRESULT_T (STDMETHODCALLTYPE* QueryInterface)(XInputDevice*, const MY_IID*, void**);
    ULONG     (STDMETHODCALLTYPE* AddRef)(XInputDevice*);
    ULONG     (STDMETHODCALLTYPE* Release)(XInputDevice*);
    HRESULT_T (STDMETHODCALLTYPE* GetCapabilities)(XInputDevice*, DIDEVCAPS*);
    HRESULT_T (STDMETHODCALLTYPE* EnumObjects)(XInputDevice*, void*, void*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* GetProperty)(XInputDevice*, const MY_GUID*, void*);
    HRESULT_T (STDMETHODCALLTYPE* SetProperty)(XInputDevice*, const MY_GUID*, const void*);
    HRESULT_T (STDMETHODCALLTYPE* Acquire)(XInputDevice*);
    HRESULT_T (STDMETHODCALLTYPE* Unacquire)(XInputDevice*);
    HRESULT_T (STDMETHODCALLTYPE* GetDeviceState)(XInputDevice*, DWORD, void*);
    HRESULT_T (STDMETHODCALLTYPE* GetDeviceData)(XInputDevice*, DWORD, void*, DWORD*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* SetDataFormat)(XInputDevice*, const DIDATAFORMAT*);
    HRESULT_T (STDMETHODCALLTYPE* SetEventNotification)(XInputDevice*, HANDLE);
    HRESULT_T (STDMETHODCALLTYPE* SetCooperativeLevel)(XInputDevice*, HWND, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* GetObjectInfo)(XInputDevice*, void*, DWORD, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* GetDeviceInfo)(XInputDevice*, void*);
    HRESULT_T (STDMETHODCALLTYPE* RunControlPanel)(XInputDevice*, HWND, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* Initialize)(XInputDevice*, HINSTANCE, DWORD, const MY_GUID*);
    /* IDirectInputDevice2A */
    HRESULT_T (STDMETHODCALLTYPE* CreateEffect)(XInputDevice*, const MY_GUID*, const void*, void**, void*);
    HRESULT_T (STDMETHODCALLTYPE* EnumEffects)(XInputDevice*, void*, void*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* GetEffectInfo)(XInputDevice*, void*, const MY_GUID*);
    HRESULT_T (STDMETHODCALLTYPE* GetForceFeedbackState)(XInputDevice*, DWORD*);
    HRESULT_T (STDMETHODCALLTYPE* SendForceFeedbackCommand)(XInputDevice*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* EnumCreatedEffectObjects)(XInputDevice*, void*, void*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* Escape)(XInputDevice*, void*);
    HRESULT_T (STDMETHODCALLTYPE* Poll)(XInputDevice*);
    HRESULT_T (STDMETHODCALLTYPE* SendDeviceData)(XInputDevice*, DWORD, const void*, DWORD*, DWORD);
    /* IDirectInputDevice7A */
    HRESULT_T (STDMETHODCALLTYPE* EnumEffectsInFile)(XInputDevice*, const char*, void*, void*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* WriteEffectToFile)(XInputDevice*, const char*, DWORD, void*, DWORD);
} XInputDeviceVtbl;

/* DIPROP_* are small integers, not real GUID pointers */
#define DIPROP_RANGE     ((const MY_GUID*)(uintptr_t)4)
#define DIPROP_DEADZONE  ((const MY_GUID*)(uintptr_t)5)
#define DIPROP_SATURATION ((const MY_GUID*)(uintptr_t)6)
#define IS_DIPROP(g)     ((uintptr_t)(g) < 0x1000)

/* DIPH_* dwHow values */
#define DIPH_DEVICE    0
#define DIPH_BYOFFSET  1
#define DIPH_BYID      2

/* Property header */
typedef struct { DWORD dwSize, dwHeaderSize, dwObj, dwHow; } DIPROPHEADER;
typedef struct { DIPROPHEADER diph; LONG lMin, lMax; } DIPROPRANGE;
typedef struct { DIPROPHEADER diph; DWORD dwData; } DIPROPDWORD;

struct XInputDevice {
    const XInputDeviceVtbl* vtbl;
    LONG ref_count;
    DWORD user_index;
    BOOL acquired;
    DWORD data_size;
    AxisConfig axes[NUM_AXES];  /* indexed by offset/4: 0=lX,1=lY,2=lZ,3=lRx,4=lRy,5=lRz */
};

/* ── XInput device method implementations ─────────────────────────── */

static HRESULT_T STDMETHODCALLTYPE xdev_QueryInterface(XInputDevice* self, const MY_IID* riid, void** ppv) {
    di_log("XInputDevice: QueryInterface(iid=%08lx-%04x-%04x) ppv=%p self=%p",
           riid ? riid->Data1 : 0, riid ? riid->Data2 : 0, riid ? riid->Data3 : 0,
           (void*)ppv, (void*)self);
    if (!ppv) return (HRESULT_T)0x80000003L; /* E_POINTER */
    /* Accept any DInput device IID — we implement them all through one vtable */
    *ppv = self;
    self->ref_count++;
    di_log("XInputDevice: QI wrote *ppv=%p refcount=%ld", (void*)*ppv, self->ref_count);
    return DI_OK;
}

static ULONG STDMETHODCALLTYPE xdev_AddRef(XInputDevice* self) {
    return InterlockedIncrement(&self->ref_count);
}

static ULONG STDMETHODCALLTYPE xdev_Release(XInputDevice* self) {
    LONG r = InterlockedDecrement(&self->ref_count);
    if (r <= 0) {
        di_log("XInputDevice: destroyed (user %lu)", self->user_index);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return r;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetCapabilities(XInputDevice* self, DIDEVCAPS* caps) {
    di_log("XInputDevice: GetCapabilities");
    (void)self;
    if (!caps) return DIERR_INVALIDPARAM;
    memset(caps, 0, caps->dwSize);
    caps->dwFlags = 0;
    caps->dwDevType = DIDEVTYPE_JOYSTICK | (0x01 << 8);  /* subtype: gamepad */
    caps->dwAxes = 6;      /* lX, lY, lZ, lRx, lRy, lRz */
    caps->dwButtons = 10;  /* A B X Y LB RB Back Start LS RS */
    caps->dwPOVs = 1;      /* d-pad */
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_EnumObjects(XInputDevice* s, void* cb, void* ref, DWORD fl) {
    di_log("XInputDevice: EnumObjects(flags=0x%lx)", fl);
    (void)s;
    LPDIENUMDEVICEOBJECTSCALLBACKA callback = (LPDIENUMDEVICEOBJECTSCALLBACKA)cb;
    if (!callback) return DIERR_INVALIDPARAM;

    /* Filter: fl=0 means all, otherwise match type bits */
    #define MATCH(type_bits) (fl == 0 || (fl & (type_bits)))

    /* Axes */
    if (MATCH(0x03)) {  /* DIDFT_AXIS = DIDFT_ABSAXIS | DIDFT_RELAXIS */
        static const struct { const MY_GUID* guid; DWORD ofs; int inst; const char* name; } axes[] = {
            { &GUID_XAxis,  0,  0, "X Axis" },
            { &GUID_YAxis,  4,  1, "Y Axis" },
            { &GUID_ZAxis,  8,  2, "Z Axis" },
            { &GUID_RxAxis, 12, 3, "Rx Axis" },
            { &GUID_RyAxis, 16, 4, "Ry Axis" },
            { &GUID_RzAxis, 20, 5, "Rz Axis" },
        };
        for (int i = 0; i < 6; i++) {
            DIDEVICEOBJECTINSTANCEA obj;
            memset(&obj, 0, sizeof(obj));
            obj.dwSize = sizeof(obj);
            memcpy(&obj.guidType, axes[i].guid, sizeof(MY_GUID));
            obj.dwOfs = axes[i].ofs;
            obj.dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(axes[i].inst);
            obj.dwFlags = 0x0100; /* DIDOI_ASPECTPOSITION */
            strncpy(obj.tszName, axes[i].name, 259);
            if (callback(&obj, ref) == DIENUM_STOP) return DI_OK;
        }
    }

    /* POV hat */
    if (MATCH(0x10)) {  /* DIDFT_POV */
        DIDEVICEOBJECTINSTANCEA obj;
        memset(&obj, 0, sizeof(obj));
        obj.dwSize = sizeof(obj);
        memcpy(&obj.guidType, &GUID_POV, sizeof(MY_GUID));
        obj.dwOfs = 32;  /* offset of rgdwPOV[0] in DIJOYSTATE */
        obj.dwType = DIDFT_POV_ | DIDFT_MAKEINSTANCE(0);
        strncpy(obj.tszName, "Hat Switch", 259);
        if (callback(&obj, ref) == DIENUM_STOP) return DI_OK;
    }

    /* Buttons */
    if (MATCH(0x0C)) {  /* DIDFT_BUTTON */
        static const char* btn_names[] = {
            "A","B","X","Y","LB","RB","Back","Start","LS","RS"
        };
        for (int i = 0; i < 10; i++) {
            DIDEVICEOBJECTINSTANCEA obj;
            memset(&obj, 0, sizeof(obj));
            obj.dwSize = sizeof(obj);
            memcpy(&obj.guidType, &GUID_Button, sizeof(MY_GUID));
            obj.dwOfs = 48 + i;  /* offset of rgbButtons[i] in DIJOYSTATE */
            obj.dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i);
            strncpy(obj.tszName, btn_names[i], 259);
            if (callback(&obj, ref) == DIENUM_STOP) return DI_OK;
        }
    }

    #undef MATCH
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetProperty(XInputDevice* self, const MY_GUID* g, void* h) {
    di_log("XInputDevice: GetProperty(prop=%lu)", IS_DIPROP(g) ? (unsigned long)(uintptr_t)g : 0);
    if (!h) return DIERR_INVALIDPARAM;

    if (g == DIPROP_RANGE) {
        DIPROPRANGE* pr = (DIPROPRANGE*)h;
        DWORD idx = 0;
        if (pr->diph.dwHow == DIPH_BYOFFSET && pr->diph.dwObj / 4 < (DWORD)NUM_AXES)
            idx = pr->diph.dwObj / 4;
        pr->lMin = self->axes[idx].range_min;
        pr->lMax = self->axes[idx].range_max;
        return DI_OK;
    }

    if (g == DIPROP_DEADZONE) {
        DIPROPDWORD* pd = (DIPROPDWORD*)h;
        DWORD idx = 0;
        if (pd->diph.dwHow == DIPH_BYOFFSET && pd->diph.dwObj / 4 < (DWORD)NUM_AXES)
            idx = pd->diph.dwObj / 4;
        pd->dwData = self->axes[idx].deadzone_pct;
        return DI_OK;
    }

    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_SetProperty(XInputDevice* self, const MY_GUID* g, const void* h) {
    if (!h) return DIERR_INVALIDPARAM;
    const DIPROPHEADER* hdr = (const DIPROPHEADER*)h;

    if (g == DIPROP_RANGE) {
        const DIPROPRANGE* pr = (const DIPROPRANGE*)h;
        di_log("XInputDevice: SetProperty DIPROP_RANGE obj=%lu how=%lu min=%ld max=%ld",
               hdr->dwObj, hdr->dwHow, pr->lMin, pr->lMax);
        if (hdr->dwHow == DIPH_DEVICE) {
            /* Apply to all axes */
            for (int i = 0; i < NUM_AXES; i++) {
                self->axes[i].range_min = pr->lMin;
                self->axes[i].range_max = pr->lMax;
            }
        } else if (hdr->dwHow == DIPH_BYOFFSET && hdr->dwObj / 4 < (DWORD)NUM_AXES) {
            self->axes[hdr->dwObj / 4].range_min = pr->lMin;
            self->axes[hdr->dwObj / 4].range_max = pr->lMax;
        }
        return DI_OK;
    }

    if (g == DIPROP_DEADZONE) {
        const DIPROPDWORD* pd = (const DIPROPDWORD*)h;
        di_log("XInputDevice: SetProperty DIPROP_DEADZONE obj=%lu how=%lu val=%lu",
               hdr->dwObj, hdr->dwHow, pd->dwData);
        if (hdr->dwHow == DIPH_DEVICE) {
            for (int i = 0; i < NUM_AXES; i++)
                self->axes[i].deadzone_pct = pd->dwData;
        } else if (hdr->dwHow == DIPH_BYOFFSET && hdr->dwObj / 4 < (DWORD)NUM_AXES) {
            self->axes[hdr->dwObj / 4].deadzone_pct = pd->dwData;
        }
        return DI_OK;
    }

    di_log("XInputDevice: SetProperty(prop=%lu)", IS_DIPROP(g) ? (unsigned long)(uintptr_t)g : 0);
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_Acquire(XInputDevice* self) {
    self->acquired = TRUE;
    di_log("XInputDevice: acquired (user %lu)", self->user_index);
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_Unacquire(XInputDevice* self) {
    di_log("XInputDevice: Unacquire");
    self->acquired = FALSE;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetDeviceState(XInputDevice* self, DWORD cbData, void* lpvData) {
    /* Only log first call to avoid flooding */
    static int logged = 0;
    if (!logged) { di_log("XInputDevice: GetDeviceState(size=%lu) [first call]", cbData); logged = 1; }
    if (!lpvData) return DIERR_INVALIDPARAM;
    if (!self->acquired) return DIERR_NOTACQUIRED;

    DIJOYSTATE js;
    if (!xinput_read_joystate(self->user_index, self->axes, &js)) {
        memset(lpvData, 0, cbData);
        /* Return zeroed state with centered POVs */
        if (cbData >= sizeof(DIJOYSTATE)) {
            DIJOYSTATE* p = (DIJOYSTATE*)lpvData;
            p->rgdwPOV[0] = p->rgdwPOV[1] = p->rgdwPOV[2] = p->rgdwPOV[3] = (DWORD)-1;
        }
        return DI_OK;
    }

    DWORD copy = cbData < sizeof(js) ? cbData : sizeof(js);
    memcpy(lpvData, &js, copy);
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetDeviceData(XInputDevice* s, DWORD a, void* b, DWORD* c, DWORD d) {
    di_log("XInputDevice: GetDeviceData");
    (void)s; (void)a; (void)b; (void)d;
    if (c) *c = 0;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_SetDataFormat(XInputDevice* self, const DIDATAFORMAT* df) {
    if (!df) return DIERR_INVALIDPARAM;
    self->data_size = df->dwDataSize;
    di_log("XInputDevice: SetDataFormat size=%lu", df->dwDataSize);
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_SetEventNotification(XInputDevice* s, HANDLE h) {
    (void)s; (void)h;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_SetCooperativeLevel(XInputDevice* s, HWND hw, DWORD fl) {
    (void)s; (void)hw; (void)fl;
    di_log("XInputDevice: SetCooperativeLevel flags=0x%lx", fl);
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetObjectInfo(XInputDevice* s, void* a, DWORD b, DWORD c) {
    (void)s; (void)a; (void)b; (void)c;
    return DIERR_INVALIDPARAM;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetDeviceInfo(XInputDevice* s, void* a) {
    di_log("XInputDevice: GetDeviceInfo");
    (void)s; (void)a;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_RunControlPanel(XInputDevice* s, HWND h, DWORD d) {
    (void)s; (void)h; (void)d;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_Initialize(XInputDevice* s, HINSTANCE h, DWORD v, const MY_GUID* g) {
    (void)s; (void)h; (void)v; (void)g;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_CreateEffect(XInputDevice* s, const MY_GUID* a, const void* b, void** c, void* d) {
    (void)s; (void)a; (void)b; (void)c; (void)d;
    return DIERR_INVALIDPARAM;
}
static HRESULT_T STDMETHODCALLTYPE xdev_EnumEffects(XInputDevice* s, void* a, void* b, DWORD c) {
    (void)s; (void)a; (void)b; (void)c;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_GetEffectInfo(XInputDevice* s, void* a, const MY_GUID* b) {
    (void)s; (void)a; (void)b;
    return DIERR_INVALIDPARAM;
}
static HRESULT_T STDMETHODCALLTYPE xdev_GetForceFeedbackState(XInputDevice* s, DWORD* a) {
    (void)s; if (a) *a = 0;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_SendForceFeedbackCommand(XInputDevice* s, DWORD a) {
    (void)s; (void)a;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_EnumCreatedEffectObjects(XInputDevice* s, void* a, void* b, DWORD c) {
    (void)s; (void)a; (void)b; (void)c;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_Escape(XInputDevice* s, void* a) {
    (void)s; (void)a;
    return DIERR_INVALIDPARAM;
}
static HRESULT_T STDMETHODCALLTYPE xdev_Poll(XInputDevice* s) {
    static int logged = 0;
    if (!logged) { di_log("XInputDevice: Poll [first call]"); logged = 1; }
    (void)s;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_SendDeviceData(XInputDevice* s, DWORD a, const void* b, DWORD* c, DWORD d) {
    (void)s; (void)a; (void)b; (void)c; (void)d;
    return DIERR_INVALIDPARAM;
}
static HRESULT_T STDMETHODCALLTYPE xdev_EnumEffectsInFile(XInputDevice* s, const char* a, void* b, void* c, DWORD d) {
    (void)s; (void)a; (void)b; (void)c; (void)d;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_WriteEffectToFile(XInputDevice* s, const char* a, DWORD b, void* c, DWORD d) {
    (void)s; (void)a; (void)b; (void)c; (void)d;
    return DIERR_INVALIDPARAM;
}

static const XInputDeviceVtbl g_xdev_vtbl = {
    xdev_QueryInterface,
    xdev_AddRef,
    xdev_Release,
    xdev_GetCapabilities,
    xdev_EnumObjects,
    xdev_GetProperty,
    xdev_SetProperty,
    xdev_Acquire,
    xdev_Unacquire,
    xdev_GetDeviceState,
    xdev_GetDeviceData,
    xdev_SetDataFormat,
    xdev_SetEventNotification,
    xdev_SetCooperativeLevel,
    xdev_GetObjectInfo,
    xdev_GetDeviceInfo,
    xdev_RunControlPanel,
    xdev_Initialize,
    xdev_CreateEffect,
    xdev_EnumEffects,
    xdev_GetEffectInfo,
    xdev_GetForceFeedbackState,
    xdev_SendForceFeedbackCommand,
    xdev_EnumCreatedEffectObjects,
    xdev_Escape,
    xdev_Poll,
    xdev_SendDeviceData,
    xdev_EnumEffectsInFile,
    xdev_WriteEffectToFile,
};

static XInputDevice* xinput_device_create(DWORD user_index) {
    XInputDevice* dev = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(XInputDevice));
    if (!dev) return NULL;
    dev->vtbl = &g_xdev_vtbl;
    dev->ref_count = 1;
    dev->user_index = user_index;
    dev->acquired = FALSE;
    dev->data_size = sizeof(DIJOYSTATE);
    /* Default axis ranges — will be overridden by SetProperty(DIPROP_RANGE) */
    for (int i = 0; i < NUM_AXES; i++) {
        dev->axes[i].range_min = -1000;
        dev->axes[i].range_max = 1000;
        dev->axes[i].deadzone_pct = 0;
    }
    di_log("XInputDevice: created (user %lu)", user_index);
    return dev;
}

/* ══════════════════════════════════════════════════════════════════════
 * IDirectInput7A wrapper — intercepts EnumDevices + CreateDevice
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct WrappedDI WrappedDI;
struct WrappedDI {
    void** vtbl;       /* our custom vtable */
    LONG ref_count;
    void* real_di;     /* real IDirectInput7A pointer */
    void** real_vtbl;  /* real vtable (read through real_di) */
};

/* Helper: call real vtable method */
#define REAL_VTBL(w) (*(void***)(w)->real_di)

/* ── Wrapped IDirectInput methods ─────────────────────────────────── */

static HRESULT_T STDMETHODCALLTYPE wdi_QueryInterface(WrappedDI* self, const MY_IID* riid, void** ppv) {
    if (!ppv) return (HRESULT_T)0x80000003L;
    /* Return ourselves for any DInput IID */
    if (guid_eq(riid, &MY_IID_IUnknown) || guid_eq(riid, &IID_IDirectInputA) ||
        guid_eq(riid, &IID_IDirectInput2A) || guid_eq(riid, &IID_IDirectInput7A)) {
        *ppv = self;
        self->ref_count++;
        return DI_OK;
    }
    /* Forward unknown IIDs */
    typedef HRESULT_T (STDMETHODCALLTYPE* FnQI)(void*, const MY_IID*, void**);
    return ((FnQI)REAL_VTBL(self)[DI_VTBL_QI])(self->real_di, riid, ppv);
}

static ULONG STDMETHODCALLTYPE wdi_AddRef(WrappedDI* self) {
    return InterlockedIncrement(&self->ref_count);
}

static ULONG STDMETHODCALLTYPE wdi_Release(WrappedDI* self) {
    LONG r = InterlockedDecrement(&self->ref_count);
    if (r <= 0) {
        /* Release the real interface */
        typedef ULONG (STDMETHODCALLTYPE* FnRelease)(void*);
        ((FnRelease)REAL_VTBL(self)[DI_VTBL_RELEASE])(self->real_di);
        di_log("WrappedDI: destroyed");
        HeapFree(GetProcessHeap(), 0, self->vtbl);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return r;
}

static HRESULT_T STDMETHODCALLTYPE wdi_CreateDevice(WrappedDI* self, const MY_GUID* rguid,
                                                      void** ppDevice, void* pUnkOuter) {
    if (!rguid || !ppDevice) return DIERR_INVALIDPARAM;

    /* Intercept our synthetic XInput GUID */
    if (guid_eq(rguid, &GUID_XInputPad)) {
        di_log("CreateDevice: XInput pad requested — creating virtual device");
        XInputDevice* xdev = xinput_device_create(0);  /* player 1 */
        if (!xdev) return DIERR_INVALIDPARAM;
        *ppDevice = xdev;
        return DI_OK;
    }

    /* Forward to real DI */
    typedef HRESULT_T (STDMETHODCALLTYPE* FnCreate)(void*, const MY_GUID*, void**, void*);
    return ((FnCreate)REAL_VTBL(self)[DI_VTBL_CREATEDEVICE])(self->real_di, rguid, ppDevice, pUnkOuter);
}

static HRESULT_T STDMETHODCALLTYPE wdi_EnumDevices(WrappedDI* self, DWORD devType,
                                                     LPDIENUMDEVICESCALLBACKA callback,
                                                     void* pvRef, DWORD dwFlags) {
    if (!callback) return DIERR_INVALIDPARAM;

    typedef HRESULT_T (STDMETHODCALLTYPE* FnEnum)(void*, DWORD, LPDIENUMDEVICESCALLBACKA, void*, DWORD);

    /* For joystick enumeration: inject our XInput device FIRST so it gets slot 0,
       then skip real DI7 joystick enumeration (legacy DInput Xbox driver is broken
       on modern Windows and would steal the slot). */
    if (devType == DIDEVTYPE_JOYSTICK || devType == 0) {
        if (g_XInputGetState && !(dwFlags & DIEDFL_FORCEFEEDBACK)) {
            XINPUT_STATE xs;
            if (g_XInputGetState(0, &xs) == 0) {
                di_log("EnumDevices: injecting XInput controller as primary device");
                DIDEVICEINSTANCEA di;
                memset(&di, 0, sizeof(di));
                di.dwSize = sizeof(di);
                memcpy(&di.guidInstance, &GUID_XInputPad, sizeof(MY_GUID));
                memcpy(&di.guidProduct, &GUID_XInputPad, sizeof(MY_GUID));
                di.dwDevType = DIDEVTYPE_JOYSTICK | (0x01 << 8);
                strncpy(di.tszInstanceName, "Xbox Controller (XInput)", 259);
                strncpy(di.tszProductName, "Xbox Controller", 259);
                if (callback(&di, pvRef) == DIENUM_STOP) {
                    di_log("EnumDevices: callback returned STOP after XInput injection");
                    return DI_OK;
                }
            }
        } else if (dwFlags & DIEDFL_FORCEFEEDBACK) {
            di_log("EnumDevices: skipping XInput pad (FF-only filter)");
        }

        /* For joystick-only queries, skip real DI7 enumeration entirely —
           the legacy DInput driver for Xbox controllers is broken on Win11 */
        if (devType == DIDEVTYPE_JOYSTICK) {
            return DI_OK;
        }
    }

    /* Forward non-joystick enumeration (keyboard, mouse, all) to real DI */
    HRESULT_T hr = ((FnEnum)REAL_VTBL(self)[DI_VTBL_ENUMDEVICES])(self->real_di, devType, callback, pvRef, dwFlags);
    di_log("EnumDevices: real call returned hr=0x%08lx (type=%lu)", (unsigned long)hr, devType);

    return hr;
}

/* Forwarding helpers for remaining methods */
static HRESULT_T STDMETHODCALLTYPE wdi_GetDeviceStatus(WrappedDI* self, const MY_GUID* rguid) {
    typedef HRESULT_T (STDMETHODCALLTYPE* Fn)(void*, const MY_GUID*);
    return ((Fn)REAL_VTBL(self)[DI_VTBL_GETDEVSTATUS])(self->real_di, rguid);
}

static HRESULT_T STDMETHODCALLTYPE wdi_RunControlPanel(WrappedDI* self, HWND hwnd, DWORD flags) {
    typedef HRESULT_T (STDMETHODCALLTYPE* Fn)(void*, HWND, DWORD);
    return ((Fn)REAL_VTBL(self)[DI_VTBL_RUNCONTROLPANEL])(self->real_di, hwnd, flags);
}

static HRESULT_T STDMETHODCALLTYPE wdi_Initialize(WrappedDI* self, HINSTANCE hinst, DWORD version) {
    typedef HRESULT_T (STDMETHODCALLTYPE* Fn)(void*, HINSTANCE, DWORD);
    return ((Fn)REAL_VTBL(self)[DI_VTBL_INITIALIZE])(self->real_di, hinst, version);
}

static HRESULT_T STDMETHODCALLTYPE wdi_FindDevice(WrappedDI* self, const MY_GUID* rguidClass,
                                                    const char* ptszName, MY_GUID* pguidInstance) {
    typedef HRESULT_T (STDMETHODCALLTYPE* Fn)(void*, const MY_GUID*, const char*, MY_GUID*);
    return ((Fn)REAL_VTBL(self)[DI_VTBL_FINDDEVICE])(self->real_di, rguidClass, ptszName, pguidInstance);
}

static HRESULT_T STDMETHODCALLTYPE wdi_CreateDeviceEx(WrappedDI* self, const MY_GUID* rguid,
                                                        const MY_IID* riid, void** ppvObj,
                                                        void* pUnkOuter) {
    if (rguid && guid_eq(rguid, &GUID_XInputPad)) {
        return wdi_CreateDevice(self, rguid, ppvObj, pUnkOuter);
    }
    typedef HRESULT_T (STDMETHODCALLTYPE* Fn)(void*, const MY_GUID*, const MY_IID*, void**, void*);
    return ((Fn)REAL_VTBL(self)[DI_VTBL_CREATEDEVICEEX])(self->real_di, rguid, riid, ppvObj, pUnkOuter);
}

/* ── Create the wrapped IDirectInput7A ────────────────────────────── */

static WrappedDI* wrap_directinput(void* real_di) {
    WrappedDI* w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WrappedDI));
    if (!w) return NULL;

    void** vtbl = HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * DI_VTBL_COUNT);
    if (!vtbl) { HeapFree(GetProcessHeap(), 0, w); return NULL; }

    vtbl[DI_VTBL_QI]              = (void*)wdi_QueryInterface;
    vtbl[DI_VTBL_ADDREF]          = (void*)wdi_AddRef;
    vtbl[DI_VTBL_RELEASE]         = (void*)wdi_Release;
    vtbl[DI_VTBL_CREATEDEVICE]    = (void*)wdi_CreateDevice;
    vtbl[DI_VTBL_ENUMDEVICES]     = (void*)wdi_EnumDevices;
    vtbl[DI_VTBL_GETDEVSTATUS]    = (void*)wdi_GetDeviceStatus;
    vtbl[DI_VTBL_RUNCONTROLPANEL] = (void*)wdi_RunControlPanel;
    vtbl[DI_VTBL_INITIALIZE]      = (void*)wdi_Initialize;
    vtbl[DI_VTBL_FINDDEVICE]      = (void*)wdi_FindDevice;
    vtbl[DI_VTBL_CREATEDEVICEEX]  = (void*)wdi_CreateDeviceEx;

    w->vtbl = vtbl;
    w->ref_count = 1;
    w->real_di = real_di;
    w->real_vtbl = *(void***)real_di;

    di_log("WrappedDI: created (real=%p)", real_di);
    return w;
}

/* ══════════════════════════════════════════════════════════════════════
 * DLL initialization + exports
 * ══════════════════════════════════════════════════════════════════════ */

static BOOL load_real_dinput(void) {
    if (g_real_dinput) return TRUE;

    WCHAR path[MAX_PATH];
    int len = GetSystemWindowsDirectoryW(path, MAX_PATH);
    if (len <= 0) return FALSE;
    wcscat(path, L"\\SysWOW64\\dinput.dll");
    g_real_dinput = LoadLibraryW(path);

    if (!g_real_dinput) {
        wcscpy(path, L"C:\\Windows\\System32\\dinput.dll");
        g_real_dinput = LoadLibraryW(path);
    }
    if (!g_real_dinput) return FALSE;

    g_real_DirectInputCreateA = (PFN_DirectInputCreateA)GetProcAddress(g_real_dinput, "DirectInputCreateA");
    g_real_DirectInputCreateW = (PFN_DirectInputCreateW)GetProcAddress(g_real_dinput, "DirectInputCreateW");
    g_real_DirectInputCreateEx = (PFN_DirectInputCreateEx)GetProcAddress(g_real_dinput, "DirectInputCreateEx");

    di_log("Real dinput.dll loaded from system (%p)", (void*)g_real_dinput);
    return g_real_DirectInputCreateA != NULL;
}

static void load_xinput(void) {
    if (g_xinput) return;
    g_xinput = LoadLibraryA("xinput1_4.dll");
    if (!g_xinput) g_xinput = LoadLibraryA("xinput1_3.dll");
    if (!g_xinput) g_xinput = LoadLibraryA("xinput9_1_0.dll");
    if (g_xinput) {
        g_XInputGetState = (PFN_XInputGetState)GetProcAddress(g_xinput, "XInputGetState");
        di_log("XInput loaded (%p), GetState=%p", (void*)g_xinput, (void*)g_XInputGetState);
    } else {
        di_log("XInput NOT available — no gamepad support");
    }
}

/* ── Exported functions ───────────────────────────────────────────── */

__declspec(dllexport) HRESULT_T WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD version,
                                                           void** ppDI, void* pUnkOuter) {
    di_log("DirectInputCreateA(version=0x%lx)", version);
    if (!load_real_dinput()) return DIERR_NOTINITIALIZED;

    void* real_di = NULL;
    HRESULT_T hr = g_real_DirectInputCreateA(hinst, version, &real_di, pUnkOuter);
    if (hr != DI_OK || !real_di) {
        di_log("DirectInputCreateA: real call failed (0x%08lx)", (unsigned long)hr);
        return hr;
    }

    WrappedDI* wrapped = wrap_directinput(real_di);
    if (!wrapped) {
        *ppDI = real_di;
    } else {
        *ppDI = wrapped;
    }
    return DI_OK;
}

__declspec(dllexport) HRESULT_T WINAPI DirectInputCreateW(HINSTANCE hinst, DWORD version,
                                                           void** ppDI, void* pUnkOuter) {
    di_log("DirectInputCreateW(version=0x%lx)", version);
    if (!load_real_dinput()) return DIERR_NOTINITIALIZED;
    if (!g_real_DirectInputCreateW) return DIERR_NOTINITIALIZED;
    /* Don't wrap W version — game uses A only */
    return g_real_DirectInputCreateW(hinst, version, ppDI, pUnkOuter);
}

__declspec(dllexport) HRESULT_T WINAPI DirectInputCreateEx(HINSTANCE hinst, DWORD version,
                                                            const MY_IID* riid, void** ppvOut,
                                                            void* pUnkOuter) {
    di_log("DirectInputCreateEx(version=0x%lx)", version);
    if (!load_real_dinput()) return DIERR_NOTINITIALIZED;
    if (!g_real_DirectInputCreateEx) return DIERR_NOTINITIALIZED;

    void* real_di = NULL;
    HRESULT_T hr = g_real_DirectInputCreateEx(hinst, version, riid, &real_di, pUnkOuter);
    if (hr != DI_OK || !real_di) return hr;

    WrappedDI* wrapped = wrap_directinput(real_di);
    if (!wrapped) {
        *ppvOut = real_di;
    } else {
        *ppvOut = wrapped;
    }
    return DI_OK;
}

__declspec(dllexport) HRESULT_T WINAPI DllCanUnloadNow(void) { return 1; /* S_FALSE */ }
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    (void)rclsid; (void)riid; (void)ppv;
    return (HRESULT)0x80040111; /* CLASS_E_CLASSNOTAVAILABLE */
}
__declspec(dllexport) HRESULT_T WINAPI DllRegisterServer(void) { return DI_OK; }
__declspec(dllexport) HRESULT_T WINAPI DllUnregisterServer(void) { return DI_OK; }

/* ── DllMain ──────────────────────────────────────────────────────── */

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        di_log_init();
        di_log("=== dinput proxy DLL loaded (PID %lu) ===", GetCurrentProcessId());
        load_xinput();
    } else if (reason == DLL_PROCESS_DETACH) {
        di_log("=== dinput proxy DLL unloading ===");
        di_log_close();
        if (g_real_dinput) { FreeLibrary(g_real_dinput); g_real_dinput = NULL; }
        if (g_xinput) { FreeLibrary(g_xinput); g_xinput = NULL; }
    }
    return TRUE;
}

/*
 * dinput.dll proxy — SDL3 gamepad support for Secret Agent Barbie
 *
 * Forwards keyboard/mouse DInput calls to real system dinput.dll.
 * Adds the first SDL3 gamepad (Xbox, PlayStation, Switch, generic) as a
 * virtual DI7 joystick. SDL3.dll is loaded at runtime from the game folder.
 *
 * Build: gcc -m32 -shared -DINITGUID -o dinput.dll src/dinput_main.c
 *        -Iinclude -lole32 -luuid dinput.def -Wl,--enable-stdcall-fixup
 */

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ── Logging (debug builds only) ──────────────────────────────────── */

#define SHIM_LOG_NAME "dinput_proxy.log"
#include "shim_log.h"
FILE* g_log_file = NULL;
bool g_log_suspended = false;

/* ── DInput / pad types (avoid SDK headers) ───────────────────────── */

typedef struct { unsigned long Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } MY_GUID;
typedef MY_GUID MY_IID;
typedef long HRESULT_T;

#define DI_OK                   0
#define DIERR_INVALIDPARAM      ((HRESULT_T)0x80070057)
#define DIERR_NOTINITIALIZED    ((HRESULT_T)0x80070015)
#define DIERR_NOTACQUIRED       ((HRESULT_T)0x8007001E)
#define DIERR_NOTFOUND          ((HRESULT_T)0x80070002)
#define DIERR_NOINTERFACE       ((HRESULT_T)0x80004002)
#define E_POINTER_T             ((HRESULT_T)0x80004003)
#define E_NOTIMPL_T             ((HRESULT_T)0x80004001)
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

/* Pad state in XInput layout (Y axes up-positive), filled from SDL3 */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger;
                 SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PadState;

/* PadState button masks (XInput values) */
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

/* Axis range config (used by GamepadDevice and pad_read_joystate) */
typedef struct { LONG range_min, range_max; DWORD deadzone_pct; } AxisConfig;
#define NUM_AXES 6

typedef struct {
    BOOL space_down;
    BOOL escape_down;
    BOOL mouse_left_down;
    LARGE_INTEGER last_mouse_move;
    float mouse_carry_x;
    float mouse_carry_y;
} SyntheticInputState;

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

/* ── Synthetic GUID for our virtual gamepad ───────────────────────── */

static const MY_GUID GUID_VirtualGamepad = {
    0x584E5055, 0x5400, 0x0001,
    { 0x53, 0x41, 0x42, 0x41, 0x52, 0x42, 0x49, 0x45 }  /* "SABARBIE" */
};

/* IID_IDirectInputA through IID_IDirectInput7A (for QI) */
static const MY_GUID IID_IDirectInputA  = { 0x89521360, 0xAA8A, 0x11CF, {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00} };
static const MY_GUID IID_IDirectInput2A = { 0x5944E662, 0xAA8A, 0x11CF, {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00} };
static const MY_GUID IID_IDirectInput7A = { 0x9A4CB684, 0x236D, 0x11D3, {0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE} };
static const MY_GUID IID_IDirectInputDeviceA  = { 0x5944E680, 0xC92E, 0x11CF, {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00} };
static const MY_GUID IID_IDirectInputDevice2A = { 0x5944E682, 0xC92E, 0x11CF, {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00} };
static const MY_GUID IID_IDirectInputDevice7A = { 0x57D7C6BC, 0x2356, 0x11D3, {0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE} };
static const MY_GUID MY_IID_IUnknown    = { 0x00000000, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46} };

/* ── Globals ──────────────────────────────────────────────────────── */

static HMODULE g_real_dinput = NULL;

typedef HRESULT_T (WINAPI* PFN_DirectInputCreateA)(HINSTANCE, DWORD, void**, void*);
typedef HRESULT_T (WINAPI* PFN_DirectInputCreateW)(HINSTANCE, DWORD, void**, void*);
typedef HRESULT_T (WINAPI* PFN_DirectInputCreateEx)(HINSTANCE, DWORD, const MY_IID*, void**, void*);
static PFN_DirectInputCreateA g_real_DirectInputCreateA = NULL;
static PFN_DirectInputCreateW g_real_DirectInputCreateW = NULL;
static PFN_DirectInputCreateEx g_real_DirectInputCreateEx = NULL;

static int guid_eq(const MY_GUID* a, const MY_GUID* b) {
    return memcmp(a, b, sizeof(MY_GUID)) == 0;
}

/* ── SDL3 gamepad backend ─────────────────────────────────────────── */

/* Declarations match the SDL 3.4 headers: __cdecl calls and a one-byte bool. */
typedef struct SDL_Gamepad SDL_Gamepad;
#define SDL_INIT_GAMEPAD 0x00002000u
enum {
    SDL_BUTTON_SOUTH, SDL_BUTTON_EAST, SDL_BUTTON_WEST, SDL_BUTTON_NORTH,
    SDL_BUTTON_BACK, SDL_BUTTON_GUIDE, SDL_BUTTON_START, SDL_BUTTON_LEFT_STICK,
    SDL_BUTTON_RIGHT_STICK, SDL_BUTTON_LEFT_SHOULDER, SDL_BUTTON_RIGHT_SHOULDER,
    SDL_BUTTON_DPAD_UP, SDL_BUTTON_DPAD_DOWN, SDL_BUTTON_DPAD_LEFT, SDL_BUTTON_DPAD_RIGHT
};
enum {
    SDL_AXIS_LEFTX, SDL_AXIS_LEFTY, SDL_AXIS_RIGHTX, SDL_AXIS_RIGHTY,
    SDL_AXIS_LEFT_TRIGGER, SDL_AXIS_RIGHT_TRIGGER
};

static struct {
    bool (__cdecl* Init)(uint32_t flags);
    const char* (__cdecl* GetError)(void);
    bool (__cdecl* SetHint)(const char* name, const char* value);
    void (__cdecl* SetGamepadEventsEnabled)(bool enabled);
    void (__cdecl* UpdateGamepads)(void);
    uint32_t* (__cdecl* GetGamepads)(int* count);
    void (__cdecl* free)(void* memory);
    SDL_Gamepad* (__cdecl* OpenGamepad)(uint32_t instance_id);
    bool (__cdecl* GamepadConnected)(SDL_Gamepad* gamepad);
    void (__cdecl* CloseGamepad)(SDL_Gamepad* gamepad);
    int16_t (__cdecl* GetGamepadAxis)(SDL_Gamepad* gamepad, int axis);
    bool (__cdecl* GetGamepadButton)(SDL_Gamepad* gamepad, int button);
} g_sdl;
static bool g_sdl_ready = false;
static SDL_Gamepad* g_gamepad = NULL;

static void load_sdl(void) {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    char path[MAX_PATH];
    shim_game_path(path, "SDL3.dll");
    HMODULE sdl = LoadLibraryA(path);
    if (!sdl) {
        shim_log("SDL3.dll not found beside the game; gamepad support disabled");
        return;
    }
    const struct { const char* name; void** slot; } imports[] = {
        {"SDL_Init", (void**)&g_sdl.Init},
        {"SDL_GetError", (void**)&g_sdl.GetError},
        {"SDL_SetHint", (void**)&g_sdl.SetHint},
        {"SDL_SetGamepadEventsEnabled", (void**)&g_sdl.SetGamepadEventsEnabled},
        {"SDL_UpdateGamepads", (void**)&g_sdl.UpdateGamepads},
        {"SDL_GetGamepads", (void**)&g_sdl.GetGamepads},
        {"SDL_free", (void**)&g_sdl.free},
        {"SDL_OpenGamepad", (void**)&g_sdl.OpenGamepad},
        {"SDL_GamepadConnected", (void**)&g_sdl.GamepadConnected},
        {"SDL_CloseGamepad", (void**)&g_sdl.CloseGamepad},
        {"SDL_GetGamepadAxis", (void**)&g_sdl.GetGamepadAxis},
        {"SDL_GetGamepadButton", (void**)&g_sdl.GetGamepadButton},
    };
    for (size_t i = 0; i < sizeof(imports) / sizeof(imports[0]); ++i) {
        *imports[i].slot = (void*)GetProcAddress(sdl, imports[i].name);
        if (!*imports[i].slot) {
            shim_log("SDL3.dll lacks %s; gamepad support disabled", imports[i].name);
            return;
        }
    }
    /* SDL owns no window here, so focus must never mute pad input. */
    g_sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    if (!g_sdl.Init(SDL_INIT_GAMEPAD)) {
        shim_log("SDL_Init failed: %s", g_sdl.GetError());
        return;
    }
    /* Nothing pumps SDL events in this process; state is polled instead. */
    g_sdl.SetGamepadEventsEnabled(false);
    g_sdl_ready = true;
    shim_log("SDL3 gamepad backend ready");
}

/* Returns the first connected gamepad, reopening after a disconnect. */
static SDL_Gamepad* current_gamepad(void) {
    if (!g_sdl_ready) return NULL;
    g_sdl.UpdateGamepads();
    if (g_gamepad && g_sdl.GamepadConnected(g_gamepad)) return g_gamepad;
    if (g_gamepad) {
        g_sdl.CloseGamepad(g_gamepad);
        g_gamepad = NULL;
        shim_log("SDL gamepad disconnected");
    }
    int count = 0;
    uint32_t* ids = g_sdl.GetGamepads(&count);
    for (int i = 0; ids && i < count && !g_gamepad; ++i) {
        g_gamepad = g_sdl.OpenGamepad(ids[i]);
    }
    g_sdl.free(ids);
    if (g_gamepad) shim_log("SDL gamepad opened (%d connected)", count);
    return g_gamepad;
}

/* SDL sticks are down-positive; PadState keeps XInput's up-positive Y. */
static SHORT flip_axis(int16_t value) {
    return value == -32768 ? 32767 : (SHORT)-value;
}

/* SDL triggers run 0..32767; PadState triggers run 0..255. */
static BYTE trigger_byte(int16_t value) {
    return value <= 0 ? 0 : (BYTE)(value * 255 / 32767);
}

static BOOL read_pad(PadState* pad) {
    static const struct { int button; WORD mask; } buttons[] = {
        {SDL_BUTTON_SOUTH, XBTN_A}, {SDL_BUTTON_EAST, XBTN_B},
        {SDL_BUTTON_WEST, XBTN_X}, {SDL_BUTTON_NORTH, XBTN_Y},
        {SDL_BUTTON_BACK, XBTN_BACK}, {SDL_BUTTON_START, XBTN_START},
        {SDL_BUTTON_LEFT_STICK, XBTN_LTHUMB}, {SDL_BUTTON_RIGHT_STICK, XBTN_RTHUMB},
        {SDL_BUTTON_LEFT_SHOULDER, XBTN_LSHOULDER},
        {SDL_BUTTON_RIGHT_SHOULDER, XBTN_RSHOULDER},
        {SDL_BUTTON_DPAD_UP, XBTN_DPAD_UP}, {SDL_BUTTON_DPAD_DOWN, XBTN_DPAD_DOWN},
        {SDL_BUTTON_DPAD_LEFT, XBTN_DPAD_LEFT}, {SDL_BUTTON_DPAD_RIGHT, XBTN_DPAD_RIGHT},
    };
    SDL_Gamepad* gamepad = current_gamepad();
    if (!gamepad) return FALSE;

    memset(pad, 0, sizeof(*pad));
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        if (g_sdl.GetGamepadButton(gamepad, buttons[i].button)) pad->wButtons |= buttons[i].mask;
    }
    pad->sThumbLX = g_sdl.GetGamepadAxis(gamepad, SDL_AXIS_LEFTX);
    pad->sThumbLY = flip_axis(g_sdl.GetGamepadAxis(gamepad, SDL_AXIS_LEFTY));
    pad->sThumbRX = g_sdl.GetGamepadAxis(gamepad, SDL_AXIS_RIGHTX);
    pad->sThumbRY = flip_axis(g_sdl.GetGamepadAxis(gamepad, SDL_AXIS_RIGHTY));
    pad->bLeftTrigger = trigger_byte(g_sdl.GetGamepadAxis(gamepad, SDL_AXIS_LEFT_TRIGGER));
    pad->bRightTrigger = trigger_byte(g_sdl.GetGamepadAxis(gamepad, SDL_AXIS_RIGHT_TRIGGER));
    return TRUE;
}

/* ── Apply deadzone to a thumbstick ───────────────────────────────── */

static SHORT apply_deadzone(SHORT val, SHORT dz) {
    if (dz < 0) dz = 0;
    if (dz >= 32767) dz = 32766;
    if (val > dz) return (SHORT)(((val - dz) * 32767L) / (32767 - dz));
    if (val < -dz) return (SHORT)(((val + dz) * 32767L) / (32767 - dz));
    return 0;
}

static SHORT configured_deadzone(const AxisConfig* axis) {
    DWORD pct = axis->deadzone_pct > 10000 ? 10000 : axis->deadzone_pct;
    return (SHORT)((pct * 32767UL) / 10000UL);
}

/* ── PadState → DIJOYSTATE mapping ────────────────────────────────── */

/* Scale a normalized value (-32768..32767) to the configured axis range */
static LONG scale_axis(LONG raw, LONG rmin, LONG rmax) {
    /* raw is -32768..32767, scale to rmin..rmax */
    /* center = (rmin+rmax)/2, half_range = (rmax-rmin)/2 */
    LONG center = (rmin + rmax) / 2;
    LONG half = (rmax - rmin) / 2;
    LONG scaled = center + (LONG)((int64_t)raw * half / 32767);
    if (scaled < rmin) return rmin;
    if (scaled > rmax) return rmax;
    return scaled;
}

/* Scale a positive value (0..255) to the configured axis range */
static LONG scale_trigger(BYTE raw, LONG rmin, LONG rmax) {
    return rmin + (LONG)((int64_t)raw * (rmax - rmin) / 255);
}

/* Full deflection moves the cursor 900 px/s (the old 15 px per poll at 60 Hz). */
#define MOUSE_PIXELS_PER_SECOND 900.0f

/* Converts stick travel over elapsed time to whole pixels, keeping the fraction. */
static int stick_mouse_pixels(float* carry, int value, float seconds) {
    *carry += (float)value * (MOUSE_PIXELS_PER_SECOND / 32767.0f) * seconds;
    int pixels = (int)*carry;
    *carry -= (float)pixels;
    return pixels;
}

/* Inject mouse movement from a stick via SendInput, independent of poll rate */
static void inject_mouse_from_stick(SyntheticInputState* state, SHORT raw_x, SHORT raw_y,
                                    SHORT deadzone_x, SHORT deadzone_y) {
    LARGE_INTEGER now;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float seconds = state->last_mouse_move.QuadPart == 0 ? 0.0f
        : (float)(now.QuadPart - state->last_mouse_move.QuadPart) / (float)frequency.QuadPart;
    state->last_mouse_move = now;
    if (seconds > 0.1f) seconds = 0.1f;  /* no cursor jump after a stall */

    int mx = stick_mouse_pixels(&state->mouse_carry_x, apply_deadzone(raw_x, deadzone_x), seconds);
    /* invert Y: stick up → mouse up */
    int my = stick_mouse_pixels(&state->mouse_carry_y, -apply_deadzone(raw_y, deadzone_y), seconds);
    if (mx == 0 && my == 0) return;

    INPUT inp;
    memset(&inp, 0, sizeof(inp));
    inp.type = INPUT_MOUSE;
    inp.mi.dx = mx;
    inp.mi.dy = my;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &inp, sizeof(INPUT));
}

static BOOL inject_key(WORD scan, BOOL pressed) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scan;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (pressed ? 0 : KEYEVENTF_KEYUP);
    return SendInput(1, &input, sizeof(input)) == 1;
}

static BOOL inject_mouse_left(BOOL pressed) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    return SendInput(1, &input, sizeof(input)) == 1;
}

static void set_synthetic_input(SyntheticInputState* state, BOOL space_down,
                                BOOL escape_down, BOOL mouse_left_down) {
    if (state->space_down != space_down) {
        if (inject_key(0x39, space_down)) state->space_down = space_down;
    }
    if (state->escape_down != escape_down) {
        if (inject_key(0x01, escape_down)) state->escape_down = escape_down;
    }
    if (state->mouse_left_down != mouse_left_down) {
        if (inject_mouse_left(mouse_left_down)) state->mouse_left_down = mouse_left_down;
    }
}

static void release_synthetic_input(SyntheticInputState* state) {
    set_synthetic_input(state, FALSE, FALSE, FALSE);
    state->last_mouse_move.QuadPart = 0;
}

static BOOL pad_read_joystate(const AxisConfig axes[NUM_AXES],
                              SyntheticInputState* synthetic, BOOL allow_injection,
                              DIJOYSTATE* js) {
    PadState pad;
    if (!read_pad(&pad)) {
        release_synthetic_input(synthetic);
        return FALSE;
    }

    memset(js, 0, sizeof(*js));
    PadState* gp = &pad;
    WORD btn = gp->wButtons;

    if (allow_injection) {
        inject_mouse_from_stick(synthetic, gp->sThumbRX, gp->sThumbRY,
                                configured_deadzone(&axes[3]),
                                configured_deadzone(&axes[4]));
    } else {
        release_synthetic_input(synthetic);
    }

    /* Left stick → movement axes (lX/lY) — analog */
    SHORT ls_x = apply_deadzone(gp->sThumbLX, configured_deadzone(&axes[0]));
    SHORT ls_y = apply_deadzone(gp->sThumbLY, configured_deadzone(&axes[1]));

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
        js->lY = scale_axis(-(LONG)ls_y, axes[1].range_min, axes[1].range_max);
    }

    /* Right stick also on lRx/lRy for any game code that reads those */
    js->lRx = scale_axis(apply_deadzone(gp->sThumbRX, configured_deadzone(&axes[3])), axes[3].range_min, axes[3].range_max);
    js->lRy = scale_axis(-apply_deadzone(gp->sThumbRY, configured_deadzone(&axes[4])), axes[4].range_min, axes[4].range_max);

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

    if (allow_injection) {
        set_synthetic_input(synthetic,
                            (btn & XBTN_A) != 0,
                            (btn & XBTN_START) != 0,
                            (btn & (XBTN_B | XBTN_X)) != 0 || rt > 128);
    }

    return TRUE;
}

/* ══════════════════════════════════════════════════════════════════════
 * IDirectInputDevice7A wrapper — SDL3-backed virtual joystick
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct GamepadDevice GamepadDevice;

/* COM vtable — 29 methods for IDirectInputDevice7A */
typedef struct {
    HRESULT_T (STDMETHODCALLTYPE* QueryInterface)(GamepadDevice*, const MY_IID*, void**);
    ULONG     (STDMETHODCALLTYPE* AddRef)(GamepadDevice*);
    ULONG     (STDMETHODCALLTYPE* Release)(GamepadDevice*);
    HRESULT_T (STDMETHODCALLTYPE* GetCapabilities)(GamepadDevice*, DIDEVCAPS*);
    HRESULT_T (STDMETHODCALLTYPE* EnumObjects)(GamepadDevice*, void*, void*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* GetProperty)(GamepadDevice*, const MY_GUID*, void*);
    HRESULT_T (STDMETHODCALLTYPE* SetProperty)(GamepadDevice*, const MY_GUID*, const void*);
    HRESULT_T (STDMETHODCALLTYPE* Acquire)(GamepadDevice*);
    HRESULT_T (STDMETHODCALLTYPE* Unacquire)(GamepadDevice*);
    HRESULT_T (STDMETHODCALLTYPE* GetDeviceState)(GamepadDevice*, DWORD, void*);
    HRESULT_T (STDMETHODCALLTYPE* GetDeviceData)(GamepadDevice*, DWORD, void*, DWORD*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* SetDataFormat)(GamepadDevice*, const DIDATAFORMAT*);
    HRESULT_T (STDMETHODCALLTYPE* SetEventNotification)(GamepadDevice*, HANDLE);
    HRESULT_T (STDMETHODCALLTYPE* SetCooperativeLevel)(GamepadDevice*, HWND, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* GetObjectInfo)(GamepadDevice*, void*, DWORD, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* GetDeviceInfo)(GamepadDevice*, void*);
    HRESULT_T (STDMETHODCALLTYPE* RunControlPanel)(GamepadDevice*, HWND, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* Initialize)(GamepadDevice*, HINSTANCE, DWORD, const MY_GUID*);
    /* IDirectInputDevice2A */
    HRESULT_T (STDMETHODCALLTYPE* CreateEffect)(GamepadDevice*, const MY_GUID*, const void*, void**, void*);
    HRESULT_T (STDMETHODCALLTYPE* EnumEffects)(GamepadDevice*, void*, void*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* GetEffectInfo)(GamepadDevice*, void*, const MY_GUID*);
    HRESULT_T (STDMETHODCALLTYPE* GetForceFeedbackState)(GamepadDevice*, DWORD*);
    HRESULT_T (STDMETHODCALLTYPE* SendForceFeedbackCommand)(GamepadDevice*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* EnumCreatedEffectObjects)(GamepadDevice*, void*, void*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* Escape)(GamepadDevice*, void*);
    HRESULT_T (STDMETHODCALLTYPE* Poll)(GamepadDevice*);
    HRESULT_T (STDMETHODCALLTYPE* SendDeviceData)(GamepadDevice*, DWORD, const void*, DWORD*, DWORD);
    /* IDirectInputDevice7A */
    HRESULT_T (STDMETHODCALLTYPE* EnumEffectsInFile)(GamepadDevice*, const char*, void*, void*, DWORD);
    HRESULT_T (STDMETHODCALLTYPE* WriteEffectToFile)(GamepadDevice*, const char*, DWORD, void*, DWORD);
} GamepadDeviceVtbl;

/* DIPROP_* are small integers, not real GUID pointers */
#define DIPROP_RANGE     ((const MY_GUID*)(uintptr_t)4)
#define DIPROP_DEADZONE  ((const MY_GUID*)(uintptr_t)5)
#define IS_DIPROP(g)     ((uintptr_t)(g) < 0x1000)

/* DIPH_* dwHow values */
#define DIPH_DEVICE    0
#define DIPH_BYOFFSET  1
#define DIPH_BYID      2

/* Property header */
typedef struct { DWORD dwSize, dwHeaderSize, dwObj, dwHow; } DIPROPHEADER;
typedef struct { DIPROPHEADER diph; LONG lMin, lMax; } DIPROPRANGE;
typedef struct { DIPROPHEADER diph; DWORD dwData; } DIPROPDWORD;

struct GamepadDevice {
    const GamepadDeviceVtbl* vtbl;
    LONG ref_count;
    BOOL acquired;
    HWND hwnd;
    SyntheticInputState synthetic;
    AxisConfig axes[NUM_AXES];  /* indexed by offset/4: 0=lX,1=lY,2=lZ,3=lRx,4=lRy,5=lRz */
};

/* ── Virtual gamepad method implementations ───────────────────────── */

static HRESULT_T STDMETHODCALLTYPE xdev_QueryInterface(GamepadDevice* self, const MY_IID* riid, void** ppv) {
    shim_log("GamepadDevice: QueryInterface(iid=%08lx-%04x-%04x) ppv=%p self=%p",
           riid ? riid->Data1 : 0, riid ? riid->Data2 : 0, riid ? riid->Data3 : 0,
           (void*)ppv, (void*)self);
    if (!ppv || !riid) return E_POINTER_T;
    if (!guid_eq(riid, &MY_IID_IUnknown) &&
        !guid_eq(riid, &IID_IDirectInputDeviceA) &&
        !guid_eq(riid, &IID_IDirectInputDevice2A) &&
        !guid_eq(riid, &IID_IDirectInputDevice7A)) {
        *ppv = NULL;
        return DIERR_NOINTERFACE;
    }
    *ppv = self;
    LONG refs = InterlockedIncrement(&self->ref_count);
    shim_log("GamepadDevice: QI wrote *ppv=%p refcount=%ld", (void*)*ppv, refs);
    (void)refs;
    return DI_OK;
}

static ULONG STDMETHODCALLTYPE xdev_AddRef(GamepadDevice* self) {
    return InterlockedIncrement(&self->ref_count);
}

static ULONG STDMETHODCALLTYPE xdev_Release(GamepadDevice* self) {
    LONG r = InterlockedDecrement(&self->ref_count);
    if (r <= 0) {
        release_synthetic_input(&self->synthetic);
        shim_log("GamepadDevice: destroyed");
        HeapFree(GetProcessHeap(), 0, self);
    }
    return r;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetCapabilities(GamepadDevice* self, DIDEVCAPS* caps) {
    shim_log("GamepadDevice: GetCapabilities");
    (void)self;
    if (!caps) return DIERR_INVALIDPARAM;
    DWORD size = caps->dwSize;
    if (size != 6 * sizeof(DWORD) && size != sizeof(DIDEVCAPS)) {
        return DIERR_INVALIDPARAM;
    }
    memset(caps, 0, size);
    caps->dwSize = size;
    caps->dwFlags = 0;
    caps->dwDevType = DIDEVTYPE_JOYSTICK | (0x01 << 8);  /* subtype: gamepad */
    caps->dwAxes = 6;      /* lX, lY, lZ, lRx, lRy, lRz */
    caps->dwButtons = 10;  /* A B X Y LB RB Back Start LS RS */
    caps->dwPOVs = 1;      /* d-pad */
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_EnumObjects(GamepadDevice* s, void* cb, void* ref, DWORD fl) {
    shim_log("GamepadDevice: EnumObjects(flags=0x%lx)", fl);
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

static HRESULT_T STDMETHODCALLTYPE xdev_GetProperty(GamepadDevice* self, const MY_GUID* g, void* h) {
    shim_log("GamepadDevice: GetProperty(prop=%lu)", IS_DIPROP(g) ? (unsigned long)(uintptr_t)g : 0);
    if (!h) return DIERR_INVALIDPARAM;
    DIPROPHEADER* hdr = (DIPROPHEADER*)h;
    if (hdr->dwHeaderSize != sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;

    DWORD idx = 0;
    if (hdr->dwHow == DIPH_BYOFFSET) {
        if (hdr->dwObj % 4 != 0 || hdr->dwObj / 4 >= (DWORD)NUM_AXES)
            return DIERR_INVALIDPARAM;
        idx = hdr->dwObj / 4;
    } else if (hdr->dwHow != DIPH_DEVICE) {
        return E_NOTIMPL_T;
    }

    if (g == DIPROP_RANGE) {
        DIPROPRANGE* pr = (DIPROPRANGE*)h;
        if (hdr->dwSize != sizeof(DIPROPRANGE)) return DIERR_INVALIDPARAM;
        pr->lMin = self->axes[idx].range_min;
        pr->lMax = self->axes[idx].range_max;
        return DI_OK;
    }

    if (g == DIPROP_DEADZONE) {
        DIPROPDWORD* pd = (DIPROPDWORD*)h;
        if (hdr->dwSize != sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        pd->dwData = self->axes[idx].deadzone_pct;
        return DI_OK;
    }

    return E_NOTIMPL_T;
}

static HRESULT_T STDMETHODCALLTYPE xdev_SetProperty(GamepadDevice* self, const MY_GUID* g, const void* h) {
    if (!h) return DIERR_INVALIDPARAM;
    const DIPROPHEADER* hdr = (const DIPROPHEADER*)h;
    if (hdr->dwHeaderSize != sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;

    if (g == DIPROP_RANGE) {
        const DIPROPRANGE* pr = (const DIPROPRANGE*)h;
        shim_log("GamepadDevice: SetProperty DIPROP_RANGE obj=%lu how=%lu min=%ld max=%ld",
               hdr->dwObj, hdr->dwHow, pr->lMin, pr->lMax);
        if (hdr->dwSize != sizeof(DIPROPRANGE) || pr->lMin >= pr->lMax)
            return DIERR_INVALIDPARAM;
        if (hdr->dwHow == DIPH_DEVICE) {
            /* Apply to all axes */
            for (int i = 0; i < NUM_AXES; i++) {
                self->axes[i].range_min = pr->lMin;
                self->axes[i].range_max = pr->lMax;
            }
        } else if (hdr->dwHow == DIPH_BYOFFSET && hdr->dwObj % 4 == 0 &&
                   hdr->dwObj / 4 < (DWORD)NUM_AXES) {
            self->axes[hdr->dwObj / 4].range_min = pr->lMin;
            self->axes[hdr->dwObj / 4].range_max = pr->lMax;
        } else {
            return E_NOTIMPL_T;
        }
        return DI_OK;
    }

    if (g == DIPROP_DEADZONE) {
        const DIPROPDWORD* pd = (const DIPROPDWORD*)h;
        shim_log("GamepadDevice: SetProperty DIPROP_DEADZONE obj=%lu how=%lu val=%lu",
               hdr->dwObj, hdr->dwHow, pd->dwData);
        if (hdr->dwSize != sizeof(DIPROPDWORD) || pd->dwData > 10000)
            return DIERR_INVALIDPARAM;
        if (hdr->dwHow == DIPH_DEVICE) {
            for (int i = 0; i < NUM_AXES; i++)
                self->axes[i].deadzone_pct = pd->dwData;
        } else if (hdr->dwHow == DIPH_BYOFFSET && hdr->dwObj % 4 == 0 &&
                   hdr->dwObj / 4 < (DWORD)NUM_AXES) {
            self->axes[hdr->dwObj / 4].deadzone_pct = pd->dwData;
        } else {
            return E_NOTIMPL_T;
        }
        return DI_OK;
    }

    shim_log("GamepadDevice: SetProperty(prop=%lu)", IS_DIPROP(g) ? (unsigned long)(uintptr_t)g : 0);
    return E_NOTIMPL_T;
}

static HRESULT_T STDMETHODCALLTYPE xdev_Acquire(GamepadDevice* self) {
    self->acquired = TRUE;
    shim_log("GamepadDevice: acquired");
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_Unacquire(GamepadDevice* self) {
    shim_log("GamepadDevice: Unacquire");
    release_synthetic_input(&self->synthetic);
    self->acquired = FALSE;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetDeviceState(GamepadDevice* self, DWORD cbData, void* lpvData) {
    /* Only log first call to avoid flooding */
    static int logged = 0;
    if (!logged) { shim_log("GamepadDevice: GetDeviceState(size=%lu) [first call]", cbData); logged = 1; }
    if (!lpvData) return DIERR_INVALIDPARAM;
    if (!self->acquired) {
        release_synthetic_input(&self->synthetic);
        return DIERR_NOTACQUIRED;
    }
    memset(lpvData, 0, cbData);

    DIJOYSTATE js;
    BOOL allow_injection = self->hwnd != NULL && GetForegroundWindow() == self->hwnd;
    if (!pad_read_joystate(self->axes, &self->synthetic, allow_injection, &js)) {
        memset(&js, 0, sizeof(js));
        js.lX = (self->axes[0].range_min + self->axes[0].range_max) / 2;
        js.lY = (self->axes[1].range_min + self->axes[1].range_max) / 2;
        js.lZ = (self->axes[2].range_min + self->axes[2].range_max) / 2;
        js.lRx = (self->axes[3].range_min + self->axes[3].range_max) / 2;
        js.lRy = (self->axes[4].range_min + self->axes[4].range_max) / 2;
        js.lRz = (self->axes[5].range_min + self->axes[5].range_max) / 2;
        js.rgdwPOV[0] = js.rgdwPOV[1] = js.rgdwPOV[2] = js.rgdwPOV[3] = (DWORD)-1;
    }

    DWORD copy = cbData < sizeof(js) ? cbData : sizeof(js);
    memcpy(lpvData, &js, copy);
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetDeviceData(GamepadDevice* s, DWORD a, void* b, DWORD* c, DWORD d) {
    shim_log("GamepadDevice: GetDeviceData");
    (void)s; (void)a; (void)b; (void)d;
    if (c) *c = 0;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_SetDataFormat(GamepadDevice* self, const DIDATAFORMAT* df) {
    if (!df) return DIERR_INVALIDPARAM;
    (void)self;
    shim_log("GamepadDevice: SetDataFormat size=%lu", df->dwDataSize);
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_SetEventNotification(GamepadDevice* s, HANDLE h) {
    (void)s; (void)h;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_SetCooperativeLevel(GamepadDevice* s, HWND hw, DWORD fl) {
    s->hwnd = hw;
    shim_log("GamepadDevice: SetCooperativeLevel flags=0x%lx", fl);
    (void)fl;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetObjectInfo(GamepadDevice* s, void* a, DWORD b, DWORD c) {
    (void)s; (void)a; (void)b; (void)c;
    return DIERR_INVALIDPARAM;
}

static HRESULT_T fill_gamepad_device_info(DIDEVICEINSTANCEA* info) {
    if (!info) return DIERR_INVALIDPARAM;
    DWORD size = info->dwSize;
    DWORD dx3_size = (DWORD)offsetof(DIDEVICEINSTANCEA, guidFFDriver);
    if (size != dx3_size && size != sizeof(DIDEVICEINSTANCEA)) {
        return DIERR_INVALIDPARAM;
    }
    memset(info, 0, size);
    info->dwSize = size;
    memcpy(&info->guidInstance, &GUID_VirtualGamepad, sizeof(MY_GUID));
    memcpy(&info->guidProduct, &GUID_VirtualGamepad, sizeof(MY_GUID));
    info->dwDevType = DIDEVTYPE_JOYSTICK | (0x01 << 8);
    strncpy(info->tszInstanceName, "Xbox Controller (SDL)", 259);
    strncpy(info->tszProductName, "Xbox Controller", 259);
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_GetDeviceInfo(GamepadDevice* s, void* a) {
    shim_log("GamepadDevice: GetDeviceInfo");
    (void)s;
    return fill_gamepad_device_info((DIDEVICEINSTANCEA*)a);
}

static HRESULT_T STDMETHODCALLTYPE xdev_RunControlPanel(GamepadDevice* s, HWND h, DWORD d) {
    (void)s; (void)h; (void)d;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_Initialize(GamepadDevice* s, HINSTANCE h, DWORD v, const MY_GUID* g) {
    (void)s; (void)h; (void)v; (void)g;
    return DI_OK;
}

static HRESULT_T STDMETHODCALLTYPE xdev_CreateEffect(GamepadDevice* s, const MY_GUID* a, const void* b, void** c, void* d) {
    (void)s; (void)a; (void)b; (void)c; (void)d;
    return DIERR_INVALIDPARAM;
}
static HRESULT_T STDMETHODCALLTYPE xdev_EnumEffects(GamepadDevice* s, void* a, void* b, DWORD c) {
    (void)s; (void)a; (void)b; (void)c;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_GetEffectInfo(GamepadDevice* s, void* a, const MY_GUID* b) {
    (void)s; (void)a; (void)b;
    return DIERR_INVALIDPARAM;
}
static HRESULT_T STDMETHODCALLTYPE xdev_GetForceFeedbackState(GamepadDevice* s, DWORD* a) {
    (void)s; if (a) *a = 0;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_SendForceFeedbackCommand(GamepadDevice* s, DWORD a) {
    (void)s; (void)a;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_EnumCreatedEffectObjects(GamepadDevice* s, void* a, void* b, DWORD c) {
    (void)s; (void)a; (void)b; (void)c;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_Escape(GamepadDevice* s, void* a) {
    (void)s; (void)a;
    return DIERR_INVALIDPARAM;
}
static HRESULT_T STDMETHODCALLTYPE xdev_Poll(GamepadDevice* s) {
    static int logged = 0;
    if (!logged) { shim_log("GamepadDevice: Poll [first call]"); logged = 1; }
    (void)s;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_SendDeviceData(GamepadDevice* s, DWORD a, const void* b, DWORD* c, DWORD d) {
    (void)s; (void)a; (void)b; (void)c; (void)d;
    return DIERR_INVALIDPARAM;
}
static HRESULT_T STDMETHODCALLTYPE xdev_EnumEffectsInFile(GamepadDevice* s, const char* a, void* b, void* c, DWORD d) {
    (void)s; (void)a; (void)b; (void)c; (void)d;
    return DI_OK;
}
static HRESULT_T STDMETHODCALLTYPE xdev_WriteEffectToFile(GamepadDevice* s, const char* a, DWORD b, void* c, DWORD d) {
    (void)s; (void)a; (void)b; (void)c; (void)d;
    return DIERR_INVALIDPARAM;
}

static const GamepadDeviceVtbl g_xdev_vtbl = {
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

static GamepadDevice* gamepad_device_create(void) {
    GamepadDevice* dev = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GamepadDevice));
    if (!dev) return NULL;
    dev->vtbl = &g_xdev_vtbl;
    dev->ref_count = 1;
    dev->acquired = FALSE;
    /* Default axis ranges — will be overridden by SetProperty(DIPROP_RANGE) */
    for (int i = 0; i < NUM_AXES; i++) {
        dev->axes[i].range_min = -1000;
        dev->axes[i].range_max = 1000;
        dev->axes[i].deadzone_pct = (THUMB_DEADZONE * 10000UL) / 32767UL;
    }
    shim_log("GamepadDevice: created");
    return dev;
}

/* ══════════════════════════════════════════════════════════════════════
 * IDirectInput7A wrapper — intercepts EnumDevices + CreateDevice
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct WrappedDI WrappedDI;
struct WrappedDI {
    void** vtbl;       /* our custom vtable */
    LONG ref_count;
    void* real_di;     /* real DirectInput interface pointer */
    DWORD interface_level;
};

/* Helper: call real vtable method */
#define REAL_VTBL(w) (*(void***)(w)->real_di)

/* ── Wrapped IDirectInput methods ─────────────────────────────────── */

static DWORD directinput_iid_level(const MY_IID* riid) {
    if (guid_eq(riid, &IID_IDirectInputA)) return 1;
    if (guid_eq(riid, &IID_IDirectInput2A)) return 2;
    if (guid_eq(riid, &IID_IDirectInput7A)) return 7;
    return 0;
}

static HRESULT_T STDMETHODCALLTYPE wdi_QueryInterface(WrappedDI* self, const MY_IID* riid, void** ppv) {
    if (!ppv || !riid) return E_POINTER_T;
    *ppv = NULL;
    DWORD requested_level = directinput_iid_level(riid);
    if (guid_eq(riid, &MY_IID_IUnknown) ||
        (requested_level != 0 && requested_level <= self->interface_level)) {
        *ppv = self;
        InterlockedIncrement(&self->ref_count);
        return DI_OK;
    }

    if (requested_level != 0) {
        typedef HRESULT_T (STDMETHODCALLTYPE* FnQI)(void*, const MY_IID*, void**);
        void* upgraded = NULL;
        HRESULT_T hr = ((FnQI)REAL_VTBL(self)[DI_VTBL_QI])(self->real_di, riid,
                                                           &upgraded);
        if (hr != DI_OK) return hr;
        if (!upgraded) return DIERR_NOINTERFACE;

        void* old_real = self->real_di;
        self->real_di = upgraded;
        self->interface_level = requested_level;
        typedef ULONG (STDMETHODCALLTYPE* FnRelease)(void*);
        ((FnRelease)(*(void***)old_real)[DI_VTBL_RELEASE])(old_real);

        *ppv = self;
        InterlockedIncrement(&self->ref_count);
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
        shim_log("WrappedDI: destroyed");
        HeapFree(GetProcessHeap(), 0, self->vtbl);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return r;
}

static HRESULT_T STDMETHODCALLTYPE wdi_CreateDevice(WrappedDI* self, const MY_GUID* rguid,
                                                      void** ppDevice, void* pUnkOuter) {
    if (!rguid || !ppDevice) return DIERR_INVALIDPARAM;

    /* Intercept our synthetic gamepad GUID */
    if (guid_eq(rguid, &GUID_VirtualGamepad)) {
        shim_log("CreateDevice: gamepad requested — creating virtual device");
        GamepadDevice* xdev = gamepad_device_create();
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

    /* For joystick enumeration: inject our SDL gamepad FIRST so it gets slot 0,
       then skip real DI7 joystick enumeration (legacy DInput Xbox driver is broken
       on modern Windows and would steal the slot). */
    BOOL injected = FALSE;
    if (devType == DIDEVTYPE_JOYSTICK || devType == 0) {
        if (!(dwFlags & DIEDFL_FORCEFEEDBACK)) {
            if (current_gamepad() != NULL) {
                shim_log("EnumDevices: injecting SDL gamepad as primary device");
                DIDEVICEINSTANCEA di;
                memset(&di, 0, sizeof(di));
                di.dwSize = sizeof(di);
                fill_gamepad_device_info(&di);
                if (callback(&di, pvRef) == DIENUM_STOP) {
                    shim_log("EnumDevices: callback returned STOP after gamepad injection");
                    return DI_OK;
                }
                injected = TRUE;
            }
        } else {
            shim_log("EnumDevices: skipping gamepad (FF-only filter)");
        }

        /* For joystick-only queries, skip real DI7 enumeration entirely —
           the legacy DInput driver for Xbox controllers is broken on Win11 */
        if (devType == DIDEVTYPE_JOYSTICK && injected) {
            return DI_OK;
        }
    }

    /* Forward non-joystick enumeration (keyboard, mouse, all) to real DI */
    HRESULT_T hr = ((FnEnum)REAL_VTBL(self)[DI_VTBL_ENUMDEVICES])(self->real_di, devType, callback, pvRef, dwFlags);
    shim_log("EnumDevices: real call returned hr=0x%08lx (type=%lu)", (unsigned long)hr, devType);

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
    if (rguid && guid_eq(rguid, &GUID_VirtualGamepad)) {
        if (!riid || !ppvObj) return DIERR_INVALIDPARAM;
        *ppvObj = NULL;
        if (!guid_eq(riid, &IID_IDirectInputDeviceA) &&
            !guid_eq(riid, &IID_IDirectInputDevice2A) &&
            !guid_eq(riid, &IID_IDirectInputDevice7A)) {
            return DIERR_NOINTERFACE;
        }
        return wdi_CreateDevice(self, rguid, ppvObj, pUnkOuter);
    }
    typedef HRESULT_T (STDMETHODCALLTYPE* Fn)(void*, const MY_GUID*, const MY_IID*, void**, void*);
    return ((Fn)REAL_VTBL(self)[DI_VTBL_CREATEDEVICEEX])(self->real_di, rguid, riid, ppvObj, pUnkOuter);
}

/* ── Create the wrapped IDirectInput7A ────────────────────────────── */

static WrappedDI* wrap_directinput(void* real_di, DWORD interface_level) {
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
    w->interface_level = interface_level;

    shim_log("WrappedDI: created (real=%p)", real_di);
    return w;
}

/* ══════════════════════════════════════════════════════════════════════
 * DLL initialization + exports
 * ══════════════════════════════════════════════════════════════════════ */

static HMODULE load_system_library(const WCHAR* name) {
    WCHAR path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return NULL;
    if (wcscat_s(path, MAX_PATH, L"\\") != 0 ||
        wcscat_s(path, MAX_PATH, name) != 0) {
        return NULL;
    }
    return LoadLibraryW(path);
}

static BOOL load_real_dinput(void) {
    if (g_real_dinput) return TRUE;

    g_real_dinput = load_system_library(L"dinput.dll");
    if (!g_real_dinput) return FALSE;

    g_real_DirectInputCreateA = (PFN_DirectInputCreateA)GetProcAddress(g_real_dinput, "DirectInputCreateA");
    g_real_DirectInputCreateW = (PFN_DirectInputCreateW)GetProcAddress(g_real_dinput, "DirectInputCreateW");
    g_real_DirectInputCreateEx = (PFN_DirectInputCreateEx)GetProcAddress(g_real_dinput, "DirectInputCreateEx");

    shim_log("Real dinput.dll loaded from system (%p)", (void*)g_real_dinput);
    return g_real_DirectInputCreateA != NULL;
}

/* ── Exported functions ───────────────────────────────────────────── */

__declspec(dllexport) HRESULT_T WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD version,
                                                           void** ppDI, void* pUnkOuter) {
    shim_log("DirectInputCreateA(version=0x%lx)", version);
    if (!ppDI) return DIERR_INVALIDPARAM;
    *ppDI = NULL;
    if (!load_real_dinput()) return DIERR_NOTINITIALIZED;
    load_sdl();

    void* real_di = NULL;
    HRESULT_T hr = g_real_DirectInputCreateA(hinst, version, &real_di, pUnkOuter);
    if (hr != DI_OK || !real_di) {
        shim_log("DirectInputCreateA: real call failed (0x%08lx)", (unsigned long)hr);
        return hr;
    }

    WrappedDI* wrapped = wrap_directinput(real_di, 1);
    if (!wrapped) {
        *ppDI = real_di;
    } else {
        *ppDI = wrapped;
    }
    return DI_OK;
}

__declspec(dllexport) HRESULT_T WINAPI DirectInputCreateW(HINSTANCE hinst, DWORD version,
                                                           void** ppDI, void* pUnkOuter) {
    shim_log("DirectInputCreateW(version=0x%lx)", version);
    if (!ppDI) return DIERR_INVALIDPARAM;
    if (!load_real_dinput()) return DIERR_NOTINITIALIZED;
    if (!g_real_DirectInputCreateW) return DIERR_NOTINITIALIZED;
    /* Don't wrap W version — game uses A only */
    return g_real_DirectInputCreateW(hinst, version, ppDI, pUnkOuter);
}

__declspec(dllexport) HRESULT_T WINAPI DirectInputCreateEx(HINSTANCE hinst, DWORD version,
                                                            const MY_IID* riid, void** ppvOut,
                                                            void* pUnkOuter) {
    shim_log("DirectInputCreateEx(version=0x%lx)", version);
    if (!riid || !ppvOut) return DIERR_INVALIDPARAM;
    *ppvOut = NULL;
    if (!load_real_dinput()) return DIERR_NOTINITIALIZED;
    if (!g_real_DirectInputCreateEx) return DIERR_NOTINITIALIZED;
    load_sdl();

    void* real_di = NULL;
    HRESULT_T hr = g_real_DirectInputCreateEx(hinst, version, riid, &real_di, pUnkOuter);
    if (hr != DI_OK || !real_di) return hr;

    DWORD interface_level = guid_eq(riid, &IID_IDirectInput7A) ? 7 :
                            guid_eq(riid, &IID_IDirectInput2A) ? 2 :
                            guid_eq(riid, &IID_IDirectInputA) ? 1 : 0;
    WrappedDI* wrapped = interface_level ? wrap_directinput(real_di, interface_level) : NULL;
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
    }
    return TRUE;
}

#ifdef DINPUT_LOGIC_TEST
static int test_check(BOOL condition, const char* message) {
    if (condition) return 0;
    fprintf(stderr, "dinput logic test failed: %s\n", message);
    return 1;
}

int main(void) {
    int failed = 0;
    AxisConfig axis = {-1000, 1000, 2500};
    failed += test_check(configured_deadzone(&axis) == 8191,
                         "DIPROP_DEADZONE converts to stick units");
    failed += test_check(scale_axis(-32768, -1000, 1000) == -1000,
                         "negative axis scaling clamps to configured minimum");
    failed += test_check(scale_axis(-(LONG)apply_deadzone((SHORT)-32768, THUMB_DEADZONE),
                                    -1000, 1000) == 1000,
                         "full-down stick maps to the positive Y endpoint");

    GamepadDevice device;
    memset(&device, 0, sizeof(device));
    DIPROPDWORD deadzone = {
        {sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, 2500
    };
    failed += test_check(xdev_SetProperty(&device, DIPROP_DEADZONE, &deadzone) == DI_OK &&
                             configured_deadzone(&device.axes[0]) == 8191,
                         "SetProperty applies device deadzone to every axis");
    DIDEVCAPS caps;
    memset(&caps, 0xA5, sizeof(caps));
    caps.dwSize = sizeof(caps);
    failed += test_check(xdev_GetCapabilities(&device, &caps) == DI_OK,
                         "full DIDEVCAPS is accepted");
    failed += test_check(caps.dwSize == sizeof(caps) && caps.dwAxes == 6 &&
                             caps.dwButtons == 10 && caps.dwPOVs == 1,
                         "GetCapabilities preserves size and fills capabilities");
    caps.dwSize = sizeof(DWORD);
    failed += test_check(xdev_GetCapabilities(&device, &caps) == DIERR_INVALIDPARAM,
                         "invalid DIDEVCAPS size is rejected");

    DIDEVICEINSTANCEA info;
    memset(&info, 0, sizeof(info));
    info.dwSize = sizeof(info);
    failed += test_check(fill_gamepad_device_info(&info) == DI_OK,
                         "full DIDEVICEINSTANCE is accepted");
    failed += test_check(strcmp(info.tszProductName, "Xbox Controller") == 0,
                         "GetDeviceInfo fills the product name");

    MY_GUID unsupported = {0x12345678, 0, 0, {0}};
    void* queried = &device;
    failed += test_check(xdev_QueryInterface(&device, &unsupported, &queried) ==
                             DIERR_NOINTERFACE && queried == NULL,
                         "unsupported device interfaces are rejected");

    failed += test_check(flip_axis(-32768) == 32767 && flip_axis(32767) == -32767 &&
                             flip_axis(0) == 0,
                         "SDL down-positive Y converts to up-positive pad Y");
    failed += test_check(trigger_byte(32767) == 255 && trigger_byte(0) == 0 &&
                             trigger_byte(-1) == 0,
                         "SDL trigger range converts to 0..255");
    float carry_60 = 0.0f;
    float carry_240 = 0.0f;
    int pixels_60 = 0;
    int pixels_240 = 0;
    for (int i = 0; i < 60; ++i) pixels_60 += stick_mouse_pixels(&carry_60, 32767, 1.0f / 60.0f);
    for (int i = 0; i < 240; ++i) pixels_240 += stick_mouse_pixels(&carry_240, 32767, 1.0f / 240.0f);
    failed += test_check(pixels_60 >= 899 && pixels_60 <= 900 &&
                             pixels_240 >= 899 && pixels_240 <= 900,
                         "stick cursor speed does not depend on poll rate");
    float carry_slow = 0.0f;
    int pixels_slow = 0;
    for (int i = 0; i < 240; ++i) pixels_slow += stick_mouse_pixels(&carry_slow, -3277, 1.0f / 240.0f);
    failed += test_check(pixels_slow <= -89 && pixels_slow >= -90,
                         "slow stick motion accumulates instead of rounding to zero");

    if (failed == 0) {
        puts("dinput logic tests passed");
    }
    return failed != 0;
}
#endif

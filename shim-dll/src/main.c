#include <stdbool.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "shim_log.h"

FILE* g_log_file = NULL;
bool g_log_suspended = false;

static HMODULE g_real_ddraw = NULL;
static HMODULE g_advapi = NULL;
static bool g_exports_ready = false;
static bool g_registry_hooks_ready = false;

typedef LONG (WINAPI* PFN_RegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI* PFN_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI* PFN_RegCloseKey)(HKEY);
typedef VOID (WINAPI* PFN_ExitProcess)(UINT);
typedef BOOL (WINAPI* PFN_TerminateProcess)(HANDLE, UINT);
typedef UINT (WINAPI* PFN_GetDriveTypeA)(LPCSTR);
typedef BOOL (WINAPI* PFN_GetVolumeInformationA)(LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD,
                                               LPDWORD, LPSTR, DWORD);
typedef HANDLE (WINAPI* PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                        DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI* PFN_CreateDirectoryA)(LPCSTR, LPSECURITY_ATTRIBUTES);
typedef DWORD (WINAPI* PFN_GetFileAttributesA)(LPCSTR);

static PFN_RegOpenKeyExA g_fn_RegOpenKeyExA = NULL;
static PFN_RegQueryValueExA g_fn_RegQueryValueExA = NULL;
static PFN_RegCloseKey g_fn_RegCloseKey = NULL;
static PFN_ExitProcess g_fn_ExitProcess = NULL;
static PFN_TerminateProcess g_fn_TerminateProcess = NULL;
static PFN_GetDriveTypeA g_fn_GetDriveTypeA = NULL;
static PFN_GetVolumeInformationA g_fn_GetVolumeInformationA = NULL;
static PFN_CreateFileA g_fn_CreateFileA = NULL;
static PFN_CreateDirectoryA g_fn_CreateDirectoryA = NULL;
static PFN_GetFileAttributesA g_fn_GetFileAttributesA = NULL;

#define DDRAWSYM(name) FARPROC g_fn_##name
#define MAX_REDIRECTED_KEYS 8

typedef struct {
  HKEY key;
  bool is_redirected;
} redirected_key_entry_t;

typedef struct {
  const char* name;
  const char* string_value;
  DWORD dword_value;
  DWORD type;
} secretagent_default_t;

DDRAWSYM(AcquireDDThreadLock);
DDRAWSYM(CompleteCreateSysmemSurface);
DDRAWSYM(D3DParseUnknownCommand);
DDRAWSYM(DDGetAttachedSurfaceLcl);
DDRAWSYM(DDInternalLock);
DDRAWSYM(DDInternalUnlock);
DDRAWSYM(DSoundHelp);
DDRAWSYM(DirectDrawCreate);
DDRAWSYM(DirectDrawCreateClipper);
DDRAWSYM(DirectDrawCreateEx);
DDRAWSYM(DirectDrawEnumerateA);
DDRAWSYM(DirectDrawEnumerateExA);
DDRAWSYM(DirectDrawEnumerateExW);
DDRAWSYM(DirectDrawEnumerateW);
DDRAWSYM(DllCanUnloadNow);
DDRAWSYM(DllGetClassObject);
DDRAWSYM(GetDDSurfaceLocal);
DDRAWSYM(GetOLEThunkData);
DDRAWSYM(GetSurfaceFromDC);
DDRAWSYM(RegisterSpecialCase);
DDRAWSYM(ReleaseDDThreadLock);
DDRAWSYM(SetAppCompatData);

static const char kSecretAgentKeyPrefix[] = "SOFTWARE\\Gigawatt Studios\\";
static LONG g_fake_handle_counter = 0;
static char g_secretagent_cdrom[MAX_PATH] = ".\\";
static char g_secretagent_path[MAX_PATH] = ".\\";
static const char kSecretAgentLanguage[] = "ENG";
static const DWORD kSecretAgentSetup = 3;
static redirected_key_entry_t g_redirected_keys[MAX_REDIRECTED_KEYS];

static bool cache_export(const char* name, FARPROC* proc);
static bool cache_reg_export(const char* name, FARPROC* proc);
static bool resolve_real_ddraw(void);
static bool resolve_real_advapi(void);
__attribute__((used, noinline)) bool ensure_exports_loaded(void);
static bool ensure_registry_hooks_loaded(void);
static bool is_secretagent_reg_key(const char* sub_key);
static bool store_redirected_key(HKEY key);
static bool is_redirected_key(HKEY key);
static bool clear_redirected_key(HKEY key);
static bool is_fake_registry_handle(HKEY key);
static HKEY allocate_fake_registry_handle(void);
static bool query_secretagent_default_value(const char* value_name, LPBYTE data,
                                          LPDWORD data_len);
static bool is_fake_cdrom_path(const char* path);
static bool patch_iat_for_hooks(HMODULE module);
static void init_runtime_paths(void);
static UINT WINAPI shim_GetDriveTypeA(LPCSTR lpRootPathName);
static BOOL WINAPI shim_GetVolumeInformationA(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer,
                                             DWORD nVolumeNameSize, LPDWORD lpVolumeSerialNumber,
                                            LPDWORD lpMaximumComponentLength,
                                            LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer,
                                            DWORD nFileSystemNameSize);
static void WINAPI shim_exit_hook(UINT code);
static BOOL WINAPI shim_terminate_process_hook(HANDLE hProcess, UINT code);
static HANDLE WINAPI shim_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess,
                                      DWORD dwShareMode,
                                      LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                      DWORD dwCreationDisposition,
                                      DWORD dwFlagsAndAttributes,
                                      HANDLE hTemplateFile);
static BOOL WINAPI shim_CreateDirectoryA(LPCSTR lpPathName,
                                         LPSECURITY_ATTRIBUTES lpSecurityAttributes);
static DWORD WINAPI shim_GetFileAttributesA(LPCSTR lpFileName);

static void init_runtime_paths(void) {
  char exe_path[MAX_PATH];
  DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);

  if (len == 0 || len >= MAX_PATH) {
    strcpy_s(g_secretagent_cdrom, MAX_PATH, ".\\");
    strcpy_s(g_secretagent_path, MAX_PATH, ".\\");
    return;
  }

  char* slash = strrchr(exe_path, '\\');
  if (slash != NULL) {
    *(slash + 1) = '\0';
  } else {
    strcpy_s(exe_path, MAX_PATH, ".\\");
  }

  strcpy_s(g_secretagent_cdrom, MAX_PATH, exe_path);
  strcpy_s(g_secretagent_path, MAX_PATH, exe_path);
}

static bool cache_export(const char* name, FARPROC* proc) {
  *proc = GetProcAddress(g_real_ddraw, name);
  return *proc != NULL;
}

static bool cache_reg_export(const char* name, FARPROC* proc) {
  *proc = GetProcAddress(g_advapi, name);
  return *proc != NULL;
}

static bool resolve_real_ddraw(void) {
  WCHAR dll_path[MAX_PATH];
  WCHAR exe_path[MAX_PATH];
  int path_len;

  if (g_real_ddraw != NULL) {
    return true;
  }

  if (GetModuleFileNameW(NULL, exe_path, MAX_PATH) > 0) {
    WCHAR* separator = wcsrchr(exe_path, L'\\');
    if (separator != NULL) {
      *(separator + 1) = 0;
      if (wcscpy_s(dll_path, MAX_PATH, exe_path) == 0 &&
          wcscat_s(dll_path, MAX_PATH, L"dgVoodoo_ddraw.dll") == 0) {
        g_real_ddraw = LoadLibraryW(dll_path);
        if (g_real_ddraw != NULL) {
          return true;
        }
      }
    }
  }

  path_len = GetSystemDirectoryW(dll_path, MAX_PATH);
  if (path_len <= 0 || path_len >= MAX_PATH) {
    return false;
  }

  if (wcscat_s(dll_path, MAX_PATH, L"\\ddraw.dll") != 0) {
    return false;
  }

  g_real_ddraw = LoadLibraryW(dll_path);
  if (g_real_ddraw == NULL) {
    return false;
  }

  return true;
}

static bool resolve_real_advapi(void) {
  if (g_advapi != NULL) {
    return true;
  }

  g_advapi = GetModuleHandleW(L"advapi32.dll");
  return g_advapi != NULL;
}

static bool is_secretagent_reg_key(const char* sub_key) {
  if (sub_key == NULL) {
    return false;
  }
  return _strnicmp(sub_key, kSecretAgentKeyPrefix,
                   sizeof(kSecretAgentKeyPrefix) - 1) == 0;
}

static bool store_redirected_key(HKEY key) {
  for (int i = 0; i < MAX_REDIRECTED_KEYS; ++i) {
    if (!g_redirected_keys[i].is_redirected) {
      g_redirected_keys[i].key = key;
      g_redirected_keys[i].is_redirected = true;
      return true;
    }
  }
  return false;
}

static bool is_redirected_key(HKEY key) {
  for (int i = 0; i < MAX_REDIRECTED_KEYS; ++i) {
    if (g_redirected_keys[i].is_redirected && g_redirected_keys[i].key == key) {
      return true;
    }
  }
  return false;
}

static bool clear_redirected_key(HKEY key) {
  for (int i = 0; i < MAX_REDIRECTED_KEYS; ++i) {
    if (g_redirected_keys[i].is_redirected && g_redirected_keys[i].key == key) {
      g_redirected_keys[i].is_redirected = false;
      g_redirected_keys[i].key = NULL;
      return true;
    }
  }
  return false;
}

static bool is_fake_registry_handle(HKEY key) {
  uintptr_t value = (uintptr_t)key;
  return value >= 0xDEAD0000 && value <= 0xDEAD00FF;
}

static HKEY allocate_fake_registry_handle(void) {
  for (int i = 0; i < 255; ++i) {
    uintptr_t suffix = (uintptr_t)InterlockedIncrement(&g_fake_handle_counter) & 0xFF;
    if (suffix == 0) {
      continue;
    }
    HKEY candidate = (HKEY)(uintptr_t)(0xDEAD0000 | suffix);
    if (!is_redirected_key(candidate)) {
      return candidate;
    }
  }
  return NULL;
}

static bool query_secretagent_default_value(const char* value_name, LPBYTE data,
                                          LPDWORD data_len) {
  if (data_len == NULL) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  SetLastError(ERROR_SUCCESS);
  secretagent_default_t defaults[] = {
    { "CDROM", g_secretagent_cdrom, 0, REG_SZ },
    { "PATH", g_secretagent_path, 0, REG_SZ },
    { "SETUP", NULL, kSecretAgentSetup, REG_DWORD },
    { "LANGUAGE", kSecretAgentLanguage, 0, REG_SZ },
    { "Publisher", "Gigawatt Studios", 0, REG_SZ },
    { "VERSION", "1.00.00", 0, REG_SZ },
  };

  for (int i = 0; i < 6; ++i) {
    if (_stricmp(defaults[i].name, value_name) != 0) {
      continue;
    }

    if (defaults[i].type == REG_DWORD) {
      if (data != NULL && *data_len < sizeof(DWORD)) {
        *data_len = sizeof(DWORD);
        SetLastError(ERROR_MORE_DATA);
        return false;
      }
      if (data != NULL) {
        memcpy(data, &defaults[i].dword_value, sizeof(DWORD));
      }
      *data_len = sizeof(DWORD);
      return true;
    }

    if (data_len == NULL) {
      SetLastError(ERROR_INVALID_PARAMETER);
      return false;
    }

    size_t len = strlen(defaults[i].string_value) + 1;
    if (*data_len < (DWORD)len) {
      *data_len = (DWORD)len;
      SetLastError(ERROR_MORE_DATA);
      return false;
    }
    if (data != NULL) {
      memcpy(data, defaults[i].string_value, len);
      *data_len = (DWORD)len;
      return true;
    }

    *data_len = (DWORD)len;
    return true;
  }
  return false;
}

static LONG WINAPI hooked_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions,
                                       REGSAM samDesired, PHKEY phkResult) {
  if (g_fn_RegOpenKeyExA == NULL || phkResult == NULL) {
    return ERROR_PROC_NOT_FOUND;
  }

  if (hKey == HKEY_LOCAL_MACHINE && is_secretagent_reg_key(lpSubKey)) {
    shim_log("RegOpenKeyExA: intercepted HKLM\\%s -> trying HKCU first", lpSubKey);
    LONG status = g_fn_RegOpenKeyExA(HKEY_CURRENT_USER, lpSubKey, ulOptions, samDesired,
                                    phkResult);
    if (status == ERROR_SUCCESS) {
      if (!store_redirected_key(*phkResult)) {
        g_fn_RegCloseKey(*phkResult);
        *phkResult = NULL;
        return ERROR_TOO_MANY_OPEN_FILES;
      }
      shim_log("RegOpenKeyExA: HKCU key found, redirected (handle=%p)", (void*)*phkResult);
      return status;
    }
    /* Try real HKLM */
    status = g_fn_RegOpenKeyExA(HKEY_LOCAL_MACHINE, lpSubKey, ulOptions, samDesired, phkResult);
    if (status == ERROR_SUCCESS) {
      if (!store_redirected_key(*phkResult)) {
        g_fn_RegCloseKey(*phkResult);
        *phkResult = NULL;
        return ERROR_TOO_MANY_OPEN_FILES;
      }
      shim_log("RegOpenKeyExA: HKLM key found, overriding known values (handle=%p)",
               (void*)*phkResult);
      return status;
    }
    /* Both failed — return a fake handle so query hook can serve defaults */
    *phkResult = allocate_fake_registry_handle();
    if (*phkResult == NULL || !store_redirected_key(*phkResult)) {
      *phkResult = NULL;
      return ERROR_TOO_MANY_OPEN_FILES;
    }
    shim_log("RegOpenKeyExA: HKCU+HKLM both failed, using fake handle %p for defaults", (void*)*phkResult);
    return ERROR_SUCCESS;
  }

  return g_fn_RegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
}

static LONG WINAPI hooked_RegQueryValueExA(HKEY hKey, LPCSTR lpValueName,
                                          LPDWORD lpReserved, LPDWORD lpType,
                                          LPBYTE lpData, LPDWORD lpcbData) {
  (void)lpReserved;

  if (g_fn_RegQueryValueExA == NULL || lpcbData == NULL || lpValueName == NULL) {
    return ERROR_PROC_NOT_FOUND;
  }

  if (is_redirected_key(hKey)) {
    shim_log("RegQueryValueExA: intercepted query for '%s'", lpValueName);
    if (query_secretagent_default_value(lpValueName, lpData, lpcbData)) {
      if (lpType != NULL) {
        if (_stricmp(lpValueName, "SETUP") == 0) {
          *lpType = REG_DWORD;
        } else {
          *lpType = REG_SZ;
        }
      }
      shim_log("RegQueryValueExA: returned default value for '%s'", lpValueName);
      return ERROR_SUCCESS;
    }
    if (GetLastError() == ERROR_MORE_DATA) {
      return ERROR_MORE_DATA;
    }
    if (is_fake_registry_handle(hKey)) {
      return ERROR_FILE_NOT_FOUND;
    }
  }

  return g_fn_RegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

static LONG WINAPI hooked_RegCloseKey(HKEY hKey) {
  if (g_fn_RegCloseKey == NULL) {
    return ERROR_PROC_NOT_FOUND;
  }
  if (clear_redirected_key(hKey)) {
    /* If it was a fake handle, don't pass to real RegCloseKey */
    if (is_fake_registry_handle(hKey)) {
      shim_log("RegCloseKey: closing fake handle %p", (void*)hKey);
      return ERROR_SUCCESS;
    }
  }
  return g_fn_RegCloseKey(hKey);
}

static bool patch_iat_for_hooks(HMODULE module) {
  shim_log("patch_iat: module=%p", (void*)module);
  if (module == NULL) {
    shim_log("patch_iat: module is NULL");
    return false;
  }

  BYTE* base = (BYTE*)module;
  IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    shim_log("patch_iat: bad DOS signature");
    return false;
  }

  IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    shim_log("patch_iat: bad NT signature");
    return false;
  }

  DWORD import_rva =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if (import_rva == 0) {
    return false;
  }

  IMAGE_IMPORT_DESCRIPTOR* import = (IMAGE_IMPORT_DESCRIPTOR*)(base + import_rva);
  shim_log("patch_iat: import table at RVA 0x%lx", import_rva);
  bool patched = false;
  int dll_count = 0;
  for (; import->Name != 0; ++import) {
    LPCSTR dll_name = (LPCSTR)(base + import->Name);
    dll_count++;
    bool is_advapi = _stricmp(dll_name, "ADVAPI32.dll") == 0;
    bool is_kernel32 = _stricmp(dll_name, "KERNEL32.dll") == 0 ||
                       _stricmp(dll_name, "KERNEL32.DLL") == 0;
    if (is_advapi || is_kernel32) {
      shim_log("patch_iat: found target DLL '%s'", dll_name);
    }
    if (!is_advapi && !is_kernel32) {
      continue;
    }
    IMAGE_THUNK_DATA* thunk_name = (IMAGE_THUNK_DATA*)(base + import->OriginalFirstThunk);
    IMAGE_THUNK_DATA* thunk_addr = (IMAGE_THUNK_DATA*)(base + import->FirstThunk);
    for (; thunk_name->u1.AddressOfData != 0; ++thunk_name, ++thunk_addr) {
      if ((thunk_name->u1.Ordinal & IMAGE_ORDINAL_FLAG32) != 0) {
        continue;
      }

      IMAGE_IMPORT_BY_NAME* import_name =
          (IMAGE_IMPORT_BY_NAME*)(base + thunk_name->u1.AddressOfData);
      const char* function_name = (const char*)import_name->Name;

      DWORD_PTR* live_ptr = (DWORD_PTR*)&thunk_addr->u1.Function;
      DWORD old_protect = 0;
      if (!VirtualProtect(live_ptr, sizeof(*live_ptr), PAGE_READWRITE, &old_protect)) {
        continue;
      }

      if (is_advapi && strcmp(function_name, "RegOpenKeyExA") == 0) {
        *live_ptr = (DWORD_PTR)hooked_RegOpenKeyExA;
        patched = true;
        shim_log("patch_iat: hooked RegOpenKeyExA");
      } else if (is_advapi && strcmp(function_name, "RegQueryValueExA") == 0) {
        *live_ptr = (DWORD_PTR)hooked_RegQueryValueExA;
        patched = true;
      } else if (is_advapi && strcmp(function_name, "RegCloseKey") == 0) {
        *live_ptr = (DWORD_PTR)hooked_RegCloseKey;
        patched = true;
      } else if (is_kernel32 && strcmp(function_name, "ExitProcess") == 0) {
        g_fn_ExitProcess = (PFN_ExitProcess)*live_ptr;
        *live_ptr = (DWORD_PTR)shim_exit_hook;
        patched = true;
      } else if (is_kernel32 && strcmp(function_name, "TerminateProcess") == 0) {
        g_fn_TerminateProcess = (PFN_TerminateProcess)*live_ptr;
        *live_ptr = (DWORD_PTR)shim_terminate_process_hook;
        patched = true;
      } else if (is_kernel32 && strcmp(function_name, "GetDriveTypeA") == 0) {
        g_fn_GetDriveTypeA = (PFN_GetDriveTypeA)*live_ptr;
        *live_ptr = (DWORD_PTR)shim_GetDriveTypeA;
        patched = true;
      } else if (is_kernel32 && strcmp(function_name, "GetVolumeInformationA") == 0) {
        g_fn_GetVolumeInformationA = (PFN_GetVolumeInformationA)*live_ptr;
        *live_ptr = (DWORD_PTR)shim_GetVolumeInformationA;
        patched = true;
      } else if (is_kernel32 && strcmp(function_name, "CreateFileA") == 0) {
        g_fn_CreateFileA = (PFN_CreateFileA)*live_ptr;
        *live_ptr = (DWORD_PTR)shim_CreateFileA;
        patched = true;
      } else if (is_kernel32 && strcmp(function_name, "CreateDirectoryA") == 0) {
        g_fn_CreateDirectoryA = (PFN_CreateDirectoryA)*live_ptr;
        *live_ptr = (DWORD_PTR)shim_CreateDirectoryA;
        patched = true;
      } else if (is_kernel32 && strcmp(function_name, "GetFileAttributesA") == 0) {
        g_fn_GetFileAttributesA = (PFN_GetFileAttributesA)*live_ptr;
        *live_ptr = (DWORD_PTR)shim_GetFileAttributesA;
        patched = true;
      }

      VirtualProtect(live_ptr, sizeof(*live_ptr), old_protect, &old_protect);
    }
  }

  return patched;
}

static bool ensure_registry_hooks_loaded(void) {
  if (g_registry_hooks_ready) {
    return true;
  }

  if (!resolve_real_advapi()) {
    return false;
  }

  if (!cache_reg_export("RegOpenKeyExA", (FARPROC*)&g_fn_RegOpenKeyExA) ||
      !cache_reg_export("RegQueryValueExA", (FARPROC*)&g_fn_RegQueryValueExA) ||
      !cache_reg_export("RegCloseKey", (FARPROC*)&g_fn_RegCloseKey)) {
    return false;
  }

  if (!patch_iat_for_hooks(GetModuleHandleA(NULL))) {
    return false;
  }
  g_registry_hooks_ready = true;
  return true;
}

__attribute__((used, noinline)) bool ensure_exports_loaded(void) {
  if (g_exports_ready) {
    return true;
  }

  if (!resolve_real_ddraw()) {
    return false;
  }

  /* Critical exports — must exist */
  if (!cache_export("DirectDrawCreate", &g_fn_DirectDrawCreate) ||
      !cache_export("DirectDrawCreateEx", &g_fn_DirectDrawCreateEx) ||
      !cache_export("DirectDrawCreateClipper", &g_fn_DirectDrawCreateClipper) ||
      !cache_export("DirectDrawEnumerateA", &g_fn_DirectDrawEnumerateA) ||
      !cache_export("DirectDrawEnumerateExA", &g_fn_DirectDrawEnumerateExA)) {
    shim_log("ensure_exports: CRITICAL export missing");
    return false;
  }
  /* Optional exports — log but don't fail */
  cache_export("AcquireDDThreadLock", &g_fn_AcquireDDThreadLock);
  cache_export("CompleteCreateSysmemSurface", &g_fn_CompleteCreateSysmemSurface);
  cache_export("D3DParseUnknownCommand", &g_fn_D3DParseUnknownCommand);
  cache_export("DDGetAttachedSurfaceLcl", &g_fn_DDGetAttachedSurfaceLcl);
  cache_export("DDInternalLock", &g_fn_DDInternalLock);
  cache_export("DDInternalUnlock", &g_fn_DDInternalUnlock);
  cache_export("DSoundHelp", &g_fn_DSoundHelp);
  cache_export("DirectDrawEnumerateExW", &g_fn_DirectDrawEnumerateExW);
  cache_export("DirectDrawEnumerateW", &g_fn_DirectDrawEnumerateW);
  cache_export("DllCanUnloadNow", &g_fn_DllCanUnloadNow);
  cache_export("DllGetClassObject", &g_fn_DllGetClassObject);
  cache_export("GetDDSurfaceLocal", &g_fn_GetDDSurfaceLocal);
  cache_export("GetOLEThunkData", &g_fn_GetOLEThunkData);
  cache_export("GetSurfaceFromDC", &g_fn_GetSurfaceFromDC);
  cache_export("RegisterSpecialCase", &g_fn_RegisterSpecialCase);
  cache_export("ReleaseDDThreadLock", &g_fn_ReleaseDDThreadLock);
  cache_export("SetAppCompatData", &g_fn_SetAppCompatData);

  g_exports_ready = true;
  return true;
}

__attribute__((used, noinline, noreturn)) void ddraw_forwarder_failure(void) {
  shim_log("ddraw forwarder: required backend export is unavailable");
  ExitProcess(ERROR_PROC_NOT_FOUND);
  __builtin_unreachable();
}

#define DDRAW_FORWARDER(name) \
  __attribute__((naked)) __declspec(dllexport) void name(void) { \
    __asm__ __volatile__( \
      "movl _g_fn_" #name ", %%eax\n\t" \
      "testl %%eax, %%eax\n\t" \
      "jnz 2f\n\t" \
      "pushal\n\t" \
      "call _ensure_exports_loaded\n\t" \
      "popal\n\t" \
      "movl _g_fn_" #name ", %%eax\n\t" \
      "testl %%eax, %%eax\n\t" \
      "jz 1f\n\t" \
      "2:\n\t" \
      "jmp *%%eax\n\t" \
      "1: call _ddraw_forwarder_failure\n\t" \
      ::: "eax" \
    ); \
  }

DDRAW_FORWARDER(AcquireDDThreadLock)
DDRAW_FORWARDER(CompleteCreateSysmemSurface)
DDRAW_FORWARDER(D3DParseUnknownCommand)
DDRAW_FORWARDER(DDGetAttachedSurfaceLcl)
DDRAW_FORWARDER(DDInternalLock)
DDRAW_FORWARDER(DDInternalUnlock)
DDRAW_FORWARDER(DSoundHelp)
DDRAW_FORWARDER(DirectDrawCreate)
DDRAW_FORWARDER(DirectDrawCreateClipper)
DDRAW_FORWARDER(DirectDrawEnumerateA)
DDRAW_FORWARDER(DirectDrawEnumerateExA)
DDRAW_FORWARDER(DirectDrawEnumerateExW)
DDRAW_FORWARDER(DirectDrawEnumerateW)
DDRAW_FORWARDER(GetDDSurfaceLocal)
DDRAW_FORWARDER(GetOLEThunkData)
DDRAW_FORWARDER(GetSurfaceFromDC)
DDRAW_FORWARDER(RegisterSpecialCase)
DDRAW_FORWARDER(ReleaseDDThreadLock)
DDRAW_FORWARDER(SetAppCompatData)

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) {
  typedef HRESULT (WINAPI *Fn)(void);
  ensure_exports_loaded();
  Fn fn = (Fn)g_fn_DllCanUnloadNow;
  return fn ? fn() : S_OK;
}

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
  typedef HRESULT (WINAPI *Fn)(REFCLSID, REFIID, LPVOID*);
  ensure_exports_loaded();
  Fn fn = (Fn)g_fn_DllGetClassObject;
  return fn ? fn(rclsid, riid, ppv) : E_NOTIMPL;
}

static void WINAPI shim_exit_hook(UINT code) {
  shim_log("!!! ExitProcess called with code %u !!!", code);
  /* Walk the stack to find who called ExitProcess */
  void* ret_addr = __builtin_return_address(0);
  shim_log("  caller return address: 0x%08lx", (unsigned long)(uintptr_t)ret_addr);
  shim_log_close();
  if (g_fn_ExitProcess != NULL) {
    g_fn_ExitProcess(code);
  }
  TerminateProcess(GetCurrentProcess(), code);
}

static BOOL WINAPI shim_terminate_process_hook(HANDLE hProcess, UINT code) {
  shim_log("!!! TerminateProcess called (process=%p, code=%u) !!!", hProcess, code);
  void* ret_addr = __builtin_return_address(0);
  shim_log("  caller return address: 0x%08lx", (unsigned long)(uintptr_t)ret_addr);
  shim_log_close();
  if (g_fn_TerminateProcess != NULL) {
    return g_fn_TerminateProcess(hProcess, code);
  }
  SetLastError(ERROR_PROC_NOT_FOUND);
  return FALSE;
}

static HANDLE WINAPI shim_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess,
                                      DWORD dwShareMode,
                                      LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                      DWORD dwCreationDisposition,
                                      DWORD dwFlagsAndAttributes,
                                      HANDLE hTemplateFile) {
  if (g_fn_CreateFileA == NULL) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }

  HANDLE result = g_fn_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode,
                                   lpSecurityAttributes, dwCreationDisposition,
                                   dwFlagsAndAttributes, hTemplateFile);
  if (result == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
      shim_log("CreateFileA: ACCESS DENIED path='%s' access=0x%08lx share=0x%08lx "
               "disposition=0x%08lx attrs=0x%08lx",
               lpFileName ? lpFileName : "(null)", dwDesiredAccess, dwShareMode,
               dwCreationDisposition, dwFlagsAndAttributes);
    }
    SetLastError(err);
  }
  return result;
}

static BOOL WINAPI shim_CreateDirectoryA(LPCSTR lpPathName,
                                         LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
  if (g_fn_CreateDirectoryA == NULL) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
  }

  BOOL result = g_fn_CreateDirectoryA(lpPathName, lpSecurityAttributes);
  if (!result) {
    DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
      shim_log("CreateDirectoryA: ACCESS DENIED path='%s'",
               lpPathName ? lpPathName : "(null)");
    }
    SetLastError(err);
  }
  return result;
}

static DWORD WINAPI shim_GetFileAttributesA(LPCSTR lpFileName) {
  if (g_fn_GetFileAttributesA == NULL) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
  }

  DWORD result = g_fn_GetFileAttributesA(lpFileName);
  if (result == INVALID_FILE_ATTRIBUTES) {
    DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
      shim_log("GetFileAttributesA: ACCESS DENIED path='%s'",
               lpFileName ? lpFileName : "(null)");
    }
    SetLastError(err);
  }
  return result;
}

static bool is_fake_cdrom_path(const char* path) {
  if (path == NULL) {
    return false;
  }
  if (_strnicmp(path, g_secretagent_cdrom, strlen(g_secretagent_cdrom)) == 0) {
    return true;
  }
  return _strnicmp(path, g_secretagent_path, strlen(g_secretagent_path)) == 0;
}

static UINT WINAPI shim_GetDriveTypeA(LPCSTR lpRootPathName) {
  if (is_fake_cdrom_path(lpRootPathName)) {
    shim_log("GetDriveTypeA: forced DRIVE_CDROM for %s", lpRootPathName);
    return DRIVE_CDROM;
  }
  if (g_fn_GetDriveTypeA == NULL) {
    return DRIVE_UNKNOWN;
  }
  return g_fn_GetDriveTypeA(lpRootPathName);
}

static BOOL WINAPI shim_GetVolumeInformationA(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer,
                                            DWORD nVolumeNameSize, LPDWORD lpVolumeSerialNumber,
                                            LPDWORD lpMaximumComponentLength,
                                            LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer,
                                            DWORD nFileSystemNameSize) {
  if (is_fake_cdrom_path(lpRootPathName)) {
    static const char kVolumeLabel[] = "SECRET AGENT";
    static const char kFsName[] = "FAT32";

    if (lpVolumeSerialNumber != NULL) {
      *lpVolumeSerialNumber = 0x5A5A5A5A;
    }
    if (lpMaximumComponentLength != NULL) {
      *lpMaximumComponentLength = 255;
    }
    if (lpFileSystemFlags != NULL) {
      *lpFileSystemFlags = FILE_CASE_SENSITIVE_SEARCH;
    }

    if (lpVolumeNameBuffer != NULL && nVolumeNameSize > 0) {
      strncpy(lpVolumeNameBuffer, kVolumeLabel, nVolumeNameSize - 1);
      lpVolumeNameBuffer[nVolumeNameSize - 1] = 0;
    }
    if (lpFileSystemNameBuffer != NULL && nFileSystemNameSize > 0) {
      strncpy(lpFileSystemNameBuffer, kFsName, nFileSystemNameSize - 1);
      lpFileSystemNameBuffer[nFileSystemNameSize - 1] = 0;
    }

    shim_log("GetVolumeInformationA: returning fake info for %s", lpRootPathName);
    return TRUE;
  }
  if (g_fn_GetVolumeInformationA == NULL) {
    return FALSE;
  }
  return g_fn_GetVolumeInformationA(lpRootPathName, lpVolumeNameBuffer, nVolumeNameSize,
                                    lpVolumeSerialNumber, lpMaximumComponentLength,
                                    lpFileSystemFlags, lpFileSystemNameBuffer,
                                    nFileSystemNameSize);
}

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;

  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    init_runtime_paths();
    shim_log_suspend(true);
    ensure_registry_hooks_loaded();
    shim_log_suspend(false);
  }

  return TRUE;
}

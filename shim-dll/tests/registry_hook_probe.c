#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef LONG (WINAPI* probe_RegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI* probe_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI* probe_RegCloseKey)(HKEY);

static void print_last_error(const char* step, LONG status) {
  fprintf(stderr, "%s failed: %ld (GetLastError=%lu)\n", step, status, GetLastError());
}

static int starts_with_case_insensitive(const char* value, const char* prefix) {
  size_t prefix_len = strlen(prefix);
  return _strnicmp(value, prefix, prefix_len) == 0;
}

static int query_and_verify_redirected_value(probe_RegQueryValueExA query_value,
                                             HKEY opened_key,
                                             const char* value_name,
                                             const char* expected_path) {
  char value[MAX_PATH];
  DWORD value_len = sizeof(value);
  DWORD value_type = 0;

  LONG status = query_value(opened_key, value_name, NULL, &value_type, (BYTE*)value,
                            &value_len);
  if (status != ERROR_SUCCESS) {
    print_last_error(value_name, status);
    return 0;
  }

  if (value_type != REG_SZ || !starts_with_case_insensitive(value, expected_path)) {
    fprintf(stderr, "%s was not overridden. got='%s' expected prefix='%s'\n",
            value_name, value, expected_path);
    return 0;
  }

  printf("%s override OK: %s\n", value_name, value);
  return 1;
}

static void delete_test_tree(void) {
  RegDeleteKeyA(HKEY_CURRENT_USER,
                "Software\\BarbieSecretAgentShimTestHKLM\\SOFTWARE\\Gigawatt Studios\\SecretAgent");
  RegDeleteKeyA(HKEY_CURRENT_USER,
                "Software\\BarbieSecretAgentShimTestHKLM\\SOFTWARE\\Gigawatt Studios");
  RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\BarbieSecretAgentShimTestHKLM\\SOFTWARE");
  RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\BarbieSecretAgentShimTestHKLM");
}

static FARPROC* find_import_slot(HMODULE module, const char* dll_name,
                                 const char* function_name) {
  BYTE* base = (BYTE*)module;
  IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return NULL;
  }

  IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return NULL;
  }

  DWORD import_rva =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if (import_rva == 0) {
    return NULL;
  }

  IMAGE_IMPORT_DESCRIPTOR* import = (IMAGE_IMPORT_DESCRIPTOR*)(base + import_rva);
  for (; import->Name != 0; ++import) {
    const char* current_dll = (const char*)(base + import->Name);
    if (_stricmp(current_dll, dll_name) != 0) {
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
      if (strcmp((const char*)import_name->Name, function_name) == 0) {
        return (FARPROC*)&thunk_addr->u1.Function;
      }
    }
  }

  return NULL;
}

int main(int argc, char** argv) {
  const char* shim_path_arg = argc > 1 ? argv[1] : "ddraw.dll";
  const char test_root[] = "Software\\BarbieSecretAgentShimTestHKLM";
  const char test_game_key[] =
      "Software\\BarbieSecretAgentShimTestHKLM\\SOFTWARE\\Gigawatt Studios\\SecretAgent";
  HKEY root_key = NULL;
  HKEY game_key = NULL;
  HKEY opened_key = NULL;
  HKEY fake_key = NULL;
  HKEY capacity_keys[8] = {0};
  HMODULE shim = NULL;
  FARPROC* open_slot = NULL;
  FARPROC* query_slot = NULL;
  FARPROC* close_slot = NULL;
  char exe_path[MAX_PATH];
  char expected_path[MAX_PATH];
  char shim_path[MAX_PATH];
  LONG status;
  int exit_code = 1;

  delete_test_tree();

  if (GetFullPathNameA(shim_path_arg, MAX_PATH, shim_path, NULL) == 0) {
    fprintf(stderr, "GetFullPathNameA(%s) failed: %lu\n", shim_path_arg, GetLastError());
    goto cleanup;
  }

  status = RegCreateKeyExA(HKEY_CURRENT_USER, test_game_key, 0, NULL, 0, KEY_ALL_ACCESS,
                           NULL, &game_key, NULL);
  if (status != ERROR_SUCCESS) {
    print_last_error("RegCreateKeyExA(test key)", status);
    goto cleanup;
  }

  const char stale_path[] = "C:\\stale-install-path\\";
  status = RegSetValueExA(game_key, "PATH", 0, REG_SZ, (const BYTE*)stale_path,
                          (DWORD)sizeof(stale_path));
  if (status != ERROR_SUCCESS) {
    print_last_error("RegSetValueExA(PATH)", status);
    goto cleanup;
  }
  const char stale_cdrom[] = "Z:\\missing-cdrom\\";
  status = RegSetValueExA(game_key, "CDROM", 0, REG_SZ, (const BYTE*)stale_cdrom,
                          (DWORD)sizeof(stale_cdrom));
  if (status != ERROR_SUCCESS) {
    print_last_error("RegSetValueExA(CDROM)", status);
    goto cleanup;
  }
  RegCloseKey(game_key);
  game_key = NULL;

  status = RegOpenKeyExA(HKEY_CURRENT_USER, test_root, 0, KEY_ALL_ACCESS, &root_key);
  if (status != ERROR_SUCCESS) {
    print_last_error("RegOpenKeyExA(test root)", status);
    goto cleanup;
  }

  status = RegOverridePredefKey(HKEY_LOCAL_MACHINE, root_key);
  if (status != ERROR_SUCCESS) {
    print_last_error("RegOverridePredefKey", status);
    goto cleanup;
  }

  DWORD ignored_len = 0;
  RegQueryValueExA(HKEY_CURRENT_USER, "__shim_probe_unused__", NULL, NULL, NULL,
                   &ignored_len);

  shim = LoadLibraryA(shim_path);
  if (shim == NULL) {
    fprintf(stderr, "LoadLibraryA(%s) failed: %lu\n", shim_path, GetLastError());
    goto cleanup;
  }

  open_slot = find_import_slot(GetModuleHandleA(NULL), "ADVAPI32.dll", "RegOpenKeyExA");
  query_slot = find_import_slot(GetModuleHandleA(NULL), "ADVAPI32.dll", "RegQueryValueExA");
  close_slot = find_import_slot(GetModuleHandleA(NULL), "ADVAPI32.dll", "RegCloseKey");
  if (open_slot == NULL || query_slot == NULL || close_slot == NULL) {
    fprintf(stderr, "failed to find registry import slots: open=%p query=%p close=%p\n",
            (void*)open_slot, (void*)query_slot, (void*)close_slot);
    goto cleanup;
  }

  probe_RegOpenKeyExA iat_RegOpenKeyExA = (probe_RegOpenKeyExA)*open_slot;
  probe_RegQueryValueExA iat_RegQueryValueExA = (probe_RegQueryValueExA)*query_slot;

  status = iat_RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Gigawatt Studios\\SecretAgent",
                             0, KEY_READ, &opened_key);
  if (status != ERROR_SUCCESS) {
    print_last_error("RegOpenKeyExA(HKLM SecretAgent)", status);
    goto cleanup;
  }

  if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) == 0) {
    fprintf(stderr, "GetModuleFileNameA failed: %lu\n", GetLastError());
    goto cleanup;
  }
  strcpy_s(expected_path, MAX_PATH, exe_path);
  char* slash = strrchr(expected_path, '\\');
  if (slash != NULL) {
    *(slash + 1) = '\0';
  }

  if (!query_and_verify_redirected_value(iat_RegQueryValueExA, opened_key, "PATH",
                                         expected_path)) {
    goto cleanup;
  }
  if (!query_and_verify_redirected_value(iat_RegQueryValueExA, opened_key, "CDROM",
                                         expected_path)) {
    goto cleanup;
  }

  status = iat_RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                             "SOFTWARE\\Gigawatt Studios\\MissingProduct",
                             0, KEY_READ, &fake_key);
  if (status != ERROR_SUCCESS) {
    print_last_error("RegOpenKeyExA(fake key)", status);
    goto cleanup;
  }
  DWORD missing_len = 0;
  status = iat_RegQueryValueExA(fake_key, "MissingValue", NULL, NULL, NULL,
                                &missing_len);
  if (status != ERROR_FILE_NOT_FOUND) {
    fprintf(stderr, "MissingValue returned %ld; expected ERROR_FILE_NOT_FOUND\n", status);
    goto cleanup;
  }

  probe_RegCloseKey iat_RegCloseKey = (probe_RegCloseKey)*close_slot;
  iat_RegCloseKey(fake_key);
  fake_key = NULL;
  iat_RegCloseKey(opened_key);
  opened_key = NULL;

  for (int i = 0; i < 300; ++i) {
    HKEY recycled_key = NULL;
    status = iat_RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                               "SOFTWARE\\Gigawatt Studios\\MissingRecycled",
                               0, KEY_READ, &recycled_key);
    if (status != ERROR_SUCCESS) {
      fprintf(stderr, "fake-handle recycle open %d failed: %ld\n", i, status);
      goto cleanup;
    }
    missing_len = 0;
    status = iat_RegQueryValueExA(recycled_key, "MissingValue", NULL, NULL, NULL,
                                  &missing_len);
    if (status != ERROR_FILE_NOT_FOUND) {
      fprintf(stderr, "fake-handle recycle query %d failed: %ld\n", i, status);
      iat_RegCloseKey(recycled_key);
      goto cleanup;
    }
    iat_RegCloseKey(recycled_key);
  }

  for (int i = 0; i < 8; ++i) {
    char key_name[96];
    snprintf(key_name, sizeof(key_name),
             "SOFTWARE\\Gigawatt Studios\\MissingProduct%d", i);
    status = iat_RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_name, 0, KEY_READ,
                               &capacity_keys[i]);
    if (status != ERROR_SUCCESS) {
      fprintf(stderr, "redirected-key slot %d failed early: %ld\n", i, status);
      goto cleanup;
    }
  }

  HKEY overflow_key = NULL;
  status = iat_RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                             "SOFTWARE\\Gigawatt Studios\\MissingOverflow",
                             0, KEY_READ, &overflow_key);
  if (status != ERROR_TOO_MANY_OPEN_FILES || overflow_key != NULL) {
    fprintf(stderr, "redirected-key overflow returned %ld handle=%p\n",
            status, (void*)overflow_key);
    goto cleanup;
  }

  exit_code = 0;

cleanup:
  for (int i = 0; i < 8; ++i) {
    if (capacity_keys[i] != NULL && close_slot != NULL) {
      probe_RegCloseKey close_key = (probe_RegCloseKey)*close_slot;
      close_key(capacity_keys[i]);
    }
  }
  if (fake_key != NULL) {
    if (close_slot != NULL) {
      probe_RegCloseKey iat_RegCloseKey = (probe_RegCloseKey)*close_slot;
      iat_RegCloseKey(fake_key);
    }
  }
  if (opened_key != NULL) {
    if (close_slot != NULL) {
      probe_RegCloseKey iat_RegCloseKey = (probe_RegCloseKey)*close_slot;
      iat_RegCloseKey(opened_key);
    } else {
      RegCloseKey(opened_key);
    }
  }
  RegOverridePredefKey(HKEY_LOCAL_MACHINE, NULL);
  if (game_key != NULL) {
    RegCloseKey(game_key);
  }
  if (root_key != NULL) {
    RegCloseKey(root_key);
  }
  if (shim != NULL) {
    FreeLibrary(shim);
  }
  delete_test_tree();
  return exit_code;
}

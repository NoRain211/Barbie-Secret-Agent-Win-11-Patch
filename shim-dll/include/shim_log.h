#ifndef SHIM_LOG_H_
#define SHIM_LOG_H_

#include <stdio.h>
#include <stdarg.h>
#include <windows.h>

static FILE* g_log_file = NULL;

#if defined(SHIM_DEBUG) || defined(DEBUG)

static inline void shim_log_init(void) {
  if (g_log_file != NULL) return;
  char path[MAX_PATH];
  GetModuleFileNameA(NULL, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (slash) *(slash + 1) = '\0';
  strcat(path, "ddraw_proxy.log");
  g_log_file = fopen(path, "w");
}

static inline void shim_log(const char* fmt, ...) {
  va_list args;
  if (g_log_file == NULL) shim_log_init();
  if (g_log_file == NULL) return;

  DWORD tick = GetTickCount();
  fprintf(g_log_file, "[%lu.%03lu] ", tick / 1000, tick % 1000);

  va_start(args, fmt);
  vfprintf(g_log_file, fmt, args);
  va_end(args);

  fprintf(g_log_file, "\n");
  fflush(g_log_file);

  /* Also send to OutputDebugString */
  char buf[512];
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  OutputDebugStringA(buf);
}

static inline void shim_log_close(void) {
  if (g_log_file != NULL) {
    fclose(g_log_file);
    g_log_file = NULL;
  }
}

#else

static inline void shim_log_init(void) {
}

static inline void shim_log(const char* fmt, ...) {
  (void)fmt;
}

static inline void shim_log_close(void) {
}

#endif

#endif /* SHIM_LOG_H_ */

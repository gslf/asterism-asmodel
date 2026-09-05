#ifndef ASMODEL_RUNTIME_CLOCK_H
#define ASMODEL_RUNTIME_CLOCK_H
#include <stdint.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static int64_t mono_ms(void) { return (int64_t)GetTickCount64(); }
#else
#include <time.h>
static int64_t mono_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC,&ts) != 0) return 0;
  return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
#endif
#endif

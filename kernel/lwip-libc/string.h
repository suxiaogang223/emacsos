#ifndef EMACSOS_LWIP_STRING_H
#define EMACSOS_LWIP_STRING_H
#include "kernel.h"
static inline void *memmove(void *d, const void *s, size_t n) {
  unsigned char *a = d;
  const unsigned char *b = s;
  if ((uintptr_t)a < (uintptr_t)b) {
    for (size_t i = 0; i < n; i++)
      a[i] = b[i];
  } else {
    while (n) {
      --n;
      a[n] = b[n];
    }
  }
  return d;
}
static inline int strncmp(const char *a, const char *b, size_t n) {
  while (n--) {
    unsigned char x = *a++, y = *b++;
    if (x != y || !x)
      return x - y;
  }
  return 0;
}
#endif

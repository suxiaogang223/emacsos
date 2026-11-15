#ifndef EMACSOS_LWIP_STDLIB_H
#define EMACSOS_LWIP_STDLIB_H
static inline int atoi(const char *p) {
  int n = 0;
  while (*p >= '0' && *p <= '9')
    n = n * 10 + (*p++ - '0');
  return n;
}
#endif

#include "kernel.h"
void *memcpy(void *d, const void *s, size_t n) {
  unsigned char *a = d;
  const unsigned char *b = s;
  while (n--)
    *a++ = *b++;
  return d;
}
void *memset(void *d, int c, size_t n) {
  unsigned char *a = d;
  while (n--)
    *a++ = c;
  return d;
}
int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  while (n--) {
    if (*x != *y)
      return *x - *y;
    x++;
    y++;
  }
  return 0;
}
size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}
struct block {
  size_t size;
  struct block *next;
  bool free;
} __attribute__((aligned(16)));
static struct block *blocks;
void *kmalloc(size_t n) {
  if (!n)
    n = 1;
  if (n > 0x3ffffff0UL)
    return 0;
  n = (n + 15) & ~15UL;
  for (struct block *b = blocks; b; b = b->next)
    if (b->free && b->size >= n) {
      if (b->size >= n + sizeof(*b) + 16) {
        struct block *t = (void *)((char *)(b + 1) + n);
        *t = (struct block){b->size - n - sizeof(*b), b->next, true};
        b->next = t;
        b->size = n;
      }
      b->free = false;
      return b + 1;
    }
  return 0;
}
void kfree(void *p) {
  if (!p)
    return;
  struct block *b = (struct block *)p - 1;
  b->free = true;
  for (b = blocks; b && b->next;)
    if (b->free && b->next->free) {
      b->size += sizeof(*b) + b->next->size;
      b->next = b->next->next;
    } else
      b = b->next;
}
static uint64_t *page_entry(uintptr_t a) {
  uint64_t *pd = (void *)0x3000;
  unsigned ix = a >> 21;
  if (ix >= 512)
    return 0;
  if (pd[ix] & 0x80) {
    void *raw = kmalloc(PAGE_SIZE * 2);
    if (!raw)
      return 0;
    uint64_t *t = (void *)(((uintptr_t)raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    uintptr_t base = (uintptr_t)ix << 21;
    for (unsigned i = 0; i < 512; i++)
      t[i] = (base + (uintptr_t)i * PAGE_SIZE) | 3;
    pd[ix] = (uintptr_t)t | 3;
  }
  return &((uint64_t *)(uintptr_t)(pd[ix] & ~0xfffUL))[(a >> 12) & 511];
}
static unsigned char mapped[(0x1c000000UL - 0x08000000UL) / PAGE_SIZE];
static uintptr_t current_brk = 0x05000000UL;
void memory_init(uintptr_t end) {
  (void)end;
  blocks = (void *)0x1c000000UL;
  *blocks = (struct block){0x04000000UL - sizeof(struct block), 0, true};
  uint64_t cr0;
  __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
  cr0 |= 0x10000;
  __asm__ volatile("mov %0,%%cr0" ::"r"(cr0));
}
long memory_protect(uintptr_t a, size_t n, unsigned prot) {
  if (a % PAGE_SIZE || n > RAM_LIMIT || a > RAM_LIMIT - n || prot & ~7U)
    return -22;
  n = (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  for (uintptr_t p = a; p < a + n; p += PAGE_SIZE) {
    uint64_t *e = page_entry(p);
    if (!e)
      return -12;
    *e = p | (prot ? 1UL : 0UL) | ((prot & 2) ? 2UL : 0UL) |
         ((prot & 4) ? 0UL : (1UL << 63));
    __asm__ volatile("invlpg (%0)" ::"r"(p) : "memory");
  }
  return 0;
}
long memory_map(uintptr_t a, size_t n, unsigned prot, unsigned flags) {
  if (!n || n > sizeof(mapped) * PAGE_SIZE || prot & ~7U || (flags & ~0x32U) ||
      (flags & 3U) != 2U)
    return -22;
  n = (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  size_t pages = n / PAGE_SIZE;
  if (flags & 0x10) {
    if (a >= 0x05000000UL && a + n <= current_brk && !(a % PAGE_SIZE) &&
        prot == 0)
      return memory_protect(a, n, prot) < 0 ? -12 : (long)a;
    if (a < 0x08000000UL || a % PAGE_SIZE || a > 0x1c000000UL - n)
      return -22;
  } else {
    a = 0;
    size_t run = 0, start = 0;
    for (size_t i = 0; i < sizeof mapped; i++) {
      if (mapped[i])
        run = 0;
      else if (++run == pages) {
        start = i + 1 - pages;
        a = 0x08000000UL + start * PAGE_SIZE;
        break;
      }
    }
    if (!a)
      return -12;
  }
  size_t start = (a - 0x08000000UL) / PAGE_SIZE;
  for (size_t i = 0; i < pages; i++)
    mapped[start + i] = 1;
  if (memory_protect(a, n, 3))
    return -12;
  memset((void *)a, 0, n);
  if (memory_protect(a, n, prot))
    return -12;
  return a;
}
long memory_unmap(uintptr_t a, size_t n) {
  if (!n || a % PAGE_SIZE || a < 0x08000000UL || n > 0x14000000UL ||
      a > 0x1c000000UL - n)
    return -22;
  n = (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  size_t start = (a - 0x08000000UL) / PAGE_SIZE;
  for (size_t i = 0; i < n / PAGE_SIZE; i++)
    mapped[start + i] = 0;
  return memory_protect(a, n, 0);
}
long memory_brk(uintptr_t a) {
  if (!a)
    return current_brk;
  if (a < 0x05000000UL || a > 0x07c00000UL)
    return current_brk;
  if (a > current_brk)
    memset((void *)current_brk, 0, a - current_brk);
  current_brk = a;
  return a;
}

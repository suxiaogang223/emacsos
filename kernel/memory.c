#include "kernel.h"
#include "memory.h"
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

#define POOL_BASE 0x08000000UL
#define POOL_END 0x1c000000UL
#define VM_LOW 0x00400000UL
#define BRK_BASE 0x05000000UL
#define BRK_END 0x07c00000UL
#define PTE_PRESENT 1UL
#define PTE_WRITE 2UL
#define PTE_USER 4UL
#define PTE_LARGE 128UL
#define PTE_OWNED 512UL
#define PTE_NX (1UL << 63)
#define PTE_ADDRESS UINT64_C(0x000ffffffffff000)

enum frame_kind { FRAME_FREE, FRAME_DATA, FRAME_TABLE };
static unsigned char frame_kinds[MEMORY_PAGE_CAPACITY];
static struct memory_pool_usage pool;
static size_t next_frame;
struct address_space {
  uint64_t *root, *pdpt, *pd;
  uintptr_t brk;
  struct memory_usage usage;
  bool bootstrap;
};
static struct address_space bootstrap_space, *current_space;
#ifdef EMACSOS_MEMORY_TEST
static long fail_after = -1;
void memory_test_fail_after(long count) { fail_after = count; }
#endif
static void *physical_pointer(uintptr_t frame) {
  return (void *)(MEMORY_DIRECT_BASE + frame);
}
static uintptr_t physical_address(const void *pointer) {
  return (uintptr_t)pointer - MEMORY_DIRECT_BASE;
}
static uintptr_t frame_acquire(struct address_space *space, enum frame_kind kind) {
  if (!pool.free_pages || space->usage.data_pages + space->usage.table_pages >=
                              space->usage.limit_pages)
    return 0;
#ifdef EMACSOS_MEMORY_TEST
  if (fail_after == 0)
    return 0;
#endif
  for (size_t searched = 0; searched < MEMORY_PAGE_CAPACITY; searched++) {
    size_t index = next_frame;
    next_frame = (next_frame + 1) % MEMORY_PAGE_CAPACITY;
    if (frame_kinds[index] != FRAME_FREE)
      continue;
    frame_kinds[index] = kind;
    pool.free_pages--;
    if (kind == FRAME_DATA) {
      pool.data_pages++;
      space->usage.data_pages++;
    } else {
      pool.table_pages++;
      space->usage.table_pages++;
    }
#ifdef EMACSOS_MEMORY_TEST
    if (fail_after > 0)
      fail_after--;
#endif
    uintptr_t frame = POOL_BASE + index * PAGE_SIZE;
    memset(physical_pointer(frame), 0, PAGE_SIZE);
    return frame;
  }
  panic("physical page accounting");
}
static void frame_release(struct address_space *space, uintptr_t frame,
                          enum frame_kind kind) {
  if (frame < POOL_BASE || frame >= POOL_END || frame % PAGE_SIZE ||
      frame_kinds[(frame - POOL_BASE) / PAGE_SIZE] != kind)
    panic("invalid physical page release");
  size_t index = (frame - POOL_BASE) / PAGE_SIZE;
  frame_kinds[index] = FRAME_FREE;
  if (index < next_frame)
    next_frame = index;
  pool.free_pages++;
  if (kind == FRAME_DATA) {
    pool.data_pages--;
    space->usage.data_pages--;
  } else {
    pool.table_pages--;
    space->usage.table_pages--;
  }
}
static void flush(struct address_space *space) {
  if (space == current_space) {
    uintptr_t root = physical_address(space->root);
    __asm__ volatile("mov %0,%%cr3" ::"r"(root) : "memory");
  }
}
static bool range(uintptr_t a, size_t n, uintptr_t low, uintptr_t high,
                  size_t *rounded) {
  if (!n || a % PAGE_SIZE || a < low || a >= high || n > high - a)
    return false;
  *rounded = (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  return *rounded <= high - a;
}
static uint64_t *entry(struct address_space *space, uintptr_t a) {
  uint64_t directory = space->pd[a >> 21];
  if (!(directory & PTE_PRESENT) || (directory & PTE_LARGE))
    return NULL;
  uint64_t *table = physical_pointer(directory & PTE_ADDRESS);
  return &table[(a >> 12) & 511];
}
static uint64_t permissions(struct address_space *space, unsigned prot) {
  return (prot ? PTE_PRESENT : 0) | ((prot & 2) ? PTE_WRITE : 0) |
         ((prot & 4) ? 0 : PTE_NX) | (space->bootstrap ? 0 : PTE_USER);
}
/* New tables remain detached until the whole operation can commit. */
struct table_plan {
  uint64_t *tables[512];
  bool fresh[512];
};
static void discard_tables(struct address_space *space, struct table_plan *plan) {
  for (unsigned i = 0; i < 512; i++)
    if (plan->fresh[i])
      frame_release(space, physical_address(plan->tables[i]), FRAME_TABLE);
}
static bool prepare_tables(struct address_space *space, uintptr_t a, size_t n,
                           struct table_plan *plan) {
  memset(plan, 0, sizeof *plan);
  unsigned last = (a + n - 1) >> 21;
  for (unsigned i = a >> 21; i <= last; i++) {
    uint64_t old = space->pd[i];
    if ((old & PTE_PRESENT) && !(old & PTE_LARGE)) {
      plan->tables[i] = physical_pointer(old & PTE_ADDRESS);
      continue;
    }
    uintptr_t frame = frame_acquire(space, FRAME_TABLE);
    if (!frame) {
      discard_tables(space, plan);
      return false;
    }
    plan->tables[i] = physical_pointer(frame);
    plan->fresh[i] = true;
    if (old & PTE_LARGE)
      for (unsigned j = 0; j < 512; j++)
        plan->tables[i][j] = ((uintptr_t)i << 21) + j * PAGE_SIZE |
            (old & (PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX));
  }
  return true;
}
static void commit_tables(struct address_space *space, struct table_plan *plan) {
  for (unsigned i = 0; i < 512; i++)
    if (plan->fresh[i])
      space->pd[i] = physical_address(plan->tables[i]) | PTE_PRESENT | PTE_WRITE |
                     PTE_OWNED | (space->bootstrap ? 0 : PTE_USER);
}
static long map_pages(struct address_space *space, uintptr_t a, size_t n,
                      unsigned prot) {
  size_t pages = n / PAGE_SIZE;
  uintptr_t *frames = kmalloc(pages * sizeof *frames);
  if (!frames)
    return -12;
  memset(frames, 0, pages * sizeof *frames);
  struct table_plan plan;
  if (!prepare_tables(space, a, n, &plan)) {
    kfree(frames);
    return -12;
  }
  for (size_t i = 0; i < pages; i++) {
    uintptr_t address = a + i * PAGE_SIZE;
    uint64_t old = plan.tables[address >> 21][(address >> 12) & 511];
    /* Reuse replaced pages so MAP_FIXED needs no temporary quota headroom. */
    frames[i] = old & PTE_OWNED ? (old & PTE_ADDRESS) | 1
                               : frame_acquire(space, FRAME_DATA);
    if (!frames[i]) {
      for (size_t j = 0; j < i; j++)
        if (!(frames[j] & 1))
          frame_release(space, frames[j], FRAME_DATA);
      discard_tables(space, &plan);
      kfree(frames);
      return -12;
    }
  }
  commit_tables(space, &plan);
  for (size_t i = 0; i < pages; i++) {
    uintptr_t address = a + i * PAGE_SIZE, frame = frames[i] & PTE_ADDRESS;
    memset(physical_pointer(frame), 0, PAGE_SIZE);
    plan.tables[address >> 21][(address >> 12) & 511] =
        frame | PTE_OWNED | permissions(space, prot);
  }
  flush(space);
  kfree(frames);
  return a;
}
static void unmap_pages(struct address_space *space, uintptr_t a, size_t n) {
  for (uintptr_t p = a; p < a + n; p += PAGE_SIZE) {
    uint64_t *slot = entry(space, p);
    if (slot && (*slot & PTE_OWNED)) {
      frame_release(space, *slot & PTE_ADDRESS, FRAME_DATA);
      *slot = 0;
    }
  }
  for (unsigned i = a >> 21; i <= (a + n - 1) >> 21; i++) {
    if (!(space->pd[i] & PTE_OWNED))
      continue;
    uint64_t *table = physical_pointer(space->pd[i] & PTE_ADDRESS);
    unsigned j;
    for (j = 0; j < 512 && !table[j]; j++) {}
    if (j == 512) {
      frame_release(space, space->pd[i] & PTE_ADDRESS, FRAME_TABLE);
      space->pd[i] = 0;
    }
  }
  flush(space);
}
struct address_space *memory_current_space(void) { return current_space; }
void memory_space_usage(const struct address_space *space,
                         struct memory_usage *usage) {
  *usage = space->usage;
}
void memory_pool_usage(struct memory_pool_usage *usage) { *usage = pool; }
struct address_space *memory_space_create(size_t limit_pages) {
  if (limit_pages < 3 || limit_pages > MEMORY_PAGE_CAPACITY)
    return NULL;
  struct address_space *space = kmalloc(sizeof *space);
  if (!space)
    return NULL;
  *space = (struct address_space){.brk = BRK_BASE,
                                .usage.limit_pages = limit_pages};
  uint64_t **tables[] = {&space->root, &space->pdpt, &space->pd};
  for (unsigned i = 0; i < 3; i++) {
    uintptr_t frame = frame_acquire(space, FRAME_TABLE);
    if (!frame) {
      for (unsigned j = 0; j < i; j++)
        frame_release(space, physical_address(*tables[j]), FRAME_TABLE);
      kfree(space);
      return NULL;
    }
    *tables[i] = physical_pointer(frame);
  }
  space->root[0] = physical_address(space->pdpt) | 7;
  space->pdpt[0] = physical_address(space->pd) | 7;
  space->root[256] = bootstrap_space.root[256];
  /* Kernel code/stacks, heap and immutable image backing stay supervisor-only.
   * The private low-address region starts empty. */
  for (unsigned i = 0; i < 512; i++)
    if (i < VM_LOW >> 21 || i >= POOL_END >> 21)
      space->pd[i] = ((uintptr_t)i << 21) | 0x83 |
                     (i >= POOL_END >> 21 ? PTE_NX : 0);
  return space;
}
long memory_space_activate(struct address_space *space) {
  if (!space)
    return -22;
  current_space = space;
  flush(space);
  return 0;
}
long memory_space_destroy(struct address_space *space) {
  if (!space || space->bootstrap)
    return -22;
  if (space == current_space)
    return -16;
  unmap_pages(space, VM_LOW, POOL_END - VM_LOW);
  frame_release(space, physical_address(space->pd), FRAME_TABLE);
  frame_release(space, physical_address(space->pdpt), FRAME_TABLE);
  frame_release(space, physical_address(space->root), FRAME_TABLE);
  kfree(space);
  return 0;
}
void memory_init(uintptr_t end) {
  (void)end;
  blocks = (void *)0x1c000000UL;
  *blocks = (struct block){0x04000000UL - sizeof(struct block), 0, true};
  memset(frame_kinds, 0, sizeof frame_kinds);
  pool = (struct memory_pool_usage){.free_pages = MEMORY_PAGE_CAPACITY};
  next_frame = 0;
  bootstrap_space = (struct address_space){
      .root = physical_pointer(0x1000), .pdpt = physical_pointer(0x2000),
      .pd = physical_pointer(0x3000), .brk = BRK_BASE, .bootstrap = true,
      .usage.limit_pages = MEMORY_PAGE_CAPACITY};
  current_space = &bootstrap_space;
  for (unsigned i = BRK_BASE >> 21; i < BRK_END >> 21; i++)
    bootstrap_space.pd[i] = 0;
  for (unsigned i = POOL_BASE >> 21; i < POOL_END >> 21; i++)
    bootstrap_space.pd[i] = 0;
  flush(current_space);
  uint64_t cr0;
  __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
  cr0 |= 0x10000;
  __asm__ volatile("mov %0,%%cr0" ::"r"(cr0));
}
long memory_space_protect(struct address_space *space, uintptr_t a, size_t n,
                           unsigned prot) {
  size_t rounded;
  if (!space || (prot & ~7U) || !range(a, n, VM_LOW, POOL_END, &rounded))
    return -22;
  for (uintptr_t p = a; p < a + rounded; p += PAGE_SIZE) {
    uint64_t *slot = entry(space, p);
    bool borrowed = space->bootstrap && p < 0x04400000UL;
    if (!borrowed && (!slot || !(*slot & PTE_OWNED)))
      return -12;
  }
  struct table_plan plan;
  if (!prepare_tables(space, a, rounded, &plan))
    return -12;
  commit_tables(space, &plan);
  for (uintptr_t p = a; p < a + rounded; p += PAGE_SIZE) {
    uint64_t *slot = &plan.tables[p >> 21][(p >> 12) & 511];
    *slot = (*slot & (PTE_ADDRESS | PTE_OWNED)) | permissions(space, prot);
  }
  flush(space);
  return 0;
}
long memory_space_map(struct address_space *space, uintptr_t a, size_t n,
                      unsigned prot, unsigned flags) {
  if (!space || !n || n > POOL_END - POOL_BASE || (prot & ~7U) ||
      (flags & ~0x32U) || (flags & 3U) != 2U)
    return -22;
  size_t rounded = (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (flags & 0x10) {
    if (a >= BRK_BASE && a < BRK_END && a <= space->brk &&
        n <= space->brk - a && !(a % PAGE_SIZE) && prot == 0)
      return memory_space_protect(space, a, rounded, 0) < 0 ? -12 : (long)a;
    size_t checked;
    if (!range(a, n, space->bootstrap ? POOL_BASE : VM_LOW, POOL_END, &checked) ||
        (a < BRK_END && a + rounded > BRK_BASE))
      return -22;
  } else {
    size_t run = 0;
    a = 0;
    for (uintptr_t p = POOL_BASE; p < POOL_END; p += PAGE_SIZE) {
      uint64_t *slot = entry(space, p);
      if (slot && (*slot & PTE_OWNED))
        run = 0;
      else if (++run == rounded / PAGE_SIZE) {
        a = p + PAGE_SIZE - rounded;
        break;
      }
    }
    if (!a)
      return -12;
  }
  return map_pages(space, a, rounded, prot);
}
long memory_space_unmap(struct address_space *space, uintptr_t a, size_t n) {
  size_t rounded;
  if (!space || !range(a, n, space->bootstrap ? POOL_BASE : VM_LOW,
                       POOL_END, &rounded) ||
      (a < BRK_END && a + rounded > BRK_BASE))
    return -22;
  unmap_pages(space, a, rounded);
  return 0;
}
long memory_space_brk(struct address_space *space, uintptr_t a) {
  if (!space)
    return -22;
  if (!a || a < BRK_BASE || a > BRK_END)
    return space->brk;
  uintptr_t old_page = (space->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  uintptr_t new_page = (a + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (new_page > old_page && map_pages(space, old_page, new_page - old_page, 3) < 0)
    return space->brk;
  if (new_page < old_page)
    unmap_pages(space, new_page, old_page - new_page);
  for (uintptr_t p = space->brk; p < a;) {
    size_t count = PAGE_SIZE - p % PAGE_SIZE;
    if (count > a - p)
      count = a - p;
    uint64_t *slot = entry(space, p);
    memset((char *)physical_pointer(*slot & PTE_ADDRESS) + p % PAGE_SIZE, 0, count);
    p += count;
  }
  space->brk = a;
  return a;
}
long memory_map(uintptr_t a, size_t n, unsigned prot, unsigned flags) {
  return memory_space_map(current_space, a, n, prot, flags);
}
long memory_unmap(uintptr_t a, size_t n) {
  return memory_space_unmap(current_space, a, n);
}
long memory_protect(uintptr_t a, size_t n, unsigned prot) {
  return memory_space_protect(current_space, a, n, prot);
}
long memory_brk(uintptr_t a) { return memory_space_brk(current_space, a); }

/* Serialized page tables cannot change between preflight and alias copying. */
bool memory_space_user_access(struct address_space *space, uintptr_t a, size_t n,
                              unsigned prot) {
  if (!space || space->bootstrap || !prot || (prot & ~7U) ||
      a < VM_LOW || a >= POOL_END || n > POOL_END - a)
    return false;
  if (!n)
    return true;
  uint64_t required_upper = PTE_PRESENT | PTE_USER | ((prot & 2) ? PTE_WRITE : 0);
  if ((space->root[0] & required_upper) != required_upper ||
      (space->pdpt[0] & required_upper) != required_upper ||
      ((prot & 4) && ((space->root[0] | space->pdpt[0]) & PTE_NX)))
    return false;
  uintptr_t end = a + n;
  for (uintptr_t p = a & ~(PAGE_SIZE - 1); p < end; p += PAGE_SIZE) {
    uint64_t directory = space->pd[p >> 21];
    uint64_t *slot = entry(space, p);
    uint64_t required = PTE_PRESENT | PTE_USER | PTE_OWNED;
    if ((directory & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        !slot || (*slot & required) != required ||
        ((prot & 2) && (!(*slot & PTE_WRITE) || !(directory & PTE_WRITE))) ||
        ((prot & 4) && ((*slot | directory) & PTE_NX)))
      return false;
  }
  return true;
}
static long copy_user(struct address_space *space, uintptr_t user, void *buffer,
                      size_t n, bool writing) {
  if (!memory_space_user_access(space, user, n, writing ? 2 : 1))
    return -14;
  unsigned char *bytes = buffer;
  while (n) {
    size_t count = PAGE_SIZE - user % PAGE_SIZE;
    if (count > n)
      count = n;
    uint64_t *slot = entry(space, user);
    void *alias = (char *)physical_pointer(*slot & PTE_ADDRESS) + user % PAGE_SIZE;
    if (writing)
      memcpy(alias, bytes, count);
    else
      memcpy(bytes, alias, count);
    bytes += count;
    user += count;
    n -= count;
  }
  return 0;
}
long memory_space_copy_from(struct address_space *space, void *buffer,
                             uintptr_t user, size_t n) {
  return copy_user(space, user, buffer, n, false);
}
long memory_space_copy_to(struct address_space *space, uintptr_t user,
                           const void *buffer, size_t n) {
  return copy_user(space, user, (void *)buffer, n, true);
}

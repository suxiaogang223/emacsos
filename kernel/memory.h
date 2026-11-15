#ifndef EMACSOS_MEMORY_H
#define EMACSOS_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEMORY_DIRECT_BASE UINT64_C(0xffff800000000000)
#define MEMORY_PAGE_CAPACITY (0x14000000UL / 4096UL)

struct address_space;
struct memory_usage {
  size_t data_pages, table_pages, limit_pages;
};
struct memory_pool_usage {
  size_t free_pages, data_pages, table_pages;
};

/* Kernel-only, serialized operations; activation supplies no ring-3 execution,
 * scheduler, user-pointer validation or process isolation by itself. Limits
 * charge owned data/table frames, excluding bootstrap mappings and heap metadata.
 */
struct address_space *memory_current_space(void);
struct address_space *memory_space_create(size_t limit_pages);
long memory_space_activate(struct address_space *space);
long memory_space_destroy(struct address_space *space);
void memory_space_usage(const struct address_space *, struct memory_usage *);
void memory_pool_usage(struct memory_pool_usage *);
long memory_space_map(struct address_space *, uintptr_t, size_t, unsigned, unsigned);
long memory_space_protect(struct address_space *, uintptr_t, size_t, unsigned);
long memory_space_unmap(struct address_space *, uintptr_t, size_t);
long memory_space_brk(struct address_space *, uintptr_t);

/* User access requires private owned mappings and full-range preflight. */
bool memory_space_user_access(struct address_space *, uintptr_t, size_t, unsigned);
long memory_space_copy_from(struct address_space *, void *, uintptr_t, size_t);
long memory_space_copy_to(struct address_space *, uintptr_t, const void *, size_t);

#ifdef EMACSOS_MEMORY_TEST
/* Fail after this many successful frame acquisitions; negative disables it. */
void memory_test_fail_after(long count);
#endif
#endif

#ifndef EMACSOS_KERNEL_H
#define EMACSOS_KERNEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PAGE_SIZE 4096UL
#define RAM_LIMIT 0x40000000UL
#define PAYLOAD_BASE 0x20000000UL
struct runtime_image {
  const unsigned char *elf;
  size_t elf_size;
  const unsigned char *archive;
  size_t archive_size;
};
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
void console_init(void);
void console_write(const void *, size_t);
long console_read(void *, size_t);
bool console_ready(void);
void console_puts(const char *);
void console_hex(uint64_t);
void hardware_init(void);
uint64_t clock_monotonic_ns(void);
uint64_t clock_realtime_ns(void);
void wait_interrupt(void);
void *kmalloc(size_t);
void kfree(void *);
long memory_map(uintptr_t, size_t, unsigned, unsigned);
long memory_unmap(uintptr_t, size_t);
long memory_protect(uintptr_t, size_t, unsigned);
long memory_brk(uintptr_t);
void memory_init(uintptr_t);
void disk_read(uint64_t, void *, size_t);
struct runtime_image runtime_image_load(void);
_Noreturn void runtime_enter(const struct runtime_image *);
_Noreturn void panic(const char *);
long syscall_dispatch(unsigned long, const unsigned long[6]);
bool abi_files(unsigned long, const unsigned long[6], long *);
void fs_init(const void *, size_t);
#endif

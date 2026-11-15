/* Freestanding ELF workers, not a libc or GNU Emacs process profile. */
#include <stddef.h>
#include <stdint.h>
#ifndef WORKER_CASE
#define WORKER_CASE 0
#endif
static long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                 uintptr_t d, uintptr_t e, uintptr_t f) {
  register uintptr_t r10 __asm__("r10") = d;
  register uintptr_t r8 __asm__("r8") = e;
  register uintptr_t r9 __asm__("r9") = f;
  long result;
  __asm__ volatile("syscall" : "=a"(result)
      : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
      : "rcx", "r11", "memory", "cc");
  return result;
}
#define REQUIRE(value) do { if (!(value)) return __LINE__; } while (0)
extern int worker_context_round(void);
extern void _start(void);
long worker_main(void) {
#if WORKER_CASE == 1
  *(volatile unsigned char *)0x10000 = 1;
#elif WORKER_CASE == 2
  *(volatile unsigned char *)UINT64_C(0xffff800000010000) = 1;
#elif WORKER_CASE == 3
  __asm__ volatile("cli");
#elif WORKER_CASE == 4
  __asm__ volatile("hlt");
#elif WORKER_CASE == 5
  __asm__ volatile("wrmsr" :: "c"(0xc0000100U), "a"(0), "d"(0));
#elif WORKER_CASE == 6
  __asm__ volatile("outb %%al,%%dx" :: "a"(0), "d"((uint16_t)0x3f8));
#elif WORKER_CASE == 7
  __asm__ volatile("movabs $0x8000000000000000,%%rsp; mov $39,%%eax; syscall"
                   ::: "rax", "rcx", "r11", "memory");
#elif WORKER_CASE == 8
  long code = call(9, 0x08000000, 4096, 3, 0x32, UINT64_MAX, 0);
  REQUIRE(code == 0x08000000);
  ((void (*)(void))code)();
#elif WORKER_CASE == 9
  *(volatile unsigned char *)(uintptr_t)_start = 0;
#elif WORKER_CASE == 10
  __asm__ volatile("xor %%eax,%%eax; wrfsbase %%rax" ::: "rax", "memory");
#elif WORKER_CASE == 12
  __asm__ volatile("mov $39,%%eax; pushq $0x40602; popfq; syscall"
                   ::: "rax", "rcx", "r11", "memory", "cc");
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0" : "=r"(flags));
  REQUIRE(!(flags & 0x40500));
#elif WORKER_CASE == 13
  unsigned char output[4096];
  for (unsigned i = 0; i < sizeof output; i++) output[i] = 'o';
  REQUIRE(call(1, 1, (uintptr_t)output, 4096, 0, 0, 0) == 4096);
  REQUIRE(call(1, 2, (uintptr_t)output, 1, 0, 0, 0) == -28);
#elif WORKER_CASE == 14
  *(volatile unsigned char *)0x043effff = 1;
#elif WORKER_CASE == 15
  __asm__ volatile("int $0x20");
#elif WORKER_CASE == 11
  /* The worker entry must mask user-selected flags before touching CPL0 state. */
  __asm__ volatile("mov $39,%%eax; pushq $0x40702; popfq; syscall"
                   ::: "rax", "rcx", "r11", "memory", "cc");
#else
  unsigned char fresh_fx[512] __attribute__((aligned(16)));
  __asm__ volatile("fxsave64 %0" : "=m"(fresh_fx));
  REQUIRE(*(uint16_t *)fresh_fx == 0x37f && fresh_fx[4] == 0 &&
              *(uint32_t *)(fresh_fx + 24) == 0x1f80);
  for (unsigned i = 32; i < 416; i++) REQUIRE(fresh_fx[i] == 0);
  uint16_t cs;
  __asm__ volatile("mov %%cs,%0" : "=r"(cs));
  REQUIRE((cs & 3) == 3);
  long pid = call(39, 0, 0, 0, 0, 0, 0), via_interrupt;
  __asm__ volatile("int $0x80" : "=a"(via_interrupt) : "a"(39) : "memory", "cc");
  REQUIRE(pid > 1 && via_interrupt == pid && call(110, 0, 0, 0, 0, 0, 0) == 1);
  REQUIRE(call(9999, 0x10000, UINT64_MAX, 0, 0, 0, 0) == -38);
  REQUIRE(call(0, 0, 0x10000, 1, 0, 0, 0) == -9);
  REQUIRE(call(1, 64, 0x10000, 1, 0, 0, 0) == -9);
  long address = call(9, 0x08000000, 8192, 3, 0x32, UINT64_MAX, 0);
  REQUIRE(address == 0x08000000);
  unsigned char *bytes = (void *)address;
  REQUIRE(bytes[0] == 0);
  bytes[0] = pid;
  *(uint64_t *)(bytes + 128) = pid;
  *(uint64_t *)(bytes + 136) = pid + 100;
  for (unsigned i = 4080; i < 4096; i++) bytes[i] = 202;
  REQUIRE(call(10, address + 4096, 4096, 1, 0, 0, 0) == 0);
  REQUIRE(call(1, 1, 0x10000, 16, 0, 0, 0) == -14);
  REQUIRE(call(1, 1, UINT64_C(0xffff800000010000), 16, 0, 0, 0) == -14);
  REQUIRE(call(1, 1, address + 8192 - 4, 8, 0, 0, 0) == -14);
  REQUIRE(call(1, 1, UINT64_MAX - 3, 8, 0, 0, 0) == -14);
  REQUIRE(call(228, 1, address + 4096 - 8, 0, 0, 0, 0) == -14);
  for (unsigned i = 4080; i < 4096; i++) REQUIRE(bytes[i] == 202);
  REQUIRE(call(158, 0x1003, address + 4096 - 4, 0, 0, 0, 0) == -14);
  for (unsigned i = 4080; i < 4096; i++) REQUIRE(bytes[i] == 202);
  REQUIRE(call(228, 1, (uintptr_t)_start, 0, 0, 0, 0) == -14);
  REQUIRE(call(10, 0x10000, 4096, 3, 0, 0, 0) == -22);
  REQUIRE(call(9, 0, 4096, 7, 0x22, UINT64_MAX, 0) == -22);
  REQUIRE(call(9, 0, 4096, 3, 0x22, 1, 0) == -22);
  REQUIRE(call(158, 0x1002, address + 128, 0, 0, 0, 0) == 0);
  REQUIRE(call(158, 0x1001, address + 136, 0, 0, 0, 0) == 0);
  REQUIRE(call(158, 0x1002, UINT64_C(0xffff800000010000), 0, 0, 0, 0) == -14);
  for (unsigned round = 0; round < 2; round++) {
    REQUIRE(worker_context_round() == 0);
    uint64_t fs, gs;
    __asm__ volatile("movq %%fs:0,%0; movq %%gs:0,%1" : "=r"(fs), "=r"(gs));
    REQUIRE(fs == (uint64_t)pid && gs == (uint64_t)pid + 100 && bytes[0] == pid);
  }
  uint64_t time[2];
  REQUIRE(call(228, 1, (uintptr_t)time, 0, 0, 0, 0) == 0 && time[1] < 1000000000);
  REQUIRE(call(9, 0, 320UL * 1024 * 1024, 3, 0x22, UINT64_MAX, 0) == -12);
  REQUIRE(call(1, 1, (uintptr_t)"worker-ok", 9, 0, 0, 0) == 9);
  return 42;
#endif
  return 0;
}

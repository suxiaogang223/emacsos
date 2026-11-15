#include "kernel.h"
#include "process.h"
#include "elf.h"
void storage_control_service(void) {}
void network_service(void) {}
struct fixture { const unsigned char *start, *end; unsigned vector; };
#define DECLARE(n) extern const unsigned char worker_##n##_start[], worker_##n##_end[];
DECLARE(0) DECLARE(1) DECLARE(2) DECLARE(3) DECLARE(4) DECLARE(5)
DECLARE(6) DECLARE(7) DECLARE(8) DECLARE(9) DECLARE(10) DECLARE(11)
DECLARE(12) DECLARE(13) DECLARE(14) DECLARE(15)
#define FIXTURE(n,v) {worker_##n##_start,worker_##n##_end,v}
static const struct fixture fixtures[] = {
  FIXTURE(0,0), FIXTURE(1,14), FIXTURE(2,14), FIXTURE(3,13), FIXTURE(4,13),
  FIXTURE(5,13), FIXTURE(6,13), FIXTURE(7,0), FIXTURE(8,14), FIXTURE(9,14),
  FIXTURE(10,6), FIXTURE(11,1), FIXTURE(12,0), FIXTURE(13,0),
  FIXTURE(14,14), FIXTURE(15,13)
};
static void require(bool value, const char *message) {
  if (!value) panic(message);
}
static struct memory_pool_usage usage(void) {
  struct memory_pool_usage result;
  memory_pool_usage(&result);
  return result;
}
static void same_pool(struct memory_pool_usage before) {
  struct memory_pool_usage after = usage();
  require(before.free_pages == after.free_pages && before.data_pages == after.data_pages &&
              before.table_pages == after.table_pages, "process page leak");
}
static long spawn(unsigned n, size_t limit) {
  return process_spawn(fixtures[n].start, fixtures[n].end - fixtures[n].start, limit);
}
static struct process_info inspect(uint64_t pid) {
  struct process_info result;
  require(!process_inspect(pid, &result), "inspect process");
  return result;
}
static uint64_t msr(uint32_t n) {
  uint32_t a,d;
  __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(n));
  return (uint64_t)d << 32 | a;
}
static void set_msr(uint32_t n, uint64_t value) {
  __asm__ volatile("wrmsr" :: "c"(n), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}
static unsigned char before_fx[512] __attribute__((aligned(16)));
static unsigned char after_fx[512] __attribute__((aligned(16)));
static void manager_survives(void) {
  __asm__ volatile("fxsave64 %0" : "=m"(after_fx));
  require(!memcmp(before_fx, after_fx, sizeof before_fx), "manager FPU/SSE changed");
  require(msr(0xc0000081) == (8UL << 32) && msr(0xc0000082) == 0x10000 &&
              msr(0xc0000084) == 0x600, "manager syscall gate changed");
  uintptr_t tss_stack = hardware_process_stack(0x88000);
  require(tss_stack == 0x88000, "manager TSS stack changed");
  require(msr(0xc0000100) == 0x20000 && msr(0xc0000101) == 0x21000,
          "manager TLS changed");
  require(*(volatile unsigned char *)0x08000000 == 93, "manager bytes changed");
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0" : "=r"(flags));
  require(flags & 0x200, "manager interrupts not restored");
}
_Noreturn void kernel_main(void) {
  console_init(); memory_init(0); hardware_init();
  require(memory_map(0, PAGE_SIZE, 3, 0x22) == 0x08000000, "manager mmap");
  *(volatile unsigned char *)0x08000000 = 93;
  struct memory_pool_usage baseline = usage();
  set_msr(0xc0000081,8UL << 32); set_msr(0xc0000082,0x10000);
  set_msr(0xc0000084,0x600); (void)hardware_process_stack(0x88000);
  set_msr(0xc0000100, 0x20000); set_msr(0xc0000101, 0x21000);
  __asm__ volatile("fninit; fld1; pxor %%xmm0,%%xmm0; pxor %%xmm7,%%xmm7; pxor %%xmm15,%%xmm15"
                   ::: "memory");
  __asm__ volatile("fxsave64 %0" : "=m"(before_fx));
  long a = spawn(0,64), b = spawn(0,64);
  require(a > 1 && b > a, "worker identities");
  require(process_reap(a) == -16, "reap live worker");
  for (unsigned round = 0; round < 3; round++) {
    require(!process_run(a), "run worker A"); manager_survives();
    require(!process_run(b), "run worker B"); manager_survives();
    struct process_info ai = inspect(a), bi = inspect(b);
    if (round < 2) require(ai.state == PROCESS_READY && bi.state == PROCESS_READY,
                           "workers did not yield");
    else {
      if (ai.status != 42 || bi.status != 42) {
        console_puts("worker status A/B="); console_hex(ai.status); console_hex(bi.status);
      }
      require(ai.state == PROCESS_EXITED && bi.state == PROCESS_EXITED &&
                  ai.reason == PROCESS_NORMAL && bi.reason == PROCESS_NORMAL &&
                  ai.status == 42 && bi.status == 42 && !ai.memory.data_pages &&
                  !ai.memory.table_pages, "worker exit or cleanup");
    }
  }
  char output[16];
  require(process_output(a,0,output,sizeof output) == 9 && !memcmp(output,"worker-ok",9),
          "captured output");
  require(!process_reap(a) && !process_reap(b), "reap completed workers");
  same_pool(baseline);
  console_puts("PROCESS-RING3-CONTEXT-COPY-PASS\r\n");
  for (unsigned n = 1; n < sizeof fixtures / sizeof *fixtures; n++) {
    long pid = spawn(n,64);
    require(pid > 0 && !process_run(pid), "run fault worker");
    manager_survives();
    struct process_info info = inspect(pid);
    console_puts("case/state/reason/vector="); console_hex(n); console_hex(info.state);
    console_hex(info.reason); console_hex(info.fault_vector); console_puts("\r\n");
    require(info.state == PROCESS_EXITED, "fault worker still active");
    if (n == 7) require(info.reason == PROCESS_BAD_CONTEXT, "invalid stack return");
    else if (n == 11 || n == 12 || n == 13) {
      require(info.reason == PROCESS_NORMAL && info.status == 0, "flag/output boundary");
      if (n == 13) require(info.output_bytes == PROCESS_OUTPUT_CAPACITY, "output bound");
    }
    else require(info.reason == PROCESS_FAULT && info.fault_vector == fixtures[n].vector,
                 "wrong fault boundary");
    require(!process_reap(pid), "reap fault worker");
    same_pool(baseline);
  }
  console_puts("PROCESS-FAULT-BOUNDARY-PASS\r\n");
  long ids[PROCESS_CAPACITY];
  for (unsigned i = 0; i < PROCESS_CAPACITY; i++) {
    ids[i] = spawn(0,64); require(ids[i] > 0,"fill process table");
  }
  require(spawn(0,64) == -11 && process_list(NULL,0) == PROCESS_CAPACITY,
          "process table bound");
  for (unsigned i = 0; i < PROCESS_CAPACITY; i++)
    require(!process_terminate(ids[i]) && !process_terminate(ids[i]) && !process_reap(ids[i]),
            "terminate/reap lifecycle");
  a = spawn(0,64);
  require(a > ids[PROCESS_CAPACITY-1] && process_terminate(ids[0]) == -3 &&
              inspect(a).state == PROCESS_READY, "stale PID targeted new worker");
  require(!process_terminate(a) && !process_reap(a), "new generation cleanup");
  same_pool(baseline);
  for (long fail = 0; fail < 32; fail++) {
    memory_test_fail_after(fail);
    a = spawn(0,64);
    memory_test_fail_after(-1);
    if (a > 0) require(!process_terminate(a) && !process_reap(a), "fault injection cleanup");
    else require(a == -12, "allocation failure result");
    same_pool(baseline);
  }
  require(spawn(0,3) == -12, "worker page quota");
  same_pool(baseline);
  size_t bytes = fixtures[0].end - fixtures[0].start;
  unsigned char *bad = kmalloc(bytes);
  require(bad != NULL,"malformed ELF buffer");
  memcpy(bad,fixtures[0].start,bytes);
  struct elf_header *h = (void *)bad;
  struct elf_program *ph = (void *)(bad+h->phoff);
  uint64_t entry = h->entry;
  h->entry = 0x10000;
  require(process_spawn(bad,bytes,64) == -8,"kernel ELF entry");
  h->entry = entry;
  uint32_t type = ph[0].type;
  ph[0].type = 3;
  require(process_spawn(bad,bytes,64) == -8,"ELF interpreter rejection");
  ph[0].type = type;
  ph[1] = ph[0];
  require(process_spawn(bad,bytes,64) == -8,"overlapping ELF pages");
  kfree(bad);
  for (unsigned i = 0; i < 128; i++) {
    a = spawn(0,64);
    require(a > 0 && !process_terminate(a) && !process_reap(a), "repeated process cleanup");
  }
  same_pool(baseline);
  void *coalesced = kmalloc(0x03ff0000);
  require(coalesced != NULL,"kernel stack/metadata leak");
  kfree(coalesced);
  require(process_list(NULL,0) == 0,"process records leaked");
  console_puts("PROCESS-LIFECYCLE-ROLLBACK-PASS\r\nPROCESS-PROBE-PASS\r\n");
  __asm__ volatile("outl %0,%1" ::"a"(0x10), "Nd"((uint16_t)0xf4));
  for (;;) __asm__ volatile("cli; hlt");
}

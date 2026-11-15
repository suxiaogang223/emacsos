/* Real MMU/frame-pool checks; this probe does not create protected processes. */
#include "kernel.h"
#include "memory.h"
void storage_control_service(void) {}
void network_service(void) {}
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
  require(before.free_pages == after.free_pages &&
              before.data_pages == after.data_pages &&
              before.table_pages == after.table_pages, "physical frame leak");
}
static uintptr_t root(void) {
  uintptr_t result;
  __asm__ volatile("mov %%cr3,%0" : "=r"(result));
  return result;
}
static void normal(void) {
  struct address_space *main = memory_current_space();
  struct memory_pool_usage initial = usage();
  require(initial.free_pages == MEMORY_PAGE_CAPACITY, "initial reserved pool");
  long address = memory_map(0, 2 * PAGE_SIZE, 3, 0x22);
  require(address == 0x08000000, "main mmap layout");
  unsigned char *bytes = (void *)address;
  bytes[0] = 19;
  bytes[PAGE_SIZE] = 42;
  require(!memory_protect(address, 2 * PAGE_SIZE, 1), "protect remapped frames");
  require(bytes[0] == 19 && bytes[PAGE_SIZE] == 42, "mprotect changed backing");
  require(!memory_protect(address, 2 * PAGE_SIZE, 3), "restore writable pages");
  require(memory_protect(address, 3 * PAGE_SIZE, 1) == -12, "hole protect");
  bytes[0] = 23; /* A rejected multi-page protect must not change its prefix. */
  uintptr_t main_root = root();
  struct address_space *a = memory_space_create(64), *b = memory_space_create(64);
  require(a && b, "create address spaces");
  require(memory_space_map(a, address, PAGE_SIZE, 3, 0x32) == address &&
              memory_space_map(b, address, PAGE_SIZE, 3, 0x32) == address,
          "map same virtual address");
  require(!memory_space_activate(a) && root() != main_root, "activate A");
  uintptr_t a_root = root();
  require(bytes[0] == 0, "A zero fill");
  bytes[0] = 71;
  require(memory_space_destroy(a) == -16, "destroy active space");
  require(!memory_space_activate(b) && root() != a_root, "activate B");
  require(bytes[0] == 0, "B private zero fill");
  bytes[0] = 93;
  require(!memory_space_activate(a) && bytes[0] == 71, "A retains private bytes");
  require(!memory_space_activate(main) && bytes[0] == 23 && bytes[PAGE_SIZE] == 42,
          "main survives CR3 switches");
  require(!memory_space_destroy(a) && !memory_space_destroy(b), "destroy spaces");
  console_puts("MEMORY-ADDRESS-SPACES-PASS\r\n");

  struct memory_pool_usage baseline = usage();
  for (long fail = 0; fail < 3; fail++) {
    memory_test_fail_after(fail);
    require(!memory_space_create(64), "create failure injection");
    memory_test_fail_after(-1);
    same_pool(baseline);
  }
  a = memory_space_create(64);
  require(a != NULL, "rollback test space");
  uintptr_t crossing = 0x081ff000; /* Two page tables across a 2 MiB boundary. */
  for (long fail = 0; fail < 5; fail++) {
    struct memory_pool_usage before = usage();
    memory_test_fail_after(fail);
    require(memory_space_map(a, crossing, 3 * PAGE_SIZE, 3, 0x32) == -12,
            "map failure injection");
    memory_test_fail_after(-1);
    same_pool(before);
  }
  require(memory_space_map(a, crossing, 3 * PAGE_SIZE, 3, 0x32) == (long)crossing,
          "map after allocation failure");
  require(!memory_space_activate(a), "activate rollback test");
  unsigned char *cross = (void *)crossing;
  cross[0] = 61;
  struct memory_pool_usage before = usage();
  for (long fail = 0; fail < 2; fail++) {
    memory_test_fail_after(fail);
    require(memory_space_map(a, crossing, 5 * PAGE_SIZE, 3, 0x32) == -12 && cross[0] == 61,
            "failed fixed replacement changed old bytes");
    memory_test_fail_after(-1);
    same_pool(before);
  }
  require(!memory_space_protect(a, crossing, 3 * PAGE_SIZE, 0), "PROT_NONE");
  require(!memory_space_unmap(a, crossing, 3 * PAGE_SIZE), "unmap PROT_NONE frames");
  struct memory_usage stats;
  memory_space_usage(a, &stats);
  require(stats.data_pages == 0 && stats.table_pages == 3, "empty table reclamation");
  require(!memory_space_activate(main) && !memory_space_destroy(a), "rollback cleanup");
  same_pool(baseline);
  console_puts("MEMORY-ROLLBACK-PASS\r\n");

  a = memory_space_create(7);
  require(a != NULL, "quota space");
  require(memory_space_map(a, address, 3 * PAGE_SIZE, 3, 0x32) == address,
          "exact quota allocation");
  memory_space_usage(a, &stats);
  require(stats.data_pages == 3 && stats.table_pages == 4, "table-inclusive quota");
  before = usage();
  require(memory_space_map(a, 0, PAGE_SIZE, 3, 0x22) == -12, "quota rejection");
  same_pool(before);
  require(memory_space_map(a, address, 3 * PAGE_SIZE, 3, 0x32) == address,
          "fixed replacement at full quota");
  same_pool(before);
  require(!memory_space_destroy(a), "quota cleanup");
  for (unsigned i = 0; i < 64; i++) {
    a = memory_space_create(8);
    require(a && memory_space_map(a, address, PAGE_SIZE, 3, 0x32) == address,
            "repeated creation");
    require(!memory_space_activate(a) && bytes[0] == 0, "recycled page disclosure");
    bytes[0] = 255;
    require(!memory_space_activate(main) && !memory_space_destroy(a), "repeated destroy");
  }
  same_pool(baseline);
  console_puts("MEMORY-QUOTA-REUSE-PASS\r\n");

  long old_brk = memory_brk(0);
  const long page = PAGE_SIZE;
  require(memory_brk(old_brk + page + 17) == old_brk + page + 17,
          "grow brk");
  unsigned char *heap = (void *)old_brk;
  require(heap[PAGE_SIZE + 16] == 0, "brk zero fill");
  heap[12] = 4;
  heap[PAGE_SIZE] = 5;
  require(memory_brk(old_brk + 7) == old_brk + 7, "shrink brk");
  require(memory_brk(old_brk + page + 17) == old_brk + page + 17 &&
              heap[12] == 0 && heap[PAGE_SIZE] == 0, "regrow brk clears bytes");
  before = usage();
  memory_test_fail_after(0);
  require(memory_brk(old_brk + 5 * page) == old_brk + page + 17,
          "brk failure changed break");
  memory_test_fail_after(-1);
  same_pool(before);
  require(memory_brk(old_brk) == old_brk, "release brk pages");
  same_pool(baseline);
  require(memory_map(0, SIZE_MAX, 3, 0x22) == -22 &&
              memory_protect(UINTPTR_MAX - 4095, PAGE_SIZE, 3) == -22 &&
              memory_unmap(address, SIZE_MAX) == -22,
          "overflow bounds");
  console_puts("MEMORY-BRK-BOUNDS-PASS\r\n");

  a = memory_space_create(MEMORY_PAGE_CAPACITY);
  require(a != NULL, "exhaustion space");
  for (size_t chunk = 4096; chunk; chunk /= 4)
    while (memory_space_map(a, 0, chunk * PAGE_SIZE, 3, 0x22) >= 0) {}
  require(usage().free_pages <= 1, "physical pool was not exhausted");
  require(bytes[0] == 23, "pool exhaustion damaged main space");
  require(!memory_space_destroy(a), "exhaustion reclamation");
  same_pool(baseline);
  require(!memory_unmap(address, 2 * PAGE_SIZE), "main unmap");
  same_pool(initial);
  console_puts("MEMORY-EXHAUSTION-PASS\r\n");
}
_Noreturn void kernel_main(void) {
  console_init();
  memory_init(0);
  hardware_init();
#ifdef MEMORY_PROBE_DIRECT_NX
  ((void (*)(void))(MEMORY_DIRECT_BASE + 0x10000))();
#elif defined(MEMORY_PROBE_READONLY) || defined(MEMORY_PROBE_NX)
  struct address_space *space = memory_space_create(8);
  require(space && memory_space_map(space, 0x08000000, PAGE_SIZE, 3, 0x32) > 0,
          "fault space setup");
  require(!memory_space_activate(space), "fault space activation");
#ifdef MEMORY_PROBE_READONLY
  require(!memory_space_protect(space, 0x08000000, PAGE_SIZE, 1), "fault protect");
  *(volatile unsigned char *)0x08000000 = 5;
#else
  ((void (*)(void))0x08000000)();
#endif
#else
  normal();
  console_puts("MEMORY-PROBE-PASS\r\n");
  __asm__ volatile("outl %0,%1" ::"a"(0x10), "Nd"((uint16_t)0xf4));
#endif
  panic("memory probe returned");
}

/* Deterministic ATA error/status simulation complements the real-QEMU probe. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
#include "block.h"
static enum { NORMAL, ABSENT, BUSY, BAD_SIZE, COMMAND_ERROR } mode;
static unsigned word_index, command_count;
static uint8_t status = 0x50;
static uint64_t now;
static bool expected_panic;
static jmp_buf panic_return;
uint64_t clock_monotonic_ns(void) { return now += 1000000000UL; }
uint64_t block_interrupts_begin(void) { return 0x200; }
void block_interrupts_end(uint64_t flags) { assert(flags == 0x200); }
uint8_t block_in8(uint16_t port) {
  if (port == 0x1f4 || port == 0x1f5)
    return 0;
  if (mode == ABSENT)
    return 0;
  if (mode == BUSY)
    return 0x80;
  return status;
}
uint16_t block_in16(uint16_t port) {
  assert(port == 0x1f0);
  unsigned index = word_index++;
  if (word_index == 256)
    status = 0x50;
  if (index == 49)
    return 0x200;
  if (index == 60)
    return 0;
  if (index == 61)
    return mode == BAD_SIZE ? 1 : 2;
  if (index == 83)
    return 1U << 12;
  return 0;
}
void block_out8(uint16_t port, uint8_t value) {
  if (port == 0x1f7) {
    command_count++;
    word_index = 0;
    status = mode == COMMAND_ERROR ? 0x51 : value == 0xe7 ? 0x50 : 0x58;
  }
}
void block_out16(uint16_t port, uint16_t value) {
  (void)value;
  assert(port == 0x1f0);
  if (++word_index == 256)
    status = 0x50;
}
_Noreturn void panic(const char *message) {
  if (expected_panic)
    longjmp(panic_return, 1);
  fprintf(stderr, "Unexpected panic: %s\n", message);
  abort();
}
int main(void) {
  unsigned char bytes[512] = {0};
  mode = ABSENT;
  assert(data_disk_init() == -19 && data_disk_read(0, bytes, 1) == -19);
  mode = BUSY;
  assert(data_disk_init() == -110 && command_count == 0);
  mode = BAD_SIZE;
  status = 0x50;
  assert(data_disk_init() == -95 && data_disk_read(0, bytes, 1) == -95);
  mode = NORMAL;
  assert(data_disk_init() == 0);
  mode = BUSY;
  unsigned before = command_count;
  assert(data_disk_write(0, bytes, 1) == -110 && command_count == before);
  expected_panic = true;
  if (!setjmp(panic_return)) {
    disk_read(0, bytes, 1);
    assert(false);
  }
  expected_panic = false;
  assert(command_count == before);
  mode = COMMAND_ERROR;
  assert(data_disk_write(0, bytes, 1) == -5);
  assert(data_disk_read(0, bytes, 1) == -5);
  assert(data_disk_flush() == -5);
  puts("ATA absence, pre-command timeout, unsupported size and command errors passed.");
}

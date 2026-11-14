#include "kernel.h"
#include "block.h"

/* One polled primary ATA channel; no command is issued from interrupt context. */
#define ATA_DATA 0x1f0
#define ATA_STATUS 0x1f7
#define ATA_ALT_STATUS 0x3f6
#define ATA_TIMEOUT_NS 5000000000UL
static uint64_t data_sectors;
static int data_error = -19;
#ifdef EMACSOS_BLOCK_TEST
extern uint8_t block_in8(uint16_t);
extern uint16_t block_in16(uint16_t);
extern void block_out8(uint16_t, uint8_t);
extern void block_out16(uint16_t, uint16_t);
extern uint64_t block_interrupts_begin(void);
extern void block_interrupts_end(uint64_t);
#else
static uint8_t block_in8(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static uint16_t block_in16(uint16_t p) {
  uint16_t v;
  __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static void block_out8(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
static void block_out16(uint16_t p, uint16_t v) {
  __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p));
}
static uint64_t block_interrupts_begin(void) {
  uint64_t flags;
  /* SYSCALL masks IF; PIT interrupts must advance the bounded polling clock. */
  __asm__ volatile("pushfq; popq %0; sti" : "=r"(flags) :: "memory");
  return flags;
}
static void block_interrupts_end(uint64_t flags) {
  if (!(flags & 0x200))
    __asm__ volatile("cli" ::: "memory");
}
#endif
static void delay_select(void) {
  for (unsigned i = 0; i < 4; i++)
    (void)block_in8(ATA_ALT_STATUS);
}
static int wait_status(bool data, bool check_error) {
  uint64_t start = clock_monotonic_ns();
  for (;;) {
    uint8_t s = block_in8(ATA_STATUS);
    if (s == 0 || s == 0xff)
      return -19;
    if (!(s & 0x80)) {
      if (check_error && (s & 0x21))
        return -5;
      if (data ? (s & 8) : !(s & 8))
        return 0;
    }
    if (clock_monotonic_ns() - start >= ATA_TIMEOUT_NS)
      return -110;
    __asm__ volatile("pause");
  }
}
static int select_disk(unsigned drive, uint64_t lba) {
  int error = wait_status(false, false);
  if (error && error != -19)
    return error;
  block_out8(0x1f6, 0xe0 | (drive << 4) | ((lba >> 24) & 15));
  delay_select();
  return wait_status(false, false);
}
static int transfer(unsigned drive, uint64_t lba, void *data, size_t count,
                    bool writing) {
  unsigned char *bytes = data;
  if (lba > 0x0fffffff || count > 0x10000000UL - lba ||
      (count && !data))
    return -22;
  uint64_t flags = block_interrupts_begin();
  int error = 0;
  /* Disable device IRQs: the shared channel is serviced only by polling. */
  block_out8(ATA_ALT_STATUS, 2);
  while (count--) {
    error = select_disk(drive, lba);
    if (error)
      break;
    block_out8(0x1f2, 1);
    block_out8(0x1f3, lba);
    block_out8(0x1f4, lba >> 8);
    block_out8(0x1f5, lba >> 16);
    block_out8(ATA_STATUS, writing ? 0x30 : 0x20);
    delay_select();
    error = wait_status(true, true);
    if (error)
      break;
    for (unsigned i = 0; i < 256; i++) {
      if (writing)
        block_out16(ATA_DATA, bytes[2 * i] | (bytes[2 * i + 1] << 8));
      else {
        uint16_t word = block_in16(ATA_DATA);
        bytes[2 * i] = word;
        bytes[2 * i + 1] = word >> 8;
      }
    }
    delay_select();
    error = wait_status(false, true);
    if (error)
      break;
    bytes += DATA_DISK_SECTOR_SIZE;
    lba++;
  }
  block_interrupts_end(flags);
  return error;
}
void disk_read(uint64_t lba, void *data, size_t count) {
  if (transfer(0, lba, data, count, false))
    panic("SYSTEM ATA read failed or timed out");
}
int data_disk_init(void) {
  data_sectors = 0;
  uint64_t flags = block_interrupts_begin();
  block_out8(ATA_ALT_STATUS, 2);
  int error = select_disk(1, 0);
  if (!error) {
    block_out8(0x1f2, 0);
    block_out8(0x1f3, 0);
    block_out8(0x1f4, 0);
    block_out8(0x1f5, 0);
    block_out8(ATA_STATUS, 0xec);
    delay_select();
    error = wait_status(true, true);
    if (!error && (block_in8(0x1f4) || block_in8(0x1f5)))
      error = -95;
    if (!error) {
      uint16_t words[256];
      for (unsigned i = 0; i < 256; i++)
        words[i] = block_in16(ATA_DATA);
      delay_select();
      error = wait_status(false, true);
      uint64_t sectors = words[60] | ((uint64_t)words[61] << 16);
      if (!error && (!(words[49] & 0x200) || sectors != DATA_DISK_SECTORS ||
                     !(words[83] & (1U << 12))))
        error = -95;
      if (!error)
        data_sectors = sectors;
    }
  }
  block_interrupts_end(flags);
  data_error = error;
  return error;
}
static int check_bounds(uint64_t sector, const void *data, size_t count) {
  if (!data_sectors)
    return data_error;
  if (sector > data_sectors || count > data_sectors - sector ||
      (count && !data))
    return -22;
  return 0;
}
int data_disk_read(uint64_t sector, void *data, size_t count) {
  int error = check_bounds(sector, data, count);
  return error ? error : transfer(1, sector, data, count, false);
}
int data_disk_write(uint64_t sector, const void *data, size_t count) {
  int error = check_bounds(sector, data, count);
  return error ? error : transfer(1, sector, (void *)data, count, true);
}
int data_disk_flush(void) {
  if (!data_sectors)
    return data_error;
  uint64_t flags = block_interrupts_begin();
  int error = select_disk(1, 0);
  if (!error) {
    block_out8(ATA_STATUS, 0xe7);
    delay_select();
    error = wait_status(false, true);
  }
  block_interrupts_end(flags);
  return error;
}
uint64_t data_disk_sectors(void) { return data_sectors; }

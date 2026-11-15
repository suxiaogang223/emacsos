#include "ne2000.h"
#include "kernel.h"

#define BASE 0x300
#define RX_FIRST 0x46
#define RX_STOP 0x80
#define TX_PAGE 0x40
#define IO_BUDGET 100000
static unsigned next_page;
static bool present;
static inline void out8(unsigned p, uint8_t v) {
  __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"((uint16_t)p));
}
static inline uint8_t in8(unsigned p) {
  uint8_t v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"((uint16_t)p));
  return v;
}
static inline void out16(unsigned p, uint16_t v) {
  __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"((uint16_t)p));
}
static inline uint16_t in16(unsigned p) {
  uint16_t v;
  __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"((uint16_t)p));
  return v;
}
static void reg(unsigned p, uint8_t v) { out8(BASE + p, v); }
static uint8_t rd(unsigned p) { return in8(BASE + p); }
static bool wait_bit(unsigned regno, unsigned bits, bool set) {
  for (unsigned i = 0; i < IO_BUDGET; i++)
    if (!!(rd(regno) & bits) == set)
      return true;
  return false;
}
static bool dma(uint16_t address, void *data, unsigned n, bool write) {
  unsigned rounded = (n + 1) & ~1u;
  unsigned char *p = data;
  reg(0, 0x22);
  reg(7, 0x40);
  reg(10, rounded);
  reg(11, rounded >> 8);
  reg(8, address);
  reg(9, address >> 8);
  reg(0, write ? 0x12 : 0x0a);
  for (unsigned i = 0; i < rounded; i += 2) {
    if (write)
      out16(BASE + 16, p[i] | (i + 1 < n ? (uint16_t)p[i + 1] << 8 : 0));
    else {
      uint16_t v = in16(BASE + 16);
      p[i] = v;
      if (i + 1 < n)
        p[i + 1] = v >> 8;
    }
  }
  bool ok = wait_bit(7, 0x40, true);
  reg(7, 0x40);
  reg(0, 0x22);
  return ok;
}
bool ne2000_init(uint8_t mac[6]) {
  present = false;
  next_page = RX_FIRST + 1;
  if (rd(0) == 0xff)
    return false;
  uint8_t reset = rd(31);
  reg(31, reset);
  if (!wait_bit(7, 0x80, true))
    return false;
  reg(0, 0x21);
  reg(14, 0x49);
  reg(10, 0);
  reg(11, 0);
  reg(12, 0x20);
  reg(13, 2);
  uint8_t prom[32];
  if (!dma(0, prom, sizeof prom, false))
    return false;
  bool zero = true, ones = true;
  for (unsigned i = 0; i < 6; i++) {
    mac[i] = prom[2 * i];
    zero &= mac[i] == 0;
    ones &= mac[i] == 255;
  }
  if (zero || ones || (mac[0] & 1))
    return false;
  reg(0, 0x21);
  reg(1, RX_FIRST);
  reg(2, RX_STOP);
  reg(3, RX_FIRST);
  reg(4, TX_PAGE);
  reg(7, 0xff);
  reg(15, 0);
  reg(0, 0x61);
  for (unsigned i = 0; i < 6; i++)
    reg(1 + i, mac[i]);
  reg(7, next_page);
  for (unsigned i = 0; i < 8; i++)
    reg(8 + i, 0);
  reg(0, 0x22);
  reg(12, 4);
  reg(13, 0);
  present = true;
  return true;
}
enum net_error ne2000_transmit(const void *data, size_t n) {
  if (!present)
    return NET_NO_DEVICE;
  if (n < 14 || n > NE2000_FRAME_MAX)
    return NET_INVALID;
  if (!wait_bit(0, 4, false))
    return NET_WOULD_BLOCK;
  unsigned char frame[NE2000_FRAME_MAX];
  size_t length = n < 60 ? 60 : n;
  memcpy(frame, data, n);
  if (length > n)
    memset(frame + n, 0, length - n);
  reg(7, 0x0a);
  if (!dma(TX_PAGE << 8, frame, length, true))
    return NET_IO;
  reg(4, TX_PAGE);
  reg(5, length);
  reg(6, length >> 8);
  reg(0, 0x26);
  if (!wait_bit(0, 4, false))
    return NET_IO;
  unsigned status = rd(7);
  reg(7, 0x0a);
  return status & 8 ? NET_IO : NET_OK;
}
enum net_error ne2000_receive(void *data, size_t capacity, size_t *got) {
  *got = 0;
  if (!present)
    return NET_NO_DEVICE;
  unsigned status = rd(7);
  if (status & 0x10) {
    /* Discard the overrun ring before restarting reception. */
    reg(0, 0x21);
    reg(10, 0);
    reg(11, 0);
    reg(13, 2);
    reg(0, 0x62);
    unsigned current = rd(7);
    reg(0, 0x22);
    if (current < RX_FIRST || current >= RX_STOP)
      return NET_IO;
    next_page = current;
    reg(3, current == RX_FIRST ? RX_STOP - 1 : current - 1);
    reg(7, 0x1f);
    reg(13, 0);
    return NET_OK;
  }
  reg(0, 0x62);
  unsigned current = rd(7);
  reg(0, 0x22);
  if (current < RX_FIRST || current >= RX_STOP)
    return NET_IO;
  if (current == next_page)
    return NET_OK;
  uint8_t header[4];
  if (!dma(next_page << 8, header, 4, false))
    return NET_IO;
  unsigned length = header[2] | header[3] << 8;
  unsigned pages = (length + 4 + 255) / 256;
  unsigned expected = next_page + pages;
  if (expected >= RX_STOP)
    expected -= RX_STOP - RX_FIRST;
  if (!(header[0] & 1) || (header[0] & 0x1e) || length < 18 ||
      length > NE2000_FRAME_MAX + 4 || header[1] != expected ||
      header[1] < RX_FIRST || header[1] >= RX_STOP) {
    next_page = current;
    reg(3, current == RX_FIRST ? RX_STOP - 1 : current - 1);
    reg(7, 0x05);
    return NET_OK;
  }
  length -= 4; /* The NIC ring count includes the four-byte Ethernet CRC. */
  if (length <= capacity) {
    unsigned address = (next_page << 8) + 4, first = (RX_STOP << 8) - address;
    if (first > length)
      first = length;
    if (!dma(address, data, first, false))
      return NET_IO;
    if (first < length &&
        !dma(RX_FIRST << 8, (uint8_t *)data + first, length - first, false))
      return NET_IO;
    *got = length;
  }
  next_page = header[1];
  reg(3, next_page == RX_FIRST ? RX_STOP - 1 : next_page - 1);
  reg(7, 0x05);
  return NET_OK;
}

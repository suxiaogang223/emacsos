#include "kernel.h"
#include "storage_control.h"
#include "network.h"
static inline void out8(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint8_t in8(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static volatile uint64_t ticks;
static unsigned char input[4096];
static volatile unsigned head, tail;
void console_init(void) {
  out8(0x3fb, 0x80);
  out8(0x3f8, 1);
  out8(0x3f9, 0);
  out8(0x3fb, 3);
  out8(0x3fa, 0xc7);
  out8(0x3fc, 0x0b);
  out8(0x3f9, 1);
}
void console_write(const void *data, size_t n) {
  const unsigned char *s = data;
  while (n--) {
    while (!(in8(0x3fd) & 0x20))
      __asm__ volatile("pause");
    out8(0x3f8, *s++);
  }
}
void console_puts(const char *s) { console_write(s, strlen(s)); }
void console_hex(uint64_t v) {
  char b[] = "0x0000000000000000";
  for (int i = 17; i >= 2; --i) {
    b[i] = "0123456789abcdef"[v & 15];
    v >>= 4;
  }
  console_write(b, 18);
}
static void drain_serial(void) {
  uint64_t saved_flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_flags)::"memory");
  while (in8(0x3fd) & 1) {
    unsigned char c = in8(0x3f8);
    unsigned next = (head + 1) % sizeof input;
    if (next != tail) {
      input[head] = c;
      head = next;
    }
  }
  if (saved_flags & 0x200)
    __asm__ volatile("sti" ::: "memory");
}
bool console_ready(void) {
  drain_serial();
  return head != tail;
}
long console_read(void *p, size_t n) {
  unsigned char *s = p;
  size_t got = 0;
  drain_serial();
  while (got < n && tail != head) {
    s[got++] = input[tail];
    tail = (tail + 1) % sizeof input;
  }
  return got ? (long)got : (n ? -11 : 0);
}
uint64_t clock_monotonic_ns(void) { return ticks * 1000000UL; }
void wait_interrupt(void) {
  storage_control_service();
  network_service();
  __asm__ volatile("sti; hlt" ::: "memory");
}
_Noreturn void panic(const char *s) {
  __asm__ volatile("cli");
  console_puts("\r\nPANIC: ");
  console_puts(s);
  console_puts("\r\n");
  for (;;)
    __asm__ volatile("hlt");
}
struct idt_gate {
  uint16_t low, cs;
  uint8_t ist, type;
  uint16_t mid;
  uint32_t high, zero;
} __attribute__((packed));
static struct idt_gate idt[256];
struct tss {
  uint32_t zero;
  uint64_t rsp[3];
  uint64_t reserved;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3, iomap;
} __attribute__((packed));
static struct tss tss;
static unsigned char irq_stack[32768] __attribute__((aligned(16)));
extern uintptr_t interrupt_stubs[48];
extern uint64_t runtime_gdt[];
void hardware_init(void) {
  tss.ist[0] = (uintptr_t)irq_stack + sizeof irq_stack;
  tss.iomap = sizeof tss;
  uint64_t b = (uintptr_t)&tss;
  runtime_gdt[3] = (sizeof(tss) - 1) | ((b & 0xffffff) << 16) | (0x89UL << 40) |
                   ((b & 0xff000000) << 32);
  runtime_gdt[4] = b >> 32;
  uint16_t sel = 0x18;
  __asm__ volatile("ltr %0" ::"r"(sel));
  for (unsigned i = 0; i < 48; i++) {
    uintptr_t a = interrupt_stubs[i];
    idt[i] = (struct idt_gate){a, 8, 1, 0x8e, a >> 16, a >> 32, 0};
  }
  struct {
    uint16_t limit;
    uintptr_t base;
  } __attribute__((packed)) p = {sizeof idt - 1, (uintptr_t)idt};
  __asm__ volatile("lidt %0" ::"m"(p));
  out8(0x20, 0x11);
  out8(0xa0, 0x11);
  out8(0x21, 0x20);
  out8(0xa1, 0x28);
  out8(0x21, 4);
  out8(0xa1, 2);
  out8(0x21, 1);
  out8(0xa1, 1);
  out8(0x21, 0xee);
  out8(0xa1, 0xff);
  unsigned divisor = 1193;
  out8(0x43, 0x36);
  out8(0x40, divisor);
  out8(0x40, divisor >> 8);
  __asm__ volatile("sti");
}
void interrupt_dispatch(uint64_t *f) {
  unsigned vector = f[15];
  if (vector == 32)
    ticks++;
  else if (vector == 36)
    drain_serial();
  else if (vector < 32) {
    console_puts("\r\nFAULT vector=");
    console_hex(vector);
    console_puts(" error=");
    console_hex(f[16]);
    console_puts(" rip=");
    console_hex(f[17]);
    uint64_t cr2;
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    console_puts(" cr2=");
    console_hex(cr2);
    panic("CPU exception");
  }
  if (vector >= 40)
    out8(0xa0, 0x20);
  if (vector >= 32)
    out8(0x20, 0x20);
}
static unsigned rtc_value(unsigned reg) {
  out8(0x70, reg);
  return in8(0x71);
}
static unsigned decimal(unsigned x) { return (x & 15) + 10 * (x >> 4); }
uint64_t clock_realtime_ns(void) {
  static uint64_t epoch;
  static bool initialized;
  if (!initialized) {
    while (rtc_value(10) & 0x80)
      __asm__ volatile("pause");
    unsigned sec = rtc_value(0), min = rtc_value(2), hour = rtc_value(4),
             day = rtc_value(7), month = rtc_value(8), year = rtc_value(9);
    if (!(rtc_value(11) & 4)) {
      sec = decimal(sec);
      min = decimal(min);
      hour = decimal(hour);
      day = decimal(day);
      month = decimal(month);
      year = decimal(year);
    }
    year += 2000;
    uint64_t days = 0;
    for (unsigned y = 1970; y < year; y++)
      days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    static const unsigned md[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
    for (unsigned m = 1; m < month; m++)
      days += md[m - 1] +
              (m == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    days += day - 1;
    epoch = ((days * 24 + hour) * 60 + min) * 60 + sec;
    initialized = true;
  }
  return epoch * 1000000000UL + clock_monotonic_ns();
}

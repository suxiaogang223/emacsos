#include "kernel.h"
#include "files.h"
#include "storage_control.h"

/* FS owns the barrier, dirty generation, synchronization and device flush. */
extern enum fs_error fs_freeze_data(void);
extern void fs_thaw_data(void);
extern bool fs_data_mounted(void);
extern bool fs_data_frozen(void);
extern uint64_t fs_data_generation(void);
extern enum fs_error fs_data_last_error(void);
extern long fs_error_errno(enum fs_error);
#define FRAME_SIZE 128
#define FRAME_TIMEOUT_NS 1000000000UL
#define TX_TIMEOUT_NS 2000000000UL
static char input[FRAME_SIZE], output[FRAME_SIZE];
static size_t input_size, output_size, output_offset;
static uint64_t input_since, output_since, session, last_request;
static bool initialized, servicing, dropping;
static uint8_t port_read(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}
static void port_write(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
void storage_control_init(void) {
  /* COM2 is polled; timer IRQs wake the idle loop without UART IRQ work. */
  port_write(0x2f9, 0);
  port_write(0x2fb, 0x80);
  port_write(0x2f8, 1);
  port_write(0x2f9, 0);
  port_write(0x2fb, 3);
  port_write(0x2fa, 0xc7);
  port_write(0x2fc, 3);
  initialized = true;
}
static void append(const char *s) {
  while (*s && output_size < sizeof output)
    output[output_size++] = *s++;
}
static void append_hex(uint64_t n) {
  for (unsigned i = 0; i < 16; i++) {
    output[output_size++] = "0123456789abcdef"[n >> 60];
    n <<= 4;
  }
}
static void append_errno(unsigned error) {
  char digits[10];
  unsigned n = 0;
  do {
    digits[n++] = '0' + error % 10;
    error /= 10;
  } while (error && n < sizeof digits);
  while (n)
    output[output_size++] = digits[--n];
}
static bool hex(const char *p, uint64_t *value) {
  *value = 0;
  for (unsigned i = 0; i < 16; i++) {
    unsigned char c = p[i];
    unsigned digit = c >= '0' && c <= '9' ? c - '0'
                     : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
    if (digit == 16)
      return false;
    *value = (*value << 4) | digit;
  }
  return true;
}
static bool equal(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}
static void reply(uint64_t s, uint64_t request, const char *verb, long error) {
  output_size = output_offset = 0;
  append("EOS1 ");
  append_hex(s);
  append(" ");
  append_hex(request);
  append(" ");
  append(verb);
  append(" ");
  append_hex(fs_data_generation());
  append(" ");
  append_errno(error < 0 ? (unsigned)-error : (unsigned)error);
  append(fs_data_mounted() ? " 1" : " 0");
  append(fs_data_frozen() ? " 1\n" : " 0\n");
  output_since = clock_monotonic_ns();
}
static void command(void) {
  uint64_t s, request;
  if (input_size < 40 || memcmp(input, "EOS1 ", 5) || input[21] != ' ' ||
      input[38] != ' ' || !hex(input + 5, &s) ||
      !hex(input + 22, &request) || !s || !request)
    return;
  const char *verb = input + 39;
  bool status = equal(verb, "STATUS"), freeze = equal(verb, "FREEZE"),
       thaw = equal(verb, "THAW");
  long error = 0;
  const char *result = "ERROR";
  if (!status && !freeze && !thaw)
    error = 22;
  else if ((!session && !status) || (session && session != s) ||
           request <= last_request)
    error = 116;
  else {
    if (!session)
      session = s;
    last_request = request;
    if (status) {
      result = "STATUS";
      if (!fs_data_mounted())
        error = fs_error_errno(fs_data_last_error());
    } else if (!fs_data_mounted())
      error = 19;
    else if (freeze) {
      error = fs_error_errno(fs_freeze_data());
      if (!error && fs_data_frozen())
        result = "FROZEN";
      else if (!error)
        error = 5;
    } else {
      fs_thaw_data();
      result = "THAWED";
    }
  }
  reply(s, request, result, error);
}
void storage_control_service(void) {
  if (!initialized || servicing)
    return;
  servicing = true;
  uint64_t now = clock_monotonic_ns();
  if (input_size && now - input_since >= FRAME_TIMEOUT_NS) {
    input_size = 0;
    dropping = true;
  }
  /* A disconnected or stalled host must never block the guest event loop. */
  if (output_size && now - output_since >= TX_TIMEOUT_NS)
    output_size = output_offset = 0;
  for (unsigned budget = 0; budget < 256; budget++) {
    if (output_size) {
      if (!(port_read(0x2fd) & 0x20))
        break;
      port_write(0x2f8, output[output_offset++]);
      if (output_offset == output_size)
        output_size = output_offset = 0;
      continue;
    }
    uint8_t status = port_read(0x2fd);
    if (!(status & 1))
      break;
    unsigned char c = port_read(0x2f8);
    if (status & 0x1e) {
      input_size = 0;
      dropping = true;
    }
    if (c == '\n') {
      if (!dropping) {
        input[input_size] = 0;
        command();
      }
      dropping = false;
      input_size = 0;
    } else if (!dropping) {
      if (!input_size)
        input_since = clock_monotonic_ns();
      if (c < 0x20 || c > 0x7e || input_size == sizeof input - 1) {
        input_size = 0;
        dropping = true;
      } else
        input[input_size++] = c;
    }
  }
  servicing = false;
}

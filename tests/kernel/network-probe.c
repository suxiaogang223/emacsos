#include "kernel.h"
#include "network.h"
#ifndef NETWORK_PROBE_PORT
#define NETWORK_PROBE_PORT 45839
#endif
extern unsigned char __bss_end;
void storage_control_service(void) {}
static void require(bool yes, const char *what) {
  if (!yes)
    panic(what);
}
#ifndef NETWORK_PROBE_ABSENT
static void service(void) {
  network_service();
  wait_interrupt();
}
static void deadline(uint64_t until) {
  require(clock_monotonic_ns() < until, "network probe deadline");
}
static void close_ok(struct net_stream *s) {
  require(stream_close(s) == NET_OK, "close");
}
static void pattern(unsigned char *p, size_t n, size_t offset) {
  for (size_t i = 0; i < n; i++)
    p[i] = (unsigned char)((i + offset) * 17 + 3);
}
#endif
void kernel_main(void) {
  console_init();
  hardware_init();
  memory_init((uintptr_t)&__bss_end);
  network_init();
#ifdef NETWORK_PROBE_ABSENT
  require(!network_available(), "absent device");
  struct net_stream *s;
  require(stream_create(&s) == NET_NO_DEVICE && s == NULL, "absent create");
  console_puts("NETWORK-ABSENT-PASS\r\n");
#else
  require(network_available(), "device init");
  struct net_stream *slots[NET_STREAM_LIMIT], *extra;
  for (unsigned round = 0; round < 4; round++) {
    for (unsigned i = 0; i < NET_STREAM_LIMIT; i++)
      require(stream_create(&slots[i]) == NET_OK, "allocate all streams");
    require(stream_create(&extra) == NET_NO_MEMORY && extra == NULL,
            "stream bound");
    for (unsigned i = 0; i < NET_STREAM_LIMIT; i++)
      close_ok(slots[i]);
  }
  struct net_stream *s;
  require(stream_create(&s) == NET_OK, "create");
  struct net_address remote = {{10, 0, 2, 2}, NETWORK_PROBE_PORT}, local;
  require(stream_connect(s, &remote) == NET_IN_PROGRESS, "connect pending");
  require(stream_local(s, &local) == NET_OK && local.port != 0 &&
              local.ip[3] == 15,
          "real local endpoint");
  uint64_t until = clock_monotonic_ns() + 12000000000UL;
  while (!stream_write_ready(s)) {
    deadline(until);
    service();
  }
  require(stream_take_error(s) == NET_OK, "connect succeeded");
  unsigned char data[4096], reply[4096];
  size_t sent = 0, received = 0;
  unsigned char *large = kmalloc(NET_BUFFER_SIZE * 2);
  require(large != NULL, "large write buffer");
  pattern(large, NET_BUFFER_SIZE * 2, 0);
  require(stream_write(s, large, NET_BUFFER_SIZE * 2, &sent) == NET_OK &&
              sent == NET_BUFFER_SIZE,
          "accepted positive write prefix");
  size_t blocked;
  require(stream_write(s, large, 1, &blocked) == NET_WOULD_BLOCK &&
              blocked == 0,
          "bounded transmit backpressure");
  kfree(large);
  while (sent < 65536 || received < 65536) {
    deadline(until);
    size_t n;
    if (sent < 65536) {
      pattern(data, sizeof data, sent);
      enum net_error e = stream_write(s, data, sizeof data, &n);
      require(e == NET_OK || e == NET_WOULD_BLOCK, "write");
      if (e == NET_OK) {
        require(n > 0 && n <= sizeof data, "partial write count");
        sent += n;
      }
    }
    enum net_error e = stream_read(s, reply, sizeof reply, &n);
    require(e == NET_OK || e == NET_WOULD_BLOCK, "read");
    if (e == NET_OK && n) {
      pattern(data, n, received);
      require(!memcmp(data, reply, n), "echo bytes");
      received += n;
    }
    network_service();
  }
  require(stream_shutdown(s, false, true) == NET_OK, "half close");
  size_t n;
  require(stream_write(s, "x", 1, &n) == NET_BROKEN_PIPE && n == 0,
          "write after shutdown");
  for (;;) {
    deadline(until);
    enum net_error e = stream_read(s, reply, sizeof reply, &n);
    if (e == NET_OK && !n)
      break;
    require(e == NET_WOULD_BLOCK, "EOF after queued data");
    service();
  }
  close_ok(s);
  console_puts("NETWORK-ECHO-HALFCLOSE-PASS\r\n");
  /* A known closed host port must yield a consumable error and retained
   * failure. */
  require(stream_create(&s) == NET_OK, "refused create");
  remote.port = NETWORK_PROBE_PORT + 1;
  require(stream_connect(s, &remote) == NET_IN_PROGRESS, "refused pending");
  until = clock_monotonic_ns() + 12000000000UL;
  while (!stream_write_ready(s)) {
    deadline(until);
    service();
  }
  require(stream_take_error(s) == NET_REFUSED, "connection refused");
  require(stream_take_error(s) == NET_OK, "SO_ERROR read clears");
  require(stream_write(s, "x", 1, &n) == NET_REFUSED,
          "terminal write failure retained");
  require(stream_read(s, reply, 1, &n) == NET_REFUSED,
          "terminal read failure retained");
  close_ok(s);
  console_puts("NETWORK-REFUSED-PASS\r\n");
  /* Cancel a pending PCB, reuse its slot, and process late traffic repeatedly.
   */
  for (unsigned round = 0; round < 32; round++) {
    require(stream_create(&s) == NET_OK, "cancel create");
    remote.ip[3] = 99;
    require(stream_connect(s, &remote) == NET_IN_PROGRESS, "cancel pending");
    close_ok(s);
    require(stream_create(&s) == NET_OK, "reuse create");
    remote.ip[3] = 2;
    require(stream_connect(s, &remote) == NET_IN_PROGRESS, "reuse pending");
    until = clock_monotonic_ns() + 12000000000UL;
    while (!stream_write_ready(s)) {
      deadline(until);
      service();
    }
    require(stream_take_error(s) == NET_REFUSED, "reused callback identity");
    close_ok(s);
  }
  console_puts("NETWORK-CANCEL-REUSE-PASS\r\n");
  require(stream_create(&s) == NET_OK, "reset create");
  remote.port = NETWORK_PROBE_PORT;
  require(stream_connect(s, &remote) == NET_IN_PROGRESS, "reset pending");
  until = clock_monotonic_ns() + 12000000000UL;
  while (!stream_write_ready(s)) {
    deadline(until);
    service();
  }
  require(stream_take_error(s) == NET_OK, "reset connected");
  const char retained[] = "retained-before-reset";
  received = 0;
  while (received < sizeof retained - 1) {
    deadline(until);
    enum net_error e = stream_read(s, reply, sizeof reply, &n);
    require(e == NET_OK || e == NET_WOULD_BLOCK, "reset queued read");
    if (e == NET_OK && n) {
      require(n <= sizeof retained - 1 - received &&
                  !memcmp(reply, retained + received, n),
              "bytes before reset");
      received += n;
    }
    service();
  }
  while (!stream_read_ready(s)) {
    deadline(until);
    service();
  }
  require(stream_read(s, reply, 1, &n) == NET_RESET, "remote reset");
  require(stream_take_error(s) == NET_RESET && stream_take_error(s) == NET_OK,
          "reset error consumes once");
  require(stream_write(s, "x", 1, &n) == NET_RESET, "reset write retained");
  close_ok(s);
  console_puts("NETWORK-RESET-PASS\r\n");
  /* A silent peer on the local virtual LAN exercises the fixed deadline. */
  require(stream_create(&s) == NET_OK, "timeout create");
  remote.ip[3] = 99;
  require(stream_connect(s, &remote) == NET_IN_PROGRESS, "timeout pending");
  until = clock_monotonic_ns() + 12000000000UL;
  while (!stream_write_ready(s)) {
    deadline(until);
    service();
  }
  require(stream_take_error(s) == NET_TIMEOUT, "connection timeout");
  close_ok(s);
  console_puts("NETWORK-TIMEOUT-PASS\r\n");
#endif
  __asm__ volatile("outl %0,%1" ::"a"(0x10), "Nd"((uint16_t)0xf4));
  for (;;)
    __asm__ volatile("hlt");
}

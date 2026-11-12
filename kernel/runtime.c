#include "kernel.h"
__attribute__((weak)) bool abi_files(unsigned long n, const unsigned long a[6],
                                     long *r) {
  (void)n;
  (void)a;
  (void)r;
  return false;
}
__attribute__((weak)) void fs_init(const void *p, size_t n) {
  (void)p;
  (void)n;
}
__attribute__((weak)) long file_map_read(int fd, uint64_t off, void *p,
                                         size_t n) {
  (void)fd;
  (void)off;
  (void)p;
  (void)n;
  return -9;
}
struct timespec {
  int64_t sec, nsec;
};
static void set_time(struct timespec *t, uint64_t ns) {
  if (t) {
    t->sec = ns / 1000000000UL;
    t->nsec = ns % 1000000000UL;
  }
}
static bool get_time(const struct timespec *t, uint64_t *ns) {
  if (!t || t->sec < 0 || t->nsec < 0 || t->nsec >= 1000000000 ||
      (uint64_t)t->sec > (UINT64_MAX - (uint64_t)t->nsec) / 1000000000UL)
    return false;
  *ns = (uint64_t)t->sec * 1000000000UL + (uint64_t)t->nsec;
  return true;
}
struct timer_descriptor {
  bool used;
  unsigned flags, clock_id;
  uint64_t deadline, interval;
};
static struct timer_descriptor timers[16];
static uint64_t timer_now(const struct timer_descriptor *t) {
  return t->clock_id == 0 ? clock_realtime_ns() : clock_monotonic_ns();
}
static bool timer_ready(unsigned fd) {
  return fd >= 240 && fd < 256 && timers[fd - 240].used &&
         timers[fd - 240].deadline &&
         timer_now(&timers[fd - 240]) >= timers[fd - 240].deadline;
}
static long timer_syscall(unsigned long nr, const unsigned long a[6],
                          bool *handled) {
  *handled = true;
  if (nr == 283) {
    if (a[0] > 1 || (a[1] & ~(0x800UL | 0x80000UL)))
      return -22;
    for (unsigned i = 0; i < 16; i++)
      if (!timers[i].used) {
        timers[i] = (struct timer_descriptor){
            .used = true, .flags = a[1], .clock_id = a[0]};
        return 240 + i;
      }
    return -24;
  }
  if ((nr == 0 || nr == 3 || nr == 72 || nr == 286 || nr == 287) &&
      a[0] >= 240 && a[0] < 256) {
    struct timer_descriptor *t = &timers[a[0] - 240];
    if (!t->used)
      return -9;
    if (nr == 3) {
      *t = (struct timer_descriptor){0};
      return 0;
    }
    if (nr == 72) {
      if (a[1] == 1)
        return !!(t->flags & 0x80000);
      if (a[1] == 2) {
        if (a[2] & ~1UL)
          return -22;
        t->flags = (t->flags & ~0x80000U) | ((a[2] & 1) ? 0x80000U : 0);
        return 0;
      }
      if (a[1] == 3)
        return 2 | (t->flags & 0x800);
      if (a[1] == 4) {
        if (a[2] & ~0x8803UL)
          return -22;
        t->flags = (t->flags & ~0x800U) | (a[2] & 0x800U);
        return 0;
      }
      return -22;
    }
    if (nr == 0) {
      if (a[2] < 8)
        return -22;
      while (!timer_ready(a[0])) {
        if (t->flags & 0x800)
          return -11;
        wait_interrupt();
      }
      uint64_t count = 1;
      if (t->interval) {
        count += (timer_now(t) - t->deadline) / t->interval;
        t->deadline += count * t->interval;
      } else
        t->deadline = 0;
      memcpy((void *)a[1], &count, 8);
      return 8;
    }
    uint64_t interval = 0, value = 0;
    if (nr == 286) {
      if (a[1] & ~1UL)
        return -22;
      const struct timespec *v = (void *)a[2];
      if (!v || !get_time(v, &interval) || !get_time(v + 1, &value) ||
          (!(a[1] & 1) && value > UINT64_MAX - timer_now(t)))
        return -22;
    }
    struct timespec *old = (void *)(nr == 286 ? a[3] : a[1]);
    if (old) {
      set_time(old, t->interval);
      uint64_t now = timer_now(t);
      set_time(old + 1, t->deadline > now ? t->deadline - now : 0);
    }
    if (nr == 286) {
      t->interval = interval;
      t->deadline = value ? ((a[1] & 1) ? value : timer_now(t) + value) : 0;
    }
    return 0;
  }
  *handled = false;
  return 0;
}
__attribute__((weak)) long file_ready(int fd, bool writing) {
  if (fd < 0 || fd > 2)
    return -9;
  return writing ? (fd != 0) : (fd == 0 && console_ready());
}
static long descriptor_ready(unsigned fd, bool writing) {
  if (fd >= 240 && fd < 256) {
    if (!timers[fd - 240].used)
      return -9;
    return writing ? 0 : timer_ready(fd);
  }
  return file_ready(fd, writing);
}
__attribute__((weak)) int file_fd_kind(int fd) {
  return fd >= 0 && fd <= 2 ? 3 : -1;
}
static long console_transfer(unsigned fd, void *buffer, size_t count,
                             bool writing) {
  if (file_fd_kind(fd) != 3)
    return -9;
  unsigned long arguments[6] = {fd, 3, 0, 0, 0, 0};
  long flags = fd == 0 ? 0 : 1;
  (void)abi_files(72, arguments, &flags);
  if (flags < 0 || (flags & 0x200000))
    return -9;
  if (writing) {
    if ((flags & 3) == 0)
      return -9;
    console_write(buffer, count);
    return count;
  }
  if ((flags & 3) == 1)
    return -9;
  long result;
  while ((result = console_read(buffer, count)) == -11) {
    if (flags & 0x800)
      return -11;
    wait_interrupt();
  }
  return result;
}
static long terminal_ioctl(unsigned long request, void *p) {
  static unsigned char termios[36] = {
      0,  0,   0,  0, 5, 0, 0, 0,  0xbf, 0,  0, 0,  0x3b, 0x8a, 0,  0, 0, 3,
      28, 127, 21, 4, 0, 1, 0, 17, 19,   26, 0, 18, 15,   23,   22, 0, 0, 0};
  switch (request) {
  case 0x5401:
    memcpy(p, termios, sizeof termios);
    return 0;
  case 0x5402:
  case 0x5403:
  case 0x5404:
    memcpy(termios, p, sizeof termios);
    return 0;
  case 0x5413: {
    uint16_t *w = p;
    w[0] = 24;
    w[1] = 80;
    w[2] = w[3] = 0;
    return 0;
  }
  case 0x541b:
    *(int *)p = console_ready() ? 1 : 0;
    return 0;
  case 0x540f:
    *(int *)p = 1;
    return 0;
  case 0x5410:
    return 0;
  default:
    return -25;
  }
}
static long select_wait(const unsigned long *a, bool pselect) {
  if (a[0] > 1024)
    return -22;
  unsigned nfds = a[0];
  size_t bytes = ((nfds + 63) / 64) * 8;
  uint64_t rin[16] = {0}, win[16] = {0}, xin[16] = {0};
  if (a[1])
    memcpy(rin, (void *)a[1], bytes);
  if (a[2])
    memcpy(win, (void *)a[2], bytes);
  if (a[3])
    memcpy(xin, (void *)a[3], bytes);
  if (pselect && a[5]) {
    const unsigned long *mask = (void *)a[5];
    if (mask[0])
      return -38;
  }
  uint64_t deadline = UINT64_MAX;
  if (a[4]) {
    struct timespec time = *(const struct timespec *)a[4];
    if (!pselect) {
      if (time.nsec < 0 || time.nsec >= 1000000)
        return -22;
      time.nsec *= 1000;
    }
    uint64_t duration;
    if (!get_time(&time, &duration) ||
        duration > UINT64_MAX - clock_monotonic_ns())
      return -22;
    deadline = clock_monotonic_ns() + duration;
  }
  for (unsigned fd = 0; fd < nfds; fd++)
    if ((rin[fd / 64] | win[fd / 64] | xin[fd / 64]) & (1UL << (fd % 64)))
      if (descriptor_ready(fd, false) < 0)
        return -9;
  for (;;) {
    if (a[1])
      memset((void *)a[1], 0, bytes);
    if (a[2])
      memset((void *)a[2], 0, bytes);
    if (a[3])
      memset((void *)a[3], 0, bytes);
    long count = 0;
    for (unsigned fd = 0; fd < nfds; fd++) {
      uint64_t bit = 1UL << (fd % 64);
      if ((rin[fd / 64] & bit) && descriptor_ready(fd, false) > 0) {
        ((uint64_t *)a[1])[fd / 64] |= bit;
        count++;
      }
      if ((win[fd / 64] & bit) && descriptor_ready(fd, true) > 0) {
        ((uint64_t *)a[2])[fd / 64] |= bit;
        count++;
      }
    }
    if (count || clock_monotonic_ns() >= deadline)
      return count;
    wait_interrupt();
  }
}
static uint64_t signal_mask;
long syscall_dispatch(unsigned long n, const unsigned long a[6]) {
  long result;
  bool timer_handled;
  result = timer_syscall(n, a, &timer_handled);
  if (timer_handled)
    return result;
  if (abi_files(n, a, &result))
    return result;
  switch (n) {
  case 0:
    return console_transfer(a[0], (void *)a[1], a[2], false);
  case 1:
    return console_transfer(a[0], (void *)a[1], a[2], true);
  case 9: {
    if (!(a[3] & 0x20)) {
      if (!(a[3] & 2))
        return -95;
      if (a[5] & (PAGE_SIZE - 1))
        return -22;
      /* Validate the descriptor before MAP_FIXED can replace existing bytes. */
      long checked = file_map_read(a[4], a[5], 0, a[1]);
      if (checked < 0)
        return checked;
      long address = memory_map(a[0], a[1], 3, a[3]);
      if (address < 0)
        return address;
      long got = file_map_read(a[4], a[5], (void *)address, a[1]);
      if (got < 0) {
        memory_unmap(address, a[1]);
        return got;
      }
      long protected = memory_protect(address, a[1], a[2]);
      return protected < 0 ? protected : address;
    }
    return memory_map(a[0], a[1], a[2], a[3]);
  }
  case 10:
    return memory_protect(a[0], a[1], a[2]);
  case 11:
    return memory_unmap(a[0], a[1]);
  case 12:
    return memory_brk(a[0]);
  case 13: /* No asynchronous signal delivery is implemented. */
  case 131:
    return -38;
  case 14:
    if (a[3] != 8)
      return -22;
    if (a[2])
      memcpy((void *)a[2], &signal_mask, 8);
    if (a[1]) {
      uint64_t mask;
      memcpy(&mask, (void *)a[1], 8);
      if (a[0] == 0)
        signal_mask |= mask;
      else if (a[0] == 1)
        signal_mask &= ~mask;
      else if (a[0] == 2)
        signal_mask = mask;
      else
        return -22;
    }
    return 0;
  case 16:
    return file_fd_kind(a[0]) == 3 ? terminal_ioctl(a[1], (void *)a[2]) : -25;
  case 19:
  case 20: {
    struct iovec {
      void *base;
      size_t len;
    };
    if (a[2] > 1024)
      return -22;
    long checked = console_transfer(a[0], 0, 0, n == 20);
    if (checked < 0)
      return checked;
    if (a[2] && !a[1])
      return -14;
    long total = 0;
    for (size_t i = 0; i < a[2]; i++) {
      struct iovec vector;
      memcpy(&vector, (const unsigned char *)a[1] + i * sizeof vector,
             sizeof vector);
      long nread = console_transfer(a[0], vector.base, vector.len, n == 20);
      if (nread < 0)
        return total ? total : nread;
      total += nread;
      if ((size_t)nread < vector.len)
        break;
      if (n == 19 && total && !console_ready())
        break;
    }
    return total;
  }
  case 23:
    return select_wait(a, false);
  case 35: {
    uint64_t duration;
    if (!get_time((void *)a[0], &duration) ||
        duration > UINT64_MAX - clock_monotonic_ns())
      return -22;
    uint64_t end = clock_monotonic_ns() + duration;
    while (clock_monotonic_ns() < end)
      wait_interrupt();
    return 0;
  }
  case 25: /* mremap is unavailable; musl can allocate/copy/free instead. */
    return -38;
  case 38: /* Signal-driven interval timers are unavailable. */
    return -38;
  case 39:
  case 186:
    return 1;
  case 60:
  case 231:
    console_puts("\r\nRuntime exited status=");
    console_hex(a[0]);
    console_puts("\r\n");
    __asm__ volatile("outl %0,%1" ::"a"((unsigned)(a[0] ? 0x11 : 0x10)),
                     "Nd"((uint16_t)0xf4));
    panic("runtime exit");
  case 63: {
    char *u = (void *)a[0];
    memset(u, 0, 390);
    memcpy(u, "Linux", 6);
    memcpy(u + 65, "emacsos", 8);
    memcpy(u + 130, "6.0.0-emacsos", 13);
    memcpy(u + 195, "emacsOS bounded ABI", 20);
    memcpy(u + 260, "x86_64", 7);
    return 0;
  }
  case 97: {
    uint64_t *r = (void *)a[1];
    if (a[0] == 3) {
      r[0] = 4 * 1024 * 1024;
      r[1] = 4 * 1024 * 1024;
    } else if (a[0] == 7) {
      r[0] = 256;
      r[1] = 256;
    } else {
      r[0] = r[1] = UINT64_MAX;
    }
    return 0;
  }
  case 99: /* Physical free-memory reporting is not implemented. */
    return -38;
  case 102:
  case 104:
  case 107:
  case 108:
    return 0;
  case 110:
    return 0;
  case 109:
  case 111:
    return 1;
  case 121:
    return a[0] <= 1 ? 1 : -3;
  case 122:
  case 123:
    return 0; /* No alternate identity exists; report the unchanged previous ID.
               */
  case 68:
    return (a[0] <= 1 && a[1] <= 1) ? 0 : -1;

  case 158:
    if (a[0] == 0x1002) {
      __asm__ volatile("wrmsr" ::"c"(0xc0000100), "a"((uint32_t)a[1]),
                       "d"((uint32_t)(a[1] >> 32)));
      return 0;
    }
    return -22;
  case 222: /* Signal-driven POSIX timers are not supported. */
    return -38;
  case 202:
    return -38;
  case 218:
    return 1;
  case 228:
    if (a[0] != 0 && a[0] != 1 && a[0] != 4)
      return -22;
    set_time((void *)a[1],
             a[0] == 0 ? clock_realtime_ns() : clock_monotonic_ns());
    return 0;
  case 270:
    return select_wait(a, true);
  case 302:
    if (a[2])
      return -1;
    {
      unsigned long args[6] = {a[1], a[3], 0, 0, 0, 0};
      return syscall_dispatch(97, args);
    }
  case 318:
    return -38;
  default:
    console_puts("\r\nUnsupported syscall ");
    console_hex(n);
    console_puts("\r\n");
    return -38;
  }
}

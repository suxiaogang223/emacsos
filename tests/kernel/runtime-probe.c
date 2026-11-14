/* Host-built static musl probe; the marker is emitted only by the guest ELF. */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
int main(void)
{
    struct timespec start, end, delay = { 0, 10000000 };
    clock_gettime(CLOCK_MONOTONIC, &start);
    unsigned char *memory = mmap(0, 8192, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 1;
    memory[4096] = 73;
    if (mmap(memory, 8192, 8, MAP_PRIVATE | MAP_FIXED, 23, 0) != MAP_FAILED || errno != EINVAL || memory[4096] != 73) return 16;
    if (mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, -1, 0) != MAP_FAILED || errno != EBADF) return 15;
    if (mmap(memory, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, 23, 0) != MAP_FAILED || errno != EBADF || memory[4096] != 73) return 14;
    if (mprotect(memory, 8192, PROT_READ) || memory[4096] != 73) return 2;
#ifdef PROBE_WRITE_PROTECTED
    memory[4096] = 74;
    return 99;
#endif
#ifdef PROBE_EXECUTE_PROTECTED
    ((void (*)(void))memory)();
    return 99;
#endif
    if (munmap(memory, 8192)) return 3;
    nanosleep(&delay, 0);
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (end.tv_sec == start.tv_sec && end.tv_nsec <= start.tv_nsec) return 4;
    int timer = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer < 0 || fcntl(timer, F_GETFD) != FD_CLOEXEC) return 5;
    if (fcntl(timer, F_SETFD, 0) || fcntl(timer, F_GETFD) != 0) return 6;
    uint64_t expirations;
    if (read(timer, &expirations, 8) != -1 || errno != EAGAIN) return 7;
    struct itimerspec when = {{0,0},{0,1000000000}};
    if (timerfd_settime(timer, 0, &when, 0) != -1 || errno != EINVAL) return 8;
    clock_gettime(CLOCK_REALTIME, &when.it_value);
    when.it_value.tv_nsec += 10000000;
    if (when.it_value.tv_nsec >= 1000000000) { when.it_value.tv_nsec -= 1000000000; when.it_value.tv_sec++; }
    if (timerfd_settime(timer, TFD_TIMER_ABSTIME, &when, 0)) return 9;
    if (fcntl(timer, F_SETFL, 0)) return 10;
    if (read(timer, &expirations, 8) != 8 || expirations != 1) return 11;
    if (close(timer)) return 12;
    fd_set readers;
    FD_ZERO(&readers); FD_SET(23, &readers);
    struct timeval zero = {0,0};
    if (select(24, &readers, 0, 0, &zero) != -1 || errno != EBADF) return 13;
    printf("MUSL-GUEST-PROBE-PASS pid=%ld\n", (long)getpid());
    return 0;
}

/* Host-built static musl probe; the marker is emitted only by the guest ELF. */
#include <stdio.h>
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
    printf("MUSL-GUEST-PROBE-PASS pid=%ld\n", (long)getpid());
    return 0;
}

#define _POSIX_C_SOURCE 200809L
#include "files.h"
#include "littlefs_backend.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int disk_fd, failed_writes, failed_flush;
static unsigned writes, cut, flushes, fail_flush_at;
static bool tear;
static size_t allocated, peak;
union allocation {
    max_align_t alignment;
    size_t size;
};
void *kmalloc(size_t n)
{
    union allocation *p = malloc(sizeof *p + n);
    if (!p)
        return NULL;
    p->size = n;
    allocated += n;
    if (allocated > peak)
        peak = allocated;
    return p + 1;
}
void kfree(void *p)
{
    if (p) {
        union allocation *q = (union allocation *)p - 1;
        allocated -= q->size;
        free(q);
    }
}
uint64_t clock_realtime_ns(void)
{
    return 1700000000123456789ULL;
}
bool console_ready(void)
{
    return false;
}
_Noreturn void panic(const char *s)
{
    fprintf(stderr, "Unexpected panic: %s\n", s);
    exit(2);
}
int data_disk_init(void)
{
    return 0;
}
uint64_t data_disk_sectors(void)
{
    return 131072;
}
int data_disk_read(uint64_t s, void *p, size_t n)
{
    if (s > 131072 || n > 131072 - s)
        return -22;
    return pread(disk_fd, p, n * 512, (off_t)s * 512) == (ssize_t)(n * 512) ? 0 : -5;
}
int data_disk_write(uint64_t s, const void *p, size_t n)
{
    if (failed_writes)
        return failed_writes;
    if (s > 131072 || n > 131072 - s)
        return -22;
    if (cut && writes + 1 == cut && tear) {
        assert(pwrite(disk_fd, p, 256, (off_t)s * 512) == 256);
        _exit(77);
    }
    if (pwrite(disk_fd, p, n * 512, (off_t)s * 512) != (ssize_t)(n * 512))
        return -5;
    ++writes;
    if (cut && writes == cut)
        _exit(77);
    return 0;
}
int data_disk_flush(void)
{
    ++flushes;
    if (flushes == fail_flush_at)
        return -5;
    return failed_flush ? failed_flush : fsync(disk_fd) ? -5 : 0;
}
static long call(unsigned long nr, unsigned long a, unsigned long b, unsigned long c,
                 unsigned long d, unsigned long e)
{
    unsigned long args[6] = {a, b, c, d, e, 0};
    long result;
    assert(abi_files(nr, args, &result));
    return result;
}
#define P(x) ((unsigned long)(x))
static int open_file(const char *path, unsigned flags)
{
    return (int)call(257, -100, P(path), flags, 0600, 0);
}
static void close_file(int fd)
{
    assert(call(3, fd, 0, 0, 0, 0) == 0);
}
static struct fs_node *node(const char *path)
{
    struct fs_node *n;
    assert(fs_lookup(fs_root(), path, false, &n) == FS_OK);
    return n;
}
static void check_contents(const char *path, const char *expected)
{
    int fd = open_file(path, 0);
    assert(fd >= 3);
    char buffer[128] = {0};
    assert(call(0, fd, P(buffer), sizeof buffer, 0, 0) == (long)strlen(expected));
    assert(!memcmp(buffer, expected, strlen(expected)));
    close_file(fd);
}
static void write_file(const char *path, const char *text)
{
    int fd = open_file(path, 64 | 2);
    assert(fd >= 3);
    assert(call(1, fd, P(text), strlen(text), 0, 0) == (long)strlen(text));
    assert(call(74, fd, 0, 0, 0, 0) == 0);
    close_file(fd);
}
static void mount_data(void)
{
    fs_init(NULL, 0);
    assert(fs_mount_data() == FS_OK);
    assert(fs_data_mounted());
}
static void seed(void)
{
    assert(fs_data_boot_id() == 1);
    int a = open_file("/work/a", 64 | 2), b = open_file("/work/b", 64 | 2);
    assert(a >= 3 && b >= 3);
    assert(call(1, a, P("new"), 3, 0, 0) == 3);
    assert(call(1, b, P("old"), 3, 0, 0) == 3);
    int independent = open_file("/work/a", 0);
    assert(independent >= 3);
    char buffer[32];
    assert(call(0, independent, P(buffer), 3, 0, 0) == 3 && !memcmp(buffer, "new", 3));
    assert(call(77, a, 2, 0, 0, 0) == 0);
    assert(call(17, independent, P(buffer), 32, 0, 0) == 2);
    int duplicate = (int)call(32, a, 0, 0, 0, 0);
    assert(duplicate >= 3);
    assert(call(8, duplicate, 0, 0, 0, 0) == 0);
    assert(call(0, a, P(buffer), 1, 0, 0) == 1 && buffer[0] == 'n');
    assert(call(0, duplicate, P(buffer), 1, 0, 0) == 1 && buffer[0] == 'e');
    assert(call(82, P("/work/a"), P("/work/b"), 0, 0, 0) == 0);
    assert(call(17, b, P(buffer), 3, 0, 0) == 3 && !memcmp(buffer, "old", 3));
    assert(call(17, a, P(buffer), 3, 0, 0) == 2 && !memcmp(buffer, "ne", 2));
    assert(call(87, P("/work/b"), 0, 0, 0, 0) == 0);
    assert(call(18, a, P("kept"), 4, 0, 0) == 4);
    assert(call(17, independent, P(buffer), 4, 0, 0) == 4 && !memcmp(buffer, "kept", 4));
    close_file(a);
    close_file(duplicate);
    close_file(independent);
    close_file(b);
    write_file("/work/keep", "persisted source");
    assert(call(88, P("keep"), P("/work/.#ordinary"), 0, 0, 0) == 0);
    char target[1024];
    memset(target, 'x', 1023);
    target[1023] = 0;
    assert(call(88, P(target), P("/work/long-link"), 0, 0, 0) == 0);
    char back[1024];
    assert(call(89, P("/work/long-link"), P(back), sizeof back, 0, 0) == 1023);
    assert(!memcmp(back, target, 1023));
    assert(call(83, P("/work/dir"), 0700, 0, 0, 0) == 0);
    write_file("/work/dir/child", "child");
    assert(call(82, P("/work/dir"), P("/work/renamed"), 0, 0, 0) == 0);
    check_contents("/work/renamed/child", "child");
    assert(fs_chmod(node("/work/keep"), 0640) == FS_OK);
    assert(fs_set_times(node("/work/keep"), 100, 200) == FS_OK);
    assert(call(82, P("/work/keep"), P("/tmp/not-persistent"), 0, 0, 0) == -18);
    assert(call(84, P("/work"), 0, 0, 0, 0) == -16);
    assert(fs_ram_used() == 0);
    int orphan = open_file("/work/frozen-orphan", 64 | 2);
    assert(orphan >= 3);
    assert(call(1, orphan, P("orphan"), 6, 0, 0) == 6);
    assert(call(87, P("/work/frozen-orphan"), 0, 0, 0, 0) == 0);
    uint64_t generation = fs_data_generation();
    assert(fs_freeze_data() == FS_OK);
    unsigned saved_writes = writes;
    check_contents("/work/keep", "persisted source");
    assert(open_file("/work/blocked", 64 | 2) == -16);
    assert(fs_chmod(node("/work/keep"), 0600) == FS_BUSY);
    assert(fs_resize(node("/work/keep"), 0) == FS_BUSY);
    assert(call(87, P("/work/keep"), 0, 0, 0, 0) == -16);
    close_file(orphan);
    assert(writes == saved_writes && fs_data_generation() == generation);
    fs_thaw_data();
    assert(!fs_data_frozen() && fs_data_generation() > generation);
    write_file("/work/source", "new destination");
    write_file("/work/destination", "old destination");
    assert(fs_sync_all() == FS_OK);
    printf("Persistent PAL/ABI semantics pass; peak allocator=%zu, file RAM=%zu\n", peak,
           fs_ram_used());
}
static void verify(void)
{
    assert(fs_data_boot_id() > 1);
    check_contents("/work/keep", "persisted source");
    check_contents("/work/renamed/child", "child");
    struct fs_info info;
    fs_info(node("/work/keep"), &info);
    assert(info.mode == 0640 && info.modified_ns == 200);
    char target[1024];
    size_t count;
    assert(fs_readlink(node("/work/long-link"), target, sizeof target, &count) == FS_OK &&
           count == 1023);
    assert(fs_readlink(node("/work/.#ordinary"), target, sizeof target, &count) == FS_OK &&
           count == 4 && !memcmp(target, "keep", 4));
    puts("Independent remount restores contents, symlinks, modes and times");
}
static void arm_fault(void)
{
    cut = (unsigned)strtoul(getenv("CUT_AFTER") ? getenv("CUT_AFTER") : "0", NULL, 10);
    fail_flush_at = (unsigned)strtoul(getenv("FAIL_FLUSH") ? getenv("FAIL_FLUSH") : "0", NULL, 10);
    tear = getenv("TEAR_CUT") != NULL;
    writes = flushes = 0;
}
static void create_case(const char *kind, bool recovery)
{
    char target[1024];
    memset(target, 's', 1023);
    target[1023] = 0;
    enum fs_kind type = !strcmp(kind, "directory") ? FS_DIRECTORY
                        : !strcmp(kind, "symlink") ? FS_SYMLINK
                                                   : FS_REGULAR;
    if (!recovery) {
        arm_fault();
        struct fs_node *n;
        enum fs_error e = fs_create(fs_root(), "/work/created", type, target, 0711, true, &n);
        if (fail_flush_at)
            assert(e == FS_IO);
        else
            assert(e == FS_OK);
        printf("WRITES %u FLUSHES %u\n", writes, flushes);
        return;
    }
    struct fs_node *n;
    enum fs_error e = fs_lookup(fs_root(), "/work/created", false, &n);
    assert(e == FS_OK || e == FS_NOT_FOUND);
    if (e == FS_NOT_FOUND)
        return;
    struct fs_info i;
    fs_info(n, &i);
    assert(i.kind == type && i.mode == 0711);
    if (type == FS_SYMLINK) {
        char bytes[1024];
        size_t count;
        assert(fs_readlink(n, bytes, sizeof bytes, &count) == FS_OK);
        assert(count == 1023 && !memcmp(bytes, target, count));
    } else
        assert(i.size == 0);
}
static void stream(void)
{
    static unsigned char bytes[65536];
    memset(bytes, 0x5a, sizeof bytes);
    for (unsigned file = 0; file < 4; file++) {
        char path[64];
        snprintf(path, sizeof path, "/work/stream%u", file);
        int fd = open_file(path, 64 | 2);
        assert(fd >= 3);
        for (unsigned i = 0; i < 128; i++)
            assert(call(1, fd, P(bytes), sizeof bytes, 0, 0) == sizeof bytes);
        assert(call(74, fd, 0, 0, 0, 0) == 0);
        close_file(fd);
    }
    assert(fs_ram_used() == 0 && peak < 1024 * 1024);
    int fd = open_file("/work/stream3", 0);
    assert(fd >= 3);
    memset(bytes, 0, sizeof bytes);
    assert(call(17, fd, P(bytes), sizeof bytes, 8 * 1024 * 1024 - sizeof bytes, 0) == sizeof bytes);
    for (unsigned i = 0; i < sizeof bytes; i++)
        assert(bytes[i] == 0x5a);
    close_file(fd);
    assert(fs_resize(node("/work/stream3"), FS_FILE_LIMIT + 1) == FS_TOO_BIG);
    assert(fs_data_health_error() == FS_OK);
    puts("32MiB streams beyond the RAM file quota with bounded allocator use");
}
static void capacity(void)
{
    static unsigned char bytes[65536];
    memset(bytes, 0x71, sizeof bytes);
    bool full = false;
    for (unsigned index = 0; index < 10 && !full; index++) {
        char path[64];
        snprintf(path, sizeof path, "/work/capacity%u", index);
        int fd = open_file(path, 64 | 2);
        assert(fd >= 3);
        for (unsigned chunk = 0; chunk < 128; chunk++) {
            long result = call(1, fd, P(bytes), sizeof bytes, 0, 0);
            if (result == -28) {
                full = true;
                break;
            }
            assert(result == sizeof bytes);
        }
        long closed = call(3, fd, 0, 0, 0, 0);
        if (closed == -28)
            full = true;
        else
            assert(closed == 0);
        if (full) {
            assert(fs_data_health_error() == FS_OK);
            struct fs_info info;
            fs_info(node(path), &info);
            fd = open_file(path, 0);
            assert(fd >= 3);
            long count = call(0, fd, P(bytes), 1, 0, 0);
            assert(count == (info.size ? 1 : 0));
            long read_closed = call(3, fd, 0, 0, 0, 0);
            assert(read_closed == 0 || read_closed == -28);
            assert(call(87, P("/work/capacity0"), 0, 0, 0, 0) == 0);
            write_file("/work/reclaimed-space", "space is reusable");
            assert(fs_data_health_error() == FS_OK);
        }
    }
    assert(full);
    puts("ENOSPC remains healthy and closed inode state matches committed data");
}
static void replace(void)
{
    int old = open_file("/work/destination", 0);
    assert(old >= 3);
    arm_fault();
    assert(call(82, P("/work/source"), P("/work/destination"), 0, 0, 0) == 0);
    char bytes[32];
    assert(call(0, old, P(bytes), sizeof bytes, 0, 0) == 15 &&
           !memcmp(bytes, "old destination", 15));
    close_file(old);
    printf("WRITES %u\n", writes);
}
static void recovered(void)
{
    struct fs_node *source;
    enum fs_error found = fs_lookup(fs_root(), "/work/source", false, &source);
    if (found == FS_OK) {
        check_contents("/work/source", "new destination");
        check_contents("/work/destination", "old destination");
    } else {
        assert(found == FS_NOT_FOUND);
        check_contents("/work/destination", "new destination");
    }
}
static void io_failure(void)
{
    assert(fs_data_health_error() == FS_OK);
    int fd = open_file("/work/keep", 2);
    assert(fd >= 3);
    assert(call(18, fd, P("changed"), 7, 0, 0) == 7);
    failed_flush = -5;
    assert(call(74, fd, 0, 0, 0, 0) == -5);
    assert(fs_data_health_error() == FS_IO);
    assert(open_file("/work/after-fault", 64 | 2) == -30);
    assert(fs_data_health_error() == FS_IO);
    assert(fs_freeze_data() == FS_IO && !fs_data_frozen());
    failed_flush = -110;
    assert(call(3, fd, 0, 0, 0, 0) == -5);
    assert(fs_data_health_error() == FS_IO && fs_data_last_error() == FS_IO);
    assert(call(3, fd, 0, 0, 0, 0) == -9);
    puts("Sync error propagates and close consumes the descriptor");
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    disk_fd = open(argv[1], O_RDWR | O_CREAT, 0600);
    assert(disk_fd >= 0);
    if (!strcmp(argv[2], "format")) {
        assert(!ftruncate(disk_fd, 64 * 1024 * 1024));
        assert(!littlefs_backend_format());
        close(disk_fd);
        return 0;
    }
    if (!strcmp(argv[2], "reject")) {
        fs_init(NULL, 0);
        assert(fs_mount_data() != FS_OK && !fs_data_mounted());
        assert(writes == 0);
        assert(open_file("/work/unsafe", 64 | 2) == -30);
        puts("Unrecognized DATA remains unchanged and /work is read-only");
        close(disk_fd);
        return 0;
    }
    mount_data();
    if (!strcmp(argv[2], "seed"))
        seed();
    else if (!strcmp(argv[2], "verify"))
        verify();
    else if (!strcmp(argv[2], "replace"))
        replace();
    else if (!strcmp(argv[2], "recovered"))
        recovered();
    else if (!strcmp(argv[2], "io-failure"))
        io_failure();
    else if (!strcmp(argv[2], "stream"))
        stream();
    else if (!strcmp(argv[2], "capacity"))
        capacity();
    else if (!strncmp(argv[2], "create-", 7))
        create_case(argv[2] + 7, false);
    else if (!strncmp(argv[2], "recover-", 8))
        create_case(argv[2] + 8, true);
    else
        abort();
    close(disk_fd);
    return 0;
}

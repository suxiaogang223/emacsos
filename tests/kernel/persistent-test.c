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
#include "../../third_party/littlefs/lfs.h"

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
    int fd = open_file(path, 64 | 512 | 2);
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
    struct fs_node *missing;
    assert(fs_lookup(fs_root(), "/work", true, &missing) == FS_NOT_FOUND);
    check_contents("/etc/passwd", "root:x:0:0:emacsOS:/home/user:\n");
    check_contents("/etc/group", "root:x:0:\n");
    struct fs_info temporary;
    fs_info(node("/tmp"), &temporary);
    assert(temporary.mode == 01777);
    write_file("/etc/emacsos/config.el", "persistent configuration");
    write_file("/var/lib/emacsos/state", "persistent state");
    write_file("/var/log/check.log", "persistent log");
    write_file("/var/cache/check", "persistent cache");
    write_file("/etc/passwd", "root:x:0:0:Edited:/home/user:\n");
    int a = open_file("/home/user/projects/a", 64 | 2), b = open_file("/home/user/projects/b", 64 | 2);
    assert(a >= 3 && b >= 3);
    assert(call(1, a, P("new"), 3, 0, 0) == 3);
    assert(call(1, b, P("old"), 3, 0, 0) == 3);
    int independent = open_file("/home/user/projects/a", 0);
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
    assert(call(82, P("/home/user/projects/a"), P("/home/user/projects/b"), 0, 0, 0) == 0);
    assert(call(17, b, P(buffer), 3, 0, 0) == 3 && !memcmp(buffer, "old", 3));
    assert(call(17, a, P(buffer), 3, 0, 0) == 2 && !memcmp(buffer, "ne", 2));
    assert(call(87, P("/home/user/projects/b"), 0, 0, 0, 0) == 0);
    assert(call(18, a, P("kept"), 4, 0, 0) == 4);
    assert(call(17, independent, P(buffer), 4, 0, 0) == 4 && !memcmp(buffer, "kept", 4));
    close_file(a);
    close_file(duplicate);
    close_file(independent);
    close_file(b);
    write_file("/home/user/projects/keep", "persisted source");
    assert(call(88, P("keep"), P("/home/user/projects/.#ordinary"), 0, 0, 0) == 0);
    char target[1024];
    memset(target, 'x', 1023);
    target[1023] = 0;
    assert(call(88, P(target), P("/home/user/projects/long-link"), 0, 0, 0) == 0);
    char back[1024];
    assert(call(89, P("/home/user/projects/long-link"), P(back), sizeof back, 0, 0) == 1023);
    assert(!memcmp(back, target, 1023));
    assert(call(83, P("/home/user/projects/dir"), 0700, 0, 0, 0) == 0);
    write_file("/home/user/projects/dir/child", "child");
    assert(call(82, P("/home/user/projects/dir"), P("/home/user/projects/renamed"), 0, 0, 0) == 0);
    check_contents("/home/user/projects/renamed/child", "child");
    assert(fs_chmod(node("/home/user/projects/keep"), 0640) == FS_OK);
    assert(fs_set_times(node("/home/user/projects/keep"), 100, 200) == FS_OK);
    write_file("/home/user/home-file", "persistent home");
    write_file("/tmp/temp-file", "persistent temporary directory");
    write_file("/home/user/projects/movable", "same volume");
    assert(call(82, P("/home/user/projects/movable"), P("/etc/emacsos/movable"), 0, 0, 0) == 0);
    assert(call(82, P("/etc/emacsos/movable"), P("/tmp/movable"), 0, 0, 0) == 0);
    assert(call(82, P("/tmp/movable"), P("/home/user/projects/moved-back"), 0, 0, 0) == 0);
    int cross_source = open_file("/home/user/projects/cross-source", 64 | 2);
    int cross_old = open_file("/tmp/cross-destination", 64 | 2);
    assert(cross_source >= 3 && cross_old >= 3);
    assert(call(1, cross_source, P("source"), 6, 0, 0) == 6);
    assert(call(1, cross_old, P("old target"), 10, 0, 0) == 10);
    assert(call(82, P("/home/user/projects/cross-source"), P("/tmp/cross-destination"), 0, 0, 0) == 0);
    assert(call(17, cross_old, P(buffer), sizeof buffer, 0, 0) == 10 &&
           !memcmp(buffer, "old target", 10));
    assert(call(82, P("/tmp/cross-destination"), P("/var/lib/emacsos/cross-open"), 0, 0, 0) == 0);
    assert(call(17, cross_source, P(buffer), sizeof buffer, 0, 0) == 6 &&
           !memcmp(buffer, "source", 6));
    struct stat source_info, named_info, old_info;
    assert(call(5, cross_source, P(&source_info), 0, 0, 0) == 0);
    assert(call(4, P("/var/lib/emacsos/cross-open"), P(&named_info), 0, 0, 0) == 0);
    assert(call(5, cross_old, P(&old_info), 0, 0, 0) == 0);
    assert(source_info.st_ino == named_info.st_ino && old_info.st_nlink == 0);
    close_file(cross_source);
    close_file(cross_old);
    assert(call(88, P("/var/lib/emacsos/cross-open"), P("/tmp/cross-link"), 0, 0, 0) == 0);
    check_contents("/tmp/cross-link", "source");
    assert(call(83, P("/tmp/cross-dir"), 0700, 0, 0, 0) == 0);
    write_file("/tmp/cross-dir/child", "cross-directory child");
    assert(call(82, P("/tmp/cross-dir"), P("/etc/emacsos/cross-dir"), 0, 0, 0) == 0);
    assert(call(82, P("/etc/emacsos/cross-dir"), P("/home/user/projects/cross-dir"), 0, 0, 0) == 0);
    assert(open_file("/arbitrary-file", 64 | 2) == -30);
    for (unsigned i = 0; i < 4; i++) {
        const char *anchors[] = {"/etc", "/home", "/tmp", "/var"};
        assert(call(84, P(anchors[i]), 0, 0, 0, 0) == -16);
    }

    int orphan = open_file("/home/user/projects/frozen-orphan", 64 | 2);
    assert(orphan >= 3);
    assert(call(1, orphan, P("orphan"), 6, 0, 0) == 6);
    assert(call(87, P("/home/user/projects/frozen-orphan"), 0, 0, 0, 0) == 0);
    uint64_t generation = fs_data_generation();
    assert(fs_freeze_data() == FS_OK);
    unsigned saved_writes = writes;
    check_contents("/home/user/projects/keep", "persisted source");
    assert(open_file("/home/user/projects/blocked", 64 | 2) == -16);
    assert(open_file("/home/user/blocked", 64 | 2) == -16);
    assert(open_file("/tmp/blocked", 64 | 2) == -16);
    assert(open_file("/etc/emacsos/blocked", 64 | 2) == -16);
    assert(open_file("/var/lib/emacsos/blocked", 64 | 2) == -16);
    assert(fs_chmod(node("/etc/emacsos/config.el"), 0600) == FS_BUSY);
    assert(fs_set_times(node("/var/lib/emacsos/state"), 1, 2) == FS_BUSY);
    assert(fs_resize(node("/var/log/check.log"), 0) == FS_BUSY);
    assert(call(87, P("/var/cache/check"), 0, 0, 0, 0) == -16);
    assert(call(82, P("/home/user/projects/moved-back"), P("/tmp/frozen-move"), 0, 0, 0) == -16);
    assert(fs_chmod(node("/home/user/home-file"), 0600) == FS_BUSY);
    check_contents("/tmp/temp-file", "persistent temporary directory");
    assert(fs_chmod(node("/home/user/projects/keep"), 0600) == FS_BUSY);
    assert(fs_resize(node("/home/user/projects/keep"), 0) == FS_BUSY);
    assert(call(87, P("/home/user/projects/keep"), 0, 0, 0, 0) == -16);
    close_file(orphan);
    assert(writes == saved_writes && fs_data_generation() == generation);
    fs_thaw_data();
    assert(!fs_data_frozen() && fs_data_generation() > generation);
    write_file("/etc/emacsos/source", "new destination");
    write_file("/var/lib/emacsos/destination", "old destination");
    assert(fs_sync_all() == FS_OK);
    printf("Persistent PAL/ABI semantics pass; peak allocator=%zu\n", peak);
}
static void verify(void)
{
    assert(fs_data_boot_id() > 1);
    check_contents("/etc/passwd", "root:x:0:0:Edited:/home/user:\n");
    check_contents("/etc/emacsos/config.el", "persistent configuration");
    check_contents("/var/lib/emacsos/state", "persistent state");
    check_contents("/var/log/check.log", "persistent log");
    check_contents("/var/cache/check", "persistent cache");
    check_contents("/home/user/projects/keep", "persisted source");
    check_contents("/home/user/projects/renamed/child", "child");
    check_contents("/home/user/home-file", "persistent home");
    check_contents("/tmp/temp-file", "persistent temporary directory");
    check_contents("/home/user/projects/moved-back", "same volume");
    check_contents("/var/lib/emacsos/cross-open", "source");
    check_contents("/tmp/cross-link", "source");
    check_contents("/home/user/projects/cross-dir/child", "cross-directory child");
    struct fs_info info;
    fs_info(node("/home/user/projects/keep"), &info);
    assert(info.mode == 0640 && info.modified_ns == 200);
    char target[1024];
    size_t count;
    assert(fs_readlink(node("/home/user/projects/long-link"), target, sizeof target, &count) == FS_OK &&
           count == 1023);
    assert(fs_readlink(node("/home/user/projects/.#ordinary"), target, sizeof target, &count) == FS_OK &&
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
    const char *path = type == FS_DIRECTORY ? "/home/user/created"
                       : type == FS_SYMLINK ? "/tmp/created"
                                            : "/home/user/projects/created";
    if (!recovery) {
        arm_fault();
        struct fs_node *n;
        enum fs_error e = fs_create(fs_root(), path, type, target, 0711, true, &n);
        if (fail_flush_at)
            assert(e == FS_IO);
        else
            assert(e == FS_OK);
        printf("WRITES %u FLUSHES %u\n", writes, flushes);
        return;
    }
    struct fs_node *n;
    enum fs_error e = fs_lookup(fs_root(), path, false, &n);
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
        snprintf(path, sizeof path, "/home/user/projects/stream%u", file);
        int fd = open_file(path, 64 | 2);
        assert(fd >= 3);
        for (unsigned i = 0; i < 128; i++)
            assert(call(1, fd, P(bytes), sizeof bytes, 0, 0) == sizeof bytes);
        assert(call(74, fd, 0, 0, 0, 0) == 0);
        close_file(fd);
    }
    assert(peak < 1024 * 1024);
    int fd = open_file("/home/user/projects/stream3", 0);
    assert(fd >= 3);
    memset(bytes, 0, sizeof bytes);
    assert(call(17, fd, P(bytes), sizeof bytes, 8 * 1024 * 1024 - sizeof bytes, 0) == sizeof bytes);
    for (unsigned i = 0; i < sizeof bytes; i++)
        assert(bytes[i] == 0x5a);
    close_file(fd);
    assert(fs_resize(node("/home/user/projects/stream3"), FS_FILE_LIMIT + 1) == FS_TOO_BIG);
    assert(fs_data_health_error() == FS_OK);
    puts("32MiB streams through littlefs with bounded allocator use");
}
static void capacity(void)
{
    static unsigned char bytes[65536];
    memset(bytes, 0x71, sizeof bytes);
    bool full = false;
    for (unsigned index = 0; index < 10 && !full; index++) {
        char path[64];
        snprintf(path, sizeof path, "/home/user/projects/capacity%u", index);
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
            assert(call(87, P("/home/user/projects/capacity0"), 0, 0, 0, 0) == 0);
            write_file("/home/user/projects/reclaimed-space", "space is reusable");
            assert(fs_data_health_error() == FS_OK);
        }
    }
    assert(full);
    puts("ENOSPC remains healthy and closed inode state matches committed data");
}
static void replace(void)
{
    int old = open_file("/var/lib/emacsos/destination", 0);
    assert(old >= 3);
    arm_fault();
    assert(call(82, P("/etc/emacsos/source"), P("/var/lib/emacsos/destination"), 0, 0, 0) == 0);
    char bytes[32];
    assert(call(0, old, P(bytes), sizeof bytes, 0, 0) == 15 &&
           !memcmp(bytes, "old destination", 15));
    close_file(old);
    printf("WRITES %u\n", writes);
}
static void recovered(void)
{
    struct fs_node *source;
    enum fs_error found = fs_lookup(fs_root(), "/etc/emacsos/source", false, &source);
    if (found == FS_OK) {
        check_contents("/etc/emacsos/source", "new destination");
        check_contents("/var/lib/emacsos/destination", "old destination");
    } else {
        assert(found == FS_NOT_FOUND);
        check_contents("/var/lib/emacsos/destination", "new destination");
    }
}
static void io_failure(void)
{
    assert(fs_data_health_error() == FS_OK);
    int fd = open_file("/home/user/projects/keep", 2);
    assert(fd >= 3);
    assert(call(18, fd, P("changed"), 7, 0, 0) == 7);
    failed_flush = -5;
    assert(call(74, fd, 0, 0, 0, 0) == -5);
    assert(fs_data_health_error() == FS_IO);
    assert(open_file("/home/user/projects/after-fault", 64 | 2) == -30);
    assert(fs_data_health_error() == FS_IO);
    assert(fs_freeze_data() == FS_IO && !fs_data_frozen());
    failed_flush = -110;
    assert(call(3, fd, 0, 0, 0, 0) == -5);
    assert(fs_data_health_error() == FS_IO && fs_data_last_error() == FS_IO);
    assert(call(3, fd, 0, 0, 0, 0) == -9);
    puts("Sync error propagates and close consumes the descriptor");
}
static int fixture_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, void *p,
                        lfs_size_t n)
{
    (void)c;
    return data_disk_read((uint64_t)b * 8 + o / 512, p, n / 512);
}
static int fixture_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, const void *p,
                        lfs_size_t n)
{
    (void)c;
    return data_disk_write((uint64_t)b * 8 + o / 512, p, n / 512);
}
static int fixture_erase(const struct lfs_config *c, lfs_block_t b)
{
    (void)c;
    unsigned char bytes[512];
    memset(bytes, 255, sizeof bytes);
    for (unsigned i = 0; i < 8; i++) {
        int e = data_disk_write((uint64_t)b * 8 + i, bytes, 1);
        if (e)
            return e;
    }
    return 0;
}
static int fixture_sync(const struct lfs_config *c)
{
    (void)c;
    return data_disk_flush();
}
static void fixture_bytes64(unsigned char *p, uint64_t n)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (unsigned char)n;
        n >>= 8;
    }
}
static void fixture_file(lfs_t *lfs, const char *path, const void *data, size_t size,
                         void *metadata)
{
    unsigned char cache[512];
    struct lfs_attr attr = {.type = 0xe0, .buffer = metadata, .size = 48};
    const struct lfs_file_config cfg = {
        .buffer = cache, .attrs = metadata ? &attr : NULL, .attr_count = metadata ? 1 : 0};
    lfs_file_t f;
    assert(!lfs_file_opencfg(lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL, &cfg));
    assert(lfs_file_write(lfs, &f, data, size) == (lfs_ssize_t)size);
    assert(!lfs_file_close(lfs, &f));
}
/* Rebuild historical fixtures with their original tree shape and inode metadata. */
static void fixture_remove_tree(lfs_t *lfs, const char *path)
{
    struct lfs_info info;
    assert(!lfs_stat(lfs, path, &info));
    if (info.type == LFS_TYPE_DIR) {
        for (;;) {
            lfs_dir_t dir;
            assert(!lfs_dir_open(lfs, &dir, path));
            bool found = false;
            while (lfs_dir_read(lfs, &dir, &info) > 0) {
                if (strcmp(info.name, ".") && strcmp(info.name, "..")) {
                    found = true;
                    break;
                }
            }
            assert(!lfs_dir_close(lfs, &dir));
            if (!found)
                break;
            char child[1024];
            assert(snprintf(child, sizeof child, "%s/%s", path, info.name) < (int)sizeof child);
            fixture_remove_tree(lfs, child);
        }
    }
    assert(!lfs_remove(lfs, path));
}
static void fixture_directory(lfs_t *lfs, const char *path, uint64_t id,
                               const unsigned char *source)
{
    unsigned char metadata[48];
    memcpy(metadata, source, sizeof metadata);
    fixture_bytes64(metadata + 8, id);
    fixture_bytes64(metadata + 16, ((uint64_t)0755 << 32) | FS_DIRECTORY);
    assert(!lfs_mkdir(lfs, path));
    assert(!lfs_setattr(lfs, path, 0xe0, metadata, sizeof metadata));
}
static void damage_fixture(const char *kind)
{
    unsigned char read[512], prog[512], look[128];
    struct lfs_config cfg = {.read = fixture_read,
                             .prog = fixture_prog,
                             .erase = fixture_erase,
                             .sync = fixture_sync,
                             .read_size = 512,
                             .prog_size = 512,
                             .block_size = 4096,
                             .block_count = 16384,
                             .block_cycles = 500,
                             .cache_size = 512,
                             .lookahead_size = 128,
                             .read_buffer = read,
                             .prog_buffer = prog,
                             .lookahead_buffer = look,
                             .name_max = 255,
                             .file_max = FS_FILE_LIMIT,
                             .attr_max = 1022};
    lfs_t lfs;
    assert(!lfs_mount(&lfs, &cfg));
    if (!strcmp(kind, "legacy1") || !strcmp(kind, "legacy2")) {
        bool v2 = !strcmp(kind, "legacy2");
        unsigned char metadata[48], id[8];
        assert(lfs_getattr(&lfs, "/tree", 0xe0, metadata, sizeof metadata) == 48);
        fixture_remove_tree(&lfs, "/tree");
        fixture_remove_tree(&lfs, "/objects");
        assert(!lfs_mkdir(&lfs, "/objects"));
        fixture_directory(&lfs, "/tree", 1, metadata);
        if (v2) {
            fixture_directory(&lfs, "/tree/work", 2, metadata);
            fixture_directory(&lfs, "/tree/home", 3, metadata);
            fixture_directory(&lfs, "/tree/tmp", 4, metadata);
            fixture_directory(&lfs, "/tree/home/user", 5, metadata);
        }
        uint64_t file_id = v2 ? 6 : 2;
        fixture_bytes64(metadata + 8, file_id);
        fixture_bytes64(metadata + 16, (uint64_t)0600 << 32);
        fixture_bytes64(id, file_id);
        fixture_file(&lfs, v2 ? "/objects/0000000000000006" : "/objects/0000000000000002",
                     "legacy file remains intact", 26, metadata);
        fixture_file(&lfs, v2 ? "/tree/work/keep" : "/tree/keep", id, sizeof id, NULL);
        fixture_bytes64(id, file_id + 1);
        assert(!lfs_setattr(&lfs, "/", 0xf1, id, sizeof id));
        assert(!lfs_setattr(&lfs, "/", 0xf0, v2 ? "EOSDATA2" : "EOSDATA1", 8));
    } else if (!strcmp(kind, "missing"))
        assert(!lfs_remove(&lfs, "/tree/tmp"));
    else if (!strcmp(kind, "extra"))
        assert(!lfs_rename(&lfs, "/tree/tmp", "/tree/unexpected"));
    else if (!strcmp(kind, "duplicate")) {
        unsigned char metadata[48];
        assert(lfs_getattr(&lfs, "/tree/etc", 0xe0, metadata, sizeof metadata) == 48);
        fixture_bytes64(metadata + 8, 3);
        assert(!lfs_setattr(&lfs, "/tree/etc", 0xe0, metadata, sizeof metadata));
    } else if (!strcmp(kind, "wrong-type")) {
        unsigned char metadata[48], id[8];
        assert(lfs_getattr(&lfs, "/tree/tmp", 0xe0, metadata, sizeof metadata) == 48);
        assert(!lfs_remove(&lfs, "/tree/tmp"));
        fixture_bytes64(metadata + 8, 100);
        fixture_bytes64(metadata + 16, (uint64_t)0600 << 32);
        fixture_bytes64(id, 100);
        fixture_file(&lfs, "/objects/0000000000000064", "", 0, metadata);
        fixture_file(&lfs, "/tree/tmp", id, sizeof id, NULL);
        fixture_bytes64(id, 101);
        assert(!lfs_setattr(&lfs, "/", 0xf1, id, sizeof id));
    } else if (!strcmp(kind, "missing-home-user") || !strcmp(kind, "symlink-home-user")) {
        unsigned char metadata[48], id[8];
        assert(lfs_getattr(&lfs, "/tree/home/user", 0xe0, metadata, sizeof metadata) == 48);
        fixture_remove_tree(&lfs, "/tree/home/user");
        if (!strcmp(kind, "symlink-home-user")) {
            fixture_bytes64(metadata + 8, 100);
            fixture_bytes64(metadata + 16, ((uint64_t)0777 << 32) | FS_SYMLINK);
            fixture_bytes64(id, 100);
            fixture_file(&lfs, "/objects/0000000000000064", "/tmp", 4, metadata);
            fixture_file(&lfs, "/tree/home/user", id, sizeof id, NULL);
            fixture_bytes64(id, 101);
            assert(!lfs_setattr(&lfs, "/", 0xf1, id, sizeof id));
        }
    } else
        abort();
    assert(!lfs_unmount(&lfs));
    assert(!data_disk_flush());
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
    if (!strncmp(argv[2], "damage-", 7)) {
        damage_fixture(argv[2] + 7);
        close(disk_fd);
        return 0;
    }
    if (!strcmp(argv[2], "reject") || !strcmp(argv[2], "reject-legacy")) {
        fs_init(NULL, 0);
        enum fs_error rejected = fs_mount_data();
        assert(rejected != FS_OK && !fs_data_mounted());
        if (!strcmp(argv[2], "reject-legacy"))
            assert(rejected == FS_NOT_SUPPORTED);
        assert(writes == 0);
        assert(open_file("/home/user/projects/unsafe", 64 | 2) == -30);
        assert(open_file("/home/user/unsafe", 64 | 2) == -30);
        assert(open_file("/tmp/unsafe", 64 | 2) == -30);
        assert(open_file("/etc/emacsos/unsafe", 64 | 2) == -30);
        assert(open_file("/var/lib/emacsos/unsafe", 64 | 2) == -30);
        struct fs_node *missing;
        assert(fs_lookup(fs_root(), "/work", true, &missing) == FS_NOT_FOUND);
        puts("Rejected DATA remains unchanged and all file directories are read-only");
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

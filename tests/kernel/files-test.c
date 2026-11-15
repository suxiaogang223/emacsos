#define _POSIX_C_SOURCE 200809L
/* Host tests exercise the same PAL and ABI objects linked into the guest. */
#include "files.h"
#include "littlefs_backend.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int disk_fd = -1;
int data_disk_init(void)
{
    return disk_fd < 0 ? -19 : 0;
}
int data_disk_read(uint64_t sector, void *p, size_t n)
{
    if (disk_fd < 0)
        return -19;
    if (sector > 131072 || n > 131072 - sector)
        return -22;
    return pread(disk_fd, p, n * 512, (off_t)sector * 512) == (ssize_t)(n * 512) ? 0 : -5;
}
int data_disk_write(uint64_t sector, const void *p, size_t n)
{
    if (disk_fd < 0)
        return -19;
    if (sector > 131072 || n > 131072 - sector)
        return -22;
    return pwrite(disk_fd, p, n * 512, (off_t)sector * 512) == (ssize_t)(n * 512) ? 0 : -5;
}
int data_disk_flush(void)
{
    return disk_fd < 0 ? -19 : fsync(disk_fd) ? -5 : 0;
}
uint64_t data_disk_sectors(void)
{
    return disk_fd < 0 ? 0 : 131072;
}
static size_t allocated;
union allocation { max_align_t alignment; size_t size; };
void *kmalloc(size_t size)
{
    union allocation *p = malloc(sizeof *p + size);
    if (!p) return 0;
    p->size = size; allocated += size; return p + 1;
}
void kfree(void *p)
{ if (p) { union allocation *a = (union allocation *) p - 1; allocated -= a->size; free(a); } }
uint64_t clock_realtime_ns(void) { return 1700000000123456789UL; }
bool console_ready(void) { return false; }
_Noreturn void panic(const char *message)
{ fprintf(stderr, "Rejected archive: %s\n", message); exit(2); }

static long call(unsigned long nr, unsigned long a, unsigned long b,
                 unsigned long c, unsigned long d, unsigned long e)
{
    unsigned long args[6] = {a, b, c, d, e, 0}; long result;
    assert(abi_files(nr, args, &result)); return result;
}
#define P(x) ((unsigned long) (x))
static int open_path(const char *path, unsigned flags)
{ return (int) call(257, -100, P(path), flags, 0600, 0); }
static void close_file(int fd) { assert(call(3, fd, 0, 0, 0, 0) == 0); }

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long size = ftell(f); assert(size > 0); rewind(f);
    void *archive = malloc((size_t) size); assert(archive);
    assert(fread(archive, 1, (size_t) size, f) == (size_t) size); fclose(f);
    fs_init(archive, (size_t) size);
    assert(fs_mount_data() == FS_NO_DEVICE);
    struct fs_node *refused;
    assert(fs_lookup(fs_root(), "/work", true, &refused) == FS_NOT_FOUND);
    assert(fs_create_regular(fs_root(), "/etc/emacsos/no-fallback", 0600, &refused) == FS_READ_ONLY);
    assert(fs_create_regular(fs_root(), "/var/lib/emacsos/no-fallback", 0600, &refused) == FS_READ_ONLY);
    assert(fs_create_regular(fs_root(), "/home/user/projects/no-fallback", 0600, &refused) == FS_READ_ONLY);
    assert(fs_create_regular(fs_root(), "/home/user/no-fallback", 0600, &refused) == FS_READ_ONLY);
    assert(fs_create_regular(fs_root(), "/tmp/no-fallback", 0600, &refused) == FS_READ_ONLY);
    struct fs_node *defaults;
    char fallback[64]; size_t fallback_size;
    assert(fs_lookup(fs_root(), "/etc/passwd", true, &defaults) == FS_OK);
    assert(fs_read(defaults, 0, fallback, sizeof fallback, &fallback_size) == FS_OK);
    assert(fallback_size == strlen("root:x:0:0:emacsOS:/home/user:\n"));
    assert(!memcmp(fallback, "root:x:0:0:emacsOS:/home/user:\n", fallback_size));
    assert(fs_write(defaults, 0, "x", 1, &fallback_size) == FS_READ_ONLY);
    FILE *disk = tmpfile();
    assert(disk);
    disk_fd = fileno(disk);
    assert(!ftruncate(disk_fd, 64 * 1024 * 1024));
    assert(!littlefs_backend_format());
    assert(fs_mount_data() == FS_OK);
    struct fs_node *n; struct fs_info info;
    assert(fs_lookup(fs_root(), "/usr/alias", true, &n) == FS_OK);
    char buffer[64] = {0}; size_t count;
    assert(fs_read(n, 0, buffer, sizeof buffer, &count) == FS_OK);
    assert(count == 8 && !memcmp(buffer, "upstream", 8));
    assert(fs_write(n, 0, "x", 1, &count) == FS_READ_ONLY);
    assert(open_path("/usr/alias", 1) == -30);
    assert(open_path("/home/user/projects/missing/", 64 | 2) == -2);
    assert(open_path("/home/user/projects/not-directory", 64 | 65536) == -22);
    assert(open_path("/home/user/projects/not-directory", 0) == -2);
    assert(call(83, P("/home/user/projects/project"), 0755, 0, 0, 0) == 0);
    int dir = open_path("/home/user/projects/project", 65536); assert(dir >= 3);
    int fd = (int) call(257, dir, P("data"), 64 | 2, 0600, 0); assert(fd >= 3);
    assert(call(1, fd, P("abc"), 3, 0, 0) == 3);
    assert(call(87, P("/home/user/projects/project/data/"), 0, 0, 0, 0) == -20);
    assert(call(82, P("/home/user/projects/project/data"), P("/home/user/projects/invalid/"), 0, 0, 0) == -20);
    long status_flags = call(72, fd, 3, 0, 0, 0);
    assert(!(status_flags & (64 | 128 | 512)));
    assert(call(72, fd, 4, status_flags | 2048, 0, 0) == 0);
    assert(call(72, fd, 3, 0, 0, 0) & 2048);
    assert(file_map_read(fd, 0, 0, 4096) == 0);
    assert(file_map_read(fd, 0, 0, 8192) == -95);
    assert(file_map_read(fd, 4096, 0, 1) == -95);
    struct { void *base; size_t size; } vector = { (void *) "v", 1 };
    unsigned char unaligned_vector[sizeof vector + 1];
    memcpy(unaligned_vector + 1, &vector, sizeof vector);
    assert(call(20, fd, P(unaligned_vector + 1), 1, 0, 0) == 1);
    assert(call(77, fd, 3, 0, 0, 0) == 0);
    int duplicate = (int) call(32, fd, 0, 0, 0, 0); assert(duplicate >= 3);
    assert(call(8, duplicate, 0, 0, 0, 0) == 0);
    assert(call(0, fd, P(buffer), 1, 0, 0) == 1 && buffer[0] == 'a');
    assert(call(0, duplicate, P(buffer), 1, 0, 0) == 1 && buffer[0] == 'b');
    int independent = open_path("/home/user/projects/project/data", 0); assert(independent >= 3);
    assert(call(0, independent, P(buffer), 1, 0, 0) == 1 && buffer[0] == 'a');
    assert(call(82, P("/home/user/projects/project/data"), P("/home/user/projects/project/renamed"), 0, 0, 0) == 0);
    struct stat before, after;
    assert(call(5, fd, P(&before), 0, 0, 0) == 0);
    assert(call(4, P("/home/user/projects/project/renamed"), P(&after), 0, 0, 0) == 0);
    assert(before.st_ino == after.st_ino && before.st_size == 3 && S_ISREG(before.st_mode));
    unsigned char unaligned[sizeof(struct stat) + 1];
    assert(call(5, fd, P(unaligned + 1), 0, 0, 0) == 0);
    memcpy(&after, unaligned + 1, sizeof after);
    assert(after.st_ino == before.st_ino);
    assert(call(87, P("/home/user/projects/project/renamed"), 0, 0, 0, 0) == 0);
    assert(open_path("/home/user/projects/project/renamed", 0) == -2);
    assert(call(5, fd, P(&after), 0, 0, 0) == 0 && after.st_nlink == 0);
    assert(call(1, fd, P("Z"), 1, 0, 0) == 1);
    close_file(fd); close_file(duplicate); close_file(independent);
    assert(call(3, fd, 0, 0, 0, 0) == -9);

    fd = open_path("/home/user/projects/sparse", 64 | 2); assert(fd >= 3);
    assert(call(8, fd, 4096, 0, 0, 0) == 4096);
    assert(call(1, fd, P("end"), 3, 0, 0) == 3);
    assert(call(17, fd, P(buffer), sizeof buffer, 4000, 0) == sizeof buffer);
    for (size_t i = 0; i < sizeof buffer; ++i) assert(buffer[i] == 0);
    assert(call(8, fd, (unsigned long) -1, 0, 0, 0) == -22);
    assert(call(77, fd, FS_FILE_LIMIT + 1, 0, 0, 0) == -27);
    assert(call(88, P("sparse"), P("/home/user/projects/alias"), 0, 0, 0) == 0);
    assert(call(89, P("/home/user/projects/alias"), P(buffer), sizeof buffer, 0, 0) == 6);
    assert(!memcmp(buffer, "sparse", 6));
    int alias = open_path("/home/user/projects/alias", 0); assert(alias >= 3); close_file(alias);
    assert(open_path("/home/user/projects/alias", 131072) == -40);
    assert(call(88, P("cycle"), P("/home/user/projects/cycle"), 0, 0, 0) == 0);
    assert(open_path("/home/user/projects/cycle", 0) == -40);
    assert(call(87, P("/home/user/projects/cycle"), 0, 0, 0, 0) == 0);
    assert(call(87, P("/home/user/projects/alias"), 0, 0, 0, 0) == 0);
    assert(call(87, P("/home/user/projects/sparse"), 0, 0, 0, 0) == 0); close_file(fd);
    assert(call(88, P("created-target"), P("/home/user/projects/dangling"), 0, 0, 0) == 0);
    assert(open_path("/home/user/projects/dangling", 64 | 128 | 2) == -17);
    fd = open_path("/home/user/projects/dangling", 64 | 2); assert(fd >= 3);
    assert(call(1, fd, P("ok"), 2, 0, 0) == 2); close_file(fd);
    fd = open_path("/home/user/projects/created-target", 0); assert(fd >= 3); close_file(fd);
    assert(call(87, P("/home/user/projects/dangling"), 0, 0, 0, 0) == 0);
    assert(call(87, P("/home/user/projects/created-target"), 0, 0, 0, 0) == 0);
    assert(call(19, (unsigned long) -1, 0, 0, 0, 0) == -9);

    unsigned char entries[512];
    assert(call(217, dir, P(entries), sizeof entries, 0, 0) > 0);
    assert(call(84, P("/home/user/projects/project"), 0, 0, 0, 0) == -16);
    close_file(dir);
    assert(call(84, P("/home/user/projects/project"), 0, 0, 0, 0) == 0);
    size_t baseline = allocated;
    for (unsigned i = 0; i < 1000; ++i) {
        fd = open_path("/home/user/projects/repeated", 64 | 2); assert(fd >= 3);
        assert(call(1, fd, P(buffer), sizeof buffer, 0, 0) == sizeof buffer);
        assert(call(87, P("/home/user/projects/repeated"), 0, 0, 0, 0) == 0); close_file(fd);
    }
    assert(allocated == baseline);
    assert(file_ready(177, false) == -9 && file_ready(0, false) == 0);
    assert(call(33, 0, 0, 0, 0, 0) == 0);
    close_file(0);
    fd = open_path("/home/user/projects/lowest-fd", 64 | 2); assert(fd == 0);
    close_file(fd);
    assert(open_path("/dev/tty", 0) == 0);
    assert(call(87, P("/home/user/projects/lowest-fd"), 0, 0, 0, 0) == 0);
    assert(fs_lookup(fs_root(), "/home/user/projects", true, &n) == FS_OK);
    fs_info(n, &info); assert(info.kind == FS_DIRECTORY);
    puts("File PAL and Linux descriptor/namespace checks passed");
    fclose(disk);
    return 0;
}

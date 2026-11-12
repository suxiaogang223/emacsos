/* Host tests exercise the same PAL and ABI objects linked into the guest. */
#include "files.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    struct fs_node *n; struct fs_info info;
    assert(fs_lookup(fs_root(), "/usr/alias", true, &n) == FS_OK);
    char buffer[64] = {0}; size_t count;
    assert(fs_read(n, 0, buffer, sizeof buffer, &count) == FS_OK);
    assert(count == 8 && !memcmp(buffer, "upstream", 8));
    assert(fs_write(n, 0, "x", 1, &count) == FS_READ_ONLY);
    assert(open_path("/usr/alias", 1) == -30);
    assert(open_path("/work/missing/", 64 | 2) == -2);
    assert(call(83, P("/work/project"), 0755, 0, 0, 0) == 0);
    int dir = open_path("/work/project", 65536); assert(dir >= 3);
    int fd = (int) call(257, dir, P("data"), 64 | 2, 0600, 0); assert(fd >= 3);
    assert(call(1, fd, P("abc"), 3, 0, 0) == 3);
    int duplicate = (int) call(32, fd, 0, 0, 0, 0); assert(duplicate >= 3);
    assert(call(8, duplicate, 0, 0, 0, 0) == 0);
    assert(call(0, fd, P(buffer), 1, 0, 0) == 1 && buffer[0] == 'a');
    assert(call(0, duplicate, P(buffer), 1, 0, 0) == 1 && buffer[0] == 'b');
    int independent = open_path("/work/project/data", 0); assert(independent >= 3);
    assert(call(0, independent, P(buffer), 1, 0, 0) == 1 && buffer[0] == 'a');
    assert(call(82, P("/work/project/data"), P("/work/project/renamed"), 0, 0, 0) == 0);
    struct stat before, after;
    assert(call(5, fd, P(&before), 0, 0, 0) == 0);
    assert(call(4, P("/work/project/renamed"), P(&after), 0, 0, 0) == 0);
    assert(before.st_ino == after.st_ino && before.st_size == 3 && S_ISREG(before.st_mode));
    assert(call(87, P("/work/project/renamed"), 0, 0, 0, 0) == 0);
    assert(open_path("/work/project/renamed", 0) == -2);
    assert(call(5, fd, P(&after), 0, 0, 0) == 0 && after.st_nlink == 0);
    assert(call(1, fd, P("Z"), 1, 0, 0) == 1);
    assert(fs_ram_used() > 0);
    close_file(fd); close_file(duplicate); close_file(independent);
    assert(fs_ram_used() == 0);
    assert(call(3, fd, 0, 0, 0, 0) == -9);

    fd = open_path("/work/sparse", 64 | 2); assert(fd >= 3);
    assert(call(8, fd, 4096, 0, 0, 0) == 4096);
    assert(call(1, fd, P("end"), 3, 0, 0) == 3);
    assert(call(17, fd, P(buffer), sizeof buffer, 4000, 0) == sizeof buffer);
    for (size_t i = 0; i < sizeof buffer; ++i) assert(buffer[i] == 0);
    assert(call(8, fd, (unsigned long) -1, 0, 0, 0) == -22);
    assert(call(77, fd, FS_FILE_LIMIT + 1, 0, 0, 0) == -27);
    assert(call(88, P("sparse"), P("/work/alias"), 0, 0, 0) == 0);
    assert(call(89, P("/work/alias"), P(buffer), sizeof buffer, 0, 0) == 6);
    assert(!memcmp(buffer, "sparse", 6));
    int alias = open_path("/work/alias", 0); assert(alias >= 3); close_file(alias);
    assert(open_path("/work/alias", 131072) == -40);
    assert(call(88, P("cycle"), P("/work/cycle"), 0, 0, 0) == 0);
    assert(open_path("/work/cycle", 0) == -40);
    assert(call(87, P("/work/cycle"), 0, 0, 0, 0) == 0);
    assert(call(87, P("/work/alias"), 0, 0, 0, 0) == 0);
    assert(call(87, P("/work/sparse"), 0, 0, 0, 0) == 0); close_file(fd);
    assert(fs_ram_used() == 0);

    unsigned char entries[512];
    assert(call(217, dir, P(entries), sizeof entries, 0, 0) > 0);
    assert(call(84, P("/work/project"), 0, 0, 0, 0) == -16);
    close_file(dir);
    assert(call(84, P("/work/project"), 0, 0, 0, 0) == 0);
    size_t baseline = allocated;
    for (unsigned i = 0; i < 1000; ++i) {
        fd = open_path("/work/repeated", 64 | 2); assert(fd >= 3);
        assert(call(1, fd, P(buffer), sizeof buffer, 0, 0) == sizeof buffer);
        assert(call(87, P("/work/repeated"), 0, 0, 0, 0) == 0); close_file(fd);
    }
    assert(allocated == baseline && fs_ram_used() == 0);
    int large[3];
    for (unsigned i = 0; i < 3; ++i) {
        char path[32]; snprintf(path, sizeof path, "/work/large%u", i);
        large[i] = open_path(path, 64 | 2); assert(large[i] >= 3);
        assert(call(77, large[i], FS_FILE_LIMIT, 0, 0, 0) == 0);
    }
    assert(fs_ram_used() == FS_RAM_LIMIT);
    assert(call(88, P("x"), P("/work/full"), 0, 0, 0) == -28);
    for (unsigned i = 0; i < 3; ++i) {
        char path[32]; snprintf(path, sizeof path, "/work/large%u", i);
        assert(call(87, P(path), 0, 0, 0, 0) == 0); close_file(large[i]);
    }
    assert(fs_ram_used() == 0 && allocated == baseline);
    assert(file_ready(177, false) == -9 && file_ready(0, false) == 0);
    assert(call(33, 0, 0, 0, 0, 0) == 0);
    assert(fs_lookup(fs_root(), "/work", true, &n) == FS_OK);
    fs_info(n, &info); assert(info.kind == FS_DIRECTORY);
    puts("File PAL and Linux descriptor/namespace checks passed");
    return 0;
}

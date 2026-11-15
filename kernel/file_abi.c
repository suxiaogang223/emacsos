/* Bounded Linux x86-64 file/stream translation over typed backend operations. */
#include "files.h"
#include "data_layout.h"
#include "network.h"

extern void *kmalloc(size_t);
extern void kfree(void *);
extern uint64_t clock_realtime_ns(void);
extern bool console_ready(void);
extern void wait_interrupt(void);

#define FD_LIMIT 192
#define AT_CWD (-100)
#define O_ACCESS 3U
#define O_CREATE 64U
#define O_EXCL 128U
#define O_TRUNC 512U
#define O_APPEND 1024U
#define O_DIRECTORY 65536U
#define O_NOFOLLOW 131072U
#define O_CLOEXEC 524288U
#define O_PATH 2097152U
#define O_NONBLOCK 2048U
#define STREAM_KIND 5

enum description_kind { DESCRIPTION_FILE, DESCRIPTION_STREAM };
struct description {
    enum description_kind kind;
    union { struct fs_node *node; struct net_stream *stream; };
    uint64_t offset;
    unsigned flags, refs;
};
struct descriptor { struct description *open; unsigned flags; };
static struct descriptor descriptors[FD_LIMIT];
static struct fs_node *working_directory;
static bool initialized;
static unsigned creation_mask = 0022;

static void clear(void *p, size_t size) { unsigned char *q = p; while (size--) *q++ = 0; }
static void copy(void *p, const void *s, size_t size)
{ unsigned char *q = p; const unsigned char *r = s; while (size--) *q++ = *r++; }
static size_t length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static long error(enum fs_error e)
{
    return fs_error_errno(e);
}
static long network_error(enum net_error e)
{
    switch (e) {
    case NET_OK: return 0;
    case NET_INVALID: return -22;
    case NET_NO_MEMORY: return -12;
    case NET_NO_DEVICE: return -100;
    case NET_NOT_SUPPORTED: return -95;
    case NET_IN_PROGRESS: return -115;
    case NET_WOULD_BLOCK: return -11;
    case NET_REFUSED: return -111;
    case NET_TIMEOUT: return -110;
    case NET_RESET: return -104;
    case NET_NOT_CONNECTED: return -107;
    case NET_BROKEN_PIPE: return -32;
    case NET_ALREADY: return -114;
    case NET_IS_CONNECTED: return -106;
    case NET_ADDRESS_IN_USE: return -98;
    case NET_UNREACHABLE: return -113;
    case NET_IO: return -5;
    }
    return -5;
}

static bool valid(int fd) { return fd >= 0 && fd < FD_LIMIT && descriptors[fd].open; }
static bool is_stream(int fd)
{ return valid(fd) && descriptors[fd].open->kind == DESCRIPTION_STREAM; }
static struct fs_info information(struct fs_node *n)
{ struct fs_info info; fs_info(n, &info); return info; }

static long install(struct fs_node *node, unsigned flags, int minimum)
{
    int fd = minimum;
    while (fd < FD_LIMIT && descriptors[fd].open) ++fd;
    if (fd == FD_LIMIT) return -24;
    struct description *open = kmalloc(sizeof *open);
    if (!open) return -12;
    *open = (struct description) { .node = node,
        .flags = flags & (O_ACCESS | O_APPEND | 2048U | 32768U | O_DIRECTORY | O_NOFOLLOW | O_PATH), .refs = 1 };
    fs_retain(node);
    descriptors[fd].open = open;
    descriptors[fd].flags = !!(flags & O_CLOEXEC);
    return fd;
}

static void initialize(void)
{
    if (initialized) return;
    working_directory = fs_root();
    (void) fs_lookup(fs_root(), EMACSOS_HOME, true, &working_directory);
    fs_retain(working_directory);
    struct fs_node *tty;
    if (fs_lookup(fs_root(), "/dev/tty", true, &tty) == FS_OK) {
        (void) install(tty, 0, 0); (void) install(tty, 1, 1); (void) install(tty, 1, 2);
    }
    initialized = true;
}

int file_fd_kind(int fd)
{
    initialize();
    return is_stream(fd) ? STREAM_KIND
        : valid(fd) ? (int) information(descriptors[fd].open->node).kind : -1;
}

long file_ready(int fd, bool writing)
{
    int kind = file_fd_kind(fd);
    if (kind < 0) return -9;
    if (kind == STREAM_KIND) return writing
        ? stream_write_ready(descriptors[fd].open->stream)
        : stream_read_ready(descriptors[fd].open->stream);
    return kind == FS_CONSOLE ? writing || console_ready() : 1;
}

static long close_fd(int fd)
{
    if (!valid(fd)) return -9;
    struct description *open = descriptors[fd].open;
    descriptors[fd].open = 0; descriptors[fd].flags = 0;
    if (!--open->refs) {
        long result = open->kind == DESCRIPTION_STREAM
            ? network_error(stream_close(open->stream))
            : error(fs_release_checked(open->node));
        kfree(open);
        return result;
    }
    return 0;
}

static long duplicate(int old, int target, bool exact, bool cloexec)
{
    if (!valid(old)) return -9;
    if (target < 0 || target >= FD_LIMIT) return exact ? -9 : -22;
    if (exact && old == target) return old;
    if (!exact) while (target < FD_LIMIT && valid(target)) ++target;
    if (target == FD_LIMIT) return -24;
    if (valid(target)) (void) close_fd(target);
    descriptors[target].open = descriptors[old].open;
    ++descriptors[target].open->refs;
    descriptors[target].flags = cloexec;
    return target;
}

static long base_for(int fd, const char *path, struct fs_node **base)
{
    if (!path) return -14;
    if (*path == '/' || fd == AT_CWD) { *base = working_directory; return 0; }
    if (!valid(fd)) return -9;
    if (is_stream(fd)) return -20;
    *base = descriptors[fd].open->node;
    return information(*base).kind == FS_DIRECTORY ? 0 : -20;
}

static long lookup(int fd, const char *path, bool follow, struct fs_node **node)
{
    struct fs_node *base; long result = base_for(fd, path, &base);
    return result ? result : error(fs_lookup(base, path, follow, node));
}

static long open_file(int dirfd, const char *path, unsigned flags, unsigned mode)
{
    const unsigned supported = O_ACCESS | O_CREATE | O_EXCL | 256U | O_TRUNC
        | O_APPEND | 2048U | 32768U | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_PATH;
    if (flags & ~supported || (flags & O_ACCESS) == 3) return -22;
    if ((flags & O_CREATE) && (flags & O_DIRECTORY)) return -22;
    if ((flags & O_PATH) && (flags & (O_CREATE | O_EXCL | O_TRUNC | O_APPEND | O_ACCESS))) return -22;
    int available = 0;
    while (available < FD_LIMIT && valid(available)) ++available;
    if (available == FD_LIMIT) return -24;
    struct fs_node *base, *node;
    long result = base_for(dirfd, path, &base);
    if (result) return result;
    enum fs_error e = fs_lookup(base, path,
        !(flags & O_NOFOLLOW) && !((flags & O_CREATE) && (flags & O_EXCL)), &node);
    if (e == FS_NOT_FOUND && (flags & O_CREATE)) {
        size_t n = length(path);
        if (!n || path[n - 1] == '/') return -2;
        e = fs_create_regular(base, path, mode & ~creation_mask, &node);
    } else if (e == FS_OK && (flags & O_CREATE) && (flags & O_EXCL)) return -17;
    if (e != FS_OK) return error(e);
    struct fs_info info = information(node);
    if (info.kind == FS_SYMLINK && !(flags & O_PATH)) return -40;
    if ((flags & O_DIRECTORY) && info.kind != FS_DIRECTORY) return -20;
    if ((flags & O_ACCESS) && info.kind == FS_DIRECTORY) return -21;
    if ((flags & O_ACCESS) && !info.writable && info.kind != FS_CONSOLE && info.kind != FS_NULL) return -30;
    if ((flags & O_TRUNC) && !(flags & O_PATH)) {
        if (!(flags & O_ACCESS)) return -22;
        e = fs_resize(node, 0);
        if (e != FS_OK) return error(e);
    }
    return install(node, flags, 0);
}

static long stream_transfer(struct description *open, void *buffer, size_t count,
                            bool write, bool may_wait)
{
    if (count && !buffer) return -14;
    if (count > INT64_MAX) return -22;
    size_t done = 0;
    enum net_error e;
    do {
        e = write ? stream_write(open->stream, buffer, count, &done)
                  : stream_read(open->stream, buffer, count, &done);
        if (e != NET_WOULD_BLOCK || done || !may_wait || (open->flags & O_NONBLOCK)) break;
        wait_interrupt();
    } while (true);
    return done || e == NET_OK ? (long) done : network_error(e);
}

static long transfer(int fd, void *buffer, size_t count, bool write, bool positioned, uint64_t offset)
{
    if (!valid(fd)) return -9;
    if (count && !buffer) return -14;
    struct description *open = descriptors[fd].open;
    if (open->kind == DESCRIPTION_STREAM) {
        if (positioned) return -29;
        return stream_transfer(open, buffer, count, write, true);
    }
    if (open->flags & O_PATH) return -9;
    unsigned access = open->flags & O_ACCESS;
    if ((write && !access) || (!write && access == 1)) return -9;
    struct fs_info info = information(open->node);
    if (info.kind == FS_NULL) return write ? (long) count : 0;
    if (info.kind == FS_CONSOLE) return -29;
    if (!positioned) offset = write && (open->flags & O_APPEND) ? info.size : open->offset;
    size_t done;
    enum fs_error e = write ? fs_write(open->node, offset, buffer, count, &done)
                           : fs_read(open->node, offset, buffer, count, &done);
    if (e != FS_OK && !done)
        return error(e);
    if (!positioned) open->offset = offset + done;
    return (long) done;
}

long file_map_read(int fd, uint64_t offset, void *buffer, size_t count)
{
    initialize();
    if (!valid(fd)) return -9;
    struct description *open = descriptors[fd].open;
    if (open->kind == DESCRIPTION_STREAM) return -19;
    if ((open->flags & O_PATH) || (open->flags & O_ACCESS) == 1) return -9;
    struct fs_info info = information(open->node);
    if (info.kind != FS_REGULAR) return -19;
    uint64_t end = (info.size + 4095) & ~4095UL;
    /* Pages wholly beyond EOF would require SIGBUS, which this port lacks. */
    if (count && (offset >= info.size || count > end - offset)) return -95;
    if (!buffer) return 0;
    return transfer(fd, buffer, count, false, true, offset);
}

static long seek_file(int fd, int64_t offset, unsigned whence)
{
    if (!valid(fd)) return -9;
    struct description *open = descriptors[fd].open;
    if (open->kind == DESCRIPTION_STREAM) return -29;
    struct fs_info info = information(open->node);
    if (open->flags & O_PATH) return -9;
    if (info.kind == FS_CONSOLE || info.kind == FS_NULL) return -29;
    if (whence > 2) return -22;
    uint64_t base = whence == 0 ? 0 : whence == 1 ? open->offset : info.size;
    if (offset < 0 && (uint64_t) (-(offset + 1)) + 1 > base) return -22;
    uint64_t destination = base + (uint64_t) offset;
    if ((offset > 0 && destination < base) || destination > INT64_MAX) return -22;
    open->offset = destination;
    return (long) destination;
}

struct linux_stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, padding;
    uint64_t rdev;
    int64_t size, blksize, blocks, atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns, reserved[3];
};
_Static_assert(sizeof(struct linux_stat) == 144, "x86-64 stat layout");

static long stat_node(struct fs_node *node, void *destination)
{
    if (!destination) return -14;
    struct linux_stat local, *out = &local;
    struct fs_info info = information(node); clear(out, sizeof *out);
    out->dev = 1; out->ino = info.inode; out->nlink = info.links;
    out->mode = info.mode | (info.kind == FS_DIRECTORY ? 0040000U
        : info.kind == FS_SYMLINK ? 0120000U
        : info.kind == FS_CONSOLE || info.kind == FS_NULL ? 0020000U : 0100000U);
    out->size = info.size; out->blksize = 4096; out->blocks = (info.size + 511) / 512;
    out->atime = info.accessed_ns / 1000000000;
    out->atime_ns = info.accessed_ns % 1000000000;
    out->mtime = info.modified_ns / 1000000000;
    out->mtime_ns = info.modified_ns % 1000000000;
    out->ctime = info.changed_ns / 1000000000; out->ctime_ns = info.changed_ns % 1000000000;
    copy(destination, &local, sizeof local);
    return 0;
}

static long stat_fd(int fd, void *destination)
{
    if (!valid(fd)) return -9;
    if (!is_stream(fd)) return stat_node(descriptors[fd].open->node, destination);
    if (!destination) return -14;
    struct linux_stat out = { .mode = 0140000U | 0600U, .nlink = 1,
                              .blksize = NET_BUFFER_SIZE };
    copy(destination, &out, sizeof out);
    return 0;
}

static long stat_path(int dirfd, const char *path, void *out, unsigned flags)
{
    if (flags & ~(256U | 4096U)) return -22;
    struct fs_node *node;
    if (path && !*path && (flags & 4096U)) {
        if (dirfd == AT_CWD) node = working_directory;
        else return stat_fd(dirfd, out);
    } else { long e = lookup(dirfd, path, !(flags & 256U), &node); if (e) return e; }
    return stat_node(node, out);
}

static long directory_entries(int fd, unsigned char *out, size_t size)
{
    if (!valid(fd)) return -9;
    if (!out) return -14;
    struct description *open = descriptors[fd].open;
    if (open->kind == DESCRIPTION_STREAM) return -20;
    if (open->flags & O_PATH) return -9;
    if (information(open->node).kind != FS_DIRECTORY) return -20;
    size_t used = 0;
    for (;;) {
        struct fs_info info; char name[FS_NAME_MAX + 1];
        enum fs_error e = fs_directory(open->node, open->offset, name, &info);
        if (e == FS_NOT_FOUND) return (long) used;
        if (e != FS_OK) return used ? (long) used : error(e);
        size_t record = (19 + length(name) + 1 + 7) & ~(size_t) 7;
        if (record > size - used) return used ? (long) used : -22;
        clear(out + used, record);
        uint64_t next = open->offset + 1; uint16_t reclen = (uint16_t) record;
        copy(out + used, &info.inode, 8); copy(out + used + 8, &next, 8);
        copy(out + used + 16, &reclen, 2);
        out[used + 18] = info.kind == FS_DIRECTORY ? 4 : info.kind == FS_SYMLINK ? 10
            : info.kind == FS_REGULAR ? 8 : 2;
        copy(out + used + 19, name, length(name) + 1);
        used += record; ++open->offset;
    }
}

static long access_path(int dirfd, const char *path, unsigned mode, unsigned flags)
{
    if (mode & ~7U || flags & ~(256U | 512U)) return -22;
    struct fs_node *node; long result = lookup(dirfd, path, !(flags & 256U), &node);
    if (result) return result;
    struct fs_info info = information(node);
    if ((mode & 2) && !info.writable && info.kind != FS_CONSOLE && info.kind != FS_NULL) return -30;
    if ((mode & 1) && info.kind != FS_DIRECTORY && !(info.mode & 0111)) return -13;
    return 0;
}

static long fcntl_file(int fd, unsigned command, unsigned long argument)
{
    if (!valid(fd)) return -9;
    switch (command) {
    case 0: return duplicate(fd, (int) argument, false, false);
    case 1030: return duplicate(fd, (int) argument, false, true);
    case 1: return descriptors[fd].flags;
    case 2: if (argument & ~1UL) return -22; descriptors[fd].flags = (unsigned) argument; return 0;
    case 3: return descriptors[fd].open->flags;
    case 4:
        if (argument & ~(unsigned long) (O_ACCESS | O_APPEND | 2048U | 32768U | O_DIRECTORY | O_NOFOLLOW | O_PATH)) return -22;
        if (is_stream(fd) && (argument & (O_DIRECTORY | O_NOFOLLOW | O_PATH))) return -22;
        descriptors[fd].open->flags = (descriptors[fd].open->flags & ~(O_APPEND | 2048U))
            | ((unsigned) argument & (O_APPEND | 2048U)); return 0;
    default: return -22;
    }
}

static long read_link(int dirfd, const char *path, char *buffer, size_t capacity)
{
    if (!capacity) return -22;
    if (!buffer) return -14;
    struct fs_node *node; long result = lookup(dirfd, path, false, &node);
    if (result) return result;
    size_t count; enum fs_error e = fs_readlink(node, buffer, capacity, &count);
    return e == FS_OK ? (long) count : error(e);
}

static long make_path(int dirfd, const char *path, enum fs_kind kind, const char *target, unsigned mode)
{
    struct fs_node *base, *node; long result = base_for(dirfd, path, &base);
    if (result) return result;
    return error(fs_create(base, path, kind, target, mode & ~creation_mask, true, &node));
}
static long remove_path(int dirfd, const char *path, bool directory)
{
    struct fs_node *base; long result = base_for(dirfd, path, &base);
    return result ? result : error(fs_remove(base, path, directory));
}
static long rename_path(int oldfd, const char *old, int newfd, const char *new, unsigned flags)
{
    if (flags & ~1U) return -22;
    struct fs_node *oldbase, *newbase;
    long result = base_for(oldfd, old, &oldbase); if (result) return result;
    result = base_for(newfd, new, &newbase); if (result) return result;
    return error(fs_rename(oldbase, old, newbase, new, flags & 1));
}

static long open_socket(int family, unsigned type, unsigned protocol)
{
    if (family != 2) return -97;
    if (type & ~(15U | O_NONBLOCK | O_CLOEXEC)) return -22;
    if ((type & 15U) != 1) return -94;
    if (protocol && protocol != 6) return -93;
    int fd = 0;
    while (fd < FD_LIMIT && valid(fd)) ++fd;
    if (fd == FD_LIMIT) return -24;
    struct description *open = kmalloc(sizeof *open);
    if (!open) return -12;
    struct net_stream *stream;
    enum net_error e = stream_create(&stream);
    if (e != NET_OK) { kfree(open); return network_error(e); }
    *open = (struct description) { .kind = DESCRIPTION_STREAM, .stream = stream,
        .flags = 2U | (type & O_NONBLOCK), .refs = 1 };
    descriptors[fd].open = open;
    descriptors[fd].flags = !!(type & O_CLOEXEC);
    return fd;
}

static long socket_check(int fd)
{ return !valid(fd) ? -9 : !is_stream(fd) ? -88 : 0; }

struct linux_ipv4 { uint16_t family; uint8_t port[2], ip[4], padding[8]; };
_Static_assert(sizeof(struct linux_ipv4) == 16, "x86-64 IPv4 socket address");

static long connect_socket(int fd, const void *address, size_t length)
{
    long checked = socket_check(fd);
    if (checked) return checked;
    if (length < sizeof(struct linux_ipv4) || length > 128) return -22;
    if (!address) return -14;
    struct linux_ipv4 input;
    copy(&input, address, sizeof input);
    if (input.family != 2) return -97;
    struct net_address remote = { .port = (uint16_t) ((input.port[0] << 8) | input.port[1]) };
    copy(remote.ip, input.ip, sizeof remote.ip);
    struct description *open = descriptors[fd].open;
    enum net_error e = stream_connect(open->stream, &remote);
    if (e != NET_IN_PROGRESS || (open->flags & O_NONBLOCK)) return network_error(e);
    while (!stream_write_ready(open->stream)) wait_interrupt();
    return network_error(stream_take_error(open->stream));
}

static long local_socket(int fd, void *address, void *length)
{
    long checked = socket_check(fd);
    if (checked) return checked;
    if (!length) return -14;
    uint32_t capacity;
    copy(&capacity, length, sizeof capacity);
    if (capacity > INT32_MAX) return -22;
    if (capacity && !address) return -14;
    struct net_address local;
    enum net_error e = stream_local(descriptors[fd].open->stream, &local);
    if (e != NET_OK) return network_error(e);
    struct linux_ipv4 out = { .family = 2,
        .port = {(uint8_t) (local.port >> 8), (uint8_t) local.port} };
    copy(out.ip, local.ip, sizeof out.ip);
    if (capacity) copy(address, &out, capacity < sizeof out ? capacity : sizeof out);
    uint32_t actual = sizeof out;
    copy(length, &actual, sizeof actual);
    return 0;
}

static long socket_error(int fd, unsigned level, unsigned option, void *value, void *length)
{
    long checked = socket_check(fd);
    if (checked) return checked;
    if (level != 1 || option != 4) return -92;
    if (!length) return -14;
    uint32_t capacity;
    copy(&capacity, length, sizeof capacity);
    if (capacity > INT32_MAX) return -22;
    if (capacity && !value) return -14;
    int32_t code = (int32_t) -network_error(stream_take_error(descriptors[fd].open->stream));
    uint32_t actual = capacity < sizeof code ? capacity : sizeof code;
    if (actual) copy(value, &code, actual);
    copy(length, &actual, sizeof actual);
    return 0;
}

bool abi_files(unsigned long number, const unsigned long a[6], long *out)
{
    initialize();
    int fd = (int) a[0];
    const char *path = (const char *) a[0];
    struct fs_node *node;
    long result;
    /* Kernel timer descriptors use a separate reserved range. */
    if (fd >= 240 && (number == 0 || number == 1 || number == 3 || number == 5
        || number == 16 || number == 72)) return false;
    switch (number) {
    case 0: case 1:
        if (file_fd_kind(fd) == FS_CONSOLE) return false;
        result = transfer(fd, (void *) a[1], a[2], number == 1, false, 0); break;
    case 2: result = open_file(AT_CWD, path, a[1], a[2]); break;
    case 3: result = close_fd(fd); break;
    case 4: result = stat_path(AT_CWD, path, (void *) a[1], 0); break;
    case 5: result = stat_fd(fd, (void *) a[1]); break;
    case 6: result = stat_path(AT_CWD, path, (void *) a[1], 256); break;
    case 8: result = seek_file(fd, (int64_t) a[1], a[2]); break;
    case 16: if (file_fd_kind(fd) == FS_CONSOLE) return false; result = valid(fd) ? -25 : -9; break;
    case 17: case 18: result = transfer(fd, (void *) a[1], a[2], number == 18, true, a[3]); break;
    case 19: case 20: {
        if (!valid(fd) || (descriptors[fd].open->flags & O_PATH)) { result = -9; break; }
        unsigned access = descriptors[fd].open->flags & O_ACCESS;
        if ((number == 19 && access == 1) || (number == 20 && !access)) { result = -9; break; }
        if (file_fd_kind(fd) == FS_CONSOLE) return false;
        struct vector { void *base; size_t size; };
        const unsigned char *vectors = (void *) a[1];
        if (a[2] > 1024 || (!vectors && a[2])) { result = -22; break; }
        size_t total = 0;
        for (size_t i = 0; i < a[2]; ++i) {
            struct vector vector;
            copy(&vector, vectors + i * sizeof vector, sizeof vector);
            if (vector.size > INT64_MAX - total) { result = -22; goto done; }
            total += vector.size;
        }
        result = 0;
        for (size_t i = 0; i < a[2]; ++i) {
            struct vector vector;
            copy(&vector, vectors + i * sizeof vector, sizeof vector);
            /* A stream readv may wait for its first byte, never for another
             * vector after already consuming available input. */
            long n = number == 19 && result > 0 && is_stream(fd)
                ? stream_transfer(descriptors[fd].open, vector.base, vector.size, false, false)
                : transfer(fd, vector.base, vector.size, number == 20, false, 0);
            if (n < 0) { if (!result) result = n; break; }
            result += n; if ((size_t) n != vector.size) break;
        }
        break;
    }
    case 21: result = access_path(AT_CWD, path, a[1], 0); break;
    case 32: result = duplicate(fd, 0, false, false); break;
    case 33: result = duplicate(fd, (int) a[1], true, false); break;
    case 41: result = open_socket(fd, a[1], a[2]); break;
    case 42: result = connect_socket(fd, (void *) a[1], a[2]); break;
    case 48:
        result = socket_check(fd);
        if (!result) result = a[1] > 2 ? -22
            : network_error(stream_shutdown(descriptors[fd].open->stream, a[1] != 1, a[1] != 0));
        break;
    case 51: result = local_socket(fd, (void *) a[1], (void *) a[2]); break;
    case 49: case 50: case 52: case 54: case 288: case 43:
        result = socket_check(fd);
        if (!result) result = number == 54 ? -92 : -95;
        break;
    case 55: result = socket_error(fd, a[1], a[2], (void *) a[3], (void *) a[4]); break;
    case 72: result = fcntl_file(fd, a[1], a[2]); break;
    case 74: case 75:
        result = !valid(fd) || (descriptors[fd].open->flags & O_PATH)
                     ? -9
                     : is_stream(fd) ? -22
                     : error(fs_sync_node(descriptors[fd].open->node));
        break;
    case 76:
        result = lookup(AT_CWD, path, true, &node);
        if (!result) result = error(fs_resize(node, a[1]));
        break;
    case 77:
        if (!valid(fd) || !(descriptors[fd].open->flags & O_ACCESS)
            || (descriptors[fd].open->flags & O_PATH)) result = -9;
        else if (is_stream(fd)) result = -22;
        else result = error(fs_resize(descriptors[fd].open->node, a[1]));
        break;
    case 79:
        if (!a[0]) result = -14;
        else { enum fs_error e = fs_path(working_directory, (char *) a[0], a[1]);
            result = e == FS_OK ? (long) length((const char *) a[0]) + 1
                : e == FS_NAME_TOO_LONG ? -34 : error(e); } break;
    case 80: case 81:
        result = number == 80 ? lookup(AT_CWD, path, true, &node)
            : !valid(fd) ? -9 : is_stream(fd) ? -20 : 0;
        if (number == 81 && !result) node = descriptors[fd].open->node;
        if (!result && information(node).kind != FS_DIRECTORY) result = -20;
        if (!result) { fs_retain(node); fs_release(working_directory); working_directory = node; } break;
    case 82: result = rename_path(AT_CWD, path, AT_CWD, (const char *) a[1], 0); break;
    case 83: result = make_path(AT_CWD, path, FS_DIRECTORY, 0, a[1]); break;
    case 84: result = remove_path(AT_CWD, path, true); break;
    case 85: result = open_file(AT_CWD, path, O_CREATE | O_TRUNC | 1, a[1]); break;
    case 86: case 265: result = -38; break; /* Hard links are unsupported. */
    case 87: result = remove_path(AT_CWD, path, false); break;
    case 88: result = make_path(AT_CWD, (const char *) a[1], FS_SYMLINK, path, 0777); break;
    case 89: result = read_link(AT_CWD, path, (char *) a[1], a[2]); break;
    case 90: case 91:
        result = number == 90 ? lookup(AT_CWD, path, true, &node)
            : !valid(fd) ? -9 : is_stream(fd) ? -95 : 0;
        if (number == 91 && !result) node = descriptors[fd].open->node;
        if (!result) result = error(fs_chmod(node, a[1]));
        break;
    case 95: result = creation_mask; creation_mask = a[0] & 0777; break;
    case 217: result = directory_entries(fd, (void *) a[1], a[2]); break;
    case 257: result = open_file(fd, (const char *) a[1], a[2], a[3]); break;
    case 258: result = make_path(fd, (const char *) a[1], FS_DIRECTORY, 0, a[2]); break;
    case 262: result = stat_path(fd, (const char *) a[1], (void *) a[2], a[3]); break;
    case 263:
        result = a[2] & ~512UL ? -22 : remove_path(fd, (const char *) a[1], a[2] & 512); break;
    case 264: result = rename_path(fd, (const char *) a[1], (int) a[2], (const char *) a[3], 0); break;
    case 266: result = make_path((int) a[1], (const char *) a[2], FS_SYMLINK, path, 0777); break;
    case 267: result = read_link(fd, (const char *) a[1], (char *) a[2], a[3]); break;
    case 268: result = lookup(fd, (const char *) a[1], true, &node);
        if (!result) result = error(fs_chmod(node, a[2]));
        break;
    case 269: result = access_path(fd, (const char *) a[1], a[2], 0); break;
    case 280: {
        struct stamp { int64_t seconds, nanoseconds; } local[2], *times = 0;
        if (a[2]) { copy(local, (void *) a[2], sizeof local); times = local; }
        unsigned flags = a[3];
        if (flags & ~256U) { result = -22; break; }
        result = a[1] ? lookup(fd, (const char *) a[1], !(flags & 256U), &node)
            : !valid(fd) ? -9 : is_stream(fd) ? -95 : 0;
        if (!a[1] && !result) node = descriptors[fd].open->node;
        if (result) break;
        struct fs_info info = information(node);
        uint64_t stamps[2] = {clock_realtime_ns(), clock_realtime_ns()};
        for (unsigned i = 0; times && i < 2; ++i) {
            if (times[i].nanoseconds == 1073741822) {
                stamps[i] = i ? info.modified_ns : info.accessed_ns;
            } else if (times[i].nanoseconds != 1073741823) {
                if (times[i].nanoseconds < 0 || times[i].nanoseconds >= 1000000000
                    || times[i].seconds < 0 || (uint64_t) times[i].seconds >
                    (UINT64_MAX - (uint64_t) times[i].nanoseconds) / 1000000000) {
                    result = -22; break;
                }
                stamps[i] = (uint64_t) times[i].seconds * 1000000000 + times[i].nanoseconds;
            }
        }
        if (!result) result = error(fs_set_times(node, stamps[0], stamps[1]));
        break;
    }
    case 292:
        result = a[2] & ~O_CLOEXEC || fd == (int) a[1] ? -22 : duplicate(fd, (int) a[1], true, a[2] & O_CLOEXEC); break;
    case 316: result = rename_path(fd, (const char *) a[1], (int) a[2], (const char *) a[3], a[4]); break;
    case 439: result = access_path(fd, (const char *) a[1], a[2], a[3]); break;
    default: return false;
    }
done:
    *out = result;
    return true;
}

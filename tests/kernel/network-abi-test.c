/* Host ABI contracts over a controlled fake stream PAL and the real file PAL.
 * These tests do not establish NIC, TCP stack, or guest network capability. */
#include "kernel.h"
#include "files.h"
#include "network.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P(p) ((unsigned long) (p))
struct net_stream {
    bool used, connecting, connected, eof, read_closed, write_closed;
    enum net_error pending, terminal;
    uint64_t due;
    unsigned port;
    size_t used_bytes;
    unsigned char bytes[7];
};
static struct net_stream streams[NET_STREAM_LIMIT], *last_stream;
static enum net_error create_error, close_error;
static bool fail_allocation;
static size_t allocations;
static unsigned closes, services, waits, created;
static uint64_t now;
static struct net_stream *delayed_read, *delayed_drain;
static uint64_t delayed_at;

void *kmalloc(size_t n)
{ if (fail_allocation) return 0; void *p = malloc(n); assert(p); ++allocations; return p; }
void kfree(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
uint64_t clock_realtime_ns(void) { return 1700000000000000000UL + now; }
uint64_t clock_monotonic_ns(void) { return now; }
bool console_ready(void) { return false; }
void console_write(const void *p, size_t n) { (void) p; (void) n; }
long console_read(void *p, size_t n) { (void) p; (void) n; return -11; }
void console_puts(const char *p) { (void) p; }
void console_hex(uint64_t n) { (void) n; }
void storage_control_service(void) {}
long memory_map(uintptr_t a, size_t n, unsigned p, unsigned f)
{ (void) a; (void) n; (void) p; (void) f; abort(); }
long memory_unmap(uintptr_t a, size_t n) { (void) a; (void) n; abort(); }
long memory_protect(uintptr_t a, size_t n, unsigned p)
{ (void) a; (void) n; (void) p; abort(); }
long memory_brk(uintptr_t a) { (void) a; abort(); }
_Noreturn void panic(const char *p) { fprintf(stderr, "%s\n", p); abort(); }
int data_disk_init(void) { return -19; }
int data_disk_read(uint64_t s, void *p, size_t n)
{ (void) s; (void) p; (void) n; return -19; }
int data_disk_write(uint64_t s, const void *p, size_t n)
{ (void) s; (void) p; (void) n; abort(); }
int data_disk_flush(void) { abort(); }
uint64_t data_disk_sectors(void) { return 0; }

void network_service(void)
{
    ++services;
    for (unsigned i = 0; i < NET_STREAM_LIMIT; ++i) {
        struct net_stream *s = &streams[i];
        if (!s->used || !s->connecting || now < s->due) continue;
        s->connecting = false;
        s->terminal = s->port == 111 ? NET_REFUSED : s->port == 110 ? NET_TIMEOUT : NET_OK;
        s->pending = s->terminal;
        s->connected = s->terminal == NET_OK;
    }
    if (delayed_read && now >= delayed_at) {
        memcpy(delayed_read->bytes, "late", 4); delayed_read->used_bytes = 4; delayed_read = 0;
    }
    if (delayed_drain && now >= delayed_at) { delayed_drain->used_bytes = 0; delayed_drain = 0; }
}
void wait_interrupt(void) { ++waits; now += 1000000; network_service(); assert(waits < 1000); }
enum net_error stream_create(struct net_stream **out)
{
    if (create_error) return create_error;
    for (unsigned i = 0; i < NET_STREAM_LIMIT; ++i) if (!streams[i].used) {
        streams[i] = (struct net_stream) { .used = true };
        *out = last_stream = &streams[i]; ++created; return NET_OK;
    }
    return NET_NO_MEMORY;
}
enum net_error stream_connect(struct net_stream *s, const struct net_address *a)
{
    assert(s->used);
    if (s->connecting) return NET_ALREADY;
    if (s->connected) return NET_IS_CONNECTED;
    assert(a->ip[0] == 10 && a->ip[1] == 0 && a->ip[2] == 2 && a->ip[3] == 2);
    s->port = a->port; s->connecting = true; s->due = now + 3000000;
    return NET_IN_PROGRESS;
}
enum net_error stream_local(struct net_stream *s, struct net_address *a)
{
    assert(s->used);
    *a = (struct net_address) { .ip = {10, 0, 2, 15}, .port = 40000 };
    return NET_OK;
}
enum net_error stream_read(struct net_stream *s, void *p, size_t n, size_t *done)
{
    assert(s->used); *done = 0;
    if (!n || s->read_closed) return NET_OK;
    if (s->used_bytes) {
        *done = n < s->used_bytes ? n : s->used_bytes;
        memcpy(p, s->bytes, *done); s->used_bytes -= *done;
        memmove(s->bytes, s->bytes + *done, s->used_bytes); return NET_OK;
    }
    if (s->terminal) return s->terminal;
    if (s->eof) return NET_OK;
    return s->connected || s->connecting ? NET_WOULD_BLOCK : NET_NOT_CONNECTED;
}
enum net_error stream_write(struct net_stream *s, const void *p, size_t n, size_t *done)
{
    assert(s->used); *done = 0;
    if (!n) return NET_OK;
    if (s->terminal) return s->terminal;
    if (s->write_closed) return NET_BROKEN_PIPE;
    if (!s->connected) return NET_NOT_CONNECTED;
    size_t free_bytes = sizeof s->bytes - s->used_bytes;
    if (!free_bytes) return NET_WOULD_BLOCK;
    *done = n < free_bytes ? n : free_bytes;
    memcpy(s->bytes + s->used_bytes, p, *done); s->used_bytes += *done;
    return NET_OK;
}
enum net_error stream_shutdown(struct net_stream *s, bool read, bool write)
{
    assert(s->used);
    if (!s->connected) return NET_NOT_CONNECTED;
    if (read) { s->read_closed = true; s->used_bytes = 0; }
    if (write) s->write_closed = true;
    return NET_OK;
}
enum net_error stream_close(struct net_stream *s)
{ assert(s->used); s->used = false; ++closes; return close_error; }
enum net_error stream_take_error(struct net_stream *s)
{ assert(s->used); enum net_error e = s->pending; s->pending = NET_OK; return e; }
bool stream_read_ready(struct net_stream *s)
{ assert(s->used); return s->used_bytes || s->eof || s->read_closed || s->terminal; }
bool stream_write_ready(struct net_stream *s)
{ assert(s->used); return s->terminal || s->write_closed || (s->connected && s->used_bytes < sizeof s->bytes); }

static long call(unsigned long n, unsigned long a, unsigned long b,
                 unsigned long c, unsigned long d, unsigned long e)
{ unsigned long args[6] = {a, b, c, d, e, 0}; return syscall_dispatch(n, args); }
static void close_fd(int fd) { assert(call(3, fd, 0, 0, 0, 0) == 0); }
static int socket_fd(bool nonblock)
{ int fd = (int) call(41, 2, 1 | 524288 | (nonblock ? 2048 : 0), 6, 0, 0); assert(fd >= 3); return fd; }
static void sockaddr(unsigned char *p, unsigned port)
{
    const unsigned char a[16] = {2, 0, (unsigned char) (port >> 8), (unsigned char) port, 10, 0, 2, 2};
    memcpy(p, a, sizeof a);
}
static long connect_fd(int fd, unsigned port)
{ unsigned char a[17]; sockaddr(a + 1, port); return call(42, fd, P(a + 1), 16, 0, 0); }
static int so_error(int fd)
{
    unsigned char value[5], size[5]; uint32_t n = 4; int32_t result;
    memcpy(size + 1, &n, 4);
    assert(call(55, fd, 1, 4, P(value + 1), P(size + 1)) == 0);
    memcpy(&result, value + 1, 4); memcpy(&n, size + 1, 4); assert(n == 4); return result;
}
static long select_one(int fd, bool write, uint64_t duration)
{
    unsigned char set[25] = {0}, stamp[17]; uint64_t bits[3] = {0};
    bits[fd / 64] = 1UL << (fd % 64);
    memcpy(set + 1, bits, sizeof bits);
    int64_t time[2] = {(int64_t) (duration / 1000000000), (int64_t) (duration % 1000000000)};
    memcpy(stamp + 1, time, sizeof time);
    unsigned char packed_mask[17]; unsigned long mask[2] = {0, 8};
    memcpy(packed_mask + 1, mask, sizeof mask);
    unsigned long args[6] = {fd + 1, write ? 0 : P(set + 1),
        write ? P(set + 1) : 0, 0, P(stamp + 1), P(packed_mask + 1)};
    long result = syscall_dispatch(270, args);
    if (result >= 0) {
        memcpy(bits, set + 1, sizeof bits);
        assert(!!(bits[fd / 64] & (1UL << (fd % 64))) == !!result);
    }
    return result;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    unsigned char archive[2048]; size_t count = fread(archive, 1, sizeof archive, file); fclose(file);
    fs_init(archive, count);
    assert(fs_mount_data() == FS_NO_DEVICE);
    int regular = (int) call(2, P("/usr/readonly"), 0, 0, 0, 0); assert(regular == 3);
    size_t baseline = allocations;
    assert(call(41, 10, 1, 0, 0, 0) == -97);
    assert(call(41, 2, 2, 0, 0, 0) == -94);
    assert(call(41, 2, 1 | 4096, 0, 0, 0) == -22);
    assert(call(41, 2, 1, 17, 0, 0) == -93);
    for (unsigned i = 0; i < 2; ++i) {
        int fd = i ? 191 : regular;
        long expected = i ? -9 : -88;
        assert(connect_fd(fd, 80) == expected);
        assert(call(55, fd, 1, 4, 0, 0) == expected);
        assert(call(48, fd, 1, 0, 0, 0) == expected);
        assert(call(51, fd, 0, 0, 0, 0) == expected);
    }
    const enum net_error errors[] = {NET_INVALID, NET_NO_MEMORY, NET_NO_DEVICE, NET_NOT_SUPPORTED,
        NET_REFUSED, NET_TIMEOUT, NET_RESET, NET_NOT_CONNECTED, NET_BROKEN_PIPE, NET_ALREADY,
        NET_IS_CONNECTED, NET_ADDRESS_IN_USE, NET_UNREACHABLE, NET_IO};
    const int errnos[] = {22, 12, 100, 95, 111, 110, 104, 107, 32, 114, 106, 98, 113, 5};
    for (unsigned i = 0; i < sizeof errors / sizeof *errors; ++i) {
        create_error = errors[i]; assert(call(41, 2, 1, 0, 0, 0) == -errnos[i]);
        assert(allocations == baseline);
    }
    create_error = NET_OK; fail_allocation = true;
    assert(call(41, 2, 1, 0, 0, 0) == -12); fail_allocation = false;
    int timer = (int) call(283, 1, 2048, 0, 0, 0); assert(timer == 240);
    assert(connect_fd(timer, 80) == -88);
    assert(call(55, timer, 1, 4, 0, 0) == -88);
    close_fd(timer); assert(connect_fd(timer, 80) == -9);

    int fd = socket_fd(true); assert(fd == 4);
    assert(call(72, fd, 1, 0, 0, 0) == 1);
    assert(call(72, fd, 3, 0, 0, 0) == 2050);
    assert(call(72, fd, 4, 32768 | 2048, 0, 0) == 0);
    unsigned char a[16] = {10};
    assert(call(42, fd, P(a), 16, 0, 0) == -97);
    assert(call(42, fd, P(a), 15, 0, 0) == -22);
    assert(call(42, fd, 0, 16, 0, 0) == -14);
    assert(connect_fd(fd, 80) == -115);
    assert(connect_fd(fd, 80) == -114);
    unsigned char address[17], size[5]; uint32_t n = 16;
    memset(address, 0x55, sizeof address); memcpy(size + 1, &n, 4);
    assert(call(51, fd, P(address + 1), P(size + 1), 0, 0) == 0);
    const unsigned char expected[16] = {2, 0, 156, 64, 10, 0, 2, 15};
    assert(!memcmp(address + 1, expected, 16) && address[0] == 0x55);
    n = 2; memset(address, 0x55, sizeof address); memcpy(size + 1, &n, 4);
    assert(call(51, fd, P(address + 1), P(size + 1), 0, 0) == 0);
    memcpy(&n, size + 1, 4); assert(n == 16 && address[3] == 0x55);
    assert(select_one(fd, true, 1000000) == 0);
    assert(select_one(fd, true, 10000000) == 1);
    assert(so_error(fd) == 0 && connect_fd(fd, 80) == -106);
    assert(select_one(fd, false, 0) == 0);
    assert(call(0, fd, P(a), sizeof a, 0, 0) == -11);
    assert(call(0, fd, 0, 0, 0, 0) == 0);
    assert(call(1, fd, P("abcdefghij"), 10, 0, 0) == 7);
    assert(call(1, fd, P("z"), 1, 0, 0) == -11);
    assert(select_one(fd, false, 0) == 1 && select_one(fd, true, 0) == 0);
    assert(call(0, fd, P(a), 3, 0, 0) == 3 && !memcmp(a, "abc", 3));
    assert(call(0, fd, P(a), sizeof a, 0, 0) == 4 && !memcmp(a, "defg", 4));
    assert(call(0, fd, 0, 1, 0, 0) == -14);
    assert(call(8, fd, 0, 0, 0, 0) == -29);
    assert(call(17, fd, P(a), 1, 0, 0) == -29);
    assert(file_map_read(fd, 0, 0, 1) == -19);
    assert(call(81, fd, 0, 0, 0, 0) == -20);
    assert(call(217, fd, P(a), sizeof a, 0, 0) == -20);
    assert(call(257, fd, P("relative"), 0, 0, 0) == -20);
    assert(call(74, fd, 0, 0, 0, 0) == -22);
    assert(call(77, fd, 0, 0, 0, 0) == -22);
    assert(call(91, fd, 0600, 0, 0, 0) == -95);
    assert(call(280, fd, 0, 0, 0, 0) == -95);
    unsigned char stat[145];
    assert(call(5, fd, P(stat + 1), 0, 0, 0) == 0);
    uint32_t mode; memcpy(&mode, stat + 25, 4); assert((mode & 0170000) == 0140000);
    assert(call(262, fd, P(""), P(stat + 1), 4096, 0) == 0);
    assert(call(54, fd, 1, 9, 0, 0) == -92);
    assert(call(50, fd, 1, 0, 0, 0) == -95);
    assert(call(55, fd, 1, 9, 0, 0) == -92);

    struct vector { void *base; size_t size; } vectors[2] = {{"abc", 3}, {"defghi", 6}};
    unsigned char packed[sizeof vectors + 1]; memcpy(packed + 1, vectors, sizeof vectors);
    assert(call(20, fd, P(packed + 1), 2, 0, 0) == 7);
    vectors[0] = (struct vector) {a, 3}; vectors[1] = (struct vector) {a + 3, 10};
    memcpy(packed + 1, vectors, sizeof vectors);
    assert(call(19, fd, P(packed + 1), 2, 0, 0) == 7 && !memcmp(a, "abcdefg", 7));
    vectors[0].size = INT64_MAX; vectors[1].size = 1; memcpy(packed + 1, vectors, sizeof vectors);
    assert(call(20, fd, P(packed + 1), 2, 0, 0) == -22);
    assert(call(20, fd, 0, 1, 0, 0) == -22);
    assert(call(20, fd, 0, 0, 0, 0) == 0);
    assert(call(48, fd, 3, 0, 0, 0) == -22);
    assert(call(1, fd, P("xyz"), 3, 0, 0) == 3);
    assert(call(48, fd, 1, 0, 0, 0) == 0);
    assert(call(1, fd, P("z"), 1, 0, 0) == -32);
    last_stream->eof = true;
    assert(call(0, fd, P(a), sizeof a, 0, 0) == 3 && !memcmp(a, "xyz", 3));
    assert(call(0, fd, P(a), sizeof a, 0, 0) == 0);
    unsigned before = closes;
    int dup = (int) call(72, fd, 1030, 0, 0, 0); assert(dup == 5);
    assert(call(72, dup, 1, 0, 0, 0) == 1);
    assert(call(72, dup, 4, 32768, 0, 0) == 0);
    assert(call(72, fd, 3, 0, 0, 0) == 2);
    close_fd(fd); assert(closes == before);
    close_error = NET_IO; assert(call(3, dup, 0, 0, 0, 0) == -5); close_error = NET_OK;
    assert(closes == before + 1 && call(3, dup, 0, 0, 0, 0) == -9);
    assert(select_one(dup, true, 0) == -9);

    for (unsigned port = 110; port <= 111; ++port) {
        fd = socket_fd(true); assert(connect_fd(fd, port) == -115);
        assert(select_one(fd, true, 10000000) == 1);
        assert(call(55, fd, 1, 4, 0, 0) == -14);
        assert(so_error(fd) == (int) port);
        assert(so_error(fd) == 0);
        assert(call(0, fd, P(a), 1, 0, 0) == -(long) port);
        assert(call(1, fd, P(a), 1, 0, 0) == -(long) port);
        close_fd(fd);
    }
    fd = socket_fd(false); unsigned prior_waits = waits;
    assert(connect_fd(fd, 80) == 0 && waits > prior_waits);
    assert(call(1, fd, P("abc"), 3, 0, 0) == 3);
    vectors[0] = (struct vector) {a, 3}; vectors[1] = (struct vector) {a + 3, 3};
    memcpy(packed + 1, vectors, sizeof vectors);
    delayed_read = last_stream; delayed_at = now + 2000000; prior_waits = waits;
    assert(call(19, fd, P(packed + 1), 2, 0, 0) == 3 && waits == prior_waits && !memcmp(a, "abc", 3));
    delayed_read = 0;
    assert(call(1, fd, P("1234567"), 7, 0, 0) == 7);
    assert(call(19, fd, P(packed + 1), 2, 0, 0) == 6 && waits == prior_waits && !memcmp(a, "123456", 6));
    assert(call(0, fd, P(a), sizeof a, 0, 0) == 1 && a[0] == '7');
    delayed_read = last_stream; delayed_at = now + 2000000; prior_waits = waits;
    assert(call(19, fd, P(packed + 1), 2, 0, 0) == 4 && waits == prior_waits + 2 && !memcmp(a, "late", 4));
    delayed_read = last_stream; delayed_at = now + 2000000; prior_waits = waits;
    assert(call(0, fd, P(a), sizeof a, 0, 0) == 4 && waits > prior_waits && !memcmp(a, "late", 4));
    assert(call(1, fd, P("1234567"), 7, 0, 0) == 7);
    delayed_drain = last_stream; delayed_at = now + 2000000; prior_waits = waits;
    assert(call(1, fd, P("z"), 1, 0, 0) == 1 && waits > prior_waits);
    assert(call(0, fd, P(a), sizeof a, 0, 0) == 1 && a[0] == 'z');
    last_stream->terminal = NET_RESET; last_stream->pending = NET_RESET;
    assert(so_error(fd) == 104 && call(0, fd, P(a), 1, 0, 0) == -104);
    close_fd(fd);
    fd = socket_fd(false); assert(connect_fd(fd, 110) == -110); close_fd(fd);

    fd = socket_fd(true); before = closes;
    assert(call(33, regular, fd, 0, 0, 0) == fd && closes == before + 1);
    assert(call(0, fd, P(a), sizeof a, 0, 0) == 8 && !memcmp(a, "upstream", 8));
    close_fd(fd);
    fd = socket_fd(true); before = closes;
    assert(call(33, fd, regular, 0, 0, 0) == regular);
    close_fd(fd); assert(closes == before); close_fd(regular); assert(closes == before + 1);

    int held[NET_STREAM_LIMIT];
    for (unsigned i = 0; i < NET_STREAM_LIMIT; ++i) held[i] = socket_fd(true);
    assert(call(41, 2, 1, 0, 0, 0) == -12);
    for (unsigned i = 0; i < NET_STREAM_LIMIT; ++i) close_fd(held[i]);
    fd = socket_fd(true);
    for (int i = fd + 1; i < 192; ++i) assert(call(32, fd, 0, 0, 0, 0) == i);
    unsigned prior_created = created;
    assert(call(41, 2, 1, 0, 0, 0) == -24 && created == prior_created);
    assert(call(32, fd, 0, 0, 0, 0) == -24);
    assert(call(33, fd, fd, 0, 0, 0) == fd);
    assert(call(292, fd, fd, 0, 0, 0) == -22);
    before = closes;
    for (int i = fd; i < 192; ++i) close_fd(i);
    assert(closes == before + 1);
    for (unsigned i = 0; i < NET_STREAM_LIMIT; ++i) assert(!streams[i].used);
    assert(services > 100 && allocations == baseline - 1);
    puts("Network ABI fake-PAL semantics passed: flags, errors, alignment, readiness, partial IO, EOF, shutdown, dup and bounds");
    return 0;
}

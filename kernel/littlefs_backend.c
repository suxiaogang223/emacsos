/* littlefs provides storage commits; this adapter provides POSIX object identity.
 * User names refer to immutable object IDs. Replacing a name never relocates
 * an open destination's data, and unreachable objects are collected at boot.
 */
#include "littlefs_backend.h"
#include "block.h"
#include "../third_party/littlefs/lfs.h"

extern void *kmalloc(size_t);
extern void kfree(void *);
extern uint64_t clock_realtime_ns(void);
#define BLOCK_SIZE 4096U
#define BLOCK_COUNT 16384U
#define CACHE_SIZE 512U
#define META_ATTR 0xe0
#define VERSION_ATTR 0xf0
#define NEXT_ATTR 0xf1
#define BOOT_ATTR 0xf2
#define META_SIZE 48U
#define PRIVATE_LIMIT (FS_NODE_LIMIT + 256U)
#define NATIVE_PATH (FS_PATH_MAX + 16)

struct littlefs_handle {
    lfs_file_t file;
    struct lfs_file_config config;
    struct lfs_attr attr;
    unsigned char cache[CACHE_SIZE], metadata[META_SIZE];
    uint64_t id;
    bool writable, opened;
    int error;
};
static lfs_t volume;
static struct lfs_config config;
static unsigned char read_cache[CACHE_SIZE], program_cache[CACHE_SIZE], lookahead[128];
static uint64_t next_id, previous_boot;
static bool active;

static void put64(unsigned char *p, uint64_t n)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (unsigned char)n;
        n >>= 8;
    }
}
static uint64_t get64(const unsigned char *p)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; i++)
        n |= (uint64_t)p[i] << (8 * i);
    return n;
}
static size_t length(const char *p)
{
    size_t n = 0;
    while (p[n])
        ++n;
    return n;
}
static void object_path(char *path, const char *prefix, uint64_t id)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = length(prefix);
    memcpy(path, prefix, n);
    for (unsigned i = 0; i < 16; i++)
        path[n + i] = hex[(id >> (60 - i * 4)) & 15];
    path[n + 16] = 0;
}
static int native_path(const char *path, char *out)
{
    size_t n = length(path);
    if (n >= FS_PATH_MAX || (n && path[0] != '/'))
        return -22;
    memcpy(out, "/tree", 5);
    memcpy(out + 5, path, n + 1);
    return 0;
}
static int device_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *p,
                       lfs_size_t size)
{
    (void)c;
    return data_disk_read((uint64_t)block * 8 + off / 512, p, size / 512);
}
static int device_program(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                          const void *p, lfs_size_t size)
{
    (void)c;
    return data_disk_write((uint64_t)block * 8 + off / 512, p, size / 512);
}
static int device_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c;
    unsigned char erased[512];
    memset(erased, 255, sizeof erased);
    for (unsigned i = 0; i < 8; i++) {
        int e = data_disk_write((uint64_t)block * 8 + i, erased, 1);
        if (e)
            return e;
    }
    return 0;
}
static int device_sync(const struct lfs_config *c)
{
    (void)c;
    return data_disk_flush();
}
static void configure(void)
{
    memset(&config, 0, sizeof config);
    config.read = device_read;
    config.prog = device_program;
    config.erase = device_erase;
    config.sync = device_sync;
    config.read_size = 512;
    config.prog_size = 512;
    config.block_size = BLOCK_SIZE;
    config.block_count = BLOCK_COUNT;
    config.block_cycles = 500;
    config.cache_size = CACHE_SIZE;
    config.lookahead_size = sizeof lookahead;
    config.read_buffer = read_cache;
    config.prog_buffer = program_cache;
    config.lookahead_buffer = lookahead;
    config.name_max = FS_NAME_MAX;
    config.file_max = FS_FILE_LIMIT;
    config.attr_max = 1022;
}
static void encode(unsigned char *p, const struct littlefs_metadata *m)
{
    memcpy(p, "EOSN0001", 8);
    put64(p + 8, m->id);
    put64(p + 16, ((uint64_t)m->mode << 32) | (unsigned)m->kind);
    put64(p + 24, m->accessed_ns);
    put64(p + 32, m->modified_ns);
    put64(p + 40, m->changed_ns);
}
static int decode(const unsigned char *p, struct littlefs_metadata *m)
{
    uint64_t fields = get64(p + 16);
    unsigned kind = (unsigned)fields, mode = fields >> 32;
    if (memcmp(p, "EOSN0001", 8) || !get64(p + 8) || get64(p + 8) >= (1ULL << 63) ||
        kind > FS_SYMLINK || (mode & ~07777U))
        return -84;
    *m = (struct littlefs_metadata){.id = get64(p + 8),
                                    .kind = (enum fs_kind)kind,
                                    .mode = mode,
                                    .accessed_ns = get64(p + 24),
                                    .modified_ns = get64(p + 32),
                                    .changed_ns = get64(p + 40)};
    return 0;
}
static int get_metadata(const char *path, struct littlefs_metadata *m)
{
    unsigned char bytes[META_SIZE];
    int e = lfs_getattr(&volume, path, META_ATTR, bytes, sizeof bytes);
    if (e < 0)
        return e;
    if (e != META_SIZE)
        return -84;
    return decode(bytes, m);
}
static int set_metadata(const char *path, const struct littlefs_metadata *m)
{
    unsigned char bytes[META_SIZE];
    encode(bytes, m);
    return lfs_setattr(&volume, path, META_ATTR, bytes, sizeof bytes);
}
static int attribute64(unsigned type, uint64_t *out)
{
    unsigned char bytes[8];
    int e = lfs_getattr(&volume, "/", type, bytes, sizeof bytes);
    if (e < 0)
        return e;
    if (e != 8)
        return -84;
    *out = get64(bytes);
    return 0;
}
static int set64(unsigned type, uint64_t n)
{
    unsigned char bytes[8];
    put64(bytes, n);
    return lfs_setattr(&volume, "/", type, bytes, sizeof bytes);
}
static int file_open(struct littlefs_handle *h, uint64_t id, bool writable, bool create,
                     const struct littlefs_metadata *metadata)
{
    memset(h, 0, sizeof *h);
    h->id = id;
    h->writable = writable;
    h->config.buffer = h->cache;
    h->attr = (struct lfs_attr){.type = META_ATTR, .buffer = h->metadata, .size = META_SIZE};
    h->config.attrs = &h->attr;
    h->config.attr_count = 1;
    if (metadata)
        encode(h->metadata, metadata);
    char path[32];
    object_path(path, "/objects/", id);
    int flags = create     ? LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL
                : writable ? LFS_O_RDWR
                           : LFS_O_RDONLY;
    int e = lfs_file_opencfg(&volume, &h->file, path, flags, &h->config);
    h->opened = !e;
    return e;
}
static int upgrade(struct littlefs_handle *h)
{
    if (!h->opened)
        return h->error ? h->error : -5;
    if (h->writable)
        return 0;
    uint64_t id = h->id;
    int e = lfs_file_close(&volume, &h->file);
    h->opened = false;
    if (e) {
        h->error = e;
        return e;
    }
    e = file_open(h, id, true, false, NULL);
    if (e)
        h->error = e;
    return e;
}
static int marker_id(const char *path, uint64_t *id)
{
    lfs_file_t file;
    unsigned char cache[CACHE_SIZE], bytes[8];
    const struct lfs_file_config cfg = {.buffer = cache};
    int e = lfs_file_opencfg(&volume, &file, path, LFS_O_RDONLY, &cfg);
    if (e)
        return e;
    lfs_soff_t size = lfs_file_size(&volume, &file);
    lfs_ssize_t n = lfs_file_read(&volume, &file, bytes, sizeof bytes);
    int closed = lfs_file_close(&volume, &file);
    if (n < 0)
        return n;
    if (closed)
        return closed;
    if (size != 8 || n != 8)
        return -84;
    *id = get64(bytes);
    return !*id || *id >= next_id ? -84 : 0;
}
static int entry_metadata(const char *path, unsigned type, struct littlefs_metadata *m)
{
    if (type == LFS_TYPE_DIR) {
        int e = get_metadata(path, m);
        return e ? e : m->kind != FS_DIRECTORY || m->id >= next_id ? -84 : 0;
    }
    if (type != LFS_TYPE_REG)
        return -84;
    uint64_t id;
    int e = marker_id(path, &id);
    if (e)
        return e;
    char object[32];
    object_path(object, "/objects/", id);
    e = get_metadata(object, m);
    if (e)
        return e;
    if (m->id != id || m->kind == FS_DIRECTORY)
        return -84;
    struct lfs_info info;
    e = lfs_stat(&volume, object, &info);
    if (e)
        return e;
    if (info.type != LFS_TYPE_REG || info.size > FS_FILE_LIMIT)
        return -84;
    m->size = info.size;
    if (m->kind == FS_SYMLINK && (!m->size || m->size >= FS_PATH_MAX))
        return -84;
    return 0;
}
int littlefs_backend_mount(uint64_t *boot_id)
{
    unsigned char version[8];
    configure();
    active = false;
    int e = lfs_mount(&volume, &config);
    if (e)
        return e;
    active = true;
    e = lfs_getattr(&volume, "/", VERSION_ATTR, version, sizeof version);
    if (e != 8 || memcmp(version, EMACSOS_DATA_FORMAT, 8))
        return e < 0 ? e : -95;
    if ((e = attribute64(NEXT_ATTR, &next_id)) || (e = attribute64(BOOT_ATTR, &previous_boot)))
        return e;
    if (next_id < 2 || next_id >= (1ULL << 63) || previous_boot >= 2147483646ULL)
        return -95;
    struct littlefs_metadata root;
    e = get_metadata("/tree", &root);
    if (e || root.kind != FS_DIRECTORY || root.id != 1)
        return e ? e : -84;
    struct lfs_info info;
    if ((e = lfs_stat(&volume, "/objects", &info)) || info.type != LFS_TYPE_DIR)
        return e ? e : -84;
    if ((e = lfs_stat(&volume, "/stage", &info)) || info.type != LFS_TYPE_DIR)
        return e ? e : -84;
    *boot_id = previous_boot;
    return 0;
}
int littlefs_backend_root_metadata(struct littlefs_metadata *m)
{
    return get_metadata("/tree", m);
}
int littlefs_backend_accept(uint64_t *boot_id)
{
    int e = set64(BOOT_ATTR, previous_boot + 1);
    if (!e)
        e = data_disk_flush();
    if (!e)
        *boot_id = ++previous_boot;
    return e;
}
void littlefs_backend_unmount(void)
{
    if (active)
        (void)lfs_unmount(&volume);
    active = false;
}
int littlefs_backend_list(const char *relative,
                          int (*visit)(void *, const char *, const struct littlefs_metadata *),
                          void *context)
{
    char path[NATIVE_PATH];
    int e = native_path(relative, path);
    if (e)
        return e;
    lfs_dir_t dir;
    e = lfs_dir_open(&volume, &dir, path);
    if (e)
        return e;
    struct lfs_info info;
    unsigned entries = 0;
    while ((e = lfs_dir_read(&volume, &dir, &info)) > 0) {
        if (!memcmp(info.name, ".\0", 2) || !memcmp(info.name, "..\0", 3))
            continue;
        if (++entries > FS_NODE_LIMIT) {
            e = -28;
            break;
        }
        size_t at = length(path), n = length(info.name);
        if (at + 1 + n >= sizeof path) {
            e = -36;
            break;
        }
        path[at] = '/';
        memcpy(path + at + 1, info.name, n + 1);
        struct littlefs_metadata metadata;
        e = entry_metadata(path, info.type, &metadata);
        path[at] = 0;
        if (e)
            break;
        e = visit(context, info.name, &metadata);
        if (e)
            break;
    }
    int closed = lfs_dir_close(&volume, &dir);
    return e < 0 ? e : closed;
}
int littlefs_backend_create(const char *relative, struct littlefs_metadata *m, const char *target)
{
    if (next_id >= (1ULL << 63) - 1)
        return -28;
    m->id = next_id;
    int e = set64(NEXT_ATTR, next_id + 1);
    if (e)
        return e;
    ++next_id;
    char staged[32], path[NATIVE_PATH];
    object_path(staged, "/stage/", m->id);
    e = native_path(relative, path);
    if (e)
        return e;
    if (m->kind == FS_DIRECTORY) {
        e = lfs_mkdir(&volume, staged);
        if (!e)
            e = set_metadata(staged, m);
    } else {
        struct littlefs_handle object;
        e = file_open(&object, m->id, true, true, m);
        if (!e) {
            if (m->kind == FS_SYMLINK) {
                lfs_ssize_t n = lfs_file_write(&volume, &object.file, target, m->size);
                if (n != (lfs_ssize_t)m->size)
                    e = n < 0 ? n : -5;
            }
            int closed = lfs_file_close(&volume, &object.file);
            if (!e)
                e = closed;
        }
        if (!e) {
            lfs_file_t marker;
            unsigned char cache[CACHE_SIZE], bytes[8];
            put64(bytes, m->id);
            const struct lfs_file_config cfg = {.buffer = cache};
            e = lfs_file_opencfg(&volume, &marker, staged, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL,
                                 &cfg);
            if (!e) {
                lfs_ssize_t n = lfs_file_write(&volume, &marker, bytes, 8);
                if (n != 8)
                    e = n < 0 ? n : -5;
                int closed = lfs_file_close(&volume, &marker);
                if (!e)
                    e = closed;
            }
        }
    }
    if (!e)
        e = lfs_rename(&volume, staged, path);
    if (!e)
        e = data_disk_flush();
    /* A failed commit may already have published the marker. Never remove its
     * object here: validated boot collection reclaims only unreachable IDs. */
    return e;
}
int littlefs_backend_remove(const char *relative, enum fs_kind kind)
{
    (void)kind;
    char path[NATIVE_PATH];
    int e = native_path(relative, path);
    if (!e)
        e = lfs_remove(&volume, path);
    return e ? e : data_disk_flush();
}
int littlefs_backend_rename(const char *old, const char *new)
{
    char a[NATIVE_PATH], b[NATIVE_PATH];
    int e = native_path(old, a);
    if (!e)
        e = native_path(new, b);
    if (!e)
        e = lfs_rename(&volume, a, b);
    return e ? e : data_disk_flush();
}
int littlefs_backend_object_metadata(uint64_t id, struct littlefs_metadata *m)
{
    char path[32];
    object_path(path, "/objects/", id);
    int e = get_metadata(path, m);
    if (e)
        return e;
    struct lfs_info info;
    e = lfs_stat(&volume, path, &info);
    if (e)
        return e;
    if (m->id != id || m->kind == FS_DIRECTORY || info.type != LFS_TYPE_REG ||
        info.size > FS_FILE_LIMIT)
        return -84;
    m->size = info.size;
    return 0;
}
int littlefs_backend_open(uint64_t id, struct littlefs_handle **out)
{
    struct littlefs_handle *h = kmalloc(sizeof *h);
    if (!h)
        return -12;
    int e = file_open(h, id, false, false, NULL);
    if (e) {
        kfree(h);
        return e;
    }
    *out = h;
    return 0;
}
int littlefs_backend_read(struct littlefs_handle *h, uint64_t offset, void *buffer, size_t count)
{
    if (!h->opened)
        return h->error ? h->error : -5;
    lfs_soff_t p = lfs_file_seek(&volume, &h->file, (lfs_soff_t)offset, LFS_SEEK_SET);
    if (p < 0)
        return p;
    return lfs_file_read(&volume, &h->file, buffer, (lfs_size_t)count);
}
int littlefs_backend_write(struct littlefs_handle *h, uint64_t offset, const void *buffer,
                           size_t count)
{
    int e = upgrade(h);
    if (e)
        return e;
    lfs_soff_t p = lfs_file_seek(&volume, &h->file, (lfs_soff_t)offset, LFS_SEEK_SET);
    if (p < 0)
        return p;
    return lfs_file_write(&volume, &h->file, buffer, (lfs_size_t)count);
}
int littlefs_backend_resize(struct littlefs_handle *h, uint64_t size)
{
    int e = upgrade(h);
    return e ? e : lfs_file_truncate(&volume, &h->file, (lfs_off_t)size);
}
int littlefs_backend_sync(struct littlefs_handle *h, const struct littlefs_metadata *m)
{
    int e = upgrade(h);
    if (e)
        return e;
    encode(h->metadata, m);
    h->file.flags |= LFS_F_DIRTY;
    return lfs_file_sync(&volume, &h->file);
}
int littlefs_backend_close(struct littlefs_handle *h)
{
    int e = h->opened ? lfs_file_close(&volume, &h->file) : h->error;
    if (h->error)
        e = h->error;
    kfree(h);
    return e;
}
int littlefs_backend_metadata(const char *relative, const struct littlefs_metadata *m)
{
    char path[NATIVE_PATH];
    int e = native_path(relative, path);
    return e ? e : set_metadata(path, m);
}
int littlefs_backend_delete_object(uint64_t id)
{
    char path[32];
    object_path(path, "/objects/", id);
    int e = lfs_remove(&volume, path);
    return e == -2 ? 0 : e;
}
int littlefs_backend_flush(void)
{
    return data_disk_flush();
}
static int parse_id(const char *s, uint64_t *id)
{
    if (length(s) != 16)
        return -84;
    *id = 0;
    for (unsigned i = 0; i < 16; i++) {
        unsigned c = (unsigned char)s[i], n = c >= '0' && c <= '9'   ? c - '0'
                                              : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                                     : 16;
        if (n == 16)
            return -84;
        *id = (*id << 4) | n;
    }
    return !*id || *id >= next_id ? -84 : 0;
}
int littlefs_backend_collect(const uint64_t *live, size_t count, bool *changed)
{
    uint64_t *garbage = kmalloc(PRIVATE_LIMIT * sizeof *garbage);
    if (!garbage)
        return -12;
    size_t total = 0, discard = 0;
    int e = 0;
    *changed = false;
    lfs_dir_t dir;
    struct lfs_info info;
    e = lfs_dir_open(&volume, &dir, "/objects");
    if (e) {
        kfree(garbage);
        return e;
    }
    while ((e = lfs_dir_read(&volume, &dir, &info)) > 0) {
        if (!memcmp(info.name, ".\0", 2) || !memcmp(info.name, "..\0", 3))
            continue;
        uint64_t id;
        if (++total > PRIVATE_LIMIT || info.type != LFS_TYPE_REG ||
            (e = parse_id(info.name, &id))) {
            e = -84;
            break;
        }
        bool found = false;
        for (size_t i = 0; i < count; i++)
            if (live[i] == id) {
                found = true;
                break;
            }
        if (!found)
            garbage[discard++] = id;
    }
    int closed = lfs_dir_close(&volume, &dir);
    if (!e)
        e = closed;
    if (e < 0) {
        kfree(garbage);
        return e;
    }
    /* Validate all private entries before performing any recovery writes. */
    uint64_t *staged = kmalloc(PRIVATE_LIMIT * sizeof *staged);
    if (!staged) {
        kfree(garbage);
        return -12;
    }
    size_t stage_count = 0;
    e = lfs_dir_open(&volume, &dir, "/stage");
    if (!e) {
        while ((e = lfs_dir_read(&volume, &dir, &info)) > 0) {
            if (!memcmp(info.name, ".\0", 2) || !memcmp(info.name, "..\0", 3))
                continue;
            uint64_t id;
            if (++total > PRIVATE_LIMIT || (e = parse_id(info.name, &id)) ||
                (info.type != LFS_TYPE_DIR && info.type != LFS_TYPE_REG)) {
                e = -84;
                break;
            }
            if (info.type == LFS_TYPE_DIR) {
                char path[32];
                object_path(path, "/stage/", id);
                lfs_dir_t nested;
                e = lfs_dir_open(&volume, &nested, path);
                if (e)
                    break;
                struct lfs_info child;
                while ((e = lfs_dir_read(&volume, &nested, &child)) > 0) {
                    if (memcmp(child.name, ".\0", 2) && memcmp(child.name, "..\0", 3)) {
                        e = -84;
                        break;
                    }
                }
                int nc = lfs_dir_close(&volume, &nested);
                if (!e)
                    e = nc;
                if (e < 0)
                    break;
            }
            staged[stage_count++] = id;
        }
        closed = lfs_dir_close(&volume, &dir);
        if (!e)
            e = closed;
    }
    if (e < 0) {
        kfree(staged);
        kfree(garbage);
        return e;
    }
    for (size_t i = 0; i < stage_count; i++) {
        char path[32];
        object_path(path, "/stage/", staged[i]);
        e = lfs_remove(&volume, path);
        if (e)
            break;
        *changed = true;
    }
    kfree(staged);
    if (!e)
        for (size_t i = 0; i < discard; i++) {
            e = littlefs_backend_delete_object(garbage[i]);
            if (e)
                break;
            *changed = true;
        }
    kfree(garbage);
    return e ? e : data_disk_flush();
}
#ifdef EMACSOS_HOST_TEST
int littlefs_backend_format(void)
{
    configure();
    if (data_disk_sectors() != BLOCK_COUNT * 8)
        return -95;
    int e = lfs_format(&volume, &config);
    if (e)
        return e;
    e = lfs_mount(&volume, &config);
    if (e)
        return e;
    active = true;
    next_id = 2;
    const char *dirs[] = {"/objects", "/stage", "/tree"};
    for (unsigned i = 0; i < 3 && !e; i++)
        e = lfs_mkdir(&volume, dirs[i]);
    uint64_t now = clock_realtime_ns();
    struct littlefs_metadata root = {.id = 1,
                                     .kind = FS_DIRECTORY,
                                     .mode = 0755,
                                     .accessed_ns = now,
                                     .modified_ns = now,
                                     .changed_ns = now};
    if (!e)
        e = set_metadata("/tree", &root);
    const char *anchors[] = {"/tree/work", "/tree/home", "/tree/tmp", "/tree/home/user"};
    for (unsigned i = 0; i < 4 && !e; i++) {
        e = lfs_mkdir(&volume, anchors[i]);
        root.id = i + 2;
        root.mode = i == 2 ? 01777 : 0755;
        if (!e)
            e = set_metadata(anchors[i], &root);
    }
    if (!e)
        e = set64(NEXT_ATTR, 6);
    if (!e)
        e = set64(BOOT_ATTR, 0);
    if (!e)
        e = lfs_setattr(&volume, "/", VERSION_ATTR, EMACSOS_DATA_FORMAT, 8);
    if (!e)
        e = data_disk_flush();
    int closed = lfs_unmount(&volume);
    active = false;
    return e ? e : closed;
}
#endif

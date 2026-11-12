#include "files.h"

extern void *kmalloc(size_t);
extern void kfree(void *);
extern uint64_t clock_realtime_ns(void);
extern _Noreturn void panic(const char *);

struct fs_node {
    struct fs_node *parent, *children, *next;
    const unsigned char *data;
    size_t size, capacity;
    uint64_t inode, accessed_ns, modified_ns, changed_ns;
    unsigned mode, references;
    enum fs_kind kind;
    bool writable, linked, allocated;
    char name[FS_NAME_MAX + 1];
};

static struct fs_node *root_node;
static size_t node_count, ram_bytes;
static uint64_t next_inode;

static size_t text_length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static bool text_equal(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void copy_bytes(void *d, const void *s, size_t n)
{ unsigned char *to = d; const unsigned char *from = s; while (n--) *to++ = *from++; }
static void zero_bytes(void *d, size_t n)
{ unsigned char *p = d; while (n--) *p++ = 0; }
static bool copy_text(char *d, size_t capacity, const char *s)
{ size_t n = text_length(s); if (n >= capacity) return false; copy_bytes(d, s, n + 1); return true; }
static bool trailing_slash(const char *s)
{ size_t n = text_length(s); return n && s[n - 1] == '/'; }
static void changed(struct fs_node *n)
{ n->modified_ns = n->changed_ns = clock_realtime_ns(); }

static struct fs_node *child(struct fs_node *directory, const char *name)
{
    for (struct fs_node *n = directory->children; n; n = n->next)
        if (text_equal(n->name, name)) return n;
    return 0;
}

static struct fs_node *new_node(struct fs_node *parent, const char *name,
                                enum fs_kind kind, unsigned mode, bool writable)
{
    if (node_count == FS_NODE_LIMIT) return 0;
    struct fs_node *n = kmalloc(sizeof *n);
    if (!n) return 0;
    zero_bytes(n, sizeof *n);
    if (!copy_text(n->name, sizeof n->name, name)) { kfree(n); return 0; }
    n->parent = parent;
    n->kind = kind;
    n->mode = mode & 07777;
    n->writable = writable;
    n->linked = true;
    n->inode = ++next_inode;
    changed(n);
    n->accessed_ns = n->modified_ns;
    if (parent) { n->next = parent->children; parent->children = n; }
    ++node_count;
    return n;
}

static void destroy_if_unused(struct fs_node *n)
{
    if (n->linked || n->references) return;
    if (n->allocated) { ram_bytes -= n->capacity; kfree((void *) n->data); }
    --node_count;
    kfree(n);
}

void fs_retain(struct fs_node *n) { ++n->references; }
void fs_release(struct fs_node *n)
{ if (!n || !n->references) return; --n->references; destroy_if_unused(n); }
struct fs_node *fs_root(void) { return root_node; }
size_t fs_ram_used(void) { return ram_bytes; }

static enum fs_error walk(struct fs_node *base, const char *path, bool follow,
                           unsigned links, struct fs_node **out)
{
    if (!path || !*path) return FS_NOT_FOUND;
    if (text_length(path) >= FS_PATH_MAX) return FS_NAME_TOO_LONG;
    struct fs_node *n = *path == '/' ? root_node : base;
    if (!n) return FS_NOT_FOUND;
    while (*path) {
        while (*path == '/') ++path;
        if (!*path) break;
        if (n->kind != FS_DIRECTORY) return FS_NOT_DIRECTORY;
        char part[FS_NAME_MAX + 1]; size_t length = 0;
        while (*path && *path != '/') {
            if (length == FS_NAME_MAX) return FS_NAME_TOO_LONG;
            part[length++] = *path++;
        }
        part[length] = 0;
        if (text_equal(part, ".")) continue;
        if (text_equal(part, "..")) { if (n->parent) n = n->parent; continue; }
        struct fs_node *found = child(n, part);
        if (!found) return FS_NOT_FOUND;
        if (found->kind == FS_SYMLINK && (follow || *path)) {
            if (links == 16) return FS_LOOP;
            char expanded[FS_PATH_MAX];
            size_t suffix = text_length(path);
            if (found->size + suffix >= sizeof expanded) return FS_NAME_TOO_LONG;
            copy_bytes(expanded, found->data, found->size);
            copy_bytes(expanded + found->size, path, suffix + 1);
            return walk(n, expanded, follow, links + 1, out);
        }
        n = found;
        if (*path && n->kind != FS_DIRECTORY) return FS_NOT_DIRECTORY;
    }
    *out = n;
    return FS_OK;
}

enum fs_error fs_lookup(struct fs_node *base, const char *path, bool follow,
                        struct fs_node **out)
{ return walk(base, path, follow, 0, out); }

static enum fs_error parent_of(struct fs_node *base, const char *path,
                                struct fs_node **parent, char *name)
{
    if (!path || !*path) return FS_NOT_FOUND;
    size_t length = text_length(path);
    if (length >= FS_PATH_MAX) return FS_NAME_TOO_LONG;
    while (length && path[length - 1] == '/') --length;
    if (!length) return FS_BUSY;
    size_t start = length;
    while (start && path[start - 1] != '/') --start;
    size_t n = length - start;
    if (n > FS_NAME_MAX) return FS_NAME_TOO_LONG;
    copy_bytes(name, path + start, n); name[n] = 0;
    if (text_equal(name, ".") || text_equal(name, "..")) return FS_INVALID;
    if (!start) *parent = base;
    else {
        char prefix[FS_PATH_MAX];
        copy_bytes(prefix, path, start); prefix[start] = 0;
        enum fs_error error = fs_lookup(base, prefix, true, parent);
        if (error != FS_OK) return error;
    }
    if (!*parent || (*parent)->kind != FS_DIRECTORY) return FS_NOT_DIRECTORY;
    return FS_OK;
}

enum fs_error fs_create(struct fs_node *base, const char *path, enum fs_kind kind,
                        const char *target, unsigned mode, bool exclusive,
                        struct fs_node **out)
{
    char name[FS_NAME_MAX + 1]; struct fs_node *parent;
    enum fs_error error = parent_of(base, path, &parent, name);
    if (error != FS_OK) return error;
    struct fs_node *n = child(parent, name);
    if (trailing_slash(path) && kind != FS_DIRECTORY) {
        if (!n) return FS_NOT_FOUND;
        return n->kind == FS_DIRECTORY ? FS_EXISTS : FS_NOT_DIRECTORY;
    }
    if (n) {
        if (exclusive) return FS_EXISTS;
        *out = n; return FS_OK;
    }
    if (!parent->writable) return FS_READ_ONLY;
    if (kind != FS_REGULAR && kind != FS_DIRECTORY && kind != FS_SYMLINK) return FS_INVALID;
    size_t target_size = target ? text_length(target) : 0;
    if (kind == FS_SYMLINK && (!target_size || target_size >= FS_PATH_MAX)) return FS_NAME_TOO_LONG;
    if (kind == FS_SYMLINK && target_size > FS_RAM_LIMIT - ram_bytes) return FS_NO_SPACE;
    n = new_node(parent, name, kind, mode, true);
    if (!n) return FS_NO_MEMORY;
    if (kind == FS_SYMLINK) {
        unsigned char *data = kmalloc(target_size);
        if (!data) {
            parent->children = n->next; n->linked = false; destroy_if_unused(n);
            return FS_NO_MEMORY;
        }
        copy_bytes(data, target, target_size);
        n->data = data; n->size = n->capacity = target_size; n->allocated = true;
        ram_bytes += target_size;
    }
    changed(parent);
    *out = n;
    return FS_OK;
}

static enum fs_error create_regular(struct fs_node *base, const char *path,
                                     unsigned mode, unsigned links, struct fs_node **out)
{
    char name[FS_NAME_MAX + 1]; struct fs_node *parent;
    enum fs_error e = parent_of(base, path, &parent, name);
    if (e != FS_OK) return e;
    struct fs_node *n = child(parent, name);
    if (n && n->kind == FS_SYMLINK) {
        if (links == 16) return FS_LOOP;
        if (!n->size || n->size >= FS_PATH_MAX) return FS_NAME_TOO_LONG;
        char target[FS_PATH_MAX]; copy_bytes(target, n->data, n->size); target[n->size] = 0;
        return create_regular(parent, target, mode, links + 1, out);
    }
    return fs_create(base, path, FS_REGULAR, 0, mode, false, out);
}

enum fs_error fs_create_regular(struct fs_node *base, const char *path,
                                unsigned mode, struct fs_node **out)
{ return create_regular(base, path, mode, 0, out); }

static void detach(struct fs_node *n)
{
    struct fs_node **p = &n->parent->children;
    while (*p != n) p = &(*p)->next;
    *p = n->next;
    changed(n->parent);
    n->linked = false;
    n->next = 0;
}

enum fs_error fs_remove(struct fs_node *base, const char *path, bool directory)
{
    char name[FS_NAME_MAX + 1]; struct fs_node *parent;
    enum fs_error error = parent_of(base, path, &parent, name);
    if (error != FS_OK) return error;
    struct fs_node *n = child(parent, name);
    if (!n) return FS_NOT_FOUND;
    if (trailing_slash(path) && n->kind != FS_DIRECTORY) return FS_NOT_DIRECTORY;
    if (!parent->writable || !n->writable) return FS_READ_ONLY;
    if (directory != (n->kind == FS_DIRECTORY)) return directory ? FS_NOT_DIRECTORY : FS_IS_DIRECTORY;
    if (n->kind == FS_DIRECTORY && n->children) return FS_NOT_EMPTY;
    if (n->kind == FS_DIRECTORY && n->references) return FS_BUSY;
    detach(n); destroy_if_unused(n);
    return FS_OK;
}

enum fs_error fs_rename(struct fs_node *old_base, const char *old_path,
                        struct fs_node *new_base, const char *new_path, bool no_replace)
{
    char old_name[FS_NAME_MAX + 1], new_name[FS_NAME_MAX + 1];
    struct fs_node *old_parent, *new_parent;
    enum fs_error e = parent_of(old_base, old_path, &old_parent, old_name);
    if (e != FS_OK) return e;
    e = parent_of(new_base, new_path, &new_parent, new_name);
    if (e != FS_OK) return e;
    struct fs_node *source = child(old_parent, old_name), *dest = child(new_parent, new_name);
    if (!source) return FS_NOT_FOUND;
    if (trailing_slash(old_path) && source->kind != FS_DIRECTORY) return FS_NOT_DIRECTORY;
    if (trailing_slash(new_path) && source->kind != FS_DIRECTORY)
        return FS_NOT_DIRECTORY;
    if (!old_parent->writable || !new_parent->writable || !source->writable) return FS_READ_ONLY;
    if (dest && no_replace) return FS_EXISTS;
    if (source == dest) return FS_OK;
    for (struct fs_node *p = new_parent; p; p = p->parent)
        if (p == source) return FS_INVALID;
    if (dest) {
        if (!dest->writable) return FS_READ_ONLY;
        if ((source->kind == FS_DIRECTORY) != (dest->kind == FS_DIRECTORY))
            return source->kind == FS_DIRECTORY ? FS_NOT_DIRECTORY : FS_IS_DIRECTORY;
        if (dest->kind == FS_DIRECTORY && dest->children) return FS_NOT_EMPTY;
        if (dest->kind == FS_DIRECTORY && dest->references) return FS_BUSY;
        detach(dest); destroy_if_unused(dest);
    }
    detach(source);
    source->parent = new_parent;
    copy_text(source->name, sizeof source->name, new_name);
    source->next = new_parent->children; new_parent->children = source;
    source->linked = true;
    source->changed_ns = clock_realtime_ns(); changed(new_parent);
    return FS_OK;
}

void fs_info(const struct fs_node *n, struct fs_info *info)
{
    info->inode = n->inode; info->size = n->size;
    info->accessed_ns = n->accessed_ns;
    info->modified_ns = n->modified_ns; info->changed_ns = n->changed_ns;
    info->mode = n->mode; info->kind = n->kind; info->writable = n->writable;
    info->links = n->linked ? 1 : 0;
    if (n->kind == FS_DIRECTORY) {
        info->links = 2;
        for (struct fs_node *p = n->children; p; p = p->next)
            if (p->kind == FS_DIRECTORY) ++info->links;
    }
}

enum fs_error fs_path(struct fs_node *n, char *out, size_t capacity)
{
    char path[FS_PATH_MAX]; size_t offset = sizeof path - 1; path[offset] = 0;
    while (n != root_node) {
        if (!n || !n->linked) return FS_NOT_FOUND;
        size_t size = text_length(n->name);
        if (size + 1 > offset) return FS_NAME_TOO_LONG;
        offset -= size; copy_bytes(path + offset, n->name, size);
        path[--offset] = '/'; n = n->parent;
    }
    if (!path[offset]) path[--offset] = '/';
    return copy_text(out, capacity, path + offset) ? FS_OK : FS_NAME_TOO_LONG;
}

enum fs_error fs_read(struct fs_node *n, uint64_t offset, void *buffer,
                      size_t count, size_t *read_count)
{
    *read_count = 0;
    if (n->kind == FS_DIRECTORY) return FS_IS_DIRECTORY;
    if (n->kind != FS_REGULAR) return FS_INVALID;
    if (offset >= n->size) return FS_OK;
    if (count > n->size - (size_t) offset) count = n->size - (size_t) offset;
    copy_bytes(buffer, n->data + offset, count); *read_count = count;
    if (count) n->accessed_ns = clock_realtime_ns();
    return FS_OK;
}

static enum fs_error reserve(struct fs_node *n, size_t needed)
{
    if (needed <= n->capacity) return FS_OK;
    if (needed > FS_FILE_LIMIT) return FS_TOO_BIG;
    size_t capacity = n->capacity ? n->capacity : 256;
    while (capacity < needed) capacity *= 2;
    if (capacity > FS_FILE_LIMIT) capacity = FS_FILE_LIMIT;
    if (capacity - n->capacity > FS_RAM_LIMIT - ram_bytes) return FS_NO_SPACE;
    unsigned char *data = kmalloc(capacity);
    if (!data) return FS_NO_MEMORY;
    copy_bytes(data, n->data, n->size);
    if (n->allocated) kfree((void *) n->data);
    ram_bytes += capacity - n->capacity;
    n->data = data; n->capacity = capacity; n->allocated = true;
    return FS_OK;
}

enum fs_error fs_resize(struct fs_node *n, uint64_t size)
{
    if (n->kind != FS_REGULAR) return n->kind == FS_DIRECTORY ? FS_IS_DIRECTORY : FS_INVALID;
    if (!n->writable) return FS_READ_ONLY;
    if (size > FS_FILE_LIMIT) return FS_TOO_BIG;
    enum fs_error e = reserve(n, (size_t) size);
    if (e != FS_OK) return e;
    if (size > n->size) zero_bytes((unsigned char *) n->data + n->size, size - n->size);
    if (!size && n->allocated) {
        ram_bytes -= n->capacity; kfree((void *) n->data);
        n->data = 0; n->capacity = 0; n->allocated = false;
    }
    n->size = size; changed(n);
    return FS_OK;
}

enum fs_error fs_write(struct fs_node *n, uint64_t offset, const void *data,
                       size_t count, size_t *written)
{
    *written = 0;
    if (n->kind != FS_REGULAR) return n->kind == FS_DIRECTORY ? FS_IS_DIRECTORY : FS_INVALID;
    if (!n->writable) return FS_READ_ONLY;
    if (offset > FS_FILE_LIMIT || count > FS_FILE_LIMIT - offset) return FS_TOO_BIG;
    if (!count) return FS_OK;
    enum fs_error e = reserve(n, (size_t) offset + count);
    if (e != FS_OK) return e;
    if (offset > n->size) zero_bytes((unsigned char *) n->data + n->size, offset - n->size);
    copy_bytes((unsigned char *) n->data + offset, data, count);
    if (offset + count > n->size) n->size = offset + count;
    changed(n); *written = count;
    return FS_OK;
}

enum fs_error fs_readlink(struct fs_node *n, char *out, size_t size, size_t *count)
{
    if (n->kind != FS_SYMLINK) return FS_INVALID;
    if (size > n->size) size = n->size;
    copy_bytes(out, n->data, size); *count = size;
    return FS_OK;
}

enum fs_error fs_directory(struct fs_node *n, uint64_t index, char *name,
                           struct fs_info *info)
{
    if (n->kind != FS_DIRECTORY) return FS_NOT_DIRECTORY;
    struct fs_node *entry = n;
    if (index < 2) {
        copy_text(name, FS_NAME_MAX + 1, index ? ".." : ".");
        if (index && n->parent) entry = n->parent;
    } else {
        entry = n->children; index -= 2;
        while (entry && index) { --index; entry = entry->next; }
        if (!entry) return FS_NOT_FOUND;
        copy_text(name, FS_NAME_MAX + 1, entry->name);
    }
    fs_info(entry, info); return FS_OK;
}

enum fs_error fs_chmod(struct fs_node *n, unsigned mode)
{
    if (!n->writable) return FS_READ_ONLY;
    n->mode = mode & 07777; n->changed_ns = clock_realtime_ns(); return FS_OK;
}
enum fs_error fs_set_times(struct fs_node *n, uint64_t accessed, uint64_t modified)
{
    if (!n->writable) return FS_READ_ONLY;
    n->accessed_ns = accessed; n->modified_ns = modified;
    n->changed_ns = clock_realtime_ns(); return FS_OK;
}

static bool hex_field(const unsigned char *p, uint32_t *value)
{
    *value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned c = p[i];
        unsigned digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (digit == 16) return false;
        *value = *value * 16 + digit;
    }
    return true;
}

static struct fs_node *seed_parent(const char *path, char *name)
{
    struct fs_node *parent = root_node;
    while (*path == '/') ++path;
    while (*path) {
        size_t size = 0;
        while (path[size] && path[size] != '/') ++size;
        if (size > FS_NAME_MAX) panic("runtime archive component too long");
        copy_bytes(name, path, size); name[size] = 0;
        path += size;
        while (*path == '/') ++path;
        if (!*path) break;
        if (text_equal(name, ".")) continue;
        if (text_equal(name, "..")) panic("runtime archive parent traversal");
        struct fs_node *next = child(parent, name);
        if (!next) next = new_node(parent, name, FS_DIRECTORY, 0755, false);
        if (!next || next->kind != FS_DIRECTORY) panic("runtime archive invalid directory");
        parent = next;
    }
    return parent;
}

static void seed_archive(const unsigned char *archive, size_t length)
{
    size_t at = 0;
    while (at <= length && length - at >= 110) {
        const unsigned char *h = archive + at;
        if (h[0] != '0' || h[1] != '7' || h[2] != '0' || h[3] != '7' || h[4] != '0' || h[5] != '1')
            panic("runtime archive needs newc format");
        uint32_t mode, size, namesize, seconds;
        if (!hex_field(h + 14, &mode) || !hex_field(h + 54, &size)
            || !hex_field(h + 94, &namesize) || !hex_field(h + 46, &seconds)
            || !namesize || namesize > FS_PATH_MAX || namesize > length - at - 110)
            panic("invalid runtime archive header");
        const char *path = (const char *) h + 110;
        if (path[namesize - 1] || text_length(path) != namesize - 1)
            panic("invalid runtime archive name");
        size_t begin = (at + 110 + namesize + 3) & ~(size_t) 3;
        if (begin > length || size > length - begin) panic("truncated runtime archive");
        if (text_equal(path, "TRAILER!!!")) { if (size) panic("invalid archive trailer"); return; }
        enum fs_kind kind;
        switch (mode & 0170000) {
        case 0100000: kind = FS_REGULAR; break;
        case 0040000: kind = FS_DIRECTORY; break;
        case 0120000: kind = FS_SYMLINK; break;
        default: panic("unsupported runtime archive entry");
        }
        if (kind == FS_SYMLINK && (!size || size >= FS_PATH_MAX))
            panic("invalid runtime archive symlink");
        if (!text_equal(path, ".") && !text_equal(path, "/")) {
            char name[FS_NAME_MAX + 1];
            struct fs_node *parent = seed_parent(path, name);
            if (text_equal(name, "..") || !*name) panic("invalid runtime archive leaf");
            struct fs_node *n = child(parent, name);
            if (n && (kind != FS_DIRECTORY || n->kind != FS_DIRECTORY))
                panic("duplicate runtime archive entry");
            if (!n) n = new_node(parent, name, kind, mode, false);
            if (!n) panic("runtime archive exceeds node budget");
            n->data = archive + begin; n->size = size;
            n->accessed_ns = n->modified_ns = n->changed_ns = (uint64_t) seconds * 1000000000;
            n->mode = mode & 07777;
        }
        at = (begin + size + 3) & ~(size_t) 3;
    }
    panic("runtime archive missing trailer");
}

static struct fs_node *seed_directory(const char *name, bool writable)
{
    struct fs_node *n = child(root_node, name);
    if (!n) n = new_node(root_node, name, FS_DIRECTORY, 0755, writable);
    if (!n || n->kind != FS_DIRECTORY) panic("invalid initial directory");
    n->writable = writable;
    return n;
}

void fs_init(const void *archive, size_t length)
{
    root_node = 0; node_count = ram_bytes = next_inode = 0;
    root_node = new_node(0, "", FS_DIRECTORY, 0755, false);
    if (!root_node) panic("cannot allocate root directory");
    if (archive) seed_archive(archive, length);
    seed_directory("work", true); seed_directory("tmp", true);
    struct fs_node *home = seed_directory("home", true);
    if (!new_node(home, "user", FS_DIRECTORY, 0755, true)) panic("cannot allocate user directory");
    struct fs_node *dev = seed_directory("dev", false);
    if (!new_node(dev, "tty", FS_CONSOLE, 0666, false)
        || !new_node(dev, "null", FS_NULL, 0666, false)) panic("cannot allocate device entries");
}

#include "files.h"
#include "littlefs_backend.h"
#include "block.h"

extern void *kmalloc(size_t);
extern void kfree(void *);
extern uint64_t clock_realtime_ns(void);
extern _Noreturn void panic(const char *);

struct fs_node {
    struct fs_node *parent, *children, *next;
    const unsigned char *data;
    size_t size;
    uint64_t inode, accessed_ns, modified_ns, changed_ns;
    unsigned mode, references;
    enum fs_kind kind;
    bool writable, linked, persistent, metadata_dirty;
    uint64_t object_id;
    struct littlefs_handle *handle;
    struct fs_node *data_next;
    enum fs_error pending_error;
    char name[FS_NAME_MAX + 1];
};

static struct fs_node *root_node;
static size_t node_count;
static uint64_t next_inode;
static struct fs_node *data_nodes, *deferred;
static bool data_mounted, data_frozen, data_failed;
static uint64_t data_generation, data_boot;
static enum fs_error data_error;

static enum fs_error persistent_result(int);
static enum fs_error finish_transient(struct fs_node *, enum fs_error);
static enum fs_error close_handle(struct fs_node *, enum fs_error);
static enum fs_error destroy_if_unused(struct fs_node *);
static struct littlefs_metadata metadata(struct fs_node *);
static enum fs_error data_path(struct fs_node *, char *);
static enum fs_error ensure_handle(struct fs_node *);
static enum fs_error write_allowed(struct fs_node *n)
{
    return !n->persistent ? FS_READ_ONLY
           : data_frozen  ? FS_BUSY
           : data_failed  ? FS_READ_ONLY
                          : FS_OK;
}
static void data_changed(struct fs_node *n)
{
    if (n->persistent) {
        ++data_generation;
        n->metadata_dirty = true;
    }
}

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
{
    n->modified_ns = n->changed_ns = clock_realtime_ns();
    data_changed(n);
}

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

static enum fs_error destroy_if_unused(struct fs_node *n)
{
    if (n->linked || n->references)
        return FS_OK;
    if (n->persistent && data_frozen) {
        n->next = deferred;
        deferred = n;
        return FS_OK;
    }
    enum fs_error result = FS_OK;
    if (n->persistent) {
        if (n->handle) {
            result = persistent_result(littlefs_backend_close(n->handle));
            n->handle = 0;
        }
        if (n->kind != FS_DIRECTORY && !data_failed) {
            ++data_generation;
            enum fs_error e = persistent_result(littlefs_backend_delete_object(n->object_id));
            if (result == FS_OK)
                result = e;
        }
        struct fs_node **p = &data_nodes;
        while (*p && *p != n)
            p = &(*p)->data_next;
        if (*p)
            *p = n->data_next;
    }
    --node_count;
    kfree(n);
    return result;
}

void fs_retain(struct fs_node *n) { ++n->references; }
enum fs_error fs_release_checked(struct fs_node *n)
{
    if (!n || !n->references)
        return FS_OK;
    enum fs_error result = FS_OK;
    if (--n->references == 0 && n->persistent && n->handle) {
        result = fs_sync_node(n);
        result = close_handle(n, result);
    }
    enum fs_error removed = destroy_if_unused(n);
    return result == FS_OK ? removed : result;
}
void fs_release(struct fs_node *n)
{
    (void)fs_release_checked(n);
}
struct fs_node *fs_root(void) { return root_node; }

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
            size_t read_count;
            enum fs_error e = fs_readlink(found, expanded, found->size, &read_count);
            if (e != FS_OK)
                return e;
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
    if (!parent->writable || !parent->persistent)
        return FS_READ_ONLY;
    if ((error = write_allowed(parent)) != FS_OK)
        return error;
    if (kind != FS_REGULAR && kind != FS_DIRECTORY && kind != FS_SYMLINK) return FS_INVALID;
    size_t target_size = target ? text_length(target) : 0;
    if (kind == FS_SYMLINK && (!target_size || target_size >= FS_PATH_MAX)) return FS_NAME_TOO_LONG;
    n = new_node(parent, name, kind, mode, true);
    if (!n) return FS_NO_MEMORY;
    n->persistent = true;
    n->size = kind == FS_SYMLINK ? target_size : 0;
    char full[FS_PATH_MAX];
    error = data_path(n, full);
    struct littlefs_metadata m = metadata(n);
    ++data_generation;
    if (error == FS_OK)
        error = persistent_result(littlefs_backend_create(full, &m, target));
    if (error != FS_OK) {
        n->persistent = false;
        parent->children = n->next;
        n->linked = false;
        (void)destroy_if_unused(n);
        return error;
    }
    n->object_id = m.id;
    n->inode = (1ULL << 63) | m.id;
    n->data_next = data_nodes;
    data_nodes = n;

    changed(parent);
    *out = n;
    return fs_sync_node(parent);
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
        char target[FS_PATH_MAX];
        size_t count;
        e = fs_readlink(n, target, n->size, &count);
        if (e != FS_OK)
            return e;
        target[n->size] = 0;
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
    if (n->persistent && n->parent == root_node)
        return FS_BUSY;
    if (!parent->writable || !n->writable) return FS_READ_ONLY;
    if ((error = write_allowed(n)) != FS_OK)
        return error;
    if (directory != (n->kind == FS_DIRECTORY)) return directory ? FS_NOT_DIRECTORY : FS_IS_DIRECTORY;
    if (n->kind == FS_DIRECTORY && n->children) return FS_NOT_EMPTY;
    if (n->kind == FS_DIRECTORY && n->references) return FS_BUSY;
    if (n->persistent) {
        char full[FS_PATH_MAX];
        error = data_path(n, full);
        ++data_generation;
        if (error == FS_OK)
            error = persistent_result(littlefs_backend_remove(full, n->kind));
        if (error != FS_OK)
            return error;
    }
    detach(n);
    error = destroy_if_unused(n);
    if (error == FS_OK && parent->persistent)
        error = fs_sync_node(parent);
    return error;
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
    if ((source->persistent && source->parent == root_node) ||
        (dest && dest->persistent && dest->parent == root_node))
        return FS_BUSY;
    if (source->persistent != new_parent->persistent)
        return FS_CROSS_DEVICE;
    if (!old_parent->writable || !new_parent->writable || !source->writable) return FS_READ_ONLY;
    if ((e = write_allowed(source)) != FS_OK)
        return e;
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
    }
    if (source->persistent) {
        char a[FS_PATH_MAX], b[FS_PATH_MAX], parent_path[FS_PATH_MAX];
        e = data_path(source, a);
        if (e != FS_OK)
            return e;
        e = data_path(new_parent, parent_path);
        if (e != FS_OK)
            return e;
        size_t length = text_length(parent_path), leaf = text_length(new_name);
        if (length + 1 + leaf >= sizeof b)
            return FS_NAME_TOO_LONG;
        copy_bytes(b, parent_path, length);
        b[length] = '/';
        copy_bytes(b + length + 1, new_name, leaf + 1);
        ++data_generation;
        e = persistent_result(littlefs_backend_rename(a, b));
        if (e != FS_OK)
            return e;
    }
    if (dest) {
        detach(dest);
    }
    detach(source);
    source->parent = new_parent;
    copy_text(source->name, sizeof source->name, new_name);
    source->next = new_parent->children; new_parent->children = source;
    source->linked = true;
    source->changed_ns = clock_realtime_ns();
    data_changed(source);
    changed(new_parent);
    enum fs_error result = dest ? destroy_if_unused(dest) : FS_OK;
    if (source->persistent) {
        e = fs_sync_node(source);
        if (result == FS_OK)
            result = e;
        e = fs_sync_node(old_parent);
        if (result == FS_OK)
            result = e;
        e = fs_sync_node(new_parent);
        if (result == FS_OK)
            result = e;
    }
    return result;
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
    if (n->persistent) {
        enum fs_error e = ensure_handle(n);
        if (e != FS_OK)
            return e;
        int read = littlefs_backend_read(n->handle, offset, buffer, count);
        if (read < 0)
            return finish_transient(n, persistent_result(read));
        *read_count = (size_t)read;
        if (read && !data_frozen) {
            n->accessed_ns = clock_realtime_ns();
            data_changed(n);
        }
        return finish_transient(n, FS_OK);
    }
    copy_bytes(buffer, n->data + offset, count); *read_count = count;
    if (count) n->accessed_ns = clock_realtime_ns();
    return FS_OK;
}

enum fs_error fs_resize(struct fs_node *n, uint64_t size)
{
    if (n->kind != FS_REGULAR) return n->kind == FS_DIRECTORY ? FS_IS_DIRECTORY : FS_INVALID;
    if (!n->writable || !n->persistent)
        return FS_READ_ONLY;
    if (size > FS_FILE_LIMIT) return FS_TOO_BIG;
    enum fs_error allowed = write_allowed(n);
    if (allowed != FS_OK)
        return allowed;
    enum fs_error e = ensure_handle(n);
    if (e != FS_OK)
        return e;
    ++data_generation;
    e = persistent_result(littlefs_backend_resize(n->handle, size));
    if (e == FS_OK) {
        n->size = (size_t)size;
        changed(n);
    } else
        n->pending_error = e;
    return finish_transient(n, e);
}

enum fs_error fs_write(struct fs_node *n, uint64_t offset, const void *data,
                       size_t count, size_t *written)
{
    *written = 0;
    if (n->kind != FS_REGULAR) return n->kind == FS_DIRECTORY ? FS_IS_DIRECTORY : FS_INVALID;
    if (!n->writable || !n->persistent)
        return FS_READ_ONLY;
    if (offset > FS_FILE_LIMIT || count > FS_FILE_LIMIT - offset) return FS_TOO_BIG;
    if (!count) return FS_OK;
    enum fs_error allowed = write_allowed(n);
    if (allowed != FS_OK)
        return allowed;
    enum fs_error e = ensure_handle(n);
    if (e != FS_OK)
        return e;
    ++data_generation;
    int nwrite = littlefs_backend_write(n->handle, offset, data, count);
    if (nwrite < 0) {
        e = persistent_result(nwrite);
        n->pending_error = e;
    } else {
        *written = (size_t)nwrite;
        if (offset + (uint64_t)nwrite > n->size)
            n->size = (size_t)(offset + nwrite);
        changed(n);
    }
    return finish_transient(n, e);
}

enum fs_error fs_readlink(struct fs_node *n, char *out, size_t size, size_t *count)
{
    if (n->kind != FS_SYMLINK) return FS_INVALID;
    if (size > n->size) size = n->size;
    if (n->persistent) {
        enum fs_error e = ensure_handle(n);
        if (e != FS_OK)
            return e;
        int nread = littlefs_backend_read(n->handle, 0, out, size);
        if (nread < 0)
            return finish_transient(n, persistent_result(nread));
        *count = (size_t)nread;
        return finish_transient(n, FS_OK);
    }
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
    if (!n->writable || !n->persistent)
        return FS_READ_ONLY;
    enum fs_error e = write_allowed(n);
    if (e != FS_OK)
        return e;
    n->mode = mode & 07777;
    n->changed_ns = clock_realtime_ns();
    data_changed(n);
    return finish_transient(n, fs_sync_node(n));
}
enum fs_error fs_set_times(struct fs_node *n, uint64_t accessed, uint64_t modified)
{
    if (!n->writable || !n->persistent)
        return FS_READ_ONLY;
    enum fs_error e = write_allowed(n);
    if (e != FS_OK)
        return e;
    n->accessed_ns = accessed; n->modified_ns = modified;
    n->changed_ns = clock_realtime_ns();
    data_changed(n);
    return finish_transient(n, fs_sync_node(n));
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
    root_node = 0;
    node_count = next_inode = 0;
    data_nodes = deferred = 0;
    data_mounted = data_frozen = data_failed = false;
    data_generation = data_boot = 0;
    data_error = FS_OK;
    root_node = new_node(0, "", FS_DIRECTORY, 0755, false);
    if (!root_node) panic("cannot allocate root directory");
    if (length) {
        if (!archive) panic("missing runtime archive data");
        seed_archive(archive, length);
    }
    seed_directory("work", false);
    seed_directory("tmp", false);
    struct fs_node *home = seed_directory("home", false);
    if (!new_node(home, "user", FS_DIRECTORY, 0755, false))
        panic("cannot allocate user directory");
    struct fs_node *dev = seed_directory("dev", false);
    if (!new_node(dev, "tty", FS_CONSOLE, 0666, false)
        || !new_node(dev, "null", FS_NULL, 0666, false)) panic("cannot allocate device entries");
}

long fs_error_errno(enum fs_error e)
{
    static const long values[] = {0,   -2,  -20, -21, -17, -30,  -12, -28, -22, -36,
                                  -40, -39, -16, -27, -5,  -110, -84, -19, -95, -18};
    return (unsigned)e < sizeof values / sizeof values[0] ? values[e] : -5;
}
static enum fs_error persistent_result(int result)
{
    if (result >= 0)
        return FS_OK;
    enum fs_error e = FS_IO;
    for (unsigned i = 0; i <= FS_CROSS_DEVICE; i++)
        if (fs_error_errno((enum fs_error)i) == result) {
            e = (enum fs_error)i;
            break;
        }
    /* Keep the first fatal device/format fault observable after recovery starts. */
    if (!data_failed) {
        data_error = e;
        if (e == FS_IO || e == FS_TIMEOUT || e == FS_CORRUPT)
            data_failed = true;
    }
    return e;
}
static struct littlefs_metadata metadata(struct fs_node *n)
{
    return (struct littlefs_metadata){.id = n->object_id,
                                      .size = n->size,
                                      .kind = n->kind,
                                      .mode = n->mode,
                                      .accessed_ns = n->accessed_ns,
                                      .modified_ns = n->modified_ns,
                                      .changed_ns = n->changed_ns};
}
static void import_metadata(struct fs_node *n, const struct littlefs_metadata *m)
{
    n->persistent = true;
    n->object_id = m->id;
    n->inode = (1ULL << 63) | m->id;
    n->size = (size_t)m->size;
    n->kind = m->kind;
    n->mode = m->mode;
    n->accessed_ns = m->accessed_ns;
    n->modified_ns = m->modified_ns;
    n->changed_ns = m->changed_ns;
}
static enum fs_error close_handle(struct fs_node *n, enum fs_error result)
{
    enum fs_error closed = persistent_result(littlefs_backend_close(n->handle));
    n->handle = 0;
    n->pending_error = FS_OK;
    if (result == FS_OK)
        result = closed;
    if (result != FS_OK) {
        /* littlefs discards errored write caches on close. Restore the committed
         * view before another description opens this same in-memory inode. */
        struct littlefs_metadata m;
        int loaded = littlefs_backend_object_metadata(n->object_id, &m);
        if (!loaded) {
            import_metadata(n, &m);
            n->metadata_dirty = false;
        } else
            (void)persistent_result(loaded);
    }
    return result;
}
static enum fs_error data_path(struct fs_node *n, char *out)
{
    char path[FS_PATH_MAX];
    enum fs_error e = fs_path(n, path, sizeof path);
    if (e != FS_OK)
        return e;
    struct fs_node *anchor = n;
    while (anchor->parent && anchor->parent != root_node)
        anchor = anchor->parent;
    if (!anchor->persistent || anchor->parent != root_node ||
        (!text_equal(anchor->name, "work") && !text_equal(anchor->name, "home") &&
         !text_equal(anchor->name, "tmp")))
        return FS_INVALID;
    copy_bytes(out, path, text_length(path) + 1);
    return FS_OK;
}
static enum fs_error ensure_handle(struct fs_node *n)
{
    if (n->handle)
        return n->pending_error;
    n->pending_error = FS_OK;
    return persistent_result(littlefs_backend_open(n->object_id, &n->handle));
}
enum fs_error fs_sync_node(struct fs_node *n)
{
    if (!n || !n->persistent)
        return FS_OK;
    if (n->pending_error != FS_OK)
        return n->pending_error;
    if (data_failed)
        return data_error;
    if (!n->metadata_dirty)
        return persistent_result(littlefs_backend_flush());
    struct littlefs_metadata m = metadata(n);
    enum fs_error e;
    if (n->kind == FS_DIRECTORY) {
        char path[FS_PATH_MAX];
        e = data_path(n, path);
        if (e == FS_OK)
            e = persistent_result(littlefs_backend_metadata(path, &m));
    } else {
        e = ensure_handle(n);
        if (e == FS_OK)
            e = persistent_result(littlefs_backend_sync(n->handle, &m));
    }
    if (e == FS_OK)
        e = persistent_result(littlefs_backend_flush());
    if (e == FS_OK)
        n->metadata_dirty = false;
    return e;
}
static enum fs_error finish_transient(struct fs_node *n, enum fs_error result)
{
    if (n->references || !n->handle)
        return result;
    enum fs_error e = fs_sync_node(n);
    if (result == FS_OK)
        result = e;
    return close_handle(n, result);
}
enum fs_error fs_sync_all(void)
{
    if (!data_mounted)
        return data_error == FS_NO_DEVICE ? FS_NO_DEVICE : data_error;
    if (data_failed)
        return data_error;
    for (struct fs_node *n = data_nodes; n; n = n->data_next) {
        enum fs_error e = fs_sync_node(n);
        e = finish_transient(n, e);
        if (e != FS_OK)
            return e;
    }
    return persistent_result(littlefs_backend_flush());
}
enum fs_error fs_freeze_data(void)
{
    if (!data_mounted)
        return data_error;
    if (data_frozen)
        return FS_OK;
    data_frozen = true;
    enum fs_error e = fs_sync_all();
    if (e != FS_OK)
        data_frozen = false;
    return e;
}
void fs_thaw_data(void)
{
    data_frozen = false;
    while (deferred) {
        struct fs_node *n = deferred;
        deferred = n->next;
        n->next = 0;
        (void)destroy_if_unused(n);
    }
}
bool fs_data_mounted(void)
{
    return data_mounted;
}
bool fs_data_frozen(void)
{
    return data_frozen;
}
uint64_t fs_data_generation(void)
{
    return data_generation;
}
uint64_t fs_data_boot_id(void)
{
    return data_boot;
}
enum fs_error fs_data_last_error(void)
{
    return data_error;
}
enum fs_error fs_data_health_error(void)
{
    return data_mounted && data_failed ? data_error : FS_OK;
}

struct mount_context {
    struct fs_node **queue, *parent;
    uint64_t *ids;
    size_t count;
    unsigned anchors;
};
static int mount_entry(void *opaque, const char *name, const struct littlefs_metadata *m)
{
    struct mount_context *c = opaque;
    if (c->count == FS_NODE_LIMIT)
        return -28;
    for (size_t i = 0; i < c->count; i++)
        if (c->ids[i] == m->id)
            return -84;
    if (!c->parent) {
        unsigned bit = text_equal(name, "work")   ? 1
                       : text_equal(name, "home") ? 2
                       : text_equal(name, "tmp")  ? 4
                                                  : 0;
        if (!bit || (c->anchors & bit) || m->kind != FS_DIRECTORY)
            return -84;
        c->anchors |= bit;
    }
    /* Top-level candidates remain detached until the entire volume validates. */
    struct fs_node *n = new_node(c->parent, name, m->kind, m->mode, true);
    if (!n)
        return -12;
    if (!c->parent)
        n->parent = root_node;
    import_metadata(n, m);
    n->metadata_dirty = false;
    n->data_next = data_nodes;
    data_nodes = n;
    c->queue[c->count] = n;
    c->ids[c->count++] = m->id;
    return 0;
}
static bool unused_tree(struct fs_node *n)
{
    if (n->references)
        return false;
    for (struct fs_node *p = n->children; p; p = p->next)
        if (!unused_tree(p))
            return false;
    return true;
}
static void discard_readonly_tree(struct fs_node *n)
{
    while (n->children) {
        struct fs_node *p = n->children;
        n->children = p->next;
        discard_readonly_tree(p);
    }
    n->linked = false;
    (void)destroy_if_unused(n);
}
enum fs_error fs_mount_data(void)
{
    if (data_mounted)
        return FS_BUSY;
    const char *names[] = {"work", "home", "tmp"};
    for (unsigned i = 0; i < 3; i++) {
        struct fs_node *anchor = child(root_node, names[i]);
        if (!anchor || !unused_tree(anchor))
            return FS_BUSY;
    }
    int result;
    if (data_disk_sectors() != 131072) {
        unsigned char sector[512];
        result = data_disk_read(0, sector, 1);
        if (!result)
            result = -95;
        return persistent_result(result);
    }
    struct fs_node **queue = kmalloc(FS_NODE_LIMIT * sizeof *queue);
    uint64_t *ids = kmalloc(FS_NODE_LIMIT * sizeof *ids);
    if (!queue || !ids) {
        kfree(queue);
        kfree(ids);
        data_error = FS_NO_MEMORY;
        return data_error;
    }
    struct mount_context context = {.queue = queue, .ids = ids, .count = 1};
    queue[0] = NULL;
    ids[0] = 1;
    uint64_t boot = 0;
    result = littlefs_backend_mount(&boot);
    if (!result)
        result = littlefs_backend_list("", mount_entry, &context);
    if (!result && context.anchors != 7)
        result = -84;
    for (size_t at = 1; !result && at < context.count; at++) {
        if (queue[at]->kind != FS_DIRECTORY)
            continue;
        context.parent = queue[at];
        char path[FS_PATH_MAX];
        enum fs_error e = data_path(queue[at], path);
        result = e == FS_OK ? littlefs_backend_list(path, mount_entry, &context)
                            : (int)fs_error_errno(e);
    }
    bool collected = false;
    if (!result)
        result = littlefs_backend_collect(ids, context.count, &collected);
    if (!result)
        result = littlefs_backend_accept(&boot);
    if (result) {
        for (size_t i = context.count; i > 1; i--) {
            struct fs_node *n = queue[i - 1];
            n->persistent = false;
            n->linked = false;
            (void)destroy_if_unused(n);
        }
        data_nodes = 0;
        littlefs_backend_unmount();
        data_error = persistent_result(result);
    } else {
        for (size_t i = 1; i < context.count; i++) {
            struct fs_node *n = queue[i];
            if (n->parent != root_node)
                continue;
            struct fs_node **p = &root_node->children;
            while (*p && !text_equal((*p)->name, n->name))
                p = &(*p)->next;
            struct fs_node *old = *p;
            n->next = old->next;
            *p = n;
            discard_readonly_tree(old);
        }
        data_mounted = true;
        data_failed = false;
        data_boot = boot;
        data_generation = 1;
        data_error = FS_OK;
    }
    kfree(queue);
    kfree(ids);
    return data_error;
}

#ifndef EMACSOS_LITTLEFS_BACKEND_H
#define EMACSOS_LITTLEFS_BACKEND_H
#include "files.h"
#define EMACSOS_DATA_FORMAT "EOSDATA3"
struct littlefs_metadata {
    uint64_t id, size, accessed_ns, modified_ns, changed_ns;
    unsigned mode;
    enum fs_kind kind;
};
struct littlefs_handle;
/* Guest absolute paths live under /tree; fixed DATA anchors are projected. */
int littlefs_backend_object_metadata(uint64_t, struct littlefs_metadata *);
int littlefs_backend_root_metadata(struct littlefs_metadata *);
int littlefs_backend_mount(uint64_t *boot_id);
int littlefs_backend_accept(uint64_t *);
void littlefs_backend_unmount(void);
int littlefs_backend_list(const char *,
                          int (*)(void *, const char *, const struct littlefs_metadata *), void *);
int littlefs_backend_create(const char *, struct littlefs_metadata *, const char *);
int littlefs_backend_remove(const char *, enum fs_kind);
int littlefs_backend_rename(const char *, const char *);
int littlefs_backend_open(uint64_t, struct littlefs_handle **);
int littlefs_backend_read(struct littlefs_handle *, uint64_t, void *, size_t);
int littlefs_backend_write(struct littlefs_handle *, uint64_t, const void *, size_t);
int littlefs_backend_resize(struct littlefs_handle *, uint64_t);
int littlefs_backend_sync(struct littlefs_handle *, const struct littlefs_metadata *);
int littlefs_backend_close(struct littlefs_handle *);
int littlefs_backend_metadata(const char *, const struct littlefs_metadata *);
int littlefs_backend_collect(const uint64_t *, size_t, bool *);
int littlefs_backend_delete_object(uint64_t);
int littlefs_backend_flush(void);
#ifdef EMACSOS_HOST_TEST
/* Offline image creation only. The running kernel never formats a volume. */
int littlefs_backend_format(void);
#endif
#endif

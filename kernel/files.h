#ifndef EMACSOS_FILES_H
#define EMACSOS_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FS_PATH_MAX 1024
#define FS_NAME_MAX 255
#define FS_NODE_LIMIT 8192
#define FS_RAM_LIMIT (24UL * 1024 * 1024)
#define FS_FILE_LIMIT (8UL * 1024 * 1024)

enum fs_error {
    FS_OK,
    FS_NOT_FOUND,
    FS_NOT_DIRECTORY,
    FS_IS_DIRECTORY,
    FS_EXISTS,
    FS_READ_ONLY,
    FS_NO_MEMORY,
    FS_NO_SPACE,
    FS_INVALID,
    FS_NAME_TOO_LONG,
    FS_LOOP,
    FS_NOT_EMPTY,
    FS_BUSY,
    FS_TOO_BIG,
    FS_IO,
    FS_TIMEOUT,
    FS_CORRUPT,
    FS_NO_DEVICE,
    FS_NOT_SUPPORTED,
    FS_CROSS_DEVICE
};
enum fs_kind { FS_REGULAR, FS_DIRECTORY, FS_SYMLINK, FS_CONSOLE, FS_NULL };
struct fs_node;
struct fs_info {
    uint64_t inode, size, accessed_ns, modified_ns, changed_ns;
    unsigned mode, links;
    enum fs_kind kind;
    bool writable;
};

void fs_init(const void *, size_t);
struct fs_node *fs_root(void);
enum fs_error fs_lookup(struct fs_node *, const char *, bool, struct fs_node **);
enum fs_error fs_create(struct fs_node *, const char *, enum fs_kind,
                        const char *, unsigned, bool, struct fs_node **);
enum fs_error fs_create_regular(struct fs_node *, const char *, unsigned, struct fs_node **);
enum fs_error fs_remove(struct fs_node *, const char *, bool);
enum fs_error fs_rename(struct fs_node *, const char *, struct fs_node *,
                        const char *, bool);
void fs_retain(struct fs_node *);
void fs_release(struct fs_node *);
enum fs_error fs_release_checked(struct fs_node *);
void fs_info(const struct fs_node *, struct fs_info *);
enum fs_error fs_path(struct fs_node *, char *, size_t);
enum fs_error fs_read(struct fs_node *, uint64_t, void *, size_t, size_t *);
enum fs_error fs_write(struct fs_node *, uint64_t, const void *, size_t, size_t *);
enum fs_error fs_resize(struct fs_node *, uint64_t);
enum fs_error fs_readlink(struct fs_node *, char *, size_t, size_t *);
enum fs_error fs_directory(struct fs_node *, uint64_t, char *, struct fs_info *);
enum fs_error fs_chmod(struct fs_node *, unsigned);
enum fs_error fs_set_times(struct fs_node *, uint64_t, uint64_t);
size_t fs_ram_used(void);
/* DATA mount validates existing bytes; only offline tooling may format. */
enum fs_error fs_mount_data(void);
enum fs_error fs_sync_node(struct fs_node *);
enum fs_error fs_sync_all(void);
enum fs_error fs_freeze_data(void);
void fs_thaw_data(void);
bool fs_data_mounted(void);
bool fs_data_frozen(void);
uint64_t fs_data_generation(void);
uint64_t fs_data_boot_id(void);
enum fs_error fs_data_last_error(void);
/* Mounted media faults are latched; ordinary path/capacity errors are not health faults. */
enum fs_error fs_data_health_error(void);
long fs_error_errno(enum fs_error);

bool abi_files(unsigned long, const unsigned long[6], long *);
long file_map_read(int, uint64_t, void *, size_t);
int file_fd_kind(int);
long file_ready(int, bool);

#endif

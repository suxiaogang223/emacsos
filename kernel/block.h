#ifndef EMACSOS_BLOCK_H
#define EMACSOS_BLOCK_H
#include <stddef.h>
#include <stdint.h>
#define DATA_DISK_SECTOR_SIZE 512U
#define DATA_DISK_SECTORS 131072UL
/* DATA is the fixed 64 MiB primary slave. Errors are negative Linux errno. */
int data_disk_init(void);
int data_disk_read(uint64_t, void *, size_t);
int data_disk_write(uint64_t, const void *, size_t);
int data_disk_flush(void);
uint64_t data_disk_sectors(void);
#endif

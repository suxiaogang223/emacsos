/* Build-time formatter only: the guest mount path never formats a disk. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "littlefs_backend.h"
#include "block.h"

#define DATA_BYTES (DATA_DISK_SECTORS * DATA_DISK_SECTOR_SIZE)
static FILE *disk;

void *kmalloc(size_t size) { return malloc(size); }
void kfree(void *memory) { free(memory); }
uint64_t clock_realtime_ns(void) { return 0; }
int data_disk_init(void) { return 0; }
uint64_t data_disk_sectors(void) { return DATA_BYTES / 512; }
int data_disk_read(uint64_t sector, void *buffer, size_t count)
{
    if (sector > DATA_BYTES / 512 || count > DATA_BYTES / 512 - sector) return -22;
    if (fseek(disk, (long)(sector * 512), SEEK_SET)
        || fread(buffer, 512, count, disk) != count) return -5;
    return 0;
}
int data_disk_write(uint64_t sector, const void *buffer, size_t count)
{
    if (sector > DATA_BYTES / 512 || count > DATA_BYTES / 512 - sector) return -22;
    if (fseek(disk, (long)(sector * 512), SEEK_SET)
        || fwrite(buffer, 512, count, disk) != count) return -5;
    return 0;
}
int data_disk_flush(void)
{
    return fflush(disk) || fsync(fileno(disk)) ? -5 : 0;
}
int main(int argc, char **argv)
{
    if (argc != 2) { fputs("Usage: data-image OUTPUT\n", stderr); return 2; }
    disk = fopen(argv[1], "wbx+");
    if (!disk) { perror("Create new DATA image"); return 1; }
    int failed = ftruncate(fileno(disk), DATA_BYTES) != 0;
    if (!failed) failed = littlefs_backend_format() != 0;
    if (!failed) failed = data_disk_flush() != 0;
    if (fclose(disk)) failed = 1;
    if (failed) { unlink(argv[1]); fputs("DATA formatting failed\n", stderr); return 1; }
    puts("Initialized 64 MiB littlefs DATA template");
    return 0;
}

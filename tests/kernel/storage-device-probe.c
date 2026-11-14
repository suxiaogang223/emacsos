/* Native-QEMU device/control probe, independent of littlefs and Emacs. */
#include "kernel.h"
#include "block.h"
#include "files.h"
#include "storage_control.h"
static bool frozen;
static uint64_t generation = 7;
bool fs_data_mounted(void) { return data_disk_sectors() != 0; }
bool fs_data_frozen(void) { return frozen; }
uint64_t fs_data_generation(void) { return generation; }
enum fs_error fs_data_last_error(void) { return FS_NOT_FOUND; }
long fs_error_errno(enum fs_error error) { return error ? -5 : 0; }
enum fs_error fs_freeze_data(void) {
  if (data_disk_flush())
    return FS_INVALID;
  frozen = true;
  return FS_OK;
}
void fs_thaw_data(void) { frozen = false; }
static void require(bool condition, const char *message) {
  if (!condition)
    panic(message);
}
_Noreturn void kernel_main(void) {
  console_init();
  hardware_init();
  storage_control_init();
  int error = data_disk_init();
#ifdef PROBE_NO_DATA
  require(error == -19 && !data_disk_sectors(), "absent DATA classification");
  console_puts("DATA-ABSENT-PASS\r\n");
#elif defined(PROBE_WRONG_SIZE)
  require(error == -95 && !data_disk_sectors(), "unsupported DATA capacity");
  console_puts("DATA-SIZE-PASS\r\n");
#else
  require(!error && data_disk_sectors() == DATA_DISK_SECTORS, "DATA identify");
  unsigned char source[1024], readback[1024], boot[512];
  for (unsigned i = 0; i < sizeof source; i++)
    source[i] = (i * 17 + 29) & 255;
  require(!data_disk_write(7, source, 2), "DATA multi-sector write");
  require(!data_disk_flush(), "DATA flush");
  require(!data_disk_read(7, readback, 2) &&
              !memcmp(source, readback, sizeof source), "DATA readback");
  require(!data_disk_write(DATA_DISK_SECTORS - 1, source, 1), "DATA final sector");
  require(!data_disk_flush(), "DATA final flush");
  require(data_disk_read(DATA_DISK_SECTORS, readback, 1) == -22 &&
              data_disk_write(UINT64_MAX, source, 1) == -22 &&
              data_disk_read(0, 0, 1) == -22, "DATA bounds");
  disk_read(0, boot, 1);
  require(boot[510] == 0x55 && boot[511] == 0xaa, "SYSTEM reselect after DATA");
  console_puts("DATA-DEVICE-PASS CONTROL-READY\r\n");
#endif
  for (;;)
    wait_interrupt();
}

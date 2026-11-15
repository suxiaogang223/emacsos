#include "kernel.h"
#include "block.h"
#include "files.h"
#include "storage_control.h"
_Noreturn void kernel_main(void) {
  console_init();
  console_puts("emacsOS runtime bootstrap: fixed QEMU 1 GiB, trusted shared "
               "runtime\r\n");
  memory_init(0);
  hardware_init();
  struct runtime_image image = runtime_image_load();
  fs_init(image.archive, image.archive_size);
  (void)data_disk_init();
  enum fs_error mounted = fs_mount_data();
  if (mounted == FS_OK)
    console_puts("DATA mounted at /etc, /home, /tmp and /var: 64 MiB littlefs\r\n");
  else {
    console_puts("DATA unavailable: read-only recovery, errno=");
    console_hex((uint64_t)-fs_error_errno(mounted));
    console_puts("\r\n");
  }
  storage_control_init();
  runtime_enter(&image);
}

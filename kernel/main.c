#include "kernel.h"
_Noreturn void kernel_main(void) {
  console_init();
  console_puts("emacsOS runtime bootstrap: fixed QEMU 1 GiB, trusted shared "
               "runtime\r\n");
  memory_init(0);
  hardware_init();
  struct runtime_image image = runtime_image_load();
  fs_init(image.archive, image.archive_size);
  runtime_enter(&image);
}

#ifndef EMACSOS_STORAGE_CONTROL_H
#define EMACSOS_STORAGE_CONTROL_H
/* Called only at outer syscall/idle boundaries, never from an IRQ handler. */
void storage_control_init(void);
void storage_control_service(void);
#endif

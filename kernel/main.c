/* Minimal fixed-machine entry: COM1 output, then halt with interrupts off. */
static inline void out8(unsigned short port, unsigned char value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char in8(unsigned short port)
{
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void)
{
    out8(0x3fb, 0x80);     /* Set divisor: 115200 baud. */
    out8(0x3f8, 1);
    out8(0x3f9, 0);
    out8(0x3fb, 3);        /* Eight bits, no parity, one stop bit. */
    out8(0x3f9, 0);        /* Disable UART interrupts after clearing DLAB. */
    out8(0x3fa, 0xc7);     /* Enable and clear FIFOs. */
    out8(0x3fc, 3);        /* DTR and RTS. */
}

static void serial_write(const char *text)
{
    while (*text) {
        while (!(in8(0x3fd) & 0x20))
            __asm__ volatile ("pause");
        out8(0x3f8, (unsigned char)*text++);
    }
}

_Noreturn void kernel_main(void)
{
    serial_init();
    serial_write("emacsOS boot skeleton\r\n"
                 "x86-64 C kernel reached; Emacs is not started.\r\n");
    for (;;)
        __asm__ volatile ("cli; hlt");
}

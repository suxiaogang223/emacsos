#include "kernel.h"
#include "data_layout.h"
#include "elf.h"
struct runtime_image runtime_image_load(void) {
  unsigned char header[512] __attribute__((aligned(16)));
  disk_read(2048, header, 1);
  if (memcmp(header, "EMACSOS1", 8))
    panic("missing runtime payload header");
  uint64_t es, as;
  memcpy(&es, header + 8, 8);
  memcpy(&as, header + 16, 8);
  uint64_t ep = (es + 511) & ~511UL, ap = (as + 511) & ~511UL;
  if (!es || es > 0x10000000UL || as > 0x10000000UL ||
      ep + ap > RAM_LIMIT - PAYLOAD_BASE)
    panic("runtime payload bounds");
  console_puts("Loading runtime bytes=");
  console_hex(es + as);
  console_puts("\r\n");
  disk_read(2049, (void *)PAYLOAD_BASE, ep / 512);
  disk_read(2049 + ep / 512, (void *)(PAYLOAD_BASE + ep), ap / 512);
  return (struct runtime_image){(void *)PAYLOAD_BASE, es,
                                (void *)(PAYLOAD_BASE + ep), as};
}
extern void runtime_jump(uintptr_t, uintptr_t);
extern void syscall_entry(void);
extern uintptr_t syscall_stack_top;
static unsigned char syscall_stack[65536] __attribute__((aligned(16)));
static void wrmsr(uint32_t n, uint64_t x) {
  __asm__ volatile("wrmsr" ::"c"(n), "a"((uint32_t)x),
                   "d"((uint32_t)(x >> 32)));
}
_Noreturn void runtime_enter(const struct runtime_image *im) {
  const struct elf_header *h = (const void *)im->elf;
  if (im->elf_size < sizeof *h || memcmp(h->ident, "\177ELF\2\1\1", 7) ||
      h->type != 2 || h->machine != 62 ||
      h->phentsize != sizeof(struct elf_program) || h->phnum > 64 ||
      h->phoff > im->elf_size ||
      (size_t)h->phnum * sizeof(struct elf_program) > im->elf_size - h->phoff)
    panic("unsupported ELF header");
  const struct elf_program *p = (const void *)(im->elf + h->phoff);
  bool executable = false;
  uintptr_t phdr = (uintptr_t)(im->elf + h->phoff);
  for (unsigned i = 0; i < h->phnum; i++) {
    if (p[i].type == 3)
      panic("dynamic interpreter unsupported");
    if (p[i].type != 1)
      continue;
    if ((p[i].flags & ~7U) ||
        (p[i].align > 1 && ((p[i].align & (p[i].align - 1)) ||
                            ((p[i].vaddr - p[i].offset) & (p[i].align - 1)))))
      panic("ELF segment alignment or flags");
    if (p[i].vaddr < 0x400000UL || p[i].vaddr > 0x4000000UL ||
        p[i].memsz > 0x4000000UL - p[i].vaddr || p[i].filesz > p[i].memsz ||
        p[i].offset > im->elf_size || p[i].filesz > im->elf_size - p[i].offset)
      panic("ELF segment bounds");
    for (unsigned j = 0; j < i; j++)
      if (p[j].type == 1 && p[i].vaddr < p[j].vaddr + p[j].memsz &&
          p[j].vaddr < p[i].vaddr + p[i].memsz)
        panic("ELF segment overlap");
    memcpy((void *)p[i].vaddr, im->elf + p[i].offset, p[i].filesz);
    memset((void *)(p[i].vaddr + p[i].filesz), 0, p[i].memsz - p[i].filesz);
    if (h->phoff >= p[i].offset &&
        h->phoff + (size_t)h->phnum * sizeof *p <= p[i].offset + p[i].filesz)
      phdr = p[i].vaddr + h->phoff - p[i].offset;
    if ((p[i].flags & 1) && h->entry >= p[i].vaddr &&
        h->entry < p[i].vaddr + p[i].memsz)
      executable = true;
  }
  if (!executable)
    panic("ELF entry outside executable segment");
  for (unsigned i = 0; i < h->phnum; i++)
    if (p[i].type == 1) {
      uintptr_t a = p[i].vaddr & ~4095UL;
      size_t n = p[i].vaddr + p[i].memsz - a;
      unsigned prot = ((p[i].flags & 4) ? 1 : 0) | ((p[i].flags & 2) ? 2 : 0) |
                      ((p[i].flags & 1) ? 4 : 0);
      if (memory_protect(a, n, prot))
        panic("ELF protection failure");
    }
  uintptr_t sp = 0x04400000UL;
  const char *strings[] = {
      "/usr/bin/emacs",
      "-Q",
      "-nw",
      "--dump-file",
      "/usr/libexec/emacs/30.2/x86_64-pc-linux-musl/emacs.pdmp",
      "--load",
      "/usr/share/emacs/site-lisp/emacsos-storage.el",
      "TERM=xterm",
      "HOME=" EMACSOS_HOME,
      "PWD=" EMACSOS_HOME,
      "PATH=/usr/bin",
      "EMACSDATA=/usr/share/emacs/30.2/etc",
      "EMACSDOC=/usr/share/emacs/30.2/etc",
      "EMACSLOADPATH=/usr/share/emacs/30.2/lisp:/usr/share/emacs/site-lisp",
      "EMACSPATH=/usr/libexec/emacs/30.2/x86_64-pc-linux-musl"};
  enum { argument_count = 7, string_count = sizeof strings / sizeof *strings };
  uintptr_t ptr[string_count];
  for (unsigned i = 0; i < string_count; i++) {
    size_t n = strlen(strings[i]) + 1;
    sp -= n;
    memcpy((void *)sp, strings[i], n);
    ptr[i] = sp;
  }
  const uint64_t auxiliary[] = {
      3, phdr, 4, sizeof *p, 5, h->phnum, 6, 4096, 7, 0, 8, 0,
      9, h->entry, 11, 0, 12, 0, 13, 0, 14, 0, 17, 1000, 23, 0,
      31, ptr[0], 0, 0};
  uint64_t stack[1 + string_count + 2 + sizeof auxiliary / sizeof *auxiliary];
  unsigned si = 0;
  stack[si++] = argument_count;
  for (unsigned i = 0; i < argument_count; i++)
    stack[si++] = ptr[i];
  stack[si++] = 0;
  for (unsigned i = argument_count; i < string_count; i++)
    stack[si++] = ptr[i];
  stack[si++] = 0;
  memcpy(stack + si, auxiliary, sizeof auxiliary);
  sp = (sp - sizeof stack) & ~15UL;
  memcpy((void *)sp, stack, sizeof stack);
  syscall_stack_top = (uintptr_t)syscall_stack + sizeof syscall_stack;
  wrmsr(0xc0000081, 8UL << 32);
  wrmsr(0xc0000082, (uintptr_t)syscall_entry);
  wrmsr(0xc0000084, 0x600);
  console_puts("Entering static ELF at ");
  console_hex(h->entry);
  console_puts("\r\n");
  runtime_jump(h->entry, sp);
  panic("runtime returned");
}

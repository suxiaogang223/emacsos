#ifndef EMACSOS_ELF_H
#define EMACSOS_ELF_H
#include <stdint.h>
struct elf_header {
  unsigned char ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct elf_program {
  uint32_t type, flags;
  uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
#endif

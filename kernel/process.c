#include "process.h"
#include "elf.h"

#define KERNEL_STACK_SIZE 65536UL
#define USER_LOW 0x00400000UL
#define USER_CODE_END 0x04000000UL
#define USER_END 0x1c000000UL
#define MSR_FS 0xc0000100U
#define MSR_GS 0xc0000101U
#define MSR_STAR 0xc0000081U
#define MSR_LSTAR 0xc0000082U
#define MSR_FMASK 0xc0000084U

struct selectors { uint16_t ds, es, fs, gs; };
struct fx_state { unsigned char bytes[512]; } __attribute__((aligned(16)));
struct process {
  bool used;
  struct process_info info;
  struct address_space *space;
  void *kernel_stack;
  unsigned char *output;
  uint64_t fs, gs;
  struct selectors selectors;
  struct process_frame frame;
  struct fx_state fx;
};
static struct process processes[PROCESS_CAPACITY], *active;
static uint64_t next_pid = 2;
static struct {
  struct address_space *space;
  uint64_t fs, gs, star, lstar, fmask;
  uintptr_t tss_stack;
  struct selectors selectors;
  struct fx_state fx;
} manager;
extern void process_interrupt_entry(void);

static uint64_t read_msr(uint32_t n) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(n));
  return (uint64_t)high << 32 | low;
}
static void write_msr(uint32_t n, uint64_t value) {
  __asm__ volatile("wrmsr" ::"c"(n), "a"((uint32_t)value),
                   "d"((uint32_t)(value >> 32)) : "memory");
}
static uint64_t disable_interrupts(void) {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
  return flags;
}
static void restore_interrupts(uint64_t flags) {
  __asm__ volatile("pushq %0; popfq" ::"r"(flags) : "memory", "cc");
}
static struct selectors read_selectors(void) {
  struct selectors s;
  __asm__ volatile("mov %%ds,%0; mov %%es,%1; mov %%fs,%2; mov %%gs,%3"
                   : "=r"(s.ds), "=r"(s.es), "=r"(s.fs), "=r"(s.gs));
  return s;
}
static void load_selectors(struct selectors s) {
  __asm__ volatile("mov %0,%%ds; mov %1,%%es; mov %2,%%fs; mov %3,%%gs"
                   ::"r"(s.ds), "r"(s.es), "r"(s.fs), "r"(s.gs) : "memory");
}
static struct process *find(uint64_t pid) {
  for (unsigned i = 0; i < PROCESS_CAPACITY; i++)
    if (processes[i].used && processes[i].info.pid == pid)
      return &processes[i];
  return NULL;
}
static bool valid_return(struct process *p, struct process_frame *f) {
  if (f->cs != PROCESS_USER_CODE || f->ss != PROCESS_USER_DATA ||
      !memory_space_user_access(p->space, f->rip, 1, 4) ||
      f->rsp <= USER_LOW || f->rsp > USER_END ||
      !memory_space_user_access(p->space, f->rsp - 1, 1, 2))
    return false;
  /* Keep arithmetic flags, require IF, and remove privilege/debug modes. */
  f->flags = (f->flags & 0x8d5) | 0x202;
  return true;
}
static void release_execution(struct process *p) {
  if (p->space) {
    if (memory_space_destroy(p->space))
      panic("worker space reclamation");
    p->space = NULL;
  }
  kfree(p->kernel_stack);
  p->kernel_stack = NULL;
  p->info.memory.data_pages = p->info.memory.table_pages = 0;
  p->info.state = PROCESS_EXITED;
}
static _Noreturn void suspend(struct process *p) {
  __asm__ volatile("cli" ::: "memory");
  __asm__ volatile("fxsave64 %0" : "=m"(p->fx));
  p->fs = read_msr(MSR_FS);
  p->gs = read_msr(MSR_GS);
  p->selectors = read_selectors();
  if (memory_space_activate(manager.space))
    panic("manager address space");
  load_selectors(manager.selectors);
  write_msr(MSR_FS, manager.fs);
  write_msr(MSR_GS, manager.gs);
  write_msr(MSR_STAR, manager.star);
  write_msr(MSR_LSTAR, manager.lstar);
  write_msr(MSR_FMASK, manager.fmask);
  (void)hardware_process_stack(manager.tss_stack);
  __asm__ volatile("fxrstor64 %0" :: "m"(manager.fx));
  active = NULL;
  process_kernel_stack_top = process_user_stack = 0;
  process_arch_leave();
}
static _Noreturn void finish(struct process *p, enum process_reason reason,
                             int32_t status) {
  p->info.reason = reason;
  p->info.status = status;
  p->info.state = PROCESS_EXITING;
  suspend(p);
}

static long load_elf(struct process *p, const void *blob, size_t bytes) {
  if (!blob || bytes < sizeof(struct elf_header) || bytes > 0x100000)
    return -8;
  struct elf_header h;
  memcpy(&h, blob, sizeof h);
  if (memcmp(h.ident, "\177ELF\2\1\1", 7) || h.type != 2 || h.machine != 62 ||
      h.version != 1 || h.ehsize != sizeof h || h.phentsize != sizeof(struct elf_program) ||
      !h.phnum || h.phnum > 16 || h.phoff > bytes ||
      h.phnum * sizeof(struct elf_program) > bytes - h.phoff)
    return -8;
  struct elf_program ph[16];
  memcpy(ph, (const unsigned char *)blob + h.phoff, h.phnum * sizeof *ph);
  bool executable = false;
  for (unsigned i = 0; i < h.phnum; i++) {
    struct elf_program *s = &ph[i];
    if (s->type == 2 || s->type == 3 || s->type == 7)
      return -8; /* No dynamic linking, interpreter or libc TLS startup. */
    if (s->type != 1)
      continue;
    if ((s->flags & ~7U) || !(s->flags & 4) || (s->flags & 3) == 3 ||
        s->vaddr < USER_LOW || s->vaddr >= USER_CODE_END ||
        s->memsz > USER_CODE_END - s->vaddr || s->filesz > s->memsz ||
        s->offset > bytes || s->filesz > bytes - s->offset ||
        (s->align > 1 && ((s->align & (s->align - 1)) ||
                         ((s->vaddr - s->offset) & (s->align - 1)))))
      return -8;
    if (!s->memsz)
      continue;
    uintptr_t start = s->vaddr & ~(PAGE_SIZE - 1);
    uintptr_t end = (s->vaddr + s->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (unsigned j = 0; j < i; j++)
      if (ph[j].type == 1 && ph[j].memsz &&
          start < ((ph[j].vaddr + ph[j].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) &&
          (ph[j].vaddr & ~(PAGE_SIZE - 1)) < end)
        return -8;
    if ((s->flags & 1) && h.entry >= s->vaddr && h.entry - s->vaddr < s->memsz)
      executable = true;
  }
  if (!executable)
    return -8;
  for (unsigned i = 0; i < h.phnum; i++) {
    struct elf_program *s = &ph[i];
    if (s->type != 1 || !s->memsz)
      continue;
    uintptr_t start = s->vaddr & ~(PAGE_SIZE - 1);
    size_t n = s->vaddr + s->memsz - start;
    long result = memory_space_map(p->space, start, n, 3, 0x32);
    if (result < 0)
      return result;
    if (memory_space_copy_to(p->space, s->vaddr,
                            (const unsigned char *)blob + s->offset, s->filesz))
      return -14;
    unsigned prot = 1 | ((s->flags & 2) ? 2 : 0) | ((s->flags & 1) ? 4 : 0);
    if (memory_space_protect(p->space, start, n, prot))
      return -12;
  }
  long result = memory_space_map(p->space, PROCESS_STACK_TOP - PROCESS_STACK_SIZE,
                                 PROCESS_STACK_SIZE, 3, 0x32);
  if (result < 0)
    return result;
  /* Empty argc/argv/env/auxv; no libc startup or inherited environment. */
  uint64_t stack[6] = {0};
  p->frame = (struct process_frame){.rip = h.entry, .cs = PROCESS_USER_CODE,
      .ss = PROCESS_USER_DATA, .flags = 0x202, .rsp = PROCESS_STACK_TOP - sizeof stack};
  return memory_space_copy_to(p->space, p->frame.rsp, stack, sizeof stack);
}

long process_spawn(const void *elf, size_t bytes, size_t page_limit) {
  if (active)
    return -16;
  if (page_limit < 3 || page_limit > MEMORY_PAGE_CAPACITY)
    return -22;
  if (next_pid > INT64_MAX)
    return -11;
  struct process *p = NULL;
  for (unsigned i = 0; i < PROCESS_CAPACITY; i++)
    if (!processes[i].used) {
      p = &processes[i];
      break;
    }
  if (!p)
    return -11;
  memset(p, 0, sizeof *p);
  p->space = memory_space_create(page_limit);
  if (!p->space)
    return -12;
  p->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
  p->output = kmalloc(PROCESS_OUTPUT_CAPACITY);
  long result = p->kernel_stack && p->output ? load_elf(p, elf, bytes) : -12;
  if (result < 0) {
    release_execution(p);
    kfree(p->output);
    memset(p, 0, sizeof *p);
    return result;
  }
  p->used = true;
  p->info = (struct process_info){.pid = next_pid++, .parent = 1, .state = PROCESS_READY};
  p->selectors = (struct selectors){.ds = PROCESS_USER_DATA, .es = PROCESS_USER_DATA};
  memory_space_usage(p->space, &p->info.memory);
  uint16_t control = 0x37f;
  uint32_t mxcsr = 0x1f80;
  memcpy(p->fx.bytes, &control, sizeof control);
  memcpy(p->fx.bytes + 24, &mxcsr, sizeof mxcsr);
  return p->info.pid;
}
long process_run(uint64_t pid) {
  struct process *p = find(pid);
  if (!p)
    return -3;
  if (active || p->info.state != PROCESS_READY)
    return -16;
  if (!valid_return(p, &p->frame)) {
    p->info.reason = PROCESS_BAD_CONTEXT;
    p->info.status = -14;
    release_execution(p);
    return 0;
  }
  uint64_t flags = disable_interrupts();
  uint64_t cr4;
  __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
  if (cr4 & ((1UL << 16) | (1UL << 18)))
    panic("unsupported worker extended CPU state");
  manager.space = memory_current_space();
  manager.selectors = read_selectors();
  manager.fs = read_msr(MSR_FS);
  manager.gs = read_msr(MSR_GS);
  manager.star = read_msr(MSR_STAR);
  manager.lstar = read_msr(MSR_LSTAR);
  manager.fmask = read_msr(MSR_FMASK);
  __asm__ volatile("fxsave64 %0" : "=m"(manager.fx));
  hardware_process_gate((uintptr_t)process_interrupt_entry);
  process_kernel_stack_top = (uintptr_t)p->kernel_stack + KERNEL_STACK_SIZE;
  manager.tss_stack = hardware_process_stack(process_kernel_stack_top);
  write_msr(MSR_STAR, 8UL << 32);
  write_msr(MSR_LSTAR, (uintptr_t)process_syscall_entry);
  write_msr(MSR_FMASK, 0x57700); /* TF, IF, DF, IOPL, NT, RF and AC. */
  active = p;
  p->info.state = PROCESS_RUNNING;
  if (memory_space_activate(p->space))
    panic("worker address space activation");
  load_selectors(p->selectors);
  write_msr(MSR_FS, p->fs);
  write_msr(MSR_GS, p->gs);
  __asm__ volatile("fxrstor64 %0" :: "m"(p->fx));
  process_arch_enter(&p->frame);
  /* The worker stack and address space are no longer executing here. */
  if (p->info.state == PROCESS_EXITING)
    release_execution(p);
  else
    memory_space_usage(p->space, &p->info.memory);
  restore_interrupts(flags);
  return 0;
}
long process_inspect(uint64_t pid, struct process_info *info) {
  struct process *p = find(pid);
  if (!p)
    return -3;
  if (!info)
    return -22;
  *info = p->info;
  return 0;
}
size_t process_list(struct process_info *out, size_t capacity) {
  size_t count = 0;
  for (unsigned i = 0; i < PROCESS_CAPACITY; i++)
    if (processes[i].used) {
      if (out && count < capacity)
        out[count] = processes[i].info;
      count++;
    }
  return count;
}
long process_terminate(uint64_t pid) {
  struct process *p = find(pid);
  if (!p)
    return -3;
  if (p == active)
    return -16;
  if (p->info.state != PROCESS_EXITED) {
    p->info.reason = PROCESS_TERMINATED;
    p->info.status = -125;
    release_execution(p);
  }
  return 0;
}
long process_reap(uint64_t pid) {
  struct process *p = find(pid);
  if (!p)
    return -3;
  if (p->info.state != PROCESS_EXITED)
    return -16;
  kfree(p->output);
  memset(p, 0, sizeof *p);
  return 0;
}
long process_output(uint64_t pid, size_t offset, void *out, size_t n) {
  struct process *p = find(pid);
  if (!p)
    return -3;
  if ((!out && n) || offset > p->info.output_bytes)
    return -22;
  size_t count = p->info.output_bytes - offset;
  if (n < count)
    count = n;
  if (count)
    memcpy(out, p->output + offset, count);
  return count;
}

static long worker_syscall(struct process *p, struct process_frame *f) {
  switch (f->rax) {
  case 0: case 3:
    return -9; /* No stdin or inherited file descriptors. */
  case 1: {
    if (f->rdi != 1 && f->rdi != 2)
      return -9;
    if (f->rdx > PROCESS_OUTPUT_CAPACITY)
      return -22;
    if (!f->rdx)
      return 0;
    if (!memory_space_user_access(p->space, f->rsi, f->rdx, 1))
      return -14;
    size_t n = PROCESS_OUTPUT_CAPACITY - p->info.output_bytes;
    if (f->rdx < n)
      n = f->rdx;
    if (!n)
      return -28;
    if (memory_space_copy_from(p->space, p->output + p->info.output_bytes, f->rsi, n))
      return -14;
    p->info.output_bytes += n;
    return n;
  }
  case 9:
    if ((f->r10 != 0x22 && f->r10 != 0x32) || f->r8 != UINT64_MAX || f->r9 ||
        (f->rdx & ~7UL) || (f->rdx & 6) == 6)
      return -22;
    return memory_space_map(p->space, f->rdi, f->rsi, f->rdx, f->r10);
  case 10:
    if ((f->rdx & ~7UL) || (f->rdx & 6) == 6)
      return -22;
    return memory_space_protect(p->space, f->rdi, f->rsi, f->rdx);
  case 11:
    return memory_space_unmap(p->space, f->rdi, f->rsi);
  case 12:
    return memory_space_brk(p->space, f->rdi);
  case 24:
    f->rax = 0;
    p->frame = *f;
    p->info.state = PROCESS_READY;
    suspend(p);
  case 39:
    return p->info.pid;
  case 60: case 231:
    finish(p, PROCESS_NORMAL, (int32_t)f->rdi);
  case 110:
    return p->info.parent;
  case 158:
    if (f->rdi == 0x1001 || f->rdi == 0x1002) {
      if (f->rsi && !memory_space_user_access(p->space, f->rsi, 1, 1))
        return -14;
      write_msr(f->rdi == 0x1001 ? MSR_GS : MSR_FS, f->rsi);
      return 0;
    }
    if (f->rdi == 0x1003 || f->rdi == 0x1004) {
      uint64_t base = read_msr(f->rdi == 0x1003 ? MSR_FS : MSR_GS);
      return memory_space_copy_to(p->space, f->rsi, &base, sizeof base);
    }
    return -22;
  case 228: {
    if (f->rdi != 1)
      return -22;
    uint64_t ns = clock_monotonic_ns();
    uint64_t time[2] = {ns / 1000000000UL, ns % 1000000000UL};
    return memory_space_copy_to(p->space, f->rsi, time, sizeof time);
  }
  default:
    return -38;
  }
}
bool process_interrupt(struct process_frame *f) {
  struct process *p = active;
  if (!p || f->cs != PROCESS_USER_CODE)
    return false;
  if (f->vector < 32) {
    p->info.fault_vector = f->vector;
    p->info.fault_error = f->error;
    p->info.fault_ip = f->rip;
    if (f->vector == 14)
      __asm__ volatile("mov %%cr2,%0" : "=r"(p->info.fault_address));
    finish(p, PROCESS_FAULT, -14);
  }
  if (!valid_return(p, f))
    finish(p, PROCESS_BAD_CONTEXT, -14);
  if (f->vector == 128) {
    f->rax = worker_syscall(p, f);
    if (!valid_return(p, f))
      finish(p, PROCESS_BAD_CONTEXT, -14);
  }
  return true;
}

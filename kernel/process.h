#ifndef EMACSOS_PROCESS_H
#define EMACSOS_PROCESS_H
#include "kernel.h"
#include "memory.h"

#define PROCESS_CAPACITY 8
#define PROCESS_OUTPUT_CAPACITY 4096
#define PROCESS_USER_CODE 0x2bUL
#define PROCESS_USER_DATA 0x33UL
#define PROCESS_STACK_TOP 0x04400000UL
#define PROCESS_STACK_SIZE 0x10000UL

/* Matches interrupt_common, including the privilege-return stack frame. */
struct process_frame {
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
  uint64_t vector, error, rip, cs, flags, rsp, ss;
};
enum process_state { PROCESS_READY, PROCESS_RUNNING, PROCESS_EXITING, PROCESS_EXITED };
enum process_reason { PROCESS_NORMAL, PROCESS_TERMINATED, PROCESS_FAULT, PROCESS_BAD_CONTEXT };
struct process_info {
  uint64_t pid, parent;
  enum process_state state;
  enum process_reason reason;
  int32_t status;
  uint64_t fault_vector, fault_error, fault_address, fault_ip;
  struct memory_usage memory;
  size_t output_bytes;
};

/* Trusted, serialized management on a shared kernel stack. No preemption,
 * blocking syscalls, inherited descriptors or public Emacs binding yet. */
long process_spawn(const void *elf, size_t bytes, size_t page_limit);
long process_run(uint64_t pid);
long process_inspect(uint64_t pid, struct process_info *);
size_t process_list(struct process_info *, size_t capacity);
long process_terminate(uint64_t pid);
long process_reap(uint64_t pid);
long process_output(uint64_t pid, size_t offset, void *, size_t bytes);

/* Architecture hooks; worker entry MSRs never target the trusted dispatcher. */
bool process_interrupt(struct process_frame *);
void hardware_process_gate(uintptr_t entry);
uintptr_t hardware_process_stack(uintptr_t top);
extern uintptr_t process_kernel_stack_top, process_user_stack;
void process_syscall_entry(void);
void process_arch_enter(struct process_frame *);
_Noreturn void process_arch_leave(void);
#endif

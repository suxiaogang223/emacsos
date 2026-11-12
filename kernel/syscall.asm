bits 64
section .bss
align 8
global syscall_stack_top
syscall_stack_top: resq 1
runtime_stack: resq 1
section .text
global syscall_entry,runtime_jump
extern syscall_dispatch
runtime_jump:
 mov rsp,rsi
 xor edx,edx
 jmp rdi
syscall_entry:
 mov [rel runtime_stack],rsp
 mov rsp,[rel syscall_stack_top]
 push r11
 push rcx
 push rbx
 push rbp
 push r12
 push r13
 push r14
 push r15
 push r9
 push r8
 push r10
 push rdx
 push rsi
 push rdi
 mov rdi,rax
 mov rsi,rsp
 call syscall_dispatch
 pop rdi
 pop rsi
 pop rdx
 pop r10
 pop r8
 pop r9
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbp
 pop rbx
 pop rcx
 pop r11
 mov rsp,[rel runtime_stack]
 push r11
 popfq
 jmp rcx
section .note.GNU-stack noalloc noexec nowrite progbits

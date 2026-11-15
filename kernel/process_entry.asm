bits 64
section .bss
align 8
manager_stack: resq 1
global process_kernel_stack_top, process_user_stack
process_kernel_stack_top: resq 1
process_user_stack: resq 1
section .text
global process_arch_enter, process_arch_leave, process_syscall_entry
extern interrupt_common

; Serialized manager continuation, always on a shared supervisor stack.
process_arch_enter:
 push rbx
 push rbp
 push r12
 push r13
 push r14
 push r15
 mov [rel manager_stack],rsp
 mov rsp,rdi
 pop r15
 pop r14
 pop r13
 pop r12
 pop r11
 pop r10
 pop r9
 pop r8
 pop rdi
 pop rsi
 pop rbp
 pop rdx
 pop rcx
 pop rbx
 pop rax
 add rsp,16
 iretq
process_arch_leave:
 mov ax,0x10
 mov ss,ax
 mov rsp,[rel manager_stack]
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbp
 pop rbx
 ret

; LSTAR is replaced before entering a worker, then restored for trusted Emacs.
; No user stack access and no path to the legacy trusted syscall dispatcher.
process_syscall_entry:
 mov [rel process_user_stack],rsp
 mov rsp,[rel process_kernel_stack_top]
 push qword 0x33
 push qword [rel process_user_stack]
 push r11
 push qword 0x2b
 push rcx
 push qword 0
 push qword 128
 jmp interrupt_common
section .note.GNU-stack noalloc noexec nowrite progbits

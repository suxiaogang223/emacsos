bits 64
section .text
extern interrupt_dispatch
global interrupt_stubs
%macro ISR 1
isr%1:
%if %1 != 8 && %1 != 10 && %1 != 11 && %1 != 12 && %1 != 13 && %1 != 14 && %1 != 17 && %1 != 21 && %1 != 29 && %1 != 30
 push qword 0
%endif
 push qword %1
 jmp interrupt_common
%endmacro
%assign i 0
%rep 48
 ISR i
%assign i i+1
%endrep
global process_interrupt_entry, interrupt_common
process_interrupt_entry:
 push qword 0
 push qword 128
 jmp interrupt_common
interrupt_common:
 cld
 push rax
 push rbx
 push rcx
 push rdx
 push rbp
 push rsi
 push rdi
 push r8
 push r9
 push r10
 push r11
 push r12
 push r13
 push r14
 push r15
 mov rdi,rsp
 mov rbx,rsp
 and rsp,-16
 call interrupt_dispatch
 mov rsp,rbx
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
section .rodata
interrupt_stubs:
%assign i 0
%rep 48
 dq isr%+i
%assign i i+1
%endrep
section .note.GNU-stack noalloc noexec nowrite progbits

bits 64
section .text.entry
global _start
extern worker_main
_start:
 call worker_main
 mov rdi,rax
 mov eax,60
 syscall
 ud2
section .text
global worker_context_round
worker_context_round:
 push rbx
 push rbp
 push r12
 push r13
 push r14
 push r15
 sub rsp,32
 mov word [rsp],0x77f
 fldcw [rsp]
 mov dword [rsp+4],0x3f80
 ldmxcsr [rsp+4]
 fld1
 pcmpeqd xmm0,xmm0
 movdqa xmm7,xmm0
 movdqa xmm15,xmm0
 mov ebx,71
 mov ebp,72
 mov r12d,73
 mov r13d,74
 mov r14d,75
 mov r15d,76
 mov r8d,77
 mov r9d,78
 mov r10d,79
 mov edi,80
 mov esi,81
 mov edx,82
 mov eax,24
 syscall
 test rax,rax
 jnz .failed
 cmp rbx,71
 jne .failed
 cmp rbp,72
 jne .failed
 cmp r12,73
 jne .failed
 cmp r13,74
 jne .failed
 cmp r14,75
 jne .failed
 cmp r15,76
 jne .failed
 cmp r8,77
 jne .failed
 cmp r9,78
 jne .failed
 cmp r10,79
 jne .failed
 cmp rdi,80
 jne .failed
 cmp rsi,81
 jne .failed
 cmp rdx,82
 jne .failed
 fnstcw [rsp]
 cmp word [rsp],0x77f
 jne .failed
 stmxcsr [rsp+4]
 cmp dword [rsp+4],0x3f80
 jne .failed
 fstp qword [rsp+8]
 mov rax,0x3ff0000000000000
 cmp [rsp+8],rax
 jne .failed
 movq rax,xmm0
 cmp rax,-1
 jne .failed
 movq rax,xmm7
 cmp rax,-1
 jne .failed
 movdqu [rsp+8],xmm15
 cmp qword [rsp+8],-1
 jne .failed
 cmp qword [rsp+16],-1
 jne .failed
 xor eax,eax
 jmp .return
.failed:
 mov eax,1
.return:
 add rsp,32
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbp
 pop rbx
 ret
section .note.GNU-stack noalloc noexec nowrite progbits

section .text.boot
bits 16
global _start
extern kernel_main, __bss_start, __bss_end

_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    cld

    ; Enable A20 without asserting the fast-reset bit.
    in al, 0x92
    and al, 0xfe
    or al, 2
    out 0x92, al

    ; Identity map the first 2 MiB with one large page.
    xor ax, ax
    mov di, 0x1000
    mov cx, 0x1800
    rep stosw
    mov dword [0x1000], 0x2003
    mov dword [0x2000], 0x3003
    mov dword [0x3000], 0x83
    mov eax, 0x1000
    mov cr3, eax
    mov eax, cr4
    or eax, 0x20           ; PAE.
    mov cr4, eax
    mov ecx, 0xc0000080
    rdmsr
    and eax, ~1           ; Keep SYSCALL disabled.
    or eax, 0x100          ; Long-mode enable.
    wrmsr
    lgdt [cs:gdtr - _start]
    mov eax, cr0
    or eax, 0x80000001     ; Paging and protected mode.
    mov cr0, eax
    jmp dword 0x08:long_entry

align 8
gdt: dq 0, 0x00af9a000000ffff, 0x00cf92000000ffff
gdtr:
    dw $ - gdt - 1
    dd gdt

bits 64
long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    xor ebp, ebp
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    call kernel_main
.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits

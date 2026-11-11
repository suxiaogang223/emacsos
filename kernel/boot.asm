bits 16
org 0x7c00

%ifndef KERNEL_SECTORS
%error KERNEL_SECTORS is required
%endif
%if KERNEL_SECTORS < 1 || KERNEL_SECTORS > 896
%error Kernel exceeds the reserved load region
%endif

; Normalize CS and keep the BIOS stack below the boot sector.
jmp 0:initialize
initialize:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    mov [drive], dl

    ; Require BIOS extended disk reads on the selected boot drive.
    mov bx, 0x55aa
    mov ah, 0x41
    int 0x13
    mov ax, 0
    mov ds, ax
    jc disk_error
    cmp bx, 0xaa55
    jne disk_error
    test cx, 1
    jz disk_error

read_chunk:
    mov ax, [remaining]
    cmp ax, 64
    jbe .bounded
    mov ax, 64
.bounded:
    ; At most 32 KiB per read; no transfer crosses a 64 KiB boundary.
    mov [requested], ax
    mov [packet.count], ax
    mov si, packet
    mov dl, [drive]
    mov ah, 0x42
    int 0x13
    mov ax, 0
    mov ds, ax
    jc disk_error
    mov ax, [requested]
    cmp [packet.count], ax
    jne disk_error
    sub [remaining], ax
    jz loaded
    add word [packet.lba], ax
    shl ax, 5
    add [packet.segment], ax
    jmp read_chunk

loaded:
    jmp 0x1000:0

disk_error:
    mov ax, 0x0e45          ; E on the BIOS console.
    mov bx, 7
    int 0x10
    cli
.halt:
    hlt
    jmp .halt

drive: db 0
remaining: dw KERNEL_SECTORS
requested: dw 0
align 4
packet:
    db 16, 0
.count: dw 0
    dw 0
.segment: dw 0x1000
.lba: dq 1

times 510 - ($ - $$) db 0
dw 0xaa55

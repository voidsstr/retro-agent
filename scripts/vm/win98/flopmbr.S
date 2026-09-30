/* VM-only MBR: boot the floppy's boot sector (drive 00h). Replaced by FDISK /MBR in stage 1. */
    .code16
    .intel_syntax noprefix
    .text
_start:
    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0x7C00
    mov ds, ax
    mov es, ax
    sti
    cld
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    rep movsw
    .byte 0xEA
    .word go, 0
go: mov bp, 4
1:  mov ax, 0x0201
    mov bx, 0x7C00
    mov cx, 0x0001
    xor dx, dx
    int 0x13
    jnc 2f
    xor ax, ax
    xor dx, dx
    int 0x13
    dec bp
    jnz 1b
    jmp 3f
2:  cmp word ptr [0x7DFE], 0xAA55
    jne 3f
    xor dx, dx
    .byte 0xEA
    .word 0x7C00, 0
3:  mov si, offset msg
4:  lodsb
    or al, al
    jz 5f
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp 4b
5:  hlt
    jmp 5b
msg: .asciz "no bootable floppy"
    .org 0x1B8

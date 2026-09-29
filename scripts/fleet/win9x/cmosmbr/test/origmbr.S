/* stand-in for the original DOS MBR: relocate to 0:0600, boot the active partition */
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
go: mov al, 'O'
    out 0xE9, al
    mov si, 0x07BE
1:  cmp byte ptr [si], 0x80
    je 2f
    add si, 16
    cmp si, 0x07FE
    jne 1b
    hlt
2:  mov dh, [si+1]
    mov cx, [si+2]
    mov dl, 0x80
    mov ax, 0x0201
    mov bx, 0x7C00
    int 0x13
    .byte 0xEA
    .word 0x7C00, 0
    .org 0x1FE
    .byte 0x55, 0xAA

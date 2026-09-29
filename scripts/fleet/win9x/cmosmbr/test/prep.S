/* TEST ONLY boot sector: give QEMU's CMOS a valid 10h..2Dh checksum, then load
 * LBA 2 (the merged production MBR) at 0:7C00 and run it as the BIOS would. */
    .code16
    .intel_syntax noprefix
    .text
_start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    cld
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    rep movsw
    .byte 0xEA
    .word here, 0
here:
    mov al, 0x50
    out 0xE9, al
    cmp byte ptr [0x4F8], 0x77
    je 2f
    mov byte ptr [0x4F8], 0x77
    xor dx, dx
    mov bl, 0x10
1:  mov al, bl
    out 0x70, al
    in al, 0x71
    xor ah, ah
    add dx, ax
    inc bl
    cmp bl, 0x2E
    jne 1b
    mov al, 0x2E
    out 0x70, al
    mov al, dh
    out 0x71, al
    mov al, 0x2F
    out 0x70, al
    mov al, dl
    out 0x71, al
2:
    mov ax, 0x0070
    mov es, ax
    mov di, 0xFFEA
    mov si, offset rom1
    mov cx, 6
    rep movsb
    mov di, 0xFFF5
    mov cx, 8
    rep movsb
    xor ax, ax
    mov es, ax
    mov si, 0x0600          /* copy self away, then load LBA 2 over 0x7C00 */
    mov ax, 0x0201
    mov bx, 0x7C00
    mov cx, 0x0003
    xor dh, dh
    mov dl, 0x80
    int 0x13
    mov al, 0x51
    out 0xE9, al
    mov dl, 0x80
    .byte 0xEA
    .word 0x7C00, 0
rom1: .ascii "COMPAQ04/25/97"
    .org 0x1FE
    .byte 0x55, 0xAA

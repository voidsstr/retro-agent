/* test VBR: print "V<1B><2D><cs ok?>" hex to debugcon, then exit QEMU */
    .code16
    .intel_syntax noprefix
    .text
_start:
    xor ax, ax
    mov ds, ax
    mov al, 'V'
    out 0xE9, al
    mov al, 0x1B
    call rd
    call hex
    mov al, 0x2D
    call rd
    call hex
    xor dx, dx
    mov bl, 0x10
1:  mov al, bl
    call rd
    xor ah, ah
    add dx, ax
    inc bl
    cmp bl, 0x2E
    jne 1b
    mov al, 0x2E
    call rd
    mov ah, al
    mov al, 0x2F
    call rd
    cmp ax, dx
    mov al, 'K'
    je 2f
    mov al, 'X'
2:  out 0xE9, al
    mov si, 0x06DA
    mov cx, 6
4:  lodsb
    call hex
    loop 4b
    mov al, 0x0A
    out 0xE9, al
    mov al, 0x01
    out 0xF4, al
    hlt
rd: out 0x70, al
    in al, 0x71
    ret
hex: push ax
    shr al, 4
    call nib
    pop ax
    and al, 0x0F
nib: add al, '0'
    cmp al, '9'
    jbe 3f
    add al, 7
3:  out 0xE9, al
    ret
    .org 0x1FE
    .byte 0x55, 0xAA

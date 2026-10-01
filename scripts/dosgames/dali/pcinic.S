# PCINIC.COM - find a PCI device by vendor/device and print BAR0 + IRQ line.
# Build: as --32 -o pcinic.o pcinic.S && ld -m elf_i386 -Ttext 0x100 -e 0x100 --oformat binary -o PCINIC.COM pcinic.o
# Measured 2026-10-01: the Win98 build VM's RTL8029 reads BAR0=0000E001 IRQ=0A (NE2000 0x60 10 0xE000).
# Default: 10EC:8029 (RTL8029 / NE2000 PCI). Output: "PCI 10EC:8029 BAR0=xxxxxxxx IRQ=xx"
        .code16
        .intel_syntax noprefix
        .text
        .org 0
start:
        mov ax, 0xB102
        mov cx, 0x8029
        mov dx, 0x10EC
        xor si, si
        int 0x1A
        jc notfound
        cmp ah, 0
        jne notfound
        mov [devloc], bx
        mov ax, 0xB10A          # read config dword
        mov di, 0x10            # BAR0
        int 0x1A
        mov [bar0], ecx
        mov bx, [devloc]
        mov ax, 0xB108          # read config byte
        mov di, 0x3C            # interrupt line
        int 0x1A
        mov [irq], cl
        # print
        mov dx, offset msg
        mov ah, 9
        int 0x21
        mov eax, [bar0]
        mov cx, 8
        call hexout
        mov dx, offset msg2
        mov ah, 9
        int 0x21
        movzx eax, byte ptr [irq]
        shl eax, 24
        mov cx, 2
        call hexout
        mov dx, offset crlf
        mov ah, 9
        int 0x21
        mov ax, 0x4C00
        int 0x21
notfound:
        mov dx, offset nf
        mov ah, 9
        int 0x21
        mov ax, 0x4C01
        int 0x21
# print the top CX nibbles of EAX as hex
hexout:
        rol eax, 4
        push eax
        and al, 0x0F
        add al, '0'
        cmp al, '9'
        jbe 1f
        add al, 7
1:      mov dl, al
        mov ah, 2
        int 0x21
        pop eax
        loop hexout
        ret
msg:    .ascii "PCI 10EC:8029 BAR0=$"
msg2:   .ascii " IRQ=$"
crlf:   .ascii "\r\n$"
nf:     .ascii "PCI 10EC:8029 NOT FOUND\r\n$"
devloc: .word 0
bar0:   .long 0
irq:    .byte 0

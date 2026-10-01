# IPXCHK.COM - is an IPX driver resident? INT 2Fh AX=7A00h returns AL=FFh when
# one is (Novell's installation check; NWLINK.VXD, DALI and IPXODI all answer it).
# Prints "IPX PRESENT" / "IPX NOT DETECTED"; errorlevel 0 / 1.
# Build: as --32 -o ipxchk.o ipxchk.S && ld -m elf_i386 -Ttext 0x100 -e 0x100 --oformat binary -o IPXCHK.COM ipxchk.o
        .code16
        .intel_syntax noprefix
        .text
start:
        mov ax, 0x7A00
        int 0x2F
        cmp al, 0xFF
        jne no
        mov dx, offset yes
        mov bl, 0
        jmp short print
no:     mov dx, offset nomsg
        mov bl, 1
print:  mov ah, 9
        int 0x21
        mov al, bl
        mov ah, 0x4C
        int 0x21
yes:    .ascii "IPX PRESENT\r\n$"
nomsg:  .ascii "IPX NOT DETECTED\r\n$"

/* cmosmbr - boot-disk MBR stage for .243 (Compaq Deskpro 2000, 586C 04/25/97 ROM).
 *
 * WHY: .243's CMOS battery is dead. Every power loss makes POST reload Compaq
 * defaults, which set CMOS 1Bh (secondary IDE master type) to "auto" (44h).
 * POST then types the 80 GB ST380013A; the ROM's translation overflows to 256
 * heads and IO.SYS hangs at a blinking cursor probing BIOS unit 81h - measured
 * 2026-09-29 (drive removed: boots; drive fitted after a power-off: hangs).
 * With 1Bh = 00 POST leaves the disk alone and Windows' ESDI_506 drives all
 * 80 GB natively (proven 09-29), and a WARM POST does not auto-type a drive.
 *
 * WHAT: runs from LBA 0 of the boot disk after POST, before any OS code.
 * On this ROM only, if the CMOS bank is sane (640 KB at 15h/16h, checksum of
 * 10h..2Dh at 2Eh/2Fh valid) and 1Bh != 00, it writes 1Bh = 00 and sets 2Dh
 * bit 3 (skip F1), recomputes the checksum, reads all of it back, and only
 * then warm-resets once (a marker at 0:04F0 stops a second reset in the same
 * RAM lifetime). Anything unexpected -> no write / no reset, just boot on.
 * It then chains the ORIGINAL MBR code, which the installer saved at LBA 32
 * (SAVESEC-1; .243's LBA 1 and 2 hold an older DOS MBR and must stay as they are),
 * after copying this sector's live partition table and the Win9x disk stamp
 * (0xDA..0xDF, which ESDI_506 matches disks by) over it.
 *
 * Build: see Makefile. TEST=1 skips the ROM check and uses 1Bh target 5Ah so
 * the reset path can be exercised in QEMU (tests/python/test_cmosmbr.py). */
    .code16
    .intel_syntax noprefix
    .text
    .globl _start
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
    .byte 0xEA                  /* jmp 0000:reloc */
    .word reloc, 0
reloc:
    mov [drive], dl
/* ROM (TEST==0; TEST==2 = the same check against segment ROMSEG in RAM) */
.if TEST != 1
    /* ROM: "COMPAQ" at F000:FFEA and "04/25/97" at F000:FFF5 */
    push ds
    mov ax, ROMSEG
    mov ds, ax
    mov si, 0xFFEA
    mov di, offset s_compaq
    mov cx, 6
    repe cmpsb
    jne 1f
    mov si, 0xFFF5
    mov cx, 8                   /* s_date follows s_compaq */
    repe cmpsb
1:  pop ds
    jne chain_near
.endif
    /* one reset per RAM lifetime */
    cmp dword ptr [0x4F0], 0x4652424D       /* 'MBRF' */
    jne 2f
    mov dword ptr [0x4F0], 0
chain_near:
    jmp chain
2:  /* bank sanity: 15h/16h = 80h/02h (640 KB) */
    mov al, 0x15
    call rd
    cmp al, 0x80
    jne chain_near
    mov al, 0x16
    call rd
    cmp al, 0x02
    jne chain_near
    call sum                    /* dx = sum 10h..2Dh */
.if TEST == 1
    cmp byte ptr [0x4F4], 0x55  /* TEST ONLY: give QEMU's CMOS a valid checksum on the 1st boot */
    je 9f
    mov byte ptr [0x4F4], 0x55
    mov al, 0x2E
    mov ah, dh
    call wr
    mov al, 0x2F
    mov ah, dl
    call wr
9:
.endif
    call stored                 /* bx = [2Eh]:[2Fh] */
    cmp bx, dx
    jne chain_near
    mov al, 0x1B
    call rd
    cmp al, WANT1B
    je chain_near
    jmp over_stamp
    /* helpers and data live in front of the stamp hole, which would otherwise be padding */
rd: out 0x70, al
    in al, 0x84
    in al, 0x71
    ret
wr: out 0x70, al
    in al, 0x84
    mov al, ah
    out 0x71, al
    in al, 0x84
    ret
sum:                                        /* dx = sum of CMOS 10h..2Dh */
    xor dx, dx
    mov bl, 0x10
7:  mov al, bl
    call rd
    xor ah, ah
    add dx, ax
    inc bl
    cmp bl, 0x2E
    jne 7b
    ret
stored:                                     /* bx = [2Eh] << 8 | [2Fh] */
    mov al, 0x2E
    call rd
    mov bh, al
    mov al, 0x2F
    call rd
    mov bl, al
    ret
s_compaq: .ascii "COMPAQ"
s_date:   .ascii "04/25/97"
drive:    .byte 0x80
puts:
    lodsb
    or al, al
    jz 8f
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp puts
8:  ret

    /* 0xDA..0xDF: the Win9x disk stamp - the installer keeps the disk's own bytes */
    .org 0xDA
    .byte 0,0,0,0,0,0
over_stamp:
    xor ah, ah
    sub dx, ax                  /* drop old 1Bh from the sum */
    mov al, 0x2D
    call rd
    mov cl, al
    xor ch, ch
    sub dx, cx
    or cl, 0x08
    add dx, cx
    add dx, WANT1B
    cli
    mov ax, 0x1B | (WANT1B << 8)
    call wr
    mov al, 0x2D
    mov ah, cl
    call wr
    mov al, 0x2E
    mov ah, dh
    call wr
    mov al, 0x2F
    mov ah, dl
    call wr
    sti
    /* read back: 1Bh, 2Dh, and a valid checksum */
    mov al, 0x1B
    call rd
    cmp al, WANT1B
    jne chain
    mov al, 0x2D
    call rd
    cmp al, cl
    jne chain
    call sum
    call stored
    cmp bx, dx
    jne chain
    mov si, offset m_fix
    call puts
    mov dword ptr [0x4F0], 0x4652424D
    mov word ptr [0x472], 0x1234            /* warm boot flag */
3:  in al, 0x64
    test al, 2
    jnz 3b
    mov al, 0xFE
    out 0x64, al                            /* pulse reset */
    .byte 0xEA                              /* fallback: jmp FFFF:0000 */
    .word 0, 0xFFFF

chain:
    mov bp, 3
4:  mov ax, 0x0201
    mov bx, 0x7C00
    mov cx, SAVESEC                         /* cyl 0, sector SAVESEC = LBA SAVESEC-1 */
    xor dh, dh
    mov dl, [drive]
    int 0x13
    jnc 5f
    xor ax, ax
    int 0x13
    dec bp
    jnz 4b
    jmp fail
5:  cmp word ptr [0x7DFE], 0xAA55
    jne fail
    mov si, 0x0600 + 0x1BE                  /* live partition table + 55AA */
    mov di, 0x7C00 + 0x1BE
    mov cx, 33
    rep movsw
    mov si, 0x0600 + 0xDA                   /* live disk stamp */
    mov di, 0x7C00 + 0xDA
    mov cx, 3
    rep movsw
    mov dl, [drive]
    .byte 0xEA
    .word 0x7C00, 0
fail:
    mov si, offset m_fail
    call puts
6:  hlt
    jmp 6b

m_fix:    .asciz "CMOS fix\r\n"
m_fail:   .asciz "no MBR"
    .org 0x1B8

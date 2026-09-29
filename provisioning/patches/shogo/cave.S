/*
 * Shogo 2.2 CShell.dll v3 (carved from SHOGOP3.REZ @0x2bca8, md5
 * eb0286632a78bd488bd0f41f37114c4d) - bounded resolution append.
 *
 * OUR code, written 2026-09-29 for the lcd1080 lane. Assembled at
 * CAVE_VA = 0x1006a320 (the zero slack after .text's VirtualSize 0x69315,
 * inside its raw size 0x69400). Reached by a 5-byte `jmp` written over the
 * original unbounded append at 0x10003546..0x1000356b; returns to 0x1000356c,
 * the original fall-through, exactly as the original block did.
 *
 * Register state on entry (the original block's, unchanged):
 *   ebp = &device.count  (device+0x108); entries at ebp+4, 16 bytes each,
 *         12 slots (device ctor 0x10003a90 zeroes exactly 12)
 *   edi = RMode*         (width +0x194, height +0x198)
 *   eax, ecx, edx dead   (the original clobbers all three; 0x1000356c reloads)
 *   [esp+0x12] = the "device matched" flag the original sets to 1
 *
 * Behaviour: count < 12 -> the original append, byte for byte.
 * count >= 12 -> find the entry with the smallest w*h (first one on a tie);
 * if the new mode's area is not larger, drop the new mode; otherwise remove
 * that entry (shift the later entries down one slot, order preserved), set
 * count = 11 and append. Invariant: count <= 12, so nothing is ever written
 * past slot 11 (device+0x1cb) into the next device record, and the table holds
 * the 12 largest-area modes seen, in enumeration order.
 */
        .intel_syntax noprefix
        .text
        .globl cave
cave:
        mov     byte ptr [esp+0x12], 1
        mov     eax, dword ptr [ebp]
        cmp     eax, 12
        jb      append
        push    ebx
        push    esi
        push    edi
        mov     esi, dword ptr [edi+0x194]
        imul    esi, dword ptr [edi+0x198]      /* esi = new area */
        xor     ecx, ecx                        /* i */
        or      ebx, -1                         /* min area */
        xor     edx, edx                        /* min index */
scan:
        mov     eax, ecx
        shl     eax, 4
        mov     edi, dword ptr [ebp+eax+4]
        imul    edi, dword ptr [ebp+eax+8]
        cmp     edi, ebx
        jae     next
        mov     ebx, edi
        mov     edx, ecx
next:
        inc     ecx
        cmp     ecx, 12
        jb      scan
        cmp     esi, ebx
        jbe     drop
        lea     ecx, [edx*4]                    /* dword index of the victim */
shift:
        cmp     ecx, 44                         /* 11 entries * 4 dwords */
        jae     shifted
        mov     eax, dword ptr [ebp+ecx*4+0x14]
        mov     dword ptr [ebp+ecx*4+4], eax
        inc     ecx
        jmp     shift
shifted:
        mov     dword ptr [ebp], 11
        pop     edi
        pop     esi
        pop     ebx
append:                                          /* == original 0x10003546.. */
        mov     eax, dword ptr [ebp]
        mov     ecx, eax
        shl     ecx, 4
        inc     eax
        lea     ecx, [ecx+ebp+4]
        mov     dword ptr [ebp], eax
        mov     edx, dword ptr [edi+0x194]
        mov     dword ptr [ecx], edx
        mov     eax, dword ptr [edi+0x198]
        mov     dword ptr [ecx+4], eax
        jmp     back
drop:
        pop     edi
        pop     esi
        pop     ebx
        jmp     back
        .set    back, cave + (0x1000356c - 0x1006a320)

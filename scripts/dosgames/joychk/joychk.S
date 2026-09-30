/* JOYCHK.COM - is a joystick plugged into the game port?  ERRORLEVEL 1 = yes, 0 = no.
 *
 * WHY (2026-09-30). MicroProse's F-19 and F-117A ask "Do you have a joystick
 * (Y/N)?" at every start, in text mode, so an unattended launch sat windowed at
 * that question (Win98 build VM sweep). Answering it in the launcher fixes
 * that - but a fixed "N" would switch off the gameport stick .243 has, and a
 * fixed "Y" makes a box with no stick wait forever in the calibration that
 * polls for its fire buttons. So the launcher asks the port:
 *
 *     JOYCHK
 *     if errorlevel 1 goto joy
 *
 * How: a write to 201h fires the stick's one-shots; each axis bit reads 1
 * until its RC delay (~1 ms for a 100K pot) runs out, then 0. With no stick
 * the circuit is open and the bits stay 1; with no game port at all the port
 * reads FFh. So stick A is present when its X and Y bits (0 and 1) both drop
 * to 0 within the poll budget (65535 port reads - tens of ms on an ISA bus).
 *
 * Build: as --32 -o joychk.o joychk.S && ld -m elf_i386 -Ttext 0x100 --oformat binary -o JOYCHK.COM joychk.o
 */
        .code16
        .intel_syntax noprefix
        .text
        .globl _start
_start:
        mov     dx, 0x201
        out     dx, al
        mov     cx, 0xFFFF
1:      in      al, dx
        test    al, 0x03
        jz      2f
        loop    1b
        mov     ax, 0x4C00          /* no stick: ERRORLEVEL 0 */
        int     0x21
2:      mov     ax, 0x4C01          /* both axes timed out: a stick is there */
        int     0x21

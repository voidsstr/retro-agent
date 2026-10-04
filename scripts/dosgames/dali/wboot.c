/*
 * WBOOT.COM - warm-reboot the PC. Used after a real-DOS LAN game, so Windows
 * never starts with the packet driver still resident (a Crynwr driver cannot
 * unload itself). Sets the BIOS warm-boot flag (0040:0072 = 1234h, which skips
 * the memory test), then pulses the keyboard controller's reset line; if that
 * does nothing, jumps to the reset vector.
 *
 * FLUSH FIRST (2026-10-04). DOS keeps file writes in its BUFFERS and writes
 * them back lazily, so a reset straight after a program exits can lose what it
 * just wrote: on .243 a check's output file vanished and a batch that DOS had
 * deleted came back. Descent saves the pilot file when you quit and this runs
 * right after it. So: INT 21h AH=0Dh (disk reset = write every dirty buffer),
 * ask a write-back cache to commit (SMARTDRV's INT 2Fh AX=4A10h BX=1 - no-op
 * when none is loaded), wait ~2 s for the drive, and only then reset.
 */
#include <conio.h>
#include <dos.h>

static void flush_and_settle(void)
{
    union REGS r;
    volatile unsigned long far *tick = (volatile unsigned long far *)MK_FP(0x40, 0x6C);
    unsigned long start;

    r.h.ah = 0x0D;                      /* DOS: flush all file buffers */
    intdos(&r, &r);
    r.x.ax = 0x4A10;                    /* SMARTDRV: commit the cache */
    r.x.bx = 0x0001;
    int86(0x2F, &r, &r);
    r.h.ah = 0x0D;                      /* ...and DOS once more */
    intdos(&r, &r);
    start = *tick;
    while (*tick - start < 37UL && *tick >= start)   /* ~2 s at 18.2 Hz */
        ;
}

int main(void)
{
    unsigned far *flag = (unsigned far *)MK_FP(0x40, 0x72);
    flush_and_settle();
    *flag = 0x1234;
    outp(0x64, 0xFE);
    {
        void (far *reset)(void) = (void (far *)(void))MK_FP(0xFFFF, 0);
        reset();
    }
    return 0;
}

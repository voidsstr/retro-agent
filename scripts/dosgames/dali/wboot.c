/*
 * WBOOT.COM - warm-reboot the PC. Used after a real-DOS LAN game, so Windows
 * never starts with the packet driver still resident (a Crynwr driver cannot
 * unload itself). Sets the BIOS warm-boot flag (0040:0072 = 1234h, which skips
 * the memory test), then pulses the keyboard controller's reset line; if that
 * does nothing, jumps to the reset vector.
 */
#include <conio.h>
#include <dos.h>

int main(void)
{
    unsigned far *flag = (unsigned far *)MK_FP(0x40, 0x72);
    *flag = 0x1234;
    outp(0x64, 0xFE);
    {
        void (far *reset)(void) = (void (far *)(void))MK_FP(0xFFFF, 0);
        reset();
    }
    return 0;
}

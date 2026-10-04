/* sbchk - is a Sound Blaster DSP answering at 220h, and an OPL FM chip at
 * 388h, in REAL DOS? (2026-10-04, .243)
 *
 * The P1's SB16 is an ISA Plug-and-Play card. Windows configures it at every
 * boot, but real DOS (the rundos route, before Windows) loads no Creative
 * CTCM/CTCU, so unless the BIOS configured it the card is switched off and a
 * game's sound init can wait forever - Descent stopped at "LOADING DATA...".
 * Prints what it finds; errorlevel 0 = DSP ready, 1 = no DSP, 2 = DSP but no FM.
 */
#include <stdio.h>
#include <conio.h>

static int dsp_reset(unsigned base, unsigned *ver)
{
    unsigned long i;
    int v;
    outp(base + 6, 1);
    for (i = 0; i < 2000; i++)
        inp(base + 6);                      /* > 3 us */
    outp(base + 6, 0);
    for (i = 0; i < 200000UL; i++) {
        if (inp(base + 0xE) & 0x80) {
            if (inp(base + 0xA) == 0xAA)
                break;
        }
    }
    if (i == 200000UL)
        return 0;
    /* DSP version: command E1h */
    for (i = 0; i < 100000UL && (inp(base + 0xC) & 0x80); i++)
        ;
    outp(base + 0xC, 0xE1);
    v = 0;
    for (i = 0; i < 100000UL; i++)
        if (inp(base + 0xE) & 0x80) { v = inp(base + 0xA) << 8; break; }
    for (i = 0; i < 100000UL; i++)
        if (inp(base + 0xE) & 0x80) { v |= inp(base + 0xA); break; }
    *ver = (unsigned)v;
    return 1;
}

static int opl_present(void)
{
    /* the classic AdLib timer test */
    int s1, s2, i;
    outp(0x388, 4); for (i = 0; i < 6; i++) inp(0x388);
    outp(0x389, 0x60); for (i = 0; i < 35; i++) inp(0x388);
    outp(0x388, 4); for (i = 0; i < 6; i++) inp(0x388);
    outp(0x389, 0x80); for (i = 0; i < 35; i++) inp(0x388);
    s1 = inp(0x388);
    outp(0x388, 2); for (i = 0; i < 6; i++) inp(0x388);
    outp(0x389, 0xFF); for (i = 0; i < 35; i++) inp(0x388);
    outp(0x388, 4); for (i = 0; i < 6; i++) inp(0x388);
    outp(0x389, 0x21); for (i = 0; i < 400; i++) inp(0x388);
    s2 = inp(0x388);
    outp(0x388, 4); for (i = 0; i < 6; i++) inp(0x388);
    outp(0x389, 0x60); for (i = 0; i < 35; i++) inp(0x388);
    outp(0x388, 4); for (i = 0; i < 6; i++) inp(0x388);
    outp(0x389, 0x80);
    return (s1 & 0xE0) == 0x00 && (s2 & 0xE0) == 0xC0;
}

int main(void)
{
    unsigned ver = 0;
    int dsp = dsp_reset(0x220, &ver);
    int fm = opl_present();
    printf("SBCHK: DSP at 220h %s", dsp ? "READY" : "NOT ANSWERING");
    if (dsp)
        printf(" (DSP version %u.%02u)", ver >> 8, ver & 0xFF);
    printf("; OPL FM at 388h %s\n", fm ? "PRESENT" : "NOT FOUND");
    return dsp ? (fm ? 0 : 2) : 1;
}

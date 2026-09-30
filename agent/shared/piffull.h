/*
 * piffull.h - make a Windows 9x DOS shortcut (.pif) open FULL SCREEN (agent 1.96.1)
 *
 * On Windows 9x a shortcut to an MS-DOS program - every staged `Play <Game>.bat`
 * - is saved by the shell as a .pif built from _DEFAULT.PIF, which opens the
 * DOS box in a WINDOW. A game that switches to a graphics mode takes the box
 * full screen by itself, so this hid for a while; a title whose launcher starts
 * in TEXT mode sat in a small window over the desktop instead. The Win98 build
 * VM's launch sweep (2026-09-30) found five: F-19 and F-117A at their "Do you
 * have a joystick (Y/N)?" question, Frontier at its sound menu, Alone in the
 * Dark at its Infogrames front end, Falcon 3.0 behind its MS-DOS mode dialog.
 * Every staged game must run full screen (CLAUDE.md), so GAMESYNC sets the
 * PIF's own "full screen" option on every DOS shortcut it writes.
 *
 * The option is bit 3 of the flags dword at offset 10h of the PIF's
 * "WINDOWS 386 3.0" section - the Windows 3.x "Display Usage: Full Screen" bit,
 * which Windows 9x's Screen > Usage still reads. Measured in the VM: the
 * shell-written F-19 PIF carries 0x00021002 there (file offset 1ADh); with
 * 0x0002100A the same launcher's text-mode question came up full screen, and
 * Quake II's launcher (a batch that STARTs a Win32 Glide game) still ran the
 * game normally with it set.
 *
 * The section is FOUND by walking the heading chain from 171h, not assumed at
 * a fixed offset: an offset that is right for one PIF layout reads garbage in
 * another (the SafeDisc version note in CLAUDE.md is the same lesson).
 * Anything unexpected -> leave the file alone.
 *
 * Win32-free so tests/native/test_piffull.c compiles it.
 */
#ifndef RETRO_PIFFULL_H
#define RETRO_PIFFULL_H

#include <stddef.h>
#include <string.h>

#ifdef __GNUC__
#define PIFF_UNUSED __attribute__((unused))
#else
#define PIFF_UNUSED
#endif

#define PIFF_FIRST_HEADING  0x171u      /* after the 369-byte basic section */
#define PIFF_HEADING_LEN    22u         /* 16-byte name + next, data offset, data length */
#define PIFF_W386_FLAGS     0x10u       /* flags dword within the W386 data */
#define PIFF_FULLSCREEN     0x08u       /* bit 3 of that dword's first byte */
#define PIFF_MAX_SECTIONS   16

static unsigned piff_u16(const unsigned char *b)
{
    return (unsigned)b[0] | ((unsigned)b[1] << 8);
}

/* File offset of the WINDOWS 386 3.0 flags dword, or -1 when the bytes are not
 * a Windows PIF this understands (no MICROSOFT PIFEX heading first, a chain
 * that loops or runs off the end, no W386 section, a W386 section too short). */
PIFF_UNUSED static long piff_w386_flags_offset(const unsigned char *b, size_t n)
{
    size_t off = PIFF_FIRST_HEADING;
    int i;
    if (!b || n < PIFF_FIRST_HEADING + PIFF_HEADING_LEN)
        return -1;
    if (memcmp(b + off, "MICROSOFT PIFEX", 15) != 0)
        return -1;
    for (i = 0; i < PIFF_MAX_SECTIONS; i++) {
        unsigned next, doff, dlen;
        if (off + PIFF_HEADING_LEN > n)
            return -1;
        next = piff_u16(b + off + 16);
        doff = piff_u16(b + off + 18);
        dlen = piff_u16(b + off + 20);
        if (memcmp(b + off, "WINDOWS 386 3.0", 15) == 0) {
            if (dlen < PIFF_W386_FLAGS + 4 || (size_t)doff + PIFF_W386_FLAGS + 4 > n)
                return -1;
            return (long)(doff + PIFF_W386_FLAGS);
        }
        if (next == 0xFFFFu || next <= off)     /* the end, or a loop */
            return -1;
        off = next;
    }
    return -1;
}

/* 1 = the full-screen bit was clear and is now set in b (write the file back),
 * 0 = it was already set (leave the file alone), -1 = not a PIF this knows. */
PIFF_UNUSED static int piff_set_fullscreen(unsigned char *b, size_t n)
{
    long at = piff_w386_flags_offset(b, n);
    if (at < 0)
        return -1;
    if (b[at] & PIFF_FULLSCREEN)
        return 0;
    b[at] |= PIFF_FULLSCREEN;
    return 1;
}

/* Is this file name a .pif (any case)? GAMESYNC asked the shell for <name>.lnk;
 * on Windows 9x a DOS target comes back as <name>.pif. */
PIFF_UNUSED static int piff_is_pif_name(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    const char *e;
    if (n < 4)
        return 0;
    e = path + n - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'p' && (e[2] | 0x20) == 'i' && (e[3] | 0x20) == 'f';
}

#endif /* RETRO_PIFFULL_H */

/*
 * ASKIP.COM - ask for the IP address of the machine hosting a DOSBox IPX
 * game, for DALI on a real-DOS fleet box (.243, the Pentium 166).
 *
 *   ASKIP <default-file> <out.bat>
 *
 * Writes ONE line,  SET HOSTIP=<a.b.c.d>  into <out.bat>, which the caller
 * CALLs. COMMAND.COM has no "set /p", which is why this exists at all.
 *
 * NEVER redirect its stdout instead: DOS's buffered line input (INT 21h
 * AH=0Ah) ECHOES what is typed to STDOUT, so `ASKIP ... > HOSTIP.BAT` put
 * the keystrokes into the batch file and none on the screen - the typed
 * address was invisible and the CALL ran the echo as commands ("Bad command
 * or file name" x3, HOSTIP empty; measured in the W98BUILD VM, 2026-10-01).
 * The prompts go to handle 2 and stdout stays the console.
 *
 * <default-file> holds the last host: the first token of its first line that
 * does not start with ';' or '#' (the same rule the XP launchers use for
 * lanhost.txt). Pressing Enter takes it; a typed address replaces it and is
 * written back, so the next LAN game offers it again. Q, or an empty answer
 * with no default, exits with errorlevel 1 and writes nothing - the caller
 * then skips the game instead of hanging.
 *
 * Exit: 0 = SET line written, 1 = no host given.
 */
#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <bios.h>
#include "askipcore.h"

static void say(const char *s)
{
    union REGS r;
    struct SREGS sr;
    segread(&sr);
    r.h.ah = 0x40;
    r.x.bx = 2;                         /* stderr = the console */
    r.x.cx = (unsigned)strlen(s);
    r.x.dx = (unsigned)s;
    intdosx(&r, &r, &sr);
}

int main(int argc, char **argv)
{
    char def[16], buf[20];
    unsigned char kb[20];
    union REGS r;
    struct SREGS sr;
    const char *f = argc > 1 ? argv[1] : 0;
    const char *outbat = argc > 2 ? argv[2] : 0;
    FILE *ob;

    def[0] = 0;
    if (f)
        askip_read_default(f, def);
    say("\r\n  DESCENT LAN GAME (real DOS)\r\n\r\n"
        "  On the machine HOSTING the game, its screen shows its IP address.\r\n"
        "  Start the game there first, then type that address here.\r\n\r\n");
    /* A remembered host joins by itself after a countdown (askipcore.h). */
    if (def[0] && ASKIP_WAIT_TICKS) {
        unsigned long far *tick = (unsigned long far *)MK_FP(0x40, 0x6C);
        unsigned long start = *tick, left, shown = 0xFFFFFFFFul;
        say("  Joining ");
        say(def);
        say(" - press Enter to join now, any other key to type another address.\r\n");
        for (;;) {
            left = askip_ticks_left(start, *tick, ASKIP_WAIT_TICKS);
            if (left / 18 != shown) {
                char sec[8];
                shown = left / 18;
                sprintf(sec, "\r  %2lu ", shown);
                say(sec);
            }
            if (_bios_keybrd(_KEYBRD_READY)) {   /* INT 16h AH=01h: a key is waiting */
                unsigned k = _bios_keybrd(_KEYBRD_READ);
                say("\r\n");
                if (askip_countdown_key_joins((unsigned char)(k & 0xFF))) {
                    strcpy(buf, def);
                    goto have;
                }
                break;                      /* type another address */
            }
            if (!left) {
                say("\r\n");
                strcpy(buf, def);
                goto have;
            }
        }
    }
    for (;;) {
        say("  Host IP address");
        if (def[0]) {
            say(" [");
            say(def);
            say("]");
        }
        say(" (Q = cancel): ");
        kb[0] = 16;                     /* max 15 chars + CR */
        kb[1] = 0;
        segread(&sr);
        r.h.ah = 0x0A;
        r.x.dx = (unsigned)kb;
        intdosx(&r, &r, &sr);
        say("\r\n");
        memcpy(buf, kb + 2, kb[1]);
        buf[kb[1]] = 0;
        if (buf[0] == 0) {
            if (!def[0]) {
                say("  No host given - the LAN game is cancelled.\r\n");
                return 1;
            }
            strcpy(buf, def);
        }
        if (askip_is_cancel(buf)) {
            say("  Cancelled - no LAN game this time.\r\n");
            return 1;
        }
        if (ipok(buf))
            break;
        say("  That is not an IP address (four numbers 0-255 with dots).\r\n");
    }
have:
    if (f && strcmp(buf, def) != 0) {
        FILE *fp = fopen(f, "w");
        if (fp) {
            fprintf(fp, "%s\n", buf);
            fclose(fp);
        }
    }
    if (!outbat) {
        say("  ASKIP: no output batch file given.\r\n");
        return 1;
    }
    ob = fopen(outbat, "w");
    if (!ob) {
        say("  ASKIP: cannot write ");
        say(outbat);
        say("\r\n");
        return 1;
    }
    fprintf(ob, "SET HOSTIP=%s\n", buf);
    fclose(ob);
    return 0;
}

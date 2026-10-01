/*
 * test_askip.c - ASKIP.COM's parsing (scripts/dosgames/dali/askipcore.h),
 * the host-IP prompt of the P1's real-DOS DALI LAN launcher (2026-10-01).
 * Compiles the same header the DOS program uses.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../scripts/dosgames/dali/askipcore.h"

static void write_file(const char *p, const char *s)
{
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(s, f);
    fclose(f);
}

int main(void)
{
    char out[16];
    const char *tmp = "test_askip_lanhost.tmp";

    assert(ipok("192.168.1.123"));
    assert(ipok("0.0.0.0"));
    assert(ipok("255.255.255.255"));
    assert(!ipok("256.1.1.1"));
    assert(!ipok("192.168.1"));
    assert(!ipok("192.168.1.1.1"));
    assert(!ipok("192.168..1"));
    assert(!ipok("192.168.1.1 "));
    assert(!ipok("1234.1.1.1"));
    assert(!ipok(""));
    assert(!ipok("host"));

    /* the XP launchers' lanhost.txt rule: first token of the first line that
     * is not a ; or # comment */
    write_file(tmp, "; fleet host\n# another comment\n\n192.168.1.124  the P3\n10.0.0.1\n");
    askip_read_default(tmp, out);
    assert(strcmp(out, "192.168.1.124") == 0);

    write_file(tmp, "192.168.1.123\r\n");          /* CRLF as DOS writes it */
    askip_read_default(tmp, out);
    assert(strcmp(out, "192.168.1.123") == 0);

    write_file(tmp, "not-an-ip\n");                /* garbage is no default */
    askip_read_default(tmp, out);
    assert(out[0] == 0);

    askip_read_default("no-such-file.tmp", out);   /* absent file: no default */
    assert(out[0] == 0);
    remove(tmp);

    assert(askip_is_cancel("q") && askip_is_cancel("Q") && askip_is_cancel("x"));
    assert(!askip_is_cancel("") && !askip_is_cancel("qq") && !askip_is_cancel("1"));

    /* the countdown (2026-10-01): a remembered host joins by itself unless a
     * key is pressed; Enter joins now; the BIOS tick counter wraps at midnight */
    assert(ASKIP_WAIT_TICKS >= 91);                    /* at least ~5 s to react */
    assert(askip_countdown_key_joins('\r'));
    assert(!askip_countdown_key_joins('1') && !askip_countdown_key_joins(27));
    assert(askip_ticks_left(100, 100, 182) == 182);
    assert(askip_ticks_left(100, 200, 182) == 82);
    assert(askip_ticks_left(100, 400, 182) == 0);
    assert(askip_ticks_left(0x1800A0ul, 50, 182) == 182 - (50 + 0x10));   /* across midnight */

    printf("test_askip: OK\n");
    return 0;
}

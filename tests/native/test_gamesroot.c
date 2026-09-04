/*
 * test_gamesroot.c - the GamesRoot override validator.
 *
 * Fix: agent/src/gamesync.c:gs_dest_path() / agent/shared/gamesroot.h,
 * agent v1.80.0. Games used to land at a hardcoded "C:\Games", so a box
 * whose C: was full could not keep its library anywhere else - moving the
 * trees by hand worked until the next GAMESYNC put them back on C:.
 *
 * Every case here asserts BOTH the accepted value and that the rejected
 * forms are rejected, because the failure this guards against is silent: an
 * honoured bad root sends a 32 GB copy somewhere nobody looks and GAMESYNC
 * still reports state=done.
 */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/gamesroot.h"

static int fails;

static void ok_root(const char *in, const char *want)
{
    char out[260];
    memset(out, 0, sizeof(out));
    if (!gr_valid_root(in, out, sizeof(out))) {
        printf("FAIL: %-20s rejected, expected %s\n", in, want);
        fails++;
        return;
    }
    if (strcmp(out, want) != 0) {
        printf("FAIL: %-20s -> \"%s\", expected \"%s\"\n", in, out, want);
        fails++;
        return;
    }
    printf("ok  : %-20s -> %s\n", in, out);
}

static void bad_root(const char *in, const char *why)
{
    char out[260];
    strcpy(out, "UNTOUCHED");
    if (gr_valid_root(in, out, sizeof(out))) {
        printf("FAIL: %-20s accepted as \"%s\" (%s)\n", in, out, why);
        fails++;
        return;
    }
    if (strcmp(out, "UNTOUCHED") != 0) {
        printf("FAIL: %-20s rejected but wrote \"%s\"\n", in, out);
        fails++;
        return;
    }
    printf("ok  : %-20s rejected (%s)\n", in, why);
}

int main(void)
{
    /* the case that motivated this: 180 MB free on C:, 36 GB free on D: */
    ok_root("D:\\Games", "D:\\Games");
    ok_root("d:\\games", "d:\\games");
    /* a trailing separator is what a person types - strip it, do not refuse */
    ok_root("D:\\Games\\", "D:\\Games");
    ok_root("E:\\retro\\Games", "E:\\retro\\Games");

    bad_root("", "empty - the value was never set");
    bad_root("D:", "bare drive, no separator");
    bad_root("D:\\", "volume root - would mix game trees with everything else");
    bad_root("Games", "relative - resolves against whatever cwd the agent has");
    bad_root("\\\\nas\\files\\Games", "UNC - the library is the SOURCE, not the destination");
    bad_root("\\Games", "rooted but no drive");
    bad_root("4:\\Games", "not a drive letter");

    /* out too small must refuse rather than truncate a path mid-directory */
    {
        char small[16];
        strcpy(small, "UNTOUCHED");
        if (gr_valid_root("D:\\a\\very\\long\\path\\indeed", small, sizeof(small))) {
            printf("FAIL: truncating write accepted\n");
            fails++;
        } else {
            printf("ok  : oversized root rejected rather than truncated\n");
        }
    }

    if (fails) {
        printf("\n%d FAILURE(S)\n", fails);
        return 1;
    }
    printf("\nall gamesroot cases pass\n");
    return 0;
}

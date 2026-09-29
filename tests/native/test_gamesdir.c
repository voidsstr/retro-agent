/* agent/shared/gamesdir.h - which GamesDir values GAMESYNC accepts (agent
 * 1.93.0). .243's C: is 1.2 GB with 333 MB free and its games now go to a
 * 72 GB E:. A value this rejects makes GAMESYNC refuse to sync - it never
 * falls back to C:, which the setting exists to protect. */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/gamesdir.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

static int ok(const char *v, const char *want_out, const char *want_root)
{
    char out[200], root[4];
    if (!gamesdir_normalise(v, out, sizeof(out), root)) return 0;
    return strcmp(out, want_out) == 0 && strcmp(root, want_root) == 0;
}
static int bad(const char *v)
{
    char out[200], root[4];
    return !gamesdir_normalise(v, out, sizeof(out), root);
}

int main(void)
{
    char big[300], out[8], root[4];
    CHECK(ok("E:\\GAMES", "E:\\GAMES", "E:\\"), ".243's value E:\\GAMES is accepted, root E:\\");
    CHECK(ok("e:\\games\\\\", "e:\\games", "e:\\"), "trailing backslashes are dropped");
    CHECK(ok("C:\\Games", "C:\\Games", "C:\\"), "the default itself is valid");
    CHECK(ok("D:\\Win Games\\Library", "D:\\Win Games\\Library", "D:\\"), "spaces and a nested folder are fine");
    CHECK(bad("E:\\"), "a bare drive root is not a games folder");
    CHECK(bad("E:GAMES"), "a drive-relative path is refused");
    CHECK(bad("\\\\192.168.1.122\\files\\Games"), "a UNC path is refused (games are copied TO a local disk)");
    CHECK(bad("GAMES"), "a relative path is refused");
    CHECK(bad("E:\\..\\Windows"), "'..' is refused");
    CHECK(bad("E:/GAMES"), "a forward slash is refused");
    CHECK(bad("E:\\GA*ES") && bad("E:\\GAMES?") && bad("E:\\\"G\"") && bad("E:\\A<B") && bad("E:\\A|B"),
          "wildcards, quotes and redirection characters are refused");
    CHECK(bad("E:\\A:B"), "a second colon is refused");
    CHECK(bad("E:\\A\\\\B"), "an empty path component (\\\\) is refused");
    CHECK(bad("1:\\GAMES") && bad("::\\GAMES"), "the drive must be a letter");
    CHECK(bad("E:\\G\tAMES"), "control characters are refused");
    CHECK(bad(""), "an empty value is refused");
    memset(big, 'A', sizeof(big) - 1); big[sizeof(big) - 1] = 0; big[0] = 'E'; big[1] = ':'; big[2] = '\\';
    CHECK(bad(big), "a path longer than 259 characters is refused");
    CHECK(!gamesdir_normalise("E:\\GAMES", out, sizeof(out), root), "a result that does not fit the caller's buffer is refused");
    printf("-- gamesdir (agent 1.93.0): %d/%d tests passed --\n", runs - fails, runs);
    return fails ? 1 : 0;
}

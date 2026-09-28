/* agent/shared/launchline.h (agent 1.90.1): a Win9x LAUNCH of an .exe skips
 * command.com - 1.89.1's CREATE_NEW_CONSOLE made Win98 refuse any
 * "command.com /c" line over 127 characters (CreateProcess error 31, .243). */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/launchline.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

int main(void)
{
    char e[260];
    CHECK(launchline_direct_exe("C:\\GAMES\\Quake2Win9x\\quake2.exe +set basedir C:\\GAMES\\Quake2Win9x +set vid_ref gl +set gl_driver 3dfxgl +set gl_mode 3 +exec bench.cfg", e, sizeof(e))
          && !strcmp(e, "C:\\GAMES\\Quake2Win9x\\quake2.exe"), "a long Quake II line goes direct (the case that failed)");
    CHECK(launchline_direct_exe("\"C:\\Program Files\\X\\Y.EXE\" -a", e, sizeof(e)) && !strcmp(e, "C:\\Program Files\\X\\Y.EXE"),
          "a quoted path with spaces");
    CHECK(!launchline_direct_exe("W:\\FILES\\GAMES\\DOSFILL\\FILL.BAT", e, sizeof(e)), "a batch keeps command.com");
    CHECK(!launchline_direct_exe("C:\\DOSGAME\\DOSGAME.COM", e, sizeof(e)), "a .com keeps command.com");
    CHECK(!launchline_direct_exe("C:\\X\\A.EXE > C:\\OUT.TXT", e, sizeof(e)), "a redirect needs the shell");
    CHECK(!launchline_direct_exe("C:\\X\\A.EXE & C:\\B.EXE", e, sizeof(e)), "a chain needs the shell");
    CHECK(!launchline_direct_exe("\"C:\\unterminated.exe", e, sizeof(e)), "an unterminated quote is not guessed at");
    CHECK(!launchline_direct_exe("", e, sizeof(e)) && !launchline_direct_exe(NULL, e, sizeof(e)), "empty");
    printf("-- launchline (Win9x LAUNCH of an .exe skips command.com, agent 1.90.1): %d/%d tests passed --\n", runs - fails, runs);
    return fails ? 1 : 0;
}

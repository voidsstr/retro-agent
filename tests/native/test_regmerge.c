/* agent/shared/regmerge.h - GAMESYNC's install.reg merge on Windows 9x
 * (agent/src/gamesync.c: gs_merge_reg / gs_run_regedit, the fix after 1.89.1).
 *
 * 2026-09-28, .243 (Win98 SE, agent 1.86.1):
 *     HexenII: cannot run regedit (0) - game may not launch
 * gs_merge_reg() handed CreateProcessA the bare "regedit /s \"<long path>\""
 * with creation flags 0 and the inherited current directory, from the IDLE
 * library-sync thread - and Windows 98 refused it without an error code. So no
 * Win9x box had ever had an install.reg merged. The 9x path now names
 * %windir%\REGEDIT.EXE, passes the 8.3 path unquoted, sets the working
 * directory, and starts it CREATE_SUSPENDED - the forms proven on that box -
 * and then READS the .reg back against the registry, writing whatever regedit
 * did not land. This test pins both halves: the command line + flags (fixed
 * and the old failing form), and the REGEDIT4 reader the post-condition rests
 * on (every construct the staged library uses, and the ones it must refuse to
 * guess at). */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/regmerge.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } \
                           else printf("  [ ok ] %s\n", msg); } while (0)

/* The HexenII/install.reg that .243 failed to merge, verbatim in every line
 * that matters to a parser - including a COMMENT that ends in a backslash,
 * which must not swallow the next line. */
static const char HEXEN2_REG[] =
    "REGEDIT4\r\n"
    "\r\n"
    "; Hexen II (Raven Software, retail v1.03) - fleet staging.\r\n"
    ";   RegOpenKeyExA(HKLM, \"SYSTEM\\CurrentControlSet\\Control\\ComputerName\\\r\n"
    ";                        ComputerName\", KEY_READ)\r\n"
    ";   RegQueryValueExA(hkey, \"RAID\", ...)         <- a value in that key\r\n"
    ";   \\SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName]\r\n"
    "\"RAID\"=\"Santa needs a new sled!\"\r\n"
    "\r\n"
    "; REGEDIT4 rather than the v5 dialect so it would still merge on a Win9x box.\r\n";

static int count_entries(const char *text, int *unknown)
{
    rm_parser_t ps;
    rm_entry_t e;
    int n = 0;
    *unknown = 0;
    rm_init(&ps, text, strlen(text));
    while (rm_next(&ps, &e)) {
        n++;
        if (e.op == RM_OP_UNKNOWN)
            (*unknown)++;
    }
    return n;
}

static void t_command_line(void)
{
    char cmd[512];
    unsigned flags = 0xdead;

    printf("[command line and creation flags]\n");

    /* THE FIX: the 9x form. */
    CHECK(rm_build_cmd(1, "C:\\WINDOWS\\REGEDIT.EXE", "C:\\GAMES\\HEXENII\\INSTALL.REG",
                       cmd, sizeof(cmd), &flags) == 0, "9x: builds");
    CHECK(strcmp(cmd, "C:\\WINDOWS\\REGEDIT.EXE /s C:\\GAMES\\HEXENII\\INSTALL.REG") == 0,
          "9x: absolute REGEDIT.EXE, /s, the 8.3 .reg path UNQUOTED");
    CHECK(flags == RM_CREATE_SUSPENDED,
          "9x: CREATE_SUSPENDED and nothing else (resumed after, like the proven REBOOT path)");
    CHECK((flags & (RM_CREATE_NO_WINDOW | RM_CREATE_UNICODE_ENVIRONMENT)) == 0,
          "9x: never CREATE_NO_WINDOW / CREATE_UNICODE_ENVIRONMENT (9x rejects them, error 87)");
    CHECK(strncmp(cmd, "regedit ", 8) != 0,
          "9x: the module is NOT left to the search order any more");

    /* THE OLD FAILING FORM, recorded so it cannot come back unnoticed. */
    CHECK(rm_build_cmd_1861("C:\\Games\\HexenII\\install.reg", cmd, sizeof(cmd), &flags) == 0 &&
          strcmp(cmd, "regedit /s \"C:\\Games\\HexenII\\install.reg\"") == 0 && flags == 0,
          "1.86.1 (the failing call on .243): bare \"regedit\", quoted long path, flags 0");
    {
        char fixed[512];
        unsigned f9 = 0;
        rm_build_cmd(1, "C:\\WINDOWS\\REGEDIT.EXE", "C:\\GAMES\\HEXENII\\INSTALL.REG",
                     fixed, sizeof(fixed), &f9);
        CHECK(strcmp(fixed, cmd) != 0 && f9 != flags,
              "the 9x path no longer produces the 1.86.1 command or flags");
    }

    /* No regedit path on 9x is an error, never a quiet fall-back to "regedit". */
    CHECK(rm_build_cmd(1, NULL, "C:\\X.REG", cmd, sizeof(cmd), &flags) < 0 &&
          rm_build_cmd(1, "", "C:\\X.REG", cmd, sizeof(cmd), &flags) < 0,
          "9x without an absolute REGEDIT.EXE refuses to build a command");

    /* No 8.3 name available: the long path is quoted. */
    CHECK(rm_build_cmd(1, "C:\\WINDOWS\\REGEDIT.EXE", "C:\\Games\\Hexen II\\install.reg",
                       cmd, sizeof(cmd), &flags) == 0 &&
          strcmp(cmd, "C:\\WINDOWS\\REGEDIT.EXE /s \"C:\\Games\\Hexen II\\install.reg\"") == 0,
          "9x: a path that still has a space is quoted");

    /* NT: exactly what every XP box has been merging with. */
    CHECK(rm_build_cmd(0, NULL, "C:\\Games\\HexenII\\install.reg", cmd, sizeof(cmd), &flags) == 0 &&
          strcmp(cmd, "regedit /s \"C:\\Games\\HexenII\\install.reg\"") == 0 && flags == 0,
          "NT: unchanged - regedit /s \"<path>\", flags 0 (proven on every XP box)");

    CHECK(rm_build_cmd(1, "C:\\WINDOWS\\REGEDIT.EXE", "C:\\X.REG", cmd, 16, &flags) < 0,
          "a command that does not fit is refused, not truncated");
    CHECK(rm_build_cmd(0, NULL, "", cmd, sizeof(cmd), &flags) < 0,
          "an empty .reg path is refused");
}

static void t_hexen2(void)
{
    rm_parser_t ps;
    rm_entry_t e;
    int n = 0, unknown = 0;

    printf("[the HexenII install.reg .243 never merged]\n");
    rm_init(&ps, HEXEN2_REG, strlen(HEXEN2_REG));
    CHECK(rm_next(&ps, &e) == 1, "yields its one value");
    CHECK(ps.dialect == RM_DIALECT_REGEDIT4, "dialect REGEDIT4");
    CHECK(e.op == RM_OP_SET && e.root == RM_HKLM, "a SET under HKLM");
    CHECK(strcmp(e.key, "SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName") == 0,
          "the ComputerName key");
    CHECK(strcmp(e.name, "RAID") == 0 && e.type == RM_REG_SZ &&
          e.len == 23 && strcmp((const char *)e.data, "Santa needs a new sled!") == 0,
          "RAID = \"Santa needs a new sled!\" (REG_SZ) - the value the post-condition checks");
    CHECK(rm_next(&ps, &e) == 0, "and nothing else - the backslash-ended comment swallowed nothing");
    n = count_entries(HEXEN2_REG, &unknown);
    CHECK(n == 1 && unknown == 0, "1 entry, 0 not understood");

    /* The post-condition on the key alone would be vacuous: ComputerName\
     * ComputerName exists on every Windows box. It is the VALUE that counts. */
    {
        const unsigned char with_nul[] = "Santa needs a new sled!";
        rm_init(&ps, HEXEN2_REG, strlen(HEXEN2_REG));
        rm_next(&ps, &e);
        CHECK(rm_value_matches(&e, 1, RM_REG_SZ, with_nul, 24),
              "registry string WITH its NUL counted (NT) matches");
        CHECK(rm_value_matches(&e, 1, RM_REG_SZ, with_nul, 23),
              "registry string WITHOUT its NUL counted matches too");
        CHECK(!rm_value_matches(&e, 0, 0, NULL, 0), "value absent -> NOT merged");
        CHECK(!rm_value_matches(&e, 1, RM_REG_SZ, (const unsigned char *)"premerge", 9),
              "a different string (the hardware check's sentinel) -> NOT merged");
        CHECK(!rm_value_matches(&e, 1, RM_REG_BINARY, with_nul, 24), "right bytes, wrong type -> NOT merged");
    }
}

static void t_constructs(void)
{
    static const char REG[] =
        "REGEDIT4\n"
        "\n"
        "[HKEY_CURRENT_USER\\Software\\Valve\\Half-Life\\Settings]\n"
        "\"ScreenWidth\"=dword:00000400\n"
        "\"Path\"=\"C:\\\\Games\\\\HalfLife1 \\\"q\\\"\"\n"
        "@=\"default\"\n"
        "\"DigitalProductID\"=hex:a4,00,00,00,03,\\\n"
        "  00,00,00,36,\\\n"
        "  39\n"
        "\"Expand\"=hex(2):25,00\n"
        "\"StarCD\"=-\n"
        "[-HKEY_CURRENT_USER\\Software\\Gone]\n"
        "\"orphan\"=\"after a deleted key\"\n";
    rm_parser_t ps;
    rm_entry_t e;

    printf("[every construct the staged library uses]\n");
    rm_init(&ps, REG, strlen(REG));

    rm_next(&ps, &e);
    CHECK(e.op == RM_OP_SET && e.root == RM_HKCU && e.type == RM_REG_DWORD && e.len == 4 &&
          e.data[0] == 0x00 && e.data[1] == 0x04 && e.data[2] == 0 && e.data[3] == 0 &&
          strcmp(e.name, "ScreenWidth") == 0, "dword:00000400 -> REG_DWORD 1024, little-endian");

    rm_next(&ps, &e);
    CHECK(e.op == RM_OP_SET && e.type == RM_REG_SZ &&
          strcmp((const char *)e.data, "C:\\Games\\HalfLife1 \"q\"") == 0,
          "string escapes: \\\\ -> \\ and \\\" -> \"");

    rm_next(&ps, &e);
    CHECK(e.op == RM_OP_SET && e.name[0] == 0 && strcmp((const char *)e.data, "default") == 0,
          "@= is the default value (empty name)");

    rm_next(&ps, &e);
    CHECK(e.op == RM_OP_SET && e.type == RM_REG_BINARY && e.len == 10 &&
          e.data[0] == 0xa4 && e.data[4] == 0x03 && e.data[8] == 0x36 && e.data[9] == 0x39,
          "hex: across two continuation lines -> 10 bytes of REG_BINARY");
    CHECK(e.lineno == 7, "a continued entry is reported at the line it BEGINS on");

    rm_next(&ps, &e);
    CHECK(e.op == RM_OP_SET && e.type == RM_REG_EXPAND_SZ && e.len == 2 && e.data[0] == 0x25,
          "hex(2): -> REG_EXPAND_SZ raw bytes");

    rm_next(&ps, &e);
    CHECK(e.op == RM_OP_DELVALUE && strcmp(e.name, "StarCD") == 0, "\"StarCD\"=- deletes the value");

    rm_next(&ps, &e);
    CHECK(e.op == RM_OP_DELKEY && e.root == RM_HKCU && strcmp(e.key, "Software\\Gone") == 0,
          "[-HKEY_...] deletes the key");

    rm_next(&ps, &e);
    CHECK(e.op == RM_OP_UNKNOWN && e.root == RM_ROOT_NONE,
          "a value after a deleted key is not judged");

    CHECK(rm_next(&ps, &e) == 0, "end of file");
}

static void t_refuses_to_guess(void)
{
    int unknown = 0, n;

    printf("[lines it must NOT guess at - never verified, never written]\n");

    n = count_entries("REGEDIT4\n[HKEY_LOCAL_MACHINE\\S]\n\"x\"=\"a\\nb\"\n", &unknown);
    CHECK(n == 1 && unknown == 1, "an escape other than \\\\ or \\\" -> UNKNOWN");

    n = count_entries("REGEDIT4\n\"x\"=\"no key yet\"\n", &unknown);
    CHECK(n == 1 && unknown == 1, "a value before any [key] -> UNKNOWN");

    n = count_entries("REGEDIT4\n[HKEY_BOGUS\\S]\n\"x\"=\"y\"\n", &unknown);
    CHECK(n == 1 && unknown == 1, "a value under an unknown root -> UNKNOWN");

    n = count_entries("REGEDIT4\n[HKEY_LOCAL_MACHINE\\S]\n\"x\"=dword:123456789\n", &unknown);
    CHECK(n == 1 && unknown == 1, "a dword with nine digits -> UNKNOWN");

    n = count_entries("REGEDIT4\n[HKEY_LOCAL_MACHINE\\S]\n\"x\"=\"y\" trailing\n", &unknown);
    CHECK(n == 1 && unknown == 1, "junk after a string -> UNKNOWN");

    n = count_entries("REGEDIT4\n[HKEY_LOCAL_MACHINE\\S]\n\"x\"=hex:zz\n", &unknown);
    CHECK(n == 1 && unknown == 1, "a malformed hex byte -> UNKNOWN");

    n = count_entries("REGEDIT4\n[HKEY_LOCAL_MACHINE\\S]\n\"x\"=unicorn:1\n", &unknown);
    CHECK(n == 1 && unknown == 1, "an unknown data type -> UNKNOWN");

    n = count_entries("[HKEY_LOCAL_MACHINE\\S]\n\"x\"=\"y\"\n", &unknown);
    CHECK(n == 0, "no REGEDIT4 header -> nothing (regedit refuses the whole file)");

    {
        static const char utf16[] = "\xFF\xFEW\0i\0n\0";
        rm_parser_t ps;
        rm_entry_t e;
        rm_init(&ps, utf16, sizeof(utf16) - 1);
        CHECK(rm_next(&ps, &e) == 0, "a UTF-16 (v5 export) file -> nothing, not garbage");
    }
    {
        rm_parser_t ps;
        rm_entry_t e;
        static const char v5[] = "Windows Registry Editor Version 5.00\r\n\r\n"
                                 "[HKEY_LOCAL_MACHINE\\S]\r\n\"x\"=dword:00000001\r\n";
        rm_init(&ps, v5, strlen(v5));
        CHECK(rm_next(&ps, &e) == 1 && ps.dialect == RM_DIALECT_V5 && e.op == RM_OP_SET,
              "an ANSI v5 file parses, and says it is v5");
    }
}

int main(void)
{
    t_command_line();
    t_hexen2();
    t_constructs();
    t_refuses_to_guess();
    printf("-- regmerge (install.reg merge on Win9x, after agent 1.89.1): %d/%d tests passed --\n",
           runs - fails, runs);
    return fails ? 1 : 0;
}

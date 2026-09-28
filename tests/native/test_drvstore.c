/* agent/shared/drvstore.h - the driver-store bucket (agent 1.89.0). The indexer
 * prints it and the agent computes it; this pins the one function both use. */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/drvstore.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

static int is(const char *id, const char *want)
{
    char b[16];
    drvstore_bucket(id, b);
    return strcmp(b, want) == 0;
}

int main(void)
{
    CHECK(is("PCI\\VEN_10DE&DEV_0150&SUBSYS_00000000&REV_A4", "PCI_10DE"), "a GeForce2 goes to PCI_10DE");
    CHECK(is("pci\\ven_8086&dev_1080", "PCI_8086"), "case-insensitive (.110's Intel modem)");
    CHECK(is("PCI\\CC_0C0300", "OTHER"), "a class-code id has no vendor: OTHER");
    CHECK(is("USB\\VID_046D&PID_C00E", "USB_046D"), "a Logitech USB mouse goes to USB_046D");
    CHECK(is("USB\\ROOT_HUB", "OTHER"), "a bare root hub: OTHER");
    CHECK(is("HDAUDIO\\FUNC_01&VEN_10EC&DEV_0880", "HDA_10EC"), "a Realtek HD codec goes to HDA_10EC");
    CHECK(is("*PNP0501", "OTHER") && is("ACPI\\PNP0A03", "OTHER") && is("ISAPNP\\CTL0024", "OTHER"),
          "*PNP, ACPI and ISA ids: OTHER");
    CHECK(is("PCI\\VEN_12G4&DEV_0001", "OTHER"), "a non-hex vendor is not a bucket");
    {
        /* drvstore_match: the WHOLE id, never a prefix of a longer one */
        const char *txt =
            "PCI\\VEN_10DE&DEV_0150&SUBSYS_00011043\tG003\\nv4_go.inf\t-\r\n"
            "PCI\\VEN_10DE&DEV_0150\tG005\\nv4_disp.inf\t01/28/2005,7.1.8.9\r\n"
            "PCI\\VEN_10DE&DEV_01500\tG009\\bogus.inf\t-\r\n"
            "pci\\ven_10de&dev_0150\tG006\\lower.inf\t-\n"
            "PCI\\VEN_10DE&DEV_0150\t\t-\n";
        const char *cur = txt;
        char rel[260];
        int n = 0, got_g005 = 0, got_lower = 0, got_prefix = 0;
        while (drvstore_match(&cur, "PCI\\VEN_10DE&DEV_0150", rel, sizeof(rel))) {
            n++;
            if (strcmp(rel, "G005\\nv4_disp.inf") == 0) got_g005 = 1;
            if (strcmp(rel, "G006\\lower.inf") == 0) got_lower = 1;
            if (strstr(rel, "nv4_go") || strstr(rel, "bogus")) got_prefix = 1;
        }
        CHECK(n == 2 && got_g005 && got_lower, "match: two lines name the bare id (one lower-case), each returned once");
        CHECK(!got_prefix, "match: a longer id (SUBSYS, DEV_01500) is NOT the bare id (a prefix match would take it)");
        cur = txt;
        CHECK(!drvstore_match(&cur, "PCI\\VEN_1002&DEV_5144", rel, sizeof(rel)), "match: an absent id finds nothing");
        cur = NULL;
        CHECK(!drvstore_match(&cur, "X", rel, sizeof(rel)), "match: no bucket text is not a crash");
    }
    CHECK(drvstore_rel_ok("G005\\nv4_disp.inf") && drvstore_rel_ok("I015\\IALMNT5.INF"), "rel_ok: DIR\\FILE.INF");
    CHECK(!drvstore_rel_ok("..\\..\\WINDOWS\\x.inf") && !drvstore_rel_ok("G005\\..\\x.inf"), "rel_ok: no ..");
    CHECK(!drvstore_rel_ok("C:\\D\\G005\\nv4_disp.inf") && !drvstore_rel_ok("\\\\srv\\s\\a.inf"), "rel_ok: no drive, no UNC");
    CHECK(!drvstore_rel_ok("G005\\sub\\a.inf") && !drvstore_rel_ok("a.inf") && !drvstore_rel_ok("G005\\a.sys"),
          "rel_ok: exactly one directory level, and an .inf");
    printf("-- drvstore (driver-store bucket, agent 1.89.0): %d/%d tests passed --\n", runs - fails, runs);
    return fails ? 1 : 0;
}

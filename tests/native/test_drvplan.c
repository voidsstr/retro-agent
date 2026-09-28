/* agent/shared/drvplan.h - every device's driver state, DRIVERS STATUS
 * (agent 1.88.0). The devices below are the fleet's own (.243 Win98, .124/.143
 * XP). The rule that matters most: a family-id binding is only "generic" for a
 * DISPLAY adapter - a hub or bridge on its class driver is correct. */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/drvplan.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)
#define ST(p, b, d, x, c, m, s) drvplan_state(p, b, d, x, c, m, s)

int main(void)
{
    CHECK(ST(0, 1, 0, 0, "Display", "PCI\\VEN_1013&DEV_00AC", "Cirrus Logic 5436 PCI") == DRVST_OK,
          ".243's Cirrus on its own driver is ok");
    CHECK(ST(0, 1, 0, 1, "MEDIA", "PCI\\VEN_121A&DEV_0002", "3Dfx Voodoo2") == DRVST_EXCLUDED,
          "the Voodoo 2 is excluded before anything else is judged");
    CHECK(ST(28, 0, 0, 1, "Display", "", "Voodoo5") == DRVST_EXCLUDED,
          "a 3dfx device with NO driver is still excluded, not 'missing'");
    CHECK(ST(0, 1, 1, 0, "USB", "PCI\\VEN_1033&DEV_0035&CC_0C0310", "NEC USB") == DRVST_DISABLED,
          "the NEC card disabled at boot (ConfigFlags bit 0) is left alone");
    CHECK(ST(22, 1, 0, 0, "hdc", "MF\\GOODSECONDARY", "Secondary IDE controller") == DRVST_DISABLED,
          "problem 22 is disabled");
    CHECK(ST(0, 0, 0, 0, "", "", "PCI Universal Serial Bus") == DRVST_MISSING, "no driver bound is missing");
    CHECK(ST(28, 1, 0, 0, "USB", "", "USB controller") == DRVST_MISSING, "problem 28 is missing");
    CHECK(ST(10, 1, 0, 0, "Net", "PCI\\VEN_10EC&DEV_8139", "RTL8139") == DRVST_MISSING,
          "problem 10 (failed start) is driver-fixable");
    CHECK(ST(0, 1, 0, 0, "Unknown", "", "Unsupported Device") == DRVST_MISSING,
          "Win9x's Unknown class (the SB16 'Unsupported Device') is missing");
    CHECK(ST(12, 1, 0, 0, "MEDIA", "ISAPNP\\CTL0024", "Sound Blaster 16") == DRVST_PROBLEM,
          "problem 12 (resources) is not a driver's fault");
    CHECK(ST(0, 1, 0, 0, "Display", "PCI\\CC_0300", "Standard PCI Graphics Adapter (VGA)") == DRVST_GENERIC,
          "a display adapter bound through PCI\\CC_0300 is generic");
    CHECK(ST(0, 1, 0, 0, "Display", "PCI\\VEN_10DE&DEV_0150", "Standard VGA Graphics Adapter") == DRVST_GENERIC,
          "a display adapter on Standard VGA is generic");
    CHECK(ST(0, 1, 0, 0, "USB", "USB\\ROOT_HUB", "USB Root Hub") == DRVST_OK,
          "a USB root hub on its class driver is OK, not generic");
    CHECK(ST(0, 1, 0, 0, "System", "PCI\\CC_0604", "PCI to PCI bridge") == DRVST_OK,
          "a PCI bridge bound through its class code is OK");
    CHECK(ST(0, 1, 0, 0, "display", "pci\\cc_0300", "x") == DRVST_GENERIC, "class and id compare case-insensitively");
    CHECK(strcmp(drvst_name(DRVST_EXCLUDED), "excluded_3dfx") == 0 && strcmp(drvst_name(DRVST_GENERIC), "generic") == 0,
          "states are named for the JSON");
    printf("-- drvplan (DRIVERS STATUS states, agent 1.88.0): %d/%d tests passed --\n", runs - fails, runs);
    return fails ? 1 : 0;
}

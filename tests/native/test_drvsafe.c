/* agent/shared/drvsafe.h - the 3dfx rule every automatic driver path obeys
 * (agent 1.87.0). User directive 2026-09-27: 3dfx drivers are not touched
 * unless the chat explicitly asks. Until then the XP missing-driver pass had no
 * vendor filter, and the image's C:\D held four 3dfx INFs it would have chosen
 * for a Voodoo (I003 3dfxvs2k for a V5, I002 voodoo2.inf for a Voodoo 2).
 * The ids and strings below are the fleet's own. */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/drvsafe.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

/* Build a REG_MULTI_SZ from a '|'-separated list. */
static const char *msz(char *buf, size_t cap, const char *spec)
{
    size_t n = 0;
    memset(buf, 0, cap);
    for (; *spec && n + 2 < cap; spec++) buf[n++] = (*spec == '|') ? 0 : *spec;
    return buf;
}

int main(void)
{
    char a[512], b[512];

    /* ---- device ids ---- */
    CHECK(drvsafe_id_is_3dfx("PCI\\VEN_121A&DEV_0009&SUBSYS_0002121A&REV_01"), "Voodoo5 5500 (.143) is 3dfx");
    CHECK(drvsafe_id_is_3dfx("pci\\ven_121a&dev_0002"), "Voodoo 2 (.243, Class=MEDIA) is 3dfx - case-insensitive");
    CHECK(drvsafe_id_is_3dfx("DISPLAY\\3dfxV3TV"), "the V3 TV-out child (no VEN_121A at all) is 3dfx");
    CHECK(!drvsafe_id_is_3dfx("PCI\\VEN_1121&DEV_121A"), "a device id 121A under another vendor is not");
    CHECK(!drvsafe_id_is_3dfx("PCI\\VEN_121AB&DEV_0001"), "the token must be bounded (VEN_121AB is not VEN_121A)");
    CHECK(!drvsafe_id_is_3dfx("PCI\\VEN_10DE&DEV_0150&SUBSYS_0000121A"), "a 121A SUBSYSTEM vendor on an NVIDIA chip is not");
    CHECK(!drvsafe_id_is_3dfx("PCI\\VEN_1033&DEV_00E0&SUBSYS_00021799&REV_04"), "the NEC USB card on .243 is not");

    CHECK(drvsafe_ids_3dfx(msz(a, sizeof(a), "PCI\\VEN_121A&DEV_0002&SUBSYS_00000000&REV_02|PCI\\VEN_121A&DEV_0002"), NULL),
          "REG_MULTI_SZ hardware ids");
    CHECK(drvsafe_ids_3dfx(msz(a, sizeof(a), "PCI\\VEN_8086&DEV_1234"), msz(b, sizeof(b), "PCI\\CC_0400|PCI\\VEN_121A")),
          "a 3dfx id in the SECOND string of the compatible ids is still seen");
    CHECK(drvsafe_ids_3dfx("BIOS\\*PNP0A03,PCI\\VEN_121A&DEV_0002", NULL), "Win9x comma-separated ids");
    CHECK(!drvsafe_ids_3dfx(msz(a, sizeof(a), "PCI\\VEN_1013&DEV_00AC|PCI\\CC_0300"), msz(b, sizeof(b), "PCI\\CC_0300")),
          "the Cirrus 5436 is not");
    CHECK(!drvsafe_ids_3dfx(NULL, NULL), "no ids, no match");

    /* An id list is read NT-style, up to an EMPTY string, so a REG_SZ read into
     * a reused buffer must be double-NUL terminated by its reader. .243
     * 2026-09-28: agent/src/drv9x.c reset only out[0], the VIA USB controller's
     * shorter HardwareID left the Voodoo 2's tail behind its NUL, and the USB
     * card read "excluded_3dfx". drv9x_str() now clears the whole buffer. */
    memset(a, 0, sizeof(a));
    strcpy(a, "PCI\\VEN_121A&DEV_0002&SUBSYS_00000000&REV_02,PCI\\VEN_121A&DEV_0002&SUBSYS_00000000,PCI\\VEN_121A&DEV_0002&REV_02&CC_0480,PCI\\VEN_121A&DEV_0002&CC_048000");
    strcpy(a, "PCI\\VEN_1106&DEV_3038&SUBSYS_12340925&REV_04,PCI\\VEN_1106&DEV_3038");   /* only a[0] "reset" */
    CHECK(drvsafe_ids_3dfx(a, NULL),
          "HAZARD (why drv9x_str clears the buffer): a stale tail after the NUL IS scanned as more ids");
    memset(a, 0, sizeof(a));
    strcpy(a, "PCI\\VEN_1106&DEV_3038&SUBSYS_12340925&REV_04,PCI\\VEN_1106&DEV_3038");
    CHECK(!drvsafe_ids_3dfx(a, NULL), "the same VIA USB ids in a cleared buffer are not 3dfx");

    /* ---- bound drivers ---- */
    CHECK(drvsafe_driver_is_3dfx("3dfx Interactive, Inc.", NULL, NULL, NULL, NULL), "provider 3dfx");
    CHECK(drvsafe_driver_is_3dfx("vcr-kmd (retro-agent voodoo-cleanroom)", NULL, NULL, NULL, NULL), "our own vcr-kmd");
    CHECK(drvsafe_driver_is_3dfx(NULL, NULL, "Voodoo2 Accelerator", NULL, NULL), "a Voodoo description");
    CHECK(drvsafe_driver_is_3dfx(NULL, NULL, NULL, "oem12.inf", "3dfxvs"), "the 3dfxvs service");
    CHECK(drvsafe_driver_is_3dfx(NULL, "AMIGAMERLIN", NULL, NULL, NULL), "AmigaMerlin");
    CHECK(!drvsafe_driver_is_3dfx("NVIDIA", "NVIDIA", "NVIDIA GeForce2 GTS", "oem3.inf", "nv4"), "NVIDIA is not");
    CHECK(!drvsafe_driver_is_3dfx("ALPS", "ALPS", "Alps GlidePoint", "oem7.inf", "alpsprt"),
          "an Alps GlidePoint touchpad is NOT 3dfx (GLIDE alone is not a marker)");

    /* ---- INF text (whole text, not model lines) ---- */
    CHECK(drvsafe_text_3dfx("[Version]\r\nProvider=%3DFX%\r\n[Models]\r\n%V5%=V5,PCI\\VEN_121A&DEV_0009"),
          "a 3dfx INF (I003 3dfxvs2k-like)");
    CHECK(drvsafe_text_3dfx("[AddReg]\r\nHKLM,\"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLdrivers\\3dfx\""),
          "an INF that only REGISTERS a 3dfx ICD is still refused");
    CHECK(drvsafe_text_3dfx("CopyFiles=glide3x.dll"), "an INF that ships glide3x");
    CHECK(!drvsafe_text_3dfx("[Version]\r\nProvider=%NVIDIA%\r\n%NV15%=nv4,PCI\\VEN_10DE&DEV_0150"), "an NVIDIA INF is not");
    CHECK(strcmp(drvsafe_text_hit("x Voodoo y"), "VOODOO") == 0, "the hit names the word (for the log)");
    CHECK(!drvsafe_inf_is_3dfx("%adptVoodoo40.DeviceDesc% = arcsas_Inst, PCI\\VEN_9005&DEV_0285\r\n"
                               "adptVoodoo40.DeviceDesc = \"Adaptec RAID 5405\""),
          "Adaptec's RAID 'Voodoo' codename (M003\\arcsas.inf) is NOT a 3dfx INF");
    CHECK(drvsafe_inf_is_3dfx("Voodoo2 driver\r\n%V2%=V2, PCI\\VEN_121A&DEV_0002"),
          "... but an INF naming VEN_121A still is");
    CHECK(drvsafe_inf_is_3dfx("[Strings]\r\nVoodoo=\"3dfx Voodoo3\""), "... and one naming 3DFX");
    CHECK(drvsafe_driver_is_3dfx(NULL, NULL, "Voodoo2 Accelerator", NULL, NULL),
          "a bound driver DESCRIBED as Voodoo is still 3dfx (the narrower list is for INF text only)");

    /* ---- bridges ---- */
    CHECK(drvsafe_is_pci_bridge(msz(a, sizeof(a), "PCI\\VEN_3388&DEV_0021"), msz(b, sizeof(b), "PCI\\VEN_3388|PCI\\CC_060400|PCI\\CC_0604")),
          "the HiNT PCI-PCI bridge (V5 6000) is a bridge - class code in the second compat string");
    CHECK(!drvsafe_is_pci_bridge(msz(a, sizeof(a), "PCI\\VEN_8086&DEV_1250"), msz(b, sizeof(b), "PCI\\CC_060000|PCI\\CC_0600")),
          "a host bridge (0600) is not");

    /* ---- the explicit token ---- */
    CHECK(drvsafe_args_allow("PCI\\VEN_121A&DEV_0009 C:\\D\\V001\\3dfxvs.inf ALLOW3DFX"), "ALLOW3DFX naming one device");
    CHECK(drvsafe_args_allow("allow3dfx PCI\\VEN_121A&DEV_0002"), "the token is case-insensitive");
    CHECK(!drvsafe_args_allow("ALL ALLOW3DFX"), "ALLOW3DFX together with ALL is refused");
    CHECK(!drvsafe_args_allow("PCI\\VEN_121A&DEV_0009 ALLOW3DFXX"), "a longer word is not the token");
    CHECK(!drvsafe_args_allow("PCI\\VEN_121A&DEV_0009 NOALLOW3DFX"), "a word containing it is not the token");
    CHECK(!drvsafe_args_allow("PCI\\VEN_121A&DEV_0009"), "no token, no permission");
    CHECK(!drvsafe_args_allow(NULL), "no args, no permission");

    CHECK(strcmp(drvsafe_reason_name(DRVSAFE_3DFX_BRIDGE), "PCI bridge above a 3dfx device") == 0
          && strcmp(drvsafe_reason_name(DRVSAFE_OK), "ok") == 0, "reasons are named for the log and JSON");

    printf("-- drvsafe (the 3dfx rule, agent 1.87.0): %d/%d tests passed --\n", runs - fails, runs);
    return fails ? 1 : 0;
}

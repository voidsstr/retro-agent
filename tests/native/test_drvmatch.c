/* test_drvmatch.c - TRUE-SOURCE test of how the agent picks INFs from C:\D.
 *
 * WHAT THIS PROTECTS (agent 1.85.1, agent/shared/drvmatch.h, used by
 * gs_scan_driver_tree / gs_install_missing_drivers / gs_devices_unconfigured in
 * agent/src/gamesync.c).
 *
 * Until 1.85.1 the lookup was handed a device's hardware-id REG_MULTI_SZ as a
 * plain C string - so only its FIRST id, the one ending "&REV_xx" - and
 * strstr()'d it into each INF. No INF names a revision, so:
 *
 *   - the first-logon installer never installed anything, and
 *   - the reclaim guard decided nothing in C:\D served the unconfigured display
 *     and audio, and DELETED C:\D - on every freshly imaged box.
 *
 * Found 2026-09-25 while a Dell Dimension 4600 (865G + ICH5, Dell subsystem
 * 0174) was being PXE-imaged. A first fix matched ids ANYWHERE in an INF; review
 * against the real 3,669-INF tree found ~600 ids resolving to an INF that does
 * not serve them (comments, ExcludeFromSelect, PosDup, AddReg strings, models
 * sections XP never reads). The cases below are those, with the INF text copied
 * from the image's $OEM$\$1\D tree.
 *
 * Run: bash tests/run_all.sh   (section [2] compiles every tests/native/test_*.c)
 */
#include "munit.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../../agent/shared/drvmatch.h"

/* Upper-case + prepare a copy of an INF, as gs_scan_driver_tree does. */
static char g_buf[8192];
static const char *prep(const char *inf)
{
    size_t i, n = strlen(inf);
    if (n >= sizeof(g_buf))
        n = sizeof(g_buf) - 1;
    for (i = 0; i < n; i++)
        g_buf[i] = (char)toupper((unsigned char)inf[i]);
    g_buf[n] = 0;
    drvmatch_prepare(g_buf);
    return g_buf;
}

static int names(const char *inf, const char *id)
{
    return drvmatch_has_token(prep(inf), id);
}

/* ---- INFs, verbatim in the parts that matter ---- */

/* D\I015\ialmnt5.inf - Intel 865G display. */
static const char INF_865G[] =
    "[Manufacturer]\r\n"
    "%Intel%   = Intel.Mfg\r\n"
    "\r\n"
    "[Intel.Mfg]\r\n"
    "; %iMGM%  = i852GM,  PCI\\VEN_8086&DEV_3582\r\n"
    "%iSDG%\t = i865G, PCI\\VEN_8086&DEV_2572\r\n"
    "\r\n"
    "[i865G.AddReg]\r\n"
    "HKLM,\"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{8A70}\","
    "\"UninstallString\",,\"RUNDLL32.EXE %11%\\ialmrem.dll,UninstallW2KIGfx "
    "PCI\\VEN_8086&DEV_2572\"\r\n";

/* D\S007\Alcxau.inf - Realtek AC'97: claims ICH5 audio by CLASS CODE, one
 * Realtek board by subsystem. Sorts before T069 in the walk. */
static const char INF_REALTEK[] =
    "[Manufacturer]\r\n%MfgName%=Realtek\r\n[Realtek]\r\n"
    "%ALCAUD.Desc%=AC97AUD,\tPCI\\VEN_8086&DEV_24D5&CC_0401\r\n"
    "%ALCAUD.Desc%=AC97AUD,\tPCI\\VEN_8086&DEV_24D5&SUBSYS_100115BD\r\n";

/* D\T069\smwdmCH5.inf - SoundMAX, which names THIS Dell's subsystem. */
static const char INF_SOUNDMAX[] =
    "[Manufacturer]\r\n%MfgName%=AnalogDevices\r\n\r\n[AnalogDevices]\r\n"
    "%*WDM_AC97AUD.DeviceDesc%=WDM_PHO2,   pci\\ven_8086&dev_24D5&subsys_01741028\r\n";

/* D\T027\stac97.inf line 26: the Dell ICH6 SigmaTel entry, COMMENTED OUT. */
static const char INF_STAC_COMMENTED[] =
    "[Manufacturer]\r\n%STAC%=STAC\r\n[STAC]\r\n"
    ";%STAC97.DvDscInt%=STAC97, PCI\\VEN_8086&DEV_266E&SUBSYS_01821028\r\n";

/* D\L051\sisgbe_2.inf - the id only in ExcludeFromSelect. */
static const char INF_EXCLUDE_ONLY[] =
    "[Manufacturer]\r\n%SiS%=SiS\r\n[SiS]\r\n"
    "%SiS191.DeviceDesc%=SiS191.ndi, PCI\\VEN_1039&DEV_0191\r\n"
    "[ControlFlags]\r\nExcludeFromSelect = PCI\\VEN_1039&DEV_0190, PCI\\VEN_1039&DEV_0191\r\n";

/* D\C007\mesrl.inf - *PNP0501 only in a PosDup section. */
static const char INF_POSDUP_ONLY[] =
    "[Manufacturer]\r\n%Intel%=Intel\r\n[Intel]\r\n"
    "%ME.DeviceDesc%=ComPort, \"PCI\\VEN_8086&DEV_2987&CC_0700\"\r\n"
    "[ComPort.NT.PosDup]\r\n*PNP0500,*PNP0501\r\n";

/* D\I005\C2_38185.inf - Radeon 9250 only in the UNDECORATED section, while
 * [Manufacturer] points XP x86 at ATI.Mfg.NTx86, which is empty. */
static const char INF_WRONG_DECORATION[] =
    "[Manufacturer]\r\n%ATI% = ATI.Mfg, NTx86\r\n\r\n"
    "[ATI.Mfg]\r\n\"RADEON 9250\" = ati2mtag_R200, PCI\\VEN_1002&DEV_5960\r\n\r\n"
    "[ATI.Mfg.NTx86]\r\n\r\n";

/* A 64-bit-only decoration beside an x86 one. */
static const char INF_DECORATIONS[] =
    "[Manufacturer]\r\n%V% = V, NTx86, NTamd64, NTx86.6.0\r\n"
    "[V.NTx86]\r\n%A%=A, PCI\\VEN_1111&DEV_0001\r\n"
    "[V.NTamd64]\r\n%B%=B, PCI\\VEN_1111&DEV_0002\r\n"
    "[V.NTx86.6.0]\r\n%C%=C, PCI\\VEN_1111&DEV_0003\r\n";

/* D\S006\HDARt.inf - generic Realtek HD Audio: codec VEN/DEV only. */
static const char INF_HDA[] =
    "[Manufacturer]\r\n%MfgName% = AzaliaManufacturerID, NTx86\r\n"
    "[AzaliaManufacturerID.NTx86]\r\n"
    "\"Realtek High Definition Audio\"=IntcAzAudModel, HDAUDIO\\FUNC_01&VEN_10EC&DEV_0888\r\n";

/* ---- device id lists, as XP reports them (SPDRP_HARDWAREID / COMPATIBLEIDS) */
static const char HW_2572[] =
    "PCI\\VEN_8086&DEV_2572&SUBSYS_01741028&REV_02\0"
    "PCI\\VEN_8086&DEV_2572&SUBSYS_01741028\0"
    "PCI\\VEN_8086&DEV_2572&CC_030000\0"
    "PCI\\VEN_8086&DEV_2572&CC_0300\0";
static const char COMPAT_2572[] =
    "PCI\\VEN_8086&DEV_2572&REV_02\0PCI\\VEN_8086&DEV_2572\0"
    "PCI\\VEN_8086&CC_030000\0PCI\\VEN_8086&CC_0300\0PCI\\VEN_8086\0"
    "PCI\\CC_030000\0PCI\\CC_0300\0";
static const char HW_24D5[] =
    "PCI\\VEN_8086&DEV_24D5&SUBSYS_01741028&REV_02\0"
    "PCI\\VEN_8086&DEV_24D5&SUBSYS_01741028\0"
    "PCI\\VEN_8086&DEV_24D5&CC_040100\0"
    "PCI\\VEN_8086&DEV_24D5&CC_0401\0";
static const char COMPAT_24D5[] =
    "PCI\\VEN_8086&DEV_24D5&REV_02\0PCI\\VEN_8086&DEV_24D5\0"
    "PCI\\VEN_8086&CC_040100\0PCI\\VEN_8086&CC_0401\0PCI\\VEN_8086\0"
    "PCI\\CC_040100\0PCI\\CC_0401\0";

/* Rank INFs the way gs_scan_driver_tree does. */
static void rank(const char *const *infs, int n, const char *const *ids, int nids,
                 drvmatch_cands *c)
{
    int i;
    char name[16];
    c->n = 0;
    for (i = 0; i < n; i++) {
        int idx = drvmatch_best(prep(infs[i]), ids, nids);
        snprintf(name, sizeof(name), "inf%d", i);
        if (idx >= 0)
            drvmatch_cand_add(c, idx, name);
    }
}

static char g_pay[8][64];
static int  g_npay;
static void collect_payload(const char *rel, void *ctx)
{
    (void)ctx;
    if (g_npay < 8) {
        strncpy(g_pay[g_npay], rel, sizeof(g_pay[0]) - 1);
        g_pay[g_npay][sizeof(g_pay[0]) - 1] = 0;
        g_npay++;
    }
}

int main(void)
{
    const char    *ids[DRVMATCH_MAX_IDS];
    drvmatch_cands c;
    int            n;

    printf("== the Dell's 865G display ==\n");
    CHECK(strstr(INF_865G, HW_2572) == NULL,
          "OLD: first id (&REV_02) is not in ialmnt5.inf - never installed, and "
          "the reclaim then deleted C:\\D");
    n = drvmatch_collect(HW_2572, COMPAT_2572, ids, DRVMATCH_MAX_IDS);
    CHECK(n == 6, "4 hardware ids + 2 compatible ids that still name the device");
    {
        const char *infs[] = { INF_865G };
        rank(infs, 1, ids, n, &c);
        CHECK(c.n == 1 && strcmp(ids[c.idx[0]], "PCI\\VEN_8086&DEV_2572") == 0,
              "NEW: ialmnt5.inf, via the id its model line names");
    }
    CHECK(!names(INF_865G, "PCI\\VEN_8086&DEV_3582"),
          "a commented-out model line does not count");

    printf("== the Dell's AC'97 audio: most specific id wins over tree order ==\n");
    n = drvmatch_collect(HW_24D5, COMPAT_24D5, ids, DRVMATCH_MAX_IDS);
    {
        const char *infs[] = { INF_REALTEK, INF_SOUNDMAX };   /* S007 < T069 */
        rank(infs, 2, ids, n, &c);
        CHECK(c.n == 2 && strcmp(c.path[0], "inf1") == 0 && c.idx[0] == 1,
              "SoundMAX (names SUBSYS_01741028) is tried first");
        CHECK(strcmp(c.path[1], "inf0") == 0,
              "Realtek (class-code hardware id) stays as the fall-back candidate");
    }

    printf("== only real model lines count ==\n");
    CHECK(!names("[Manufacturer]\r\nX=Y\r\n[Y]\r\n%D%=s, PCI\\VEN_1234&DEV_0001\r\n"
                 "[s.AddReg]\r\nHKLM,\"a\",\"b\",,\"x PCI\\VEN_8086&DEV_2572\"\r\n",
                 "PCI\\VEN_8086&DEV_2572"),
          "T117/L017: an id only inside a quoted AddReg string does not count");
    CHECK(!names(INF_STAC_COMMENTED, "PCI\\VEN_8086&DEV_266E&SUBSYS_01821028"),
          "T027: a ';' commented model line does not count");
    CHECK(!names(INF_EXCLUDE_ONLY, "PCI\\VEN_1039&DEV_0190"),
          "L051: an ExcludeFromSelect mention does not count");
    CHECK(names(INF_EXCLUDE_ONLY, "PCI\\VEN_1039&DEV_0191"),
          "the same INF's real model line still does");
    CHECK(!names(INF_POSDUP_ONLY, "*PNP0501"), "C007: a PosDup list does not count");
    CHECK(names(INF_POSDUP_ONLY, "PCI\\VEN_8086&DEV_2987&CC_0700"),
          "a quoted id on a real model line does");
    CHECK(!names(INF_WRONG_DECORATION, "PCI\\VEN_1002&DEV_5960"),
          "I005: an undecorated section XP x86 is not pointed at does not count");
    CHECK(names(INF_DECORATIONS, "PCI\\VEN_1111&DEV_0001"), "NTx86 section is read");
    CHECK(!names(INF_DECORATIONS, "PCI\\VEN_1111&DEV_0002"), "NTamd64 section is not");
    CHECK(!names(INF_DECORATIONS, "PCI\\VEN_1111&DEV_0003"), "NTx86.6.0 (Vista) is not");
    CHECK(drvmatch_deco_applies("NTX86.5.1") && drvmatch_deco_applies("NT") &&
          drvmatch_deco_applies("NT.5.1") && !drvmatch_deco_applies("NT.5.2") &&
          !drvmatch_deco_applies("NTIA64"),
          "decoration rules for 32-bit XP (NT 5.1)");

    printf("== ids match as whole tokens, never as a prefix ==\n");
    CHECK(!names(INF_REALTEK, "PCI\\VEN_8086&DEV_24D5"),
          "a Realtek INF naming one board's SUBSYS does not claim every ICH5");
    CHECK(names(INF_REALTEK, "PCI\\VEN_8086&DEV_24D5&SUBSYS_100115BD"),
          "but does claim that board");

    printf("== family ids never pick a driver ==\n");
    CHECK(!drvmatch_usable("*PNP0501", 0), "COM port: a family even as a HARDWARE id");
    CHECK(!drvmatch_usable("USB\\ROOT_HUB", 0), "bare root hub: family");
    CHECK(!drvmatch_usable("USB\\ROOT_HUB20", 0), "bare USB 2 root hub: family");
    CHECK(drvmatch_usable("USB\\ROOT_HUB&VID8086&PID24D2", 0),
          "a root hub id that names its controller is specific");
    CHECK(!drvmatch_usable("PCI\\VEN_8086", 1), "vendor-only compatible id");
    CHECK(!drvmatch_usable("PCI\\VEN_8086&CC_0401", 1), "vendor+class compatible id");
    CHECK(!drvmatch_usable("PCI\\CC_0300", 1), "class-only id");
    CHECK(!drvmatch_usable("USB\\CLASS_03", 1), "USB class id");
    CHECK(drvmatch_usable("USB\\VID_046D&PID_C00E", 1), "USB VID/PID compatible id");
    CHECK(drvmatch_usable("PCI\\VEN_8086&DEV_2572&REV_02", 1),
          "a PCI compatible id that names the device");

    printf("== HD Audio: the codec is matched through a COMPATIBLE id ==\n");
    {
        static const char hw[] =
            "HDAUDIO\\FUNC_01&VEN_10EC&DEV_0888&SUBSYS_1043829F&REV_1001\0"
            "HDAUDIO\\FUNC_01&VEN_10EC&DEV_0888&SUBSYS_1043829F\0";
        static const char co[] =
            "HDAUDIO\\FUNC_01&VEN_10EC&DEV_0888&REV_1001\0"
            "HDAUDIO\\FUNC_01&VEN_10EC&DEV_0888\0"
            "HDAUDIO\\FUNC_01&VEN_10EC\0HDAUDIO\\FUNC_01\0";
        const char *infs[] = { INF_HDA };
        n = drvmatch_collect(hw, co, ids, DRVMATCH_MAX_IDS);
        CHECK(n == 4, "two hardware + two device-naming compatible ids");
        rank(infs, 1, ids, n, &c);
        CHECK(c.n == 1 && strcmp(ids[c.idx[0]],
                                 "HDAUDIO\\FUNC_01&VEN_10EC&DEV_0888") == 0,
              "HDARt.inf found via the codec's VEN/DEV compatible id");
    }

    printf("== ranked candidates: specificity, then order, bounded ==\n");
    {
        drvmatch_cands r;
        int i;
        r.n = 0;
        drvmatch_cand_add(&r, 3, "a");
        drvmatch_cand_add(&r, 1, "b");
        drvmatch_cand_add(&r, 3, "c");
        drvmatch_cand_add(&r, 1, "d");
        CHECK(r.n == 4 && strcmp(r.path[0], "b") == 0 && strcmp(r.path[1], "d") == 0 &&
              strcmp(r.path[2], "a") == 0 && strcmp(r.path[3], "c") == 0,
              "sorted by id index, ties keep tree order");
        for (i = 0; i < 10; i++)
            drvmatch_cand_add(&r, 5, "z");
        CHECK(r.n == DRVMATCH_MAX_CAND, "capped");
        CHECK(!drvmatch_cand_add(&r, 9, "late"), "a worse candidate than a full list is dropped");
        CHECK(drvmatch_cand_add(&r, 0, "best") && strcmp(r.path[0], "best") == 0,
              "a better one displaces the worst");
    }

    printf("== UTF-16 INFs can be matched ==\n");
    {
        const char *ascii = "[MANUFACTURER]\r\nR=R\r\n[R]\r\nX=Y, PCI\\VEN_10EC&DEV_8168\r\n";
        char   buf[512];
        size_t i, len = 0, m = strlen(ascii);
        buf[len++] = (char)0xFF;
        buf[len++] = (char)0xFE;
        for (i = 0; i < m; i++) {
            buf[len++] = ascii[i];
            buf[len++] = 0;
        }
        buf[len] = buf[len + 1] = 0;
        CHECK(strstr(buf, "PCI\\VEN_10EC") == NULL,
              "OLD: read as ANSI, the first 00 byte ends the string");
        len = drvmatch_fold_utf16(buf, len);
        CHECK(len == m, "folded to one byte per character");
        drvmatch_prepare(buf);
        CHECK(drvmatch_has_token(buf, "PCI\\VEN_10EC&DEV_8168"), "NEW: the id is found");
    }

    printf("== %%strkey%% ids are expanded through [Strings] (Matrox H010) ==\n");
    {
        static const char mx[] =
            "[Manufacturer]\r\n%M% = MX, NTX86.5.1\r\n"
            "[MX.NTX86.5.1]\r\n%D% = I.NTX86, %MxEF.Mtx3005.HardwareId%\r\n"
            "[Strings]\r\nMxEF.Mtx3005.HardwareId = \"PCI\\VEN_102B&DEV_3005&SUBSYS_2301102B\"\r\n"
            "M = \"Matrox\"\r\n";
        static const char plain[] =
            "[Manufacturer]\r\n%M% = MX\r\n[MX]\r\n%D% = I, PCI\\VEN_1234&DEV_0001\r\n"
            "[Strings]\r\nNote = \"PCI\\VEN_8086&DEV_2572\"\r\n";
        CHECK(names(mx, "PCI\\VEN_102B&DEV_3005&SUBSYS_2301102B"),
              "an id given as %KEY% on a model line is matched via its [Strings] value");
        CHECK(!names(plain, "PCI\\VEN_8086&DEV_2572"),
              "without a %KEY% id field, [Strings] values stay invisible");
    }

    printf("== more [Manufacturer] entries than the table holds (I010 has 90) ==\n");
    {
        static char big[40000];
        size_t o = 0;
        int i;
        o += snprintf(big + o, sizeof(big) - o, "[Manufacturer]\r\n");
        for (i = 0; i < 150; i++)
            o += snprintf(big + o, sizeof(big) - o, "%%M%d%% = MFG%d\r\n", i, i);
        o += snprintf(big + o, sizeof(big) - o,
                      "[MFG149]\r\n%%D%% = I, MONITOR\\AOCA566\r\n"
                      "[I.AddReg]\r\nHKR,,x,,\"MONITOR\\ZZZ0001\"\r\n");
        CHECK(names(big, "MONITOR\\AOCA566"),
              "a models section past the cap is still read");
        CHECK(!names(big, "MONITOR\\ZZZ0001"),
              "and overflow does not turn AddReg strings into model ids");
    }

    printf("== a malformed decoration cannot overflow ==\n");
    CHECK(!drvmatch_deco_applies("NT.99999999999999999999.1"),
          "a 20-digit version is clamped, not wrapped into 'applies'");

    printf("== the payload an INF declares ==\n");
    {
        /* an ATI-shaped INF: files live in a subdirectory of the source disk */
        static const char ati[] =
            "[SOURCEDISKSNAMES.X86]\r\n1 = %DISK%,,,.\\B136646\r\n"
            "[SOURCEDISKSFILES]\r\nATI2MTAG.SYS = 1\r\nATI2DVAG.DLL = 1 ; comment\r\n"
            "[SOURCEDISKSFILES.X86]\r\nATIDDC.DLL = 1, SUB\r\n";
        int n;
        g_npay = 0;
        n = drvmatch_payload(ati, collect_payload, NULL);
        CHECK(n == 3, "three files listed across both SourceDisksFiles sections");
        CHECK(g_npay == 3 && strcmp(g_pay[0], "B136646\\ATI2MTAG.SYS") == 0,
              "path = SourceDisksNames path + file, '.\\' stripped");
        CHECK(strcmp(g_pay[2], "B136646\\SUB\\ATIDDC.DLL") == 0,
              "the per-file subdirectory is inserted");
    }
    {
        /* H010\G4G5.inf's shape: the files live INSIDE a staged cabinet */
        static const char cab[] =
            "[SOURCEDISKSNAMES]\r\n1 = \"Matrox\", GSERIES.CAB, , \r\n"
            "[SOURCEDISKSFILES]\r\nG400DHM.SYS = 1\r\nG400DHD.DLL = 1\r\nG200MINI.SYS = 1\r\n";
        int n;
        g_npay = 0;
        n = drvmatch_payload(cab, collect_payload, NULL);
        CHECK(n == 1 && strcmp(g_pay[0], "GSERIES.CAB") == 0,
              "files inside a cabinet: the cabinet is the payload, reported once");
    }
    CHECK(drvmatch_payload_ok(0, 0, 0, 0), "an INF that lists nothing is not refused");
    CHECK(!drvmatch_payload_ok(33, 33, 2, 2),
          "G001: every file (and both drivers) missing -> refused");
    CHECK(drvmatch_payload_ok(10, 5, 4, 2),
          "L056 FETNDIS: only the 64-bit drivers missing -> offered");
    CHECK(drvmatch_payload_ok(1, 1, 0, 0),
          "C011 mesrl.inf: no .SYS listed (in-box serial.sys) -> never refused");
    CHECK(drvmatch_payload_ok(3, 3, 0, 0),
          "a monitor INF listing only missing .ICM files -> never refused");

    printf("== keep C:\\D, or reclaim it? ==\n");
    CHECK(drvmatch_keeps_tree(DRVMATCH_V_UNSEEN, 1, 1),
          "a device with a confirmed, untried staged driver keeps it");
    CHECK(!drvmatch_keeps_tree(DRVMATCH_V_UNSEEN, 1, 0),
          "a device nothing in C:\\D serves does not (the 6 GB Gateway rule)");
    CHECK(drvmatch_keeps_tree(DRVMATCH_V_UNSEEN, 0, 0),
          "could not search C:\\D -> keep");
    CHECK(drvmatch_keeps_tree(DRVMATCH_V_UNSEARCHED, 1, 0),
          "install pass could not search, or an install hung -> keep");
    CHECK(!drvmatch_keeps_tree(DRVMATCH_V_INSTALLED, 1, 0),
          "installed (restart pending or not): files are already in system32");
    CHECK(!drvmatch_keeps_tree(DRVMATCH_V_FAILED, 1, 1),
          "a staged driver that did not fix it is no reason to keep 2.4 GB");
    CHECK(!drvmatch_keeps_tree(DRVMATCH_V_PREFER, 1, 1),
          "PREFER.TXT gates the reclaim itself");
    CHECK(!drvmatch_keeps_tree(DRVMATCH_V_NONE, 1, 0), "nothing serves it");
    CHECK(drvmatch_keeps_tree(DRVMATCH_V_ERRORED, 1, 0),
          "every install errored (not a broken device) and a boot's attempt remains -> keep");

    printf("== which problem codes a driver can clear ==\n");
    CHECK(drvmatch_problem_driver_fixable(28) && drvmatch_problem_driver_fixable(10),
          "28 (no driver) and 10 (failed start): try the next candidate");
    CHECK(!drvmatch_problem_driver_fixable(12) && !drvmatch_problem_driver_fixable(22),
          "12 (resources) and 22 (disabled): not the driver - stop at the best one");
    CHECK(!drvmatch_problem_wants_driver(22) && !drvmatch_problem_wants_driver(29),
          "a disabled device is not an unconfigured one (would pin C:\\D forever)");
    CHECK(drvmatch_problem_wants_driver(28), "problem 28 wants a driver");

    printf("\n%s\n", munit_fails ? "FAILURES"
                                 : "drvmatch: C:\\D serves the devices it has drivers for");
    return munit_fails ? 1 : 0;
}

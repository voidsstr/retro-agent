/* test_gamegate.c - TRUE-SOURCE: compiles agent/shared/gamegate.h, the logic
 * the agent runs to decide whether a machine may receive a staged game.
 *
 * WHAT THIS PINS, and why each one is here rather than trusted:
 *
 *  1. THE FAIL-OPEN CONTRACT. Absent data must never block a title. A missing
 *     requires.json, an unclassifiable GPU, a machine whose clock could not be
 *     measured - all deploy. Fail-closed here would produce a box that silently
 *     receives no games and says nothing about why, which is the exact failure
 *     shape CLAUDE.md's "make failure VISIBLE" section is about.
 *
 *  2. THE RULES DECIDE THE OBVIOUS CASES ALONE. The whole point of the split is
 *     that a Pentium III against a shader-model-2 title is answered by
 *     arithmetic, not by asking a language model. If gg_decide ever starts
 *     returning MARGINAL for those, every one of them turns into an LLM call.
 *
 *  3. THE MARGINAL BAND IS A BAND. 25% under a published minimum is a
 *     judgement call; 60% under is not. Both directions are asserted, because
 *     a band that silently widens to "everything" is indistinguishable from
 *     having no rules at all.
 *
 *  4. THE PROFILE HASH IS STABLE. It is the host-side cache key. If it folded
 *     anything variable - uptime, free RAM, free disk - the cache would miss on
 *     every single poll and the LLM would be re-consulted forever. The test
 *     mutates exactly those and asserts the hash does NOT move, then mutates a
 *     real hardware field and asserts it does.
 *
 *  5. THE NVIDIA DEVICE-ID TABLE IS NOT MONOTONIC. 0x0150 is a GeForce2 GTS
 *     (2000, DX7); 0x0160 is a GeForce 6200 (2004, SM3.0). Any "device id
 *     above X means newer" shortcut misclassifies one of them, and .124's card
 *     is 0x0150 - the exact id that shortcut gets wrong.
 */

#include "munit.h"
#include <string.h>

#include "../../agent/shared/gamegate.h"

/* ------------------------------------------------------------------ */
/* Real fleet machines, as HWPROFILE reports them.                      */
/* ------------------------------------------------------------------ */

/* .124 - Pentium III Coppermine 845 MHz, GeForce2 GTS (10DE:0150), 511 MB.
 * No SSE2: SSE only. This is the box the gate exists for. */
static gg_profile_t box_124(void)
{
    gg_profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.cpu_vendor, "GenuineIntel");
    p.cpu_family = 6; p.cpu_model = 8; p.cpu_stepping = 3;
    p.cpu_mhz = 845; p.cpu_count = 1;
    p.cpu_features = GG_CPU_FPU | GG_CPU_MMX | GG_CPU_CMOV | GG_CPU_SSE;
    p.ram_mb = 511;
    p.gpu_ven = 0x10DE; p.gpu_dev = 0x0150; p.vram_mb = 32;
    p.gpu_level = gg_gpu_level_from_pci(p.gpu_ven, p.gpu_dev);
    p.os_level = GG_OS_WINXP; p.os_major = 5; p.os_minor = 1;
    return p;
}

/* .145 - Sandy Bridge quad, GeForce 8400 GS, 2 GB, Windows XP. */
static gg_profile_t box_145(void)
{
    gg_profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.cpu_vendor, "GenuineIntel");
    p.cpu_family = 6; p.cpu_model = 42; p.cpu_stepping = 7;
    p.cpu_mhz = 3100; p.cpu_count = 4;
    p.cpu_features = GG_CPU_FPU | GG_CPU_MMX | GG_CPU_CMOV | GG_CPU_SSE |
                     GG_CPU_SSE2 | GG_CPU_SSE3 | GG_CPU_SSSE3 | GG_CPU_SSE41;
    p.ram_mb = 2048;
    p.gpu_ven = 0x10DE; p.gpu_dev = 0x06E4; p.vram_mb = 512;
    p.gpu_level = gg_gpu_level_from_pci(p.gpu_ven, p.gpu_dev);
    p.os_level = GG_OS_WINXP; p.os_major = 5; p.os_minor = 1;
    return p;
}

/* A Pentium 1 + 2D-only S3: the machine that must receive almost nothing. */
static gg_profile_t box_p1(void)
{
    gg_profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.cpu_vendor, "GenuineIntel");
    p.cpu_family = 5; p.cpu_model = 2; p.cpu_stepping = 12;
    p.cpu_mhz = 166; p.cpu_count = 1;
    p.cpu_features = GG_CPU_FPU | GG_CPU_MMX;
    p.ram_mb = 32;
    p.gpu_ven = 0x5333; p.gpu_dev = 0x8811; p.vram_mb = 2;
    p.gpu_level = gg_gpu_level_from_pci(p.gpu_ven, p.gpu_dev);
    p.os_level = GG_OS_WIN9X; p.os_major = 4; p.os_minor = 10;
    return p;
}

static int decide_json(gg_profile_t p, const char *json, gg_decision_t *d)
{
    gg_req_t r;
    gg_req_parse(json, &r);
    gg_decide(&p, &r, d);
    return d->verdict;
}

/* ------------------------------------------------------------------ */

/* (1) FAIL-OPEN. Every "we do not know" path must deploy the title. */
TEST(fail_open_on_absent_data)
{
    gg_decision_t d;
    gg_req_t r;
    gg_profile_t p = box_124();

    /* No file at all - gg_req_parse over an empty string. */
    CHECK_EQ_I(gg_req_parse("", &r), 0);
    gg_decide(&p, &r, &d);
    CHECK_EQ_I(d.verdict, GG_V_RUN);

    /* A file that is there but says nothing the gate understands. */
    CHECK_EQ_I(gg_req_parse("{\"notes\":\"needs a fast machine\"}", &r), 0);
    gg_decide(&p, &r, &d);
    CHECK_EQ_I(d.verdict, GG_V_RUN);

    /* Truncated / corrupt JSON must not turn into a rejection. */
    CHECK_EQ_I(decide_json(p, "{\"min_cpu_mhz\":", &d), GG_V_RUN);

    /* An unclassifiable GPU. A device id that matches no table row is
     * GG_GPU_UNKNOWN, and UNKNOWN must never reject: a card we have not
     * catalogued yet is not evidence that it is slow. */
    p.gpu_ven = 0x1AF4; p.gpu_dev = 0x1050;      /* virtio-gpu, uncatalogued */
    p.gpu_level = gg_gpu_level_from_pci(p.gpu_ven, p.gpu_dev);
    CHECK_EQ_I(p.gpu_level, GG_GPU_UNKNOWN);
    CHECK_EQ_I(decide_json(p, "{\"gpu_feature_level\":\"sm3.0\"}", &d),
               GG_V_RUN);

    /* An unmeasured clock (Win9x with no TSC) must not reject either. */
    p = box_124();
    p.cpu_mhz = 0;
    CHECK_EQ_I(decide_json(p, "{\"min_cpu_mhz\":3000}", &d), GG_V_RUN);
}

/* (2) The rules answer the obvious cases WITHOUT an LLM. Each of these must be
 * a flat GG_V_NO, never MARGINAL - a MARGINAL here is an ollama round trip. */
TEST(rules_decide_the_obvious_alone)
{
    gg_decision_t d;
    gg_profile_t p124 = box_124();
    gg_profile_t p1   = box_p1();

    /* Doom 3 on a Pentium III: SM2.0 wanted, TNL present - two levels short. */
    CHECK_EQ_I(decide_json(p124,
        "{\"min_cpu_mhz\":1500,\"min_ram_mb\":384,\"min_vram_mb\":64,"
        "\"gpu_feature_level\":\"sm2.0\",\"cpu_features\":[\"sse\"]}", &d),
        GG_V_NO);

    /* An SSE2-only binary on a Coppermine. #UD on the first instruction, so
     * this is a hard no even though every other field is comfortable. */
    CHECK_EQ_I(decide_json(p124,
        "{\"min_cpu_mhz\":600,\"min_ram_mb\":256,\"cpu_features\":[\"sse2\"]}",
        &d), GG_V_NO);
    CHECK(strcmp(d.limiting, "cpu_features") == 0, "SSE2 must be the limit");

    /* An XP-only title on Windows 98. The loader refuses it; no judgement
     * call exists to be made, so it must not reach the LLM. */
    CHECK_EQ_I(decide_json(p1, "{\"min_os\":\"winxp\"}", &d), GG_V_NO);
    CHECK(strcmp(d.limiting, "os") == 0, "OS must be the limit");

    /* A 2001 title on a 166 MHz Pentium: 5x short. */
    CHECK_EQ_I(decide_json(p1, "{\"min_cpu_mhz\":700,\"min_ram_mb\":128}", &d),
               GG_V_NO);

    /* And the reverse: a modern box must sail through a period title with no
     * LLM call either. A gate that marks everything marginal is not a gate. */
    CHECK_EQ_I(decide_json(box_145(),
        "{\"min_cpu_mhz\":700,\"min_ram_mb\":128,\"min_vram_mb\":16,"
        "\"gpu_feature_level\":\"tnl\",\"min_os\":\"winxp\"}", &d), GG_V_RUN);
}

/* (3) The marginal band. 25% under is a judgement call; past that is not. */
TEST(marginal_band_has_both_edges)
{
    gg_decision_t d;
    gg_profile_t p = box_124();          /* 845 MHz */

    /* 845 against 1000 is 15.5% short - inside the band. */
    CHECK_EQ_I(decide_json(p, "{\"min_cpu_mhz\":1000}", &d), GG_V_MARGINAL);
    /* 845 against 1126 is 25.0% short - the first value outside it. */
    CHECK_EQ_I(decide_json(p, "{\"min_cpu_mhz\":1127}", &d), GG_V_NO);
    /* 845 against 1500 is not a judgement call by any reading. */
    CHECK_EQ_I(decide_json(p, "{\"min_cpu_mhz\":1500}", &d), GG_V_NO);
    /* Meeting the minimum exactly is a plain run. */
    CHECK_EQ_I(decide_json(p, "{\"min_cpu_mhz\":845}", &d), GG_V_RUN);

    /* One GPU level short is the judgement call the LLM exists for: a DX8
     * title on a DX7 card often has a fallback path, and often does not. */
    CHECK_EQ_I(decide_json(p, "{\"gpu_feature_level\":\"sm1.x\"}", &d),
               GG_V_MARGINAL);
    /* Two levels short is not. */
    CHECK_EQ_I(decide_json(p, "{\"gpu_feature_level\":\"sm2.0\"}", &d),
               GG_V_NO);

    /* RAM: 511 MB against 640 is 20% short (marginal), against 1024 is 50%. */
    CHECK_EQ_I(decide_json(p, "{\"min_ram_mb\":640}", &d), GG_V_MARGINAL);
    CHECK_EQ_I(decide_json(p, "{\"min_ram_mb\":1024}", &d), GG_V_NO);
}

/* The reported limiting factor must be the thing that actually stops the game.
 * A box that is both short of RAM and running the wrong OS is stopped by the
 * OS, and saying "not enough RAM" would send someone to buy the wrong part. */
TEST(limiting_factor_names_the_real_cause)
{
    gg_decision_t d;
    gg_profile_t p = box_p1();

    decide_json(p, "{\"min_os\":\"winxp\",\"min_ram_mb\":256,"
                   "\"min_cpu_mhz\":700,\"cpu_features\":[\"sse2\"]}", &d);
    CHECK_EQ_I(d.verdict, GG_V_NO);
    CHECK(strcmp(d.limiting, "os") == 0, "OS outranks RAM and clock");

    /* With the OS satisfied, the instruction set outranks the clock: a
     * too-slow CPU still runs the game, a missing instruction does not. */
    p.os_level = GG_OS_WINXP;
    decide_json(p, "{\"min_ram_mb\":256,\"min_cpu_mhz\":700,"
                   "\"cpu_features\":[\"sse2\"]}", &d);
    CHECK(strcmp(d.limiting, "cpu_features") == 0,
          "instruction set outranks clock");

    /* The reason text must carry the numbers, so a skip line in the agent log
     * is actionable without going back to the library to look them up. */
    decide_json(box_124(), "{\"min_cpu_mhz\":1500}", &d);
    CHECK(strstr(d.reason, "845") != 0, "reason must quote what we have");
    CHECK(strstr(d.reason, "1500") != 0, "reason must quote what is needed");
}

/* (4) The profile hash is the cache key: stable across reboots, sensitive to
 * hardware. Both halves matter and both have a distinct failure mode. */
TEST(profile_hash_is_stable_and_sensitive)
{
    gg_profile_t a = box_124(), b = box_124();
    char ha[17], hb[17];

    gg_profile_hash(&a, ha);
    CHECK_EQ_U(strlen(ha), 16);

    /* Same machine, second poll. A measured clock wobbles by a few MHz and a
     * video BIOS steals a megabyte of RAM between boots; neither is a
     * different machine, and if either moved the hash the cache would miss
     * every single time - which is the same as having no cache. */
    b.cpu_mhz = 848;                 /* inside the 25 MHz bucket */
    b.ram_mb  = 508;                 /* inside the 16 MB bucket  */
    gg_profile_hash(&b, hb);
    CHECK(strcmp(ha, hb) == 0, "measurement jitter must not move the hash");

    /* Real hardware changes must move it, or a re-carded box keeps the old
     * verdicts. This is exactly .124's history: the Voodoo 3 came out and a
     * GeForce2 GTS went in. */
    b = box_124();
    b.gpu_dev = 0x0250;              /* GeForce4 Ti */
    gg_profile_hash(&b, hb);
    CHECK(strcmp(ha, hb) != 0, "a different GPU must be a different profile");

    b = box_124();
    b.cpu_mhz = 1400;
    gg_profile_hash(&b, hb);
    CHECK(strcmp(ha, hb) != 0, "a different clock must be a different profile");

    b = box_124();
    b.ram_mb = 1024;
    gg_profile_hash(&b, hb);
    CHECK(strcmp(ha, hb) != 0, "more RAM must be a different profile");

    b = box_124();
    strcpy(b.cpu_vendor, "AuthenticAMD");
    gg_profile_hash(&b, hb);
    CHECK(strcmp(ha, hb) != 0, "a different CPU vendor must differ");

    /* Two different machines must not collide. */
    {
        gg_profile_t m = box_145();
        char hm[17];
        gg_profile_hash(&m, hm);
        CHECK(strcmp(ha, hm) != 0, ".124 and .145 must hash differently");
    }
}

/* (5) The GPU table. NVIDIA ids are not ordered by generation, and every
 * "greater than" shortcut gets one of these wrong. */
TEST(gpu_table_handles_non_monotonic_ids)
{
    /* 0x0150 GeForce2 GTS (2000) is DX7 T&L ... */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x10DE, 0x0150), GG_GPU_TNL);
    /* ... while the NUMERICALLY LARGER 0x0160 is a GeForce 6200 (2004),
     * four years and three feature levels later. */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x10DE, 0x0160), GG_GPU_SM3);
    /* And 0x0110 (GeForce2 MX) sits between them at DX7 again. */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x10DE, 0x0110), GG_GPU_TNL);

    /* GeForce4 MX is a DX7 part wearing a DX8 part's name - it has no
     * programmable shaders at all, and treating it as a GeForce4 Ti would
     * hand it every shader title on the shelf. */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x10DE, 0x0172), GG_GPU_TNL);
    CHECK_EQ_I(gg_gpu_level_from_pci(0x10DE, 0x0250), GG_GPU_SM1);

    /* Every Voodoo ever made is fixed-function - there is no 3dfx part with
     * hardware T&L, so the whole vendor collapses to one level. */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x121A, 0x0002), GG_GPU_FIXED);  /* V2 */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x121A, 0x0005), GG_GPU_FIXED);  /* V3 */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x121A, 0x0009), GG_GPU_FIXED);  /* V5 */

    /* Intel 865G: no hardware T&L at all, the driver does it on the CPU.
     * Classifying it as TNL because it is "2003 integrated graphics" would
     * let a T&L-requiring title onto .171's 2D chip. */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x8086, 0x2572), GG_GPU_FIXED);

    /* The 8400 GS in .145. */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x10DE, 0x06E4), GG_GPU_SM3);

    /* Unknown vendor and unknown device both fall through to UNKNOWN. */
    CHECK_EQ_I(gg_gpu_level_from_pci(0x0000, 0x0000), GG_GPU_UNKNOWN);
    CHECK_EQ_I(gg_gpu_level_from_pci(0x10DE, 0xFFFF), GG_GPU_UNKNOWN);
}

/* The requires.json scanner. The schema is FLAT on purpose - the scanner is
 * position-based and does not understand nesting - so the shapes it must
 * survive are pinned here rather than discovered on a box. */
TEST(requires_json_scanner)
{
    gg_req_t r;
    const char *full =
        "{\n"
        "  \"requirements_version\": 3,\n"
        "  \"title\": \"Max Payne\",\n"
        "  \"min_cpu_mhz\": 450,\n"
        "  \"min_ram_mb\": 128,\n"
        "  \"min_vram_mb\": 16,\n"
        "  \"disk_mb\": 830,\n"
        "  \"gpu_feature_level\": \"tnl\",\n"
        "  \"cpu_features\": [\"mmx\", \"sse\"],\n"
        "  \"min_os\": \"win9x\",\n"
        "  \"notes\": \"has a software renderer\"\n"
        "}\n";

    CHECK_EQ_I(gg_req_parse(full, &r), 1);
    CHECK_EQ_U(r.version, 3);
    CHECK_EQ_U(r.min_cpu_mhz, 450);
    CHECK_EQ_U(r.min_ram_mb, 128);
    CHECK_EQ_U(r.min_vram_mb, 16);
    CHECK_EQ_U(r.disk_mb, 830);
    CHECK_EQ_I(r.min_gpu_level, GG_GPU_TNL);
    CHECK_EQ_I(r.min_os_level, GG_OS_WIN9X);
    CHECK_EQ_U(r.req_features, GG_CPU_MMX | GG_CPU_SSE);

    /* A key that is a PREFIX of another must not be confused with it:
     * "min_ram_mb" and "min_ram_mb_recommended" both start the same way, and
     * a naive strstr would read the recommended figure as the minimum. */
    {
        const char *tricky =
            "{\"min_ram_mb_recommended\": 512, \"min_ram_mb\": 64}";
        gg_req_parse(tricky, &r);
        CHECK_EQ_U(r.min_ram_mb, 64);
    }

    /* An unknown feature name is ignored rather than rejecting the title. */
    {
        gg_req_parse("{\"cpu_features\":[\"avx512\",\"sse\"]}", &r);
        CHECK_EQ_U(r.req_features, GG_CPU_SSE);
    }

    /* An unknown level name reads as "no opinion", not as level 0 - level 0
     * would be GG_GPU_NONE and would silently pass everything. */
    {
        gg_req_parse("{\"gpu_feature_level\":\"raytracing\"}", &r);
        CHECK_EQ_I(r.min_gpu_level, GG_GPU_UNKNOWN);
    }
}

/* The published verdict file. Only a "no" line may block, so a half-written
 * file degrades to deploying everything rather than to deploying nothing. */
TEST(verdict_file_parser)
{
    char line[256];
    char *title, *lim, *why;
    int v;

    strcpy(line, "no\tDoom 3\tgpu_feature_level\tGPU too old\r\n");
    v = gg_verdict_parse(line, &title, &lim, &why);
    CHECK_EQ_I(v, GG_V_NO);
    CHECK(strcmp(title, "Doom 3") == 0, "title");
    CHECK(strcmp(lim, "gpu_feature_level") == 0, "limiting");
    CHECK(strcmp(why, "GPU too old") == 0, "reason");

    /* A title name with spaces survives - the separator is a tab and only a
     * tab, for the same reason PREFER.TXT's is. */
    strcpy(line, "run\tSoldier Of Fortune 2\t\tok");
    CHECK_EQ_I(gg_verdict_parse(line, &title, &lim, &why), GG_V_RUN);
    CHECK(strcmp(title, "Soldier Of Fortune 2") == 0, "spaced title");

    strcpy(line, "marginal\tThief2\tcpu_mhz\tclose");
    CHECK_EQ_I(gg_verdict_parse(line, &title, &lim, &why), GG_V_MARGINAL);

    /* Comments, blanks and junk yield -1 (ignore), never a verdict. */
    strcpy(line, "# gamegate v1 profile=abc");
    CHECK_EQ_I(gg_verdict_parse(line, &title, &lim, &why), -1);
    strcpy(line, "");
    CHECK_EQ_I(gg_verdict_parse(line, &title, &lim, &why), -1);
    strcpy(line, "banana\tQuake1\t\t");
    CHECK_EQ_I(gg_verdict_parse(line, &title, &lim, &why), -1);
    /* A truncated line - the file was cut mid-write - must not become a no. */
    strcpy(line, "no");
    CHECK_EQ_I(gg_verdict_parse(line, &title, &lim, &why), -1);
    /* A line with a verdict and a title but nothing else is still usable. */
    strcpy(line, "no\tUT2004");
    CHECK_EQ_I(gg_verdict_parse(line, &title, &lim, &why), GG_V_NO);
    CHECK(strcmp(title, "UT2004") == 0, "two-field line");
}

/* OS level mapping. Win9x is decided by the PLATFORM ID, not the version
 * number: Windows 98 is 4.10 and Windows NT 4.0 is 4.0, and reading NT4 as
 * "Windows 95" would put a 9x-only title on it. */
TEST(os_level_mapping)
{
    CHECK_EQ_I(gg_os_level_from_version(4, 10, 0), GG_OS_WIN9X);   /* 98   */
    CHECK_EQ_I(gg_os_level_from_version(4, 90, 0), GG_OS_WIN9X);   /* ME   */
    CHECK_EQ_I(gg_os_level_from_version(4,  0, 1), GG_OS_UNKNOWN); /* NT4  */
    CHECK_EQ_I(gg_os_level_from_version(5,  0, 1), GG_OS_WIN2K);
    CHECK_EQ_I(gg_os_level_from_version(5,  1, 1), GG_OS_WINXP);
    CHECK_EQ_I(gg_os_level_from_version(5,  2, 1), GG_OS_WINXP);   /* 2003 */
    CHECK_EQ_I(gg_os_level_from_version(6,  0, 1), GG_OS_VISTA);
    CHECK_EQ_I(gg_os_level_from_version(6,  1, 1), GG_OS_WIN7);    /* .246 */
    CHECK_EQ_I(gg_os_level_from_version(10, 0, 1), GG_OS_WIN10);

    /* A max_os ceiling is what keeps a 16-bit installer or a title that needs
     * real DOS off a Win7 box. Untested ceilings become wrong ceilings. */
    {
        gg_decision_t d;
        gg_profile_t win7 = box_145();
        win7.os_level = GG_OS_WIN7;
        CHECK_EQ_I(decide_json(win7, "{\"max_os\":\"winxp\"}", &d), GG_V_NO);
        CHECK_EQ_I(decide_json(box_124(), "{\"max_os\":\"winxp\"}", &d),
                   GG_V_RUN);
    }
}

/* Capabilities are SOFTWARE state and must stay out of the verdict.
 *
 * This is the .123/.246 case: neither box has a virtual disc mounter, which
 * silently breaks seven staged titles that mount an image at launch. Reporting
 * that as "this machine cannot run the game" would send someone looking for a
 * faster machine instead of running an installer. */
TEST(capabilities_are_reported_not_folded_into_the_verdict)
{
    gg_decision_t d;
    gg_req_t r;
    gg_profile_t nomount = box_145();      /* comfortably fast, no mounter */
    gg_profile_t hasmount = box_145();

    hasmount.caps = GG_CAP_DISC_MOUNT;

    gg_req_parse("{\"min_cpu_mhz\":700,\"min_ram_mb\":128,"
                 "\"requires_capabilities\":[\"disc_mount\"]}", &r);
    CHECK_EQ_U(r.req_caps, GG_CAP_DISC_MOUNT);

    gg_decide(&nomount, &r, &d);
    CHECK_EQ_I(d.verdict, GG_V_RUN);       /* the HARDWARE is fine */
    CHECK_EQ_U(d.missing_caps, GG_CAP_DISC_MOUNT);

    gg_decide(&hasmount, &r, &d);
    CHECK_EQ_I(d.verdict, GG_V_RUN);
    CHECK_EQ_U(d.missing_caps, 0);

    /* A real hardware failure still wins, and still reports the capability
     * gap alongside it - the two facts are independent. */
    gg_req_parse("{\"min_os\":\"win7\","
                 "\"requires_capabilities\":[\"disc_mount\"]}", &r);
    gg_decide(&nomount, &r, &d);
    CHECK_EQ_I(d.verdict, GG_V_NO);
    CHECK_EQ_U(d.missing_caps, GG_CAP_DISC_MOUNT);

    /* An unknown capability name is ignored rather than blocking - a library
     * written against a newer schema must not brick an older agent. */
    gg_req_parse("{\"requires_capabilities\":[\"holodeck\"]}", &r);
    CHECK_EQ_U(r.req_caps, 0);

    /* And the remedy text exists, so the log line is a next step. */
    CHECK(strstr(gg_capability_remedy(GG_CAP_DISC_MOUNT), "Daemon") != 0,
          "remedy must name the fix");
}

/* Per-shortcut requirements. Battlefield 1942 is the case that forced this:
 * single player wants a mounted disc, the LAN launchers want neither disc nor
 * CD key. Gating the whole title would take working multiplayer off most of
 * the fleet to protect a shortcut nobody could have used. */
TEST(per_shortcut_requirements_override_the_title)
{
    static const char BF1942[] =
        "{\n"
        "  \"requirements_version\": 1,\n"
        "  \"min_cpu_mhz\": 500,\n"
        "  \"min_ram_mb\": 128,\n"
        "  \"min_vram_mb\": 32,\n"
        "  \"gpu_feature_level\": \"tnl\",\n"
        "  \"shortcuts\": {\n"
        "    \"Play Battlefield 1942.bat\": {\n"
        "      \"requires_capabilities\": [\"disc_mount\"]\n"
        "    },\n"
        "    \"Host Battlefield 1942 - LAN.bat\": {\n"
        "      \"requires_capabilities\": [],\n"
        "      \"min_vram_mb\": 8\n"
        "    }\n"
        "  }\n"
        "}\n";
    gg_req_t r;
    gg_decision_t d;
    gg_profile_t p = box_124();          /* GeForce2 GTS 32 MB, no mounter */

    /* Title level: the shortcut block must NOT leak upward. Without the scan
     * limit, "min_vram_mb": 8 from the LAN shortcut would be adopted as the
     * title's own minimum and the 32 MB floor would silently vanish. */
    CHECK_EQ_I(gg_req_parse(BF1942, &r), 1);
    CHECK_EQ_U(r.min_vram_mb, 32);
    CHECK_EQ_U(r.req_caps, 0);

    /* Single player: inherits the hardware floor, adds the disc requirement. */
    gg_req_parse_shortcut(BF1942, "Play Battlefield 1942.bat", &r);
    CHECK_EQ_U(r.min_cpu_mhz, 500);
    CHECK_EQ_U(r.min_vram_mb, 32);
    CHECK_EQ_U(r.req_caps, GG_CAP_DISC_MOUNT);
    gg_decide(&p, &r, &d);
    CHECK_EQ_I(d.verdict, GG_V_RUN);                 /* .124 is on the line */
    CHECK_EQ_U(d.missing_caps, GG_CAP_DISC_MOUNT);   /* ...but cannot mount */

    /* LAN: overrides the VRAM floor and CLEARS the capability requirement.
     * An empty array must clear, which is why the overlay tests PRESENCE of
     * the key rather than the value - an empty list and an absent list are
     * indistinguishable by value and mean opposite things. */
    gg_req_parse_shortcut(BF1942, "Host Battlefield 1942 - LAN.bat", &r);
    CHECK_EQ_U(r.min_vram_mb, 8);
    CHECK_EQ_U(r.req_caps, 0);
    gg_decide(&p, &r, &d);
    CHECK_EQ_I(d.verdict, GG_V_RUN);
    CHECK_EQ_U(d.missing_caps, 0);

    /* Windows filenames are case-insensitive, and so is the lookup. A
     * case-sensitive match here would report "no rule for this shortcut" for
     * a rule sitting right there - the mistake CLAUDE.md has a section about. */
    gg_req_parse_shortcut(BF1942, "play battlefield 1942.BAT", &r);
    CHECK_EQ_U(r.req_caps, GG_CAP_DISC_MOUNT);

    /* A shortcut with no entry of its own just gets the title's rules. */
    gg_req_parse_shortcut(BF1942, "Something Else.bat", &r);
    CHECK_EQ_U(r.min_vram_mb, 32);
    CHECK_EQ_U(r.req_caps, 0);

    /* A title with no "shortcuts" map at all behaves exactly as before. */
    gg_req_parse_shortcut("{\"min_cpu_mhz\":300}", "anything.exe", &r);
    CHECK_EQ_U(r.min_cpu_mhz, 300);

    /* An unterminated shortcuts object must degrade to the title level, not
     * read off the end of the buffer. */
    gg_req_parse_shortcut("{\"min_cpu_mhz\":300,\"shortcuts\":{\"a.bat\":{",
                          "a.bat", &r);
    CHECK_EQ_U(r.min_cpu_mhz, 300);
}

/* A staged title may legitimately have NO floor - Hexen II, Turok 2, Hidden &
 * Dangerous and Max Payne all run on the weakest 3D box here. "Somebody
 * checked and there is none" and "nobody wrote one" are different facts and
 * the schema has to keep them apart, or the library can never be audited. */
TEST(a_checked_no_floor_file_is_distinguishable)
{
    gg_req_t r;
    gg_decision_t d;
    gg_profile_t weak = box_p1();

    /* requirements_version alone: parseable, records the version, states no
     * floor. gg_req_parse returns 0 = "no gate-relevant opinion" ... */
    CHECK_EQ_I(gg_req_parse("{\"requirements_version\":1,"
                            "\"notes\":\"no meaningful floor - verified\"}",
                            &r), 0);
    /* ... but the version survives, which is what lets the host tell a
     * deliberately empty file from a missing one. */
    CHECK_EQ_U(r.version, 1);

    gg_decide(&weak, &r, &d);
    CHECK_EQ_I(d.verdict, GG_V_RUN);
}


MUNIT_MAIN("gamegate (hardware capability gate)",
    RUN(fail_open_on_absent_data);
    RUN(rules_decide_the_obvious_alone);
    RUN(marginal_band_has_both_edges);
    RUN(limiting_factor_names_the_real_cause);
    RUN(profile_hash_is_stable_and_sensitive);
    RUN(gpu_table_handles_non_monotonic_ids);
    RUN(requires_json_scanner);
    RUN(verdict_file_parser);
    RUN(os_level_mapping);
    RUN(capabilities_are_reported_not_folded_into_the_verdict);
    RUN(per_shortcut_requirements_override_the_title);
    RUN(a_checked_no_floor_file_is_distinguishable);
)

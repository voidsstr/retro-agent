/* test_ipxplan.c - TRUE-SOURCE: compiles the REAL agent/shared/ipxplan.h, the
 * decisions behind agent/src/ipxsetup.c / ipxnt.c / ipx9x.c (agent 1.97.0,
 * IPXSETUP) - and regmerge.h, whose REGEDIT4 reader the Win98 template is
 * checked with.
 *
 * THE OLD BEHAVIOUR: no agent code installed IPX anywhere. .123's NWLink was
 * added by hand on 2026-08-20 and lost to the 2026-08-27 re-image; .243 (Win98
 * SE) and every other XP box had none (R3/R4, measured).
 *
 * Asserted here: the mechanism per Windows; every state transition, pending
 * vs broken decided by install time against boot time; never a reboot on NT and
 * a 9x reboot only when armed, once per install, outside the gap and never
 * mid-copy; the per-OS frame encodings (and that the SWAPPED numbers - 802.2 is
 * "1" on Win98 but "2" on XP - do not read as 802.2); the payload manifest and
 * its CRC; the command words; and the Win98 template - rendered, parsed,
 * additive, VREDIR-free and binding-last.
 */
#include "munit.h"
#include <string.h>
#include <stdio.h>

#include "../../agent/shared/ipxplan.h"

static ipx_obs_t obs(int mech)
{
    ipx_obs_t o;
    memset(&o, 0, sizeof(o));
    o.mech = mech;
    o.managed = 1;
    o.enabled = 1;
    o.files_present = 1;
    o.boot_time = 1000000;
    return o;
}

TEST(the_mechanism_follows_the_windows)
{
    const char *why;
    CHECK_EQ_I(ipx_mech_for(0, 4, 10, 2222, &why), IPX_MECH_9X);           /* 98 SE */
    CHECK_EQ_I(ipx_mech_for(0, 4, 10, 1998, &why), IPX_MECH_NONE);         /* 98 FE */
    CHECK(strstr(why, "98 SE") != NULL, "says the template is 98 SE only");
    CHECK_EQ_I(ipx_mech_for(0, 4, 0, 950, &why), IPX_MECH_NONE);           /* 95 */
    CHECK_EQ_I(ipx_mech_for(0, 4, 90, 3000, &why), IPX_MECH_NONE);         /* ME */
    CHECK_EQ_I(ipx_mech_for(1, 5, 0, 2195, &why), IPX_MECH_NETCFG);        /* 2000 */
    CHECK_EQ_I(ipx_mech_for(1, 5, 1, 2600, &why), IPX_MECH_NETCFG);        /* XP */
    CHECK_EQ_I(ipx_mech_for(1, 5, 2, 3790, &why), IPX_MECH_NETCFG);        /* 2003 */
    CHECK_EQ_I(ipx_mech_for(1, 6, 1, 7600, &why), IPX_MECH_NONE);          /* Win7 */
    CHECK(strstr(why, "Vista") != NULL && strstr(why, "IPXWrapper") != NULL,
          "Win7: NWLink was removed in Vista - IPXWrapper per title");
    CHECK_EQ_I(ipx_mech_for(1, 6, 0, 6000, &why), IPX_MECH_NONE);          /* Vista */
    CHECK_EQ_I(ipx_mech_for(1, 10, 0, 19045, &why), IPX_MECH_NONE);        /* 10/11 */
    CHECK_EQ_I(ipx_mech_for(1, 4, 0, 1381, &why), IPX_MECH_NONE);          /* NT 4 */
    CHECK(strcmp(ipx_os_name(0, 4, 10), "Windows 98") == 0, "os name 98");
    CHECK(strcmp(ipx_os_name(1, 5, 1), "Windows XP") == 0, "os name XP");
    CHECK(strcmp(ipx_os_name(1, 6, 1), "Windows 7") == 0, "os name 7");
}

TEST(every_state)
{
    ipx_obs_t o;

    o = obs(IPX_MECH_NONE);
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_NOT_SUPPORTED);
    o = obs(IPX_MECH_NETCFG); o.managed = 0;
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_POLICY);
    o = obs(IPX_MECH_NETCFG); o.hung = 1; o.winsock_ipx = 1;
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_HUNG);

    /* active: the post-condition is Winsock, not the registry */
    o = obs(IPX_MECH_NETCFG); o.winsock_ipx = 1; o.frame_configured = IPX_FRAME_8022;
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_ACTIVE);          /* XP: Auto wanted, anything goes */
    o = obs(IPX_MECH_9X); o.winsock_ipx = 1; o.frame_configured = IPX_FRAME_8022;
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_ACTIVE);
    o.frame_configured = IPX_FRAME_AUTO;                /* a GUI install left Auto on 98 */
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_FRAME_MISMATCH);

    /* pending vs broken: install time against boot time */
    o = obs(IPX_MECH_9X); o.component_present = 1;
    o.installed_at = o.boot_time + 60;
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_PENDING_REBOOT);
    o.installed_at = o.boot_time;                       /* the same second still counts */
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_PENDING_REBOOT);
    o.installed_at = o.boot_time - 60;                  /* installed before this boot */
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_BROKEN);
    o.installed_at = 0;                                 /* installed by someone else */
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_BROKEN);

    o = obs(IPX_MECH_NETCFG); o.enabled = 0;
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_DISABLED);
    o = obs(IPX_MECH_NETCFG); o.attempts = IPX_ATTEMPT_CAP;
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_GAVE_UP);
    o = obs(IPX_MECH_NETCFG); o.files_present = 0;
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_PAYLOAD_MISSING);
    o = obs(IPX_MECH_9X); o.files_present = 0;          /* 9x copies its payload in */
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_NOT_INSTALLED);
    o = obs(IPX_MECH_NETCFG);
    CHECK_EQ_I(ipx_decide(&o), IPX_ST_NOT_INSTALLED);
    CHECK(strcmp(ipx_state_name(IPX_ST_PENDING_REBOOT), "pending_reboot") == 0, "names");
    CHECK(strcmp(ipx_state_name(IPX_ST_COUNT), "unknown") == 0, "out of range");
}

TEST(the_plan)
{
    ipx_obs_t o;
    o = obs(IPX_MECH_NETCFG);
    CHECK_EQ_I(ipx_plan(IPX_ST_NOT_INSTALLED, &o, 0, 0), IPX_DO_NETCFG);
    o = obs(IPX_MECH_9X);
    CHECK_EQ_I(ipx_plan(IPX_ST_NOT_INSTALLED, &o, 0, 0), IPX_DO_STAGE_FILES | IPX_DO_REGISTRY);
    /* nothing to do once installed, whatever the flags */
    CHECK_EQ_I(ipx_plan(IPX_ST_ACTIVE, &o, 1, 1), IPX_DO_NOTHING);
    CHECK_EQ_I(ipx_plan(IPX_ST_PENDING_REBOOT, &o, 1, 1), IPX_DO_NOTHING);
    CHECK_EQ_I(ipx_plan(IPX_ST_FRAME_MISMATCH, &o, 1, 1), IPX_DO_NOTHING);
    CHECK_EQ_I(ipx_plan(IPX_ST_HUNG, &o, 1, 1), IPX_DO_NOTHING);
    CHECK_EQ_I(ipx_plan(IPX_ST_NOT_SUPPORTED, &o, 1, 1), IPX_DO_NOTHING);
    /* force = past IpxSetup=0; retry = past the cap, or re-run a broken install */
    o = obs(IPX_MECH_NETCFG); o.enabled = 0;
    CHECK_EQ_I(ipx_plan(IPX_ST_DISABLED, &o, 0, 0), IPX_DO_NOTHING);
    CHECK_EQ_I(ipx_plan(IPX_ST_DISABLED, &o, 1, 0), IPX_DO_NETCFG);
    o = obs(IPX_MECH_NETCFG); o.attempts = IPX_ATTEMPT_CAP;
    CHECK_EQ_I(ipx_plan(IPX_ST_GAVE_UP, &o, 0, 0), IPX_DO_NOTHING);
    CHECK_EQ_I(ipx_plan(IPX_ST_GAVE_UP, &o, 1, 0), IPX_DO_NOTHING);    /* force is not retry */
    CHECK_EQ_I(ipx_plan(IPX_ST_GAVE_UP, &o, 0, 1), IPX_DO_NETCFG);
    o = obs(IPX_MECH_NETCFG); o.enabled = 0; o.attempts = IPX_ATTEMPT_CAP;
    CHECK_EQ_I(ipx_plan(IPX_ST_DISABLED, &o, 1, 0), IPX_DO_NOTHING);   /* both blockers */
    CHECK_EQ_I(ipx_plan(IPX_ST_DISABLED, &o, 1, 1), IPX_DO_NETCFG);
    o = obs(IPX_MECH_9X); o.component_present = 1;
    CHECK_EQ_I(ipx_plan(IPX_ST_BROKEN, &o, 0, 0), IPX_DO_NOTHING);
    CHECK_EQ_I(ipx_plan(IPX_ST_BROKEN, &o, 0, 1), IPX_DO_STAGE_FILES | IPX_DO_REGISTRY);
    o = obs(IPX_MECH_NETCFG); o.files_present = 0;
    CHECK_EQ_I(ipx_plan(IPX_ST_NOT_INSTALLED, &o, 1, 1), IPX_DO_NOTHING);
    /* the 9x writes stay shut until the orchestrator validated the template */
    /* validated 2026-10-01: absent = open (was 0 - no Win98 box ever got IPX
     * by itself); an explicit 0 still shuts the writes */
    CHECK_EQ_I(ipx_9x_writes_allowed(0, 0), 1);
    CHECK_EQ_I(IPX_9X_TEMPLATE_VALIDATED, 1);
    CHECK_EQ_I(ipx_9x_writes_allowed(1, 0), 0);
    CHECK_EQ_I(ipx_9x_writes_allowed(1, 2), 1);
    CHECK_EQ_I(ipx_9x_writes_allowed(1, 1), 1);
}

TEST(reboots_never_on_nt_and_once_per_install_on_9x)
{
    unsigned long inst = 2000000;
    /* NT: never, whatever else holds */
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_NETCFG, 1, IPX_ST_PENDING_REBOOT, inst, 0, -1, 1200, 0), 0);
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_NONE, 1, IPX_ST_PENDING_REBOOT, inst, 0, -1, 1200, 0), 0);
    /* 9x: the one yes */
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 1, IPX_ST_PENDING_REBOOT, inst, 0, -1, 1200, 0), 1);
    /* ...and each condition alone turns it into a no */
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 0, IPX_ST_PENDING_REBOOT, inst, 0, -1, 1200, 0), 0);
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 1, IPX_ST_BROKEN, inst, 0, -1, 1200, 0), 0);
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 1, IPX_ST_ACTIVE, inst, 0, -1, 1200, 0), 0);
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 1, IPX_ST_PENDING_REBOOT, 0, 0, -1, 1200, 0), 0);
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 1, IPX_ST_PENDING_REBOOT, inst, inst + 5, 5, 1200, 0), 0);
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 1, IPX_ST_PENDING_REBOOT, inst, inst, 5000, 1200, 0), 0);
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 1, IPX_ST_PENDING_REBOOT, inst, inst - 9000, 600, 1200, 0), 0);
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 1, IPX_ST_PENDING_REBOOT, inst, inst - 9000, 9000, 1200, 0), 1);
    CHECK_EQ_I(ipx_should_reboot(IPX_MECH_9X, 1, IPX_ST_PENDING_REBOOT, inst, 0, -1, 1200, 1), 0);
}

/* NETTRANS.INF [NWLINK.ndi.reg], Windows 98 SE (md5 7b42b0f30239b10268080138d27cea46) */
static const char nettrans_frame_enum[] =
    "HKR,Ndi\\params\\Frame_Type,default,,4\r\n"
    "HKR,Ndi\\params\\Frame_Type\\enum,\"4\",,\"Auto\"\r\n"
    "HKR,Ndi\\params\\Frame_Type\\enum,\"1\",,\"Ethernet 802.2\"\r\n"
    "HKR,Ndi\\params\\Frame_Type\\enum,\"0\",,\"Ethernet 802.3\"\r\n"
    "HKR,Ndi\\params\\Frame_Type\\enum,\"2\",,\"Ethernet II\"\r\n"
    "HKR,Ndi\\params\\Frame_Type\\enum,\"5\",,\"Token Ring\"\r\n"
    "HKR,Ndi\\params\\Frame_Type\\enum,\"6\",,\"Token Ring SNAP\"\r\n";

TEST(frame_encodings_per_os_and_the_swap_trap)
{
    /* every Win98 enum value in the INF maps to the frame its label names */
    struct { const char *v, *label; int f; } inf[] = {
        { "4", "Auto", IPX_FRAME_AUTO }, { "1", "Ethernet 802.2", IPX_FRAME_8022 },
        { "0", "Ethernet 802.3", IPX_FRAME_8023 }, { "2", "Ethernet II", IPX_FRAME_ETHII },
    };
    int i;
    for (i = 0; i < 4; i++) {
        char line[96];
        snprintf(line, sizeof(line), "\"%s\",,\"%s\"\r\n", inf[i].v, inf[i].label);
        CHECK(strstr(nettrans_frame_enum, line) != NULL, "the fixture names this value");
        CHECK_EQ_I(ipx_frame_from_9x(inf[i].v), inf[i].f);
        CHECK(strcmp(ipx_frame_to_9x(inf[i].f), inf[i].v) == 0, "round trip");
    }
    CHECK_EQ_I(ipx_frame_from_9x("5"), IPX_FRAME_UNKNOWN);      /* Token Ring */
    CHECK_EQ_I(ipx_frame_from_9x(""), IPX_FRAME_AUTO);          /* the INF default */
    CHECK_EQ_I(ipx_frame_from_9x(NULL), IPX_FRAME_AUTO);
    /* XP PktType (KB150546) */
    CHECK_EQ_I(ipx_frame_from_nt("ff"), IPX_FRAME_AUTO);
    CHECK_EQ_I(ipx_frame_from_nt("FF"), IPX_FRAME_AUTO);
    CHECK_EQ_I(ipx_frame_from_nt("2"), IPX_FRAME_8022);
    CHECK_EQ_I(ipx_frame_from_nt("1"), IPX_FRAME_8023);
    CHECK_EQ_I(ipx_frame_from_nt("0"), IPX_FRAME_ETHII);
    CHECK_EQ_I(ipx_frame_from_nt("3"), IPX_FRAME_SNAP);
    /* THE TRAP: the same digit is a different frame per OS */
    CHECK(ipx_frame_from_9x("2") != IPX_FRAME_8022, "Win98 2 is Ethernet II, not 802.2");
    CHECK(ipx_frame_from_nt("1") != IPX_FRAME_8022, "XP 1 is 802.3, not 802.2");
    CHECK(ipx_frame_from_9x("1") == ipx_frame_from_nt("2"), "802.2 is 1 on 98 and 2 on XP");
    /* what the fleet wants */
    CHECK_EQ_I(ipx_frame_wanted(IPX_MECH_9X), IPX_FRAME_8022);
    CHECK_EQ_I(ipx_frame_wanted(IPX_MECH_NETCFG), IPX_FRAME_AUTO);
}

TEST(the_payload_manifest_and_its_crc)
{
    static const unsigned char check[] = "123456789";
    int i, nwlink = 0, wsipx = 0;
    /* the standard CRC-32 check value (zlib, IEEE 802.3) */
    CHECK_EQ_U(ipx_crc32(0, check, 9), 0xCBF43926UL);
    CHECK_EQ_U(ipx_crc32(ipx_crc32(0, check, 4), check + 4, 5), 0xCBF43926UL);  /* incremental */
    CHECK_EQ_U(ipx_crc32(0, check, 0), 0UL);
    CHECK_EQ_I(IPX9X_NPAYLOAD, 2);
    for (i = 0; i < IPX9X_NPAYLOAD; i++) {
        const ipx_file_t *f = &ipx9x_payload[i];
        if (!strcmp(f->name, "NWLINK.VXD")) {
            nwlink = 1;
            CHECK_EQ_U(f->size, 51010UL);
            CHECK_EQ_U(f->crc32, 0x5979320AUL);
            CHECK(!strcmp(f->md5, "d92a994f76be8ce6469affa6f3530300"), "nwlink md5 (R3)");
        }
        if (!strcmp(f->name, "WSIPX.VXD")) {
            wsipx = 1;
            CHECK_EQ_U(f->size, 14526UL);
            CHECK_EQ_U(f->crc32, 0x27ACFFF1UL);
            CHECK(!strcmp(f->md5, "ec91ebf166e66bf7108dd93d374c630d"), "wsipx md5 (R3)");
        }
        CHECK(strcmp(f->name, "NWNBLINK.VXD") != 0, "no NetBIOS over IPX");
        CHECK_EQ_I(ipx_payload_ok(f, f->size, f->crc32), 1);
        CHECK_EQ_I(ipx_payload_ok(f, f->size - 1, f->crc32), 0);       /* truncated */
        CHECK_EQ_I(ipx_payload_ok(f, f->size, f->crc32 ^ 1), 0);       /* same size, other bytes */
    }
    CHECK(nwlink && wsipx, "both VxDs in the manifest");
}

TEST(command_words)
{
    CHECK_EQ_I(ipx_parse_args(""), IPX_ARG_STATUS);
    CHECK_EQ_I(ipx_parse_args("status"), IPX_ARG_STATUS);
    CHECK_EQ_I(ipx_parse_args("  STATUS "), IPX_ARG_STATUS);
    CHECK_EQ_I(ipx_parse_args("apply"), IPX_ARG_APPLY);
    CHECK_EQ_I(ipx_parse_args("Apply Force"), IPX_ARG_APPLY | IPX_ARG_FORCE);
    CHECK_EQ_I(ipx_parse_args("apply retry force"), IPX_ARG_APPLY | IPX_ARG_FORCE | IPX_ARG_RETRY);
    /* unknown words are refused - never read as apply */
    CHECK_EQ_I(ipx_parse_args("force"), IPX_ARG_BAD);
    CHECK_EQ_I(ipx_parse_args("retry"), IPX_ARG_BAD);
    CHECK_EQ_I(ipx_parse_args("install"), IPX_ARG_BAD);
    CHECK_EQ_I(ipx_parse_args("apply install"), IPX_ARG_BAD);
    CHECK_EQ_I(ipx_parse_args("apply ALLOW3DFX"), IPX_ARG_BAD);
    CHECK_EQ_I(ipx_parse_args("status apply"), IPX_ARG_BAD);
    CHECK_EQ_I(ipx_parse_args("status force"), IPX_ARG_BAD);
    CHECK_EQ_I(ipx_parse_args("applyforce"), IPX_ARG_BAD);
}

TEST(the_com_ids_are_the_documented_ones)
{
    static const ipx_guid_t c = IPX_CLSID_CNETCFG, n = IPX_IID_INETCFG, l = IPX_IID_INETCFGLOCK;
    static const ipx_guid_t s = IPX_IID_INETCFGCLASSSETUP, t = IPX_GUID_DEVCLASS_NETTRANS;
    CHECK(c.d1 == 0x5B035261UL && c.d2 == 0x40F9 && c.d4[1] == 0xEC && c.d4[7] == 0x0E, "CLSID_CNetCfg");
    CHECK(n.d1 == 0xC0E8AE93UL && n.d2 == 0x306E && n.d4[1] == 0xCF, "IID_INetCfg");
    CHECK(l.d1 == 0xC0E8AE9FUL, "IID_INetCfgLock");
    CHECK(s.d1 == 0xC0E8AE9DUL, "IID_INetCfgClassSetup");
    CHECK(t.d1 == 0x4D36E975UL && t.d2 == 0xE325 && t.d4[7] == 0x18, "GUID_DEVCLASS_NETTRANS");
    CHECK(!strcmp(IPX_NT_COMPONENT, "ms_nwipx"), "netnwlnk.inf component");
}

/* ---- the Win98 template ---------------------------------------------------- */

#define NIC "PCI\\VEN_10EC&DEV_8029&SUBSYS_802910EC&REV_00\\BUS_00&DEV_0B&FUNC_00"

static char g_txt[3][16384];
static rm_parser_t g_ps;
static rm_entry_t g_e;

static int render_all(const char *nic, int with_ws2)
{
    ipx_var_t v[6];
    int p;
    v[0].name = "K"; v[0].value = "0002";
    v[1].name = "I"; v[1].value = "0000";
    v[2].name = "Q"; v[2].value = "0";
    v[3].name = "FRAME"; v[3].value = ipx_frame_to_9x(IPX_FRAME_8022);
    v[4].name = "NIC"; v[4].value = nic;
    v[5].name = "SYSDIR:Q"; v[5].value = "C:\\\\WINDOWS\\\\SYSTEM";
    for (p = 0; p < IPX9X_NPARTS; p++) {
        g_txt[p][0] = 0;
        if (p == IPX9X_PART_WS2 && !with_ws2)
            continue;
        if (ipx9x_render(ipx9x_part(p), v, 6, g_txt[p], sizeof(g_txt[p])) < 0)
            return -1;
    }
    return 0;
}

static int check_all(int with_ws2, const char **why)
{
    const char *parts[3];
    parts[0] = g_txt[0];
    parts[1] = with_ws2 ? g_txt[1] : NULL;
    parts[2] = g_txt[2];
    return ipx9x_template_check(parts, 3, NIC, "0000", &g_ps, &g_e, why);
}

/* the value `name` under `key` (any case) in rendered part p, or NULL */
static const rm_entry_t *find_value(int p, const char *key, const char *name)
{
    rm_init(&g_ps, g_txt[p], strlen(g_txt[p]));
    while (rm_next(&g_ps, &g_e))
        if (rm_prefix_ci(g_e.key, key) && strlen(g_e.key) == strlen(key) &&
            rm_prefix_ci(g_e.name, name) && strlen(g_e.name) == strlen(name))
            return &g_e;
    return NULL;
}

TEST(the_template_renders_and_passes_its_own_rules)
{
    const char *why = "";
    const rm_entry_t *e;
    int n;
    CHECK_EQ_I(render_all(NIC, 1), 0);
    n = check_all(1, &why);
    CHECK(n > 60, why);
    CHECK_EQ_I(render_all(NIC, 0), 0);               /* catalog already lists IPX */
    CHECK(check_all(0, &why) > 30, why);
    CHECK_EQ_I(render_all(NIC, 1), 0);

    e = find_value(0, "System\\CurrentControlSet\\Services\\Class\\NetTrans\\0002", "Frame_Type");
    CHECK(e && e->type == RM_REG_SZ && !strcmp((const char *)e->data, "1"), "Frame_Type pinned to 1 = 802.2");
    e = find_value(0, "System\\CurrentControlSet\\Services\\Class\\NetTrans\\0002", "DeviceVxDs");
    CHECK(e && !strcmp((const char *)e->data, "nwlink.vxd"), "DeviceVxDs");
    e = find_value(0, "Enum\\Network\\NWLINK\\0000", "Driver");
    CHECK(e && !strcmp((const char *)e->data, "NetTrans\\0002"), "the devnode points at the class key");
    e = find_value(0, "Enum\\Network\\NWLINK\\0000", "MasterCopy");
    CHECK(e && !strcmp((const char *)e->data, "Enum\\Network\\NWLINK\\0000"), "MasterCopy");
    e = find_value(0, "System\\CurrentControlSet\\Services\\VxD\\NWLINK", "StaticVxD");
    CHECK(e && !strcmp((const char *)e->data, "nwlink.vxd"), "StaticVxD");
    e = find_value(0, "System\\CurrentControlSet\\Services\\VxD\\Winsock", "IPX/SPX Winsock Provider");
    CHECK(e && !strcmp((const char *)e->data, "wsipx.vxd"), "the Winsock VxD provider");

    /* values a Network-applet install writes that NETTRANS.INF does not name -
     * read off W98BUILD after the golden install, 2026-10-01 */
    e = find_value(0, "System\\CurrentControlSet\\Services\\Class\\NetTrans\\0002", "DriverDate");
    CHECK(e && !strcmp((const char *)e->data, " 4-23-1999"), "DriverDate (leading space, as NETDI writes it)");
    e = find_value(0, "System\\CurrentControlSet\\Services\\Class\\NetTrans\\0002", "Network_Id");
    CHECK(e && !strcmp((const char *)e->data, "0"), "Network_Id 0");
    e = find_value(0, "System\\CurrentControlSet\\Services\\Class\\NetTrans\\0002\\Ndi", "StaticVxD");
    CHECK(e && !strcmp((const char *)e->data, "nwlink.vxd"), "Ndi StaticVxD");
    e = find_value(0, "System\\CurrentControlSet\\Services\\Class\\NetTrans\\0002\\Ndi", "InstallInf");
    CHECK(e && e->type == RM_REG_SZ && e->len == 0, "Ndi InstallInf empty");
    e = find_value(0, "System\\CurrentControlSet\\Services\\Class\\NetTrans\\0002\\Ndi\\params\\Frame_Type", "");
    CHECK(e && !strcmp((const char *)e->data, "1"), "the Frame_Type param's current value mirrors 802.2");
    e = find_value(0, "System\\CurrentControlSet\\Services\\VxD\\NWLINK", "cachesize");
    CHECK(e && !strcmp((const char *)e->data, "0"), "VxD cachesize 0");

    /* the queued WSCInstallProvider: MSWSOSP's GUID and the IPX datagram block */
    e = find_value(1, "Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item0\\1", "GUID");
    CHECK(e && e->type == RM_REG_BINARY && e->len == 16 && e->data[0] == 0xE1 && e->data[3] == 0xFF &&
          e->data[15] == 0x09, "MSWSOSP {FF017DE1-CAE9-11CF-8A99-00AA0062C609}");
    e = find_value(1, "Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item0\\1", "Provider");
    CHECK(e && !strcmp((const char *)e->data, "C:\\WINDOWS\\SYSTEM\\mswsosp.dll"), "the %11% provider path");
    e = find_value(1, "Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item0\\1\\ProtocolInfo\\3", "Protocol");
    CHECK(e && e->len == 4 && e->data[0] == 0xE8 && e->data[1] == 0x03, "NSPROTO_IPX = 1000");
    e = find_value(1, "Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item0\\1\\ProtocolInfo\\3", "AddressFamily");
    CHECK(e && e->len == 4 && e->data[0] == IPX_AF_IPX, "AF_IPX = 6");
    e = find_value(1, "Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item0\\1\\ProtocolInfo\\3", "SocketType");
    CHECK(e && e->len == 4 && e->data[0] == 2, "SOCK_DGRAM");
    e = find_value(1, "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", "NetSetup");
    CHECK(e && !strcmp((const char *)e->data, "rundll.exe netdi.dll,FirstBootCall"), "netdi's own first-boot runner");

    /* the binding: one value, empty string, under the adapter's Bindings key */
    e = find_value(2, "Enum\\" NIC "\\Bindings", "NWLINK\\0000");
    CHECK(e && e->type == RM_REG_SZ && e->len == 0, "the binding NWLINK\\0000=\"\"");
}

TEST(nothing_rides_ipx_and_nothing_is_deleted)
{
    int p;
    CHECK_EQ_I(render_all(NIC, 1), 0);
    for (p = 0; p < 3; p++) {
        CHECK(!ipx_has_ci(g_txt[p], "VREDIR"), "Client for Microsoft Networks never binds to IPX");
        CHECK(!ipx_has_ci(g_txt[p], "NWNBLINK"), "no NetBIOS over IPX");
        CHECK(strstr(g_txt[p], "[-") == NULL && strstr(g_txt[p], "=-") == NULL, "additive only");
    }
    /* the NWLINK devnode's Bindings key exists and holds NO value */
    CHECK(strstr(g_txt[0], "[HKEY_LOCAL_MACHINE\\Enum\\Network\\NWLINK\\0000\\Bindings]\r\n\r\n") != NULL,
          "the protocol's own Bindings key is empty");
    CHECK(find_value(0, "Enum\\Network\\NWLINK\\0000\\Bindings", "VREDIR\\0000") == NULL, "no VREDIR value");
}

TEST(the_rules_refuse_a_bad_template)
{
    const char *why = "";
    const char *parts[3];
    static char bad[16384 + 512];

    CHECK_EQ_I(render_all(NIC, 1), 0);
    parts[0] = g_txt[0];
    parts[1] = g_txt[1];

    /* a capture that bound Client for MS Networks over IPX */
    snprintf(bad, sizeof(bad), "%s[HKEY_LOCAL_MACHINE\\Enum\\Network\\NWLINK\\0000\\Bindings]\r\n"
             "\"VREDIR\\\\0002\"=\"\"\r\n", g_txt[0]);
    parts[0] = bad;
    parts[2] = g_txt[2];
    CHECK_EQ_I(ipx9x_template_check(parts, 3, NIC, "0000", &g_ps, &g_e, &why), -1);
    CHECK(strstr(why, "VREDIR") != NULL, why);

    /* a deletion */
    snprintf(bad, sizeof(bad), "%s[-HKEY_LOCAL_MACHINE\\Enum\\Network\\MSTCP\\0000]\r\n", g_txt[0]);
    CHECK_EQ_I(ipx9x_template_check(parts, 3, NIC, "0000", &g_ps, &g_e, &why), -1);
    snprintf(bad, sizeof(bad), "%s[HKEY_LOCAL_MACHINE\\Software\\X]\r\n\"Y\"=-\r\n", g_txt[0]);
    CHECK_EQ_I(ipx9x_template_check(parts, 3, NIC, "0000", &g_ps, &g_e, &why), -1);

    /* a value written AFTER the binding */
    parts[0] = g_txt[0];
    snprintf(bad, sizeof(bad), "%s[HKEY_LOCAL_MACHINE\\Software\\X]\r\n\"Y\"=\"1\"\r\n", g_txt[2]);
    parts[2] = bad;
    CHECK_EQ_I(ipx9x_template_check(parts, 3, NIC, "0000", &g_ps, &g_e, &why), -1);
    CHECK(strstr(why, "LAST") != NULL, why);

    /* no binding at all */
    parts[2] = NULL;
    CHECK_EQ_I(ipx9x_template_check(parts, 3, NIC, "0000", &g_ps, &g_e, &why), -1);

    /* the binding for another instance */
    parts[2] = g_txt[2];
    CHECK_EQ_I(ipx9x_template_check(parts, 3, NIC, "0001", &g_ps, &g_e, &why), -1);

    /* outside HKLM */
    snprintf(bad, sizeof(bad), "REGEDIT4\r\n\r\n[HKEY_CURRENT_USER\\Software\\X]\r\n\"Y\"=\"1\"\r\n");
    parts[0] = bad;
    CHECK_EQ_I(ipx9x_template_check(parts, 3, NIC, "0000", &g_ps, &g_e, &why), -1);

    /* not REGEDIT4 (the v5 dialect does nothing at all on Win9x) */
    snprintf(bad, sizeof(bad), "Windows Registry Editor Version 5.00\r\n\r\n%s", g_txt[0] + 10);
    parts[0] = bad;
    CHECK_EQ_I(ipx9x_template_check(parts, 3, NIC, "0000", &g_ps, &g_e, &why), -1);

    /* the real one still passes */
    parts[0] = g_txt[0];
    CHECK(ipx9x_template_check(parts, 3, NIC, "0000", &g_ps, &g_e, &why) > 0, why);
}

TEST(rendering_refuses_what_could_break_a_line)
{
    char out[256];
    ipx_var_t v[1];
    v[0].name = "NIC";
    v[0].value = "PCI\\X]";                            /* would close the key header */
    CHECK_EQ_I(ipx9x_render("[HKEY_LOCAL_MACHINE\\Enum\\%NIC%]", v, 1, out, sizeof(out)), -1);
    v[0].value = "PCI\\X\r\n[HKEY_LOCAL_MACHINE\\Y]";
    CHECK_EQ_I(ipx9x_render("[HKEY_LOCAL_MACHINE\\Enum\\%NIC%]", v, 1, out, sizeof(out)), -1);
    v[0].value = "PCI\\X";
    CHECK_EQ_I(ipx9x_render("%NOPE%", v, 1, out, sizeof(out)), -1);   /* unknown placeholder */
    CHECK_EQ_I(ipx9x_render("50% off", v, 1, out, sizeof(out)), -1); /* unterminated */
    CHECK(ipx9x_render("[HKEY_LOCAL_MACHINE\\Enum\\%NIC%]", v, 1, out, sizeof(out)) > 0 &&
          !strcmp(out, "[HKEY_LOCAL_MACHINE\\Enum\\PCI\\X]"), out);
    CHECK_EQ_I(ipx9x_render("%NIC%", v, 1, out, 3), -1);              /* does not fit */
    CHECK_EQ_I(ipx_quote_backslashes("C:\\WINDOWS\\SYSTEM", out, sizeof(out)), 0);
    CHECK(!strcmp(out, "C:\\\\WINDOWS\\\\SYSTEM"), out);
}

TEST(every_key_is_listed_in_order_the_empty_ones_too)
{
    const char *p;
    char key[RM_KEY_MAX], first[RM_KEY_MAX], last[RM_KEY_MAX];
    int n = 0, saw_bindings = 0;
    CHECK_EQ_I(render_all(NIC, 1), 0);
    p = g_txt[0];
    first[0] = last[0] = 0;
    while (ipx9x_next_key(&p, key, sizeof(key))) {
        if (!n)
            strcpy(first, key);
        strcpy(last, key);
        if (!strcmp(key, "Enum\\Network\\NWLINK\\0000\\Bindings"))
            saw_bindings = 1;
        n++;
    }
    CHECK(n > 15, "the stack part names many keys");
    CHECK(!strcmp(first, "System\\CurrentControlSet\\Services\\Class\\NetTrans\\0002"), first);
    CHECK(saw_bindings, "the empty Bindings key is listed, so the agent creates it");
    p = g_txt[2];
    CHECK(ipx9x_next_key(&p, key, sizeof(key)) && !strcmp(key, "Enum\\" NIC "\\Bindings"), key);
    CHECK(!ipx9x_next_key(&p, key, sizeof(key)), "one key in the bind part");
    (void)last;
}

TEST(the_adapter_the_binding_goes_on)
{
    const char *rtl_ids = "PCI\\VEN_10EC&DEV_8029&SUBSYS_802910EC&REV_00,PCI\\VEN_10EC&DEV_8029";
    CHECK_EQ_I(ipx9x_nic_candidate("Net", 0, 1, rtl_ids, "PCI\\CC_0200", "Realtek RTL8029(AS) PCI Ethernet NIC"), 1);
    CHECK_EQ_I(ipx9x_nic_candidate("net", 0, 1, "ISAPNP\\TCM5090", "", "3Com EtherLink III"), 1);
    /* Dial-Up / VPN, by id or by name */
    CHECK_EQ_I(ipx9x_nic_candidate("Net", 0, 1, "*PNP8387", "", "Dial-Up Adapter"), 0);
    CHECK_EQ_I(ipx9x_nic_candidate("Net", 0, 1, "", "*pnp8387", "x"), 0);
    CHECK_EQ_I(ipx9x_nic_candidate("Net", 0, 1, "*PNP8395", "", "Microsoft VPN"), 0);
    CHECK_EQ_I(ipx9x_nic_candidate("Net", 0, 1, "ROOT\\NET\\0000", "", "Dial-Up Adapter #2"), 0);
    /* not the LAN adapter */
    CHECK_EQ_I(ipx9x_nic_candidate("Net", 0, 0, rtl_ids, "", "Realtek"), 0);       /* no TCP/IP */
    CHECK_EQ_I(ipx9x_nic_candidate("Net", 22, 1, rtl_ids, "", "Realtek"), 0);      /* disabled */
    CHECK_EQ_I(ipx9x_nic_candidate("NetTrans", 0, 1, "MSTCP", "", "TCP/IP"), 0);   /* a protocol */
    CHECK_EQ_I(ipx9x_nic_candidate(NULL, 0, 1, rtl_ids, "", "x"), 0);
}

TEST(indices)
{
    int used1[] = { 0, 1, 3 };
    int used2[] = { 1 };
    CHECK_EQ_I(ipx_first_free(used1, 3), 2);
    CHECK_EQ_I(ipx_first_free(used2, 1), 0);
    CHECK_EQ_I(ipx_first_free(NULL, 0), 0);
    CHECK_EQ_I(ipx_index_ok("0002"), 1);
    CHECK_EQ_I(ipx_index_ok("002"), 0);
    CHECK_EQ_I(ipx_index_ok("00020"), 0);
    CHECK_EQ_I(ipx_index_ok("00a2"), 0);
}

MUNIT_MAIN("IPXSETUP - install IPX/SPX (agent/shared/ipxplan.h, agent 1.97.0)",
    RUN(the_mechanism_follows_the_windows);
    RUN(every_state);
    RUN(the_plan);
    RUN(reboots_never_on_nt_and_once_per_install_on_9x);
    RUN(frame_encodings_per_os_and_the_swap_trap);
    RUN(the_payload_manifest_and_its_crc);
    RUN(command_words);
    RUN(the_com_ids_are_the_documented_ones);
    RUN(the_template_renders_and_passes_its_own_rules);
    RUN(nothing_rides_ipx_and_nothing_is_deleted);
    RUN(the_rules_refuse_a_bad_template);
    RUN(rendering_refuses_what_could_break_a_line);
    RUN(every_key_is_listed_in_order_the_empty_ones_too);
    RUN(the_adapter_the_binding_goes_on);
    RUN(indices);
)

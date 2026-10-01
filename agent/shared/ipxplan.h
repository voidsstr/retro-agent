/*
 * ipxplan.h - IPXSETUP: put the IPX/SPX protocol on every fleet box that can
 * have it (agent 1.97.0). Design: ~/.retro-fleet/research-2026-10-01/
 * R4_agent_design.md section B; mechanisms measured in R3_IPX_install.md.
 *
 * WHY. Descent, Warcraft II, Carmageddon and the rest of the DOS/Win9x LAN
 * library speak IPX, and the protocol was on almost no box: .123's NWLink was
 * installed by hand through the Network control panel on 2026-08-20 and was
 * gone after the 2026-08-27 re-image - a fix applied to one box does not
 * survive. So the agent installs it, at every start, wherever it is missing.
 *
 * MECHANISM, BY WINDOWS (ipx_mech_for):
 *   Windows 98 SE (4.10.2222)  IPX_MECH_9X - no silent installer exists once
 *       Setup has finished, so the agent writes what NETDI writes: the
 *       NWLINK.VXD / WSIPX.VXD payload from the share, the registry template
 *       below, Microsoft's own queued WSCInstallProvider (run by
 *       `rundll.exe netdi.dll,FirstBootCall` at the next boot), and the NIC
 *       binding LAST. Active after ONE reboot. GATED: nothing is written until
 *       HKLM\Software\RetroAgent\IpxSetup9xTemplateOk=1, which the orchestrator
 *       sets once the template matches a golden Network-applet install in the
 *       Win98 build VM - an unvalidated template never touches .243.
 *   Windows 95 / 98 FE / ME    none - the template was captured on 98 SE only.
 *   NT 5.x (2000/XP/2003)      IPX_MECH_NETCFG - INetCfg Install("MS_NWIPX"):
 *       the payload (netnwlnk.inf, nwlnk*.sys, wshisn.dll) is on every imaged
 *       XP box already and the INF has no CopyFiles. Live with no reboot; a
 *       NETCFG_S_REBOOT is REPORTED, never acted on - the agent never reboots
 *       an NT box (scripts/fleet/safe-reboot.py owns that: PXE hold + activation).
 *   NT 6+ (Vista/7/8)          none - NWLink was removed in Vista; IPX games
 *       there use IPXWrapper per title (Files/Game Updates/ipxwrapper-0.7.2.zip).
 *   Windows 10/11              refused by host policy (hostpolicy.h).
 *
 * FRAME TYPE. The numbers differ per OS and a mismatch is a SILENT
 * non-interop, so they are pinned here: Win98 Frame_Type "1" = 802.2 (NETTRANS.INF
 * enum: 4 Auto, 1 802.2, 0 802.3, 2 Ethernet II), XP PktType "2" = 802.2 ("ff"
 * Auto, 1 802.3, 0 Ethernet II, 3 SNAP; KB150546). Win98 is pinned to 802.2 -
 * Microsoft's own FAQ says its Auto "does not always work" - and XP is left on
 * Auto, which lands on 802.2 on a quiet LAN (OPT-002; .143 read [802.2]).
 *
 * Win32-free: tests/native/test_ipxplan.c compiles exactly this - the decision
 * table, the frame encodings, the payload manifest and CRC, and the Win98
 * template (rendered, parsed with regmerge.h's REGEDIT4 reader, and checked to
 * be additive, VREDIR-free and binding-last).
 */
#ifndef RETRO_IPXPLAN_H
#define RETRO_IPXPLAN_H

#include <stddef.h>
#include <string.h>

#include "regmerge.h"

#if defined(__GNUC__)
#define IPX_API static __attribute__((unused))
#else
#define IPX_API static
#endif

/* ---- HKLM\Software\RetroAgent ------------------------------------------ */
#define IPX_REG_SWITCH      "IpxSetup"              /* DWORD: absent/1 = startup installs; 0 = never automatically */
#define IPX_REG_REBOOT      "IpxSetupReboot"        /* DWORD: 1 = a 9x box may reboot ONCE per install (default 0) */
#define IPX_REG_TEMPLATE_OK "IpxSetup9xTemplateOk"  /* DWORD: 1 = the 9x template was validated (orchestrator only) */
#define IPX_REG_PAYLOAD     "IpxPayloadPath"        /* SZ:    the 9x payload folder (default below) */
#define IPX_REG_RESULT      "IpxSetupBoot"          /* SZ:    what the last pass did (agent-written) */
#define IPX_REG_ATTEMPTS    "IpxSetupAttempts"      /* DWORD: installs tried since IPX was last active */
#define IPX_REG_INSTALLED   "IpxInstalledAt"        /* DWORD: time() of the last install the agent made */
#define IPX_REG_REBOOTLAST  "IpxRebootLast"         /* DWORD: time() of the last reboot IPXSETUP asked for */

#define IPX_DEFAULT_PAYLOAD "\\\\192.168.1.122\\files\\Utility\\Retro Automation\\ipx\\win98se"
#define IPX_PAYLOAD_MANIFEST "MANIFEST.TXT"
#define IPX_BACKUP_FILE     "C:\\RETRO_AGENT\\IPX9X.BAK"

#define IPX_ATTEMPT_CAP     2       /* automatic installs before GAVE_UP (`retry` ignores it) */
#define IPX_REBOOT_GAP_S    1200L   /* never two IPXSETUP reboots within 20 minutes */
#define IPX_AF_IPX          6       /* winsock AF_IPX (= AF_NS) */
#define IPX_NSPROTO_IPX     1000

/* ---- which mechanism ---------------------------------------------------- */
enum { IPX_MECH_NONE = 0, IPX_MECH_NETCFG, IPX_MECH_9X };

IPX_API const char *ipx_mech_name(int m)
{
    return m == IPX_MECH_NETCFG ? "netcfg" : m == IPX_MECH_9X ? "win98se_template" : "none";
}

/* build = LOWORD(dwBuildNumber) on 9x, the real build on NT. *why explains NONE. */
IPX_API int ipx_mech_for(int is_nt, unsigned long major, unsigned long minor,
                         unsigned long build, const char **why)
{
    const char *w = "";
    int m = IPX_MECH_NONE;
    if (!is_nt) {
        if (major == 4 && minor == 10 && build == 2222)
            m = IPX_MECH_9X;
        else
            w = "not supported: the Win9x registry template was captured on Windows 98 SE "
                "(4.10.2222) only";
    } else if (major == 5) {
        m = IPX_MECH_NETCFG;
    } else if (major >= 10) {
        w = "not supported: Windows 10/11 has no NWLink - and the agent does not reconfigure "
            "a modern host";
    } else if (major >= 6) {
        w = "not supported: NWLink was removed in Windows Vista - IPX games here need "
            "IPXWrapper per title (Files/Game Updates/ipxwrapper-0.7.2.zip)";
    } else {
        w = "not supported: Windows NT 4 and older are not a fleet OS";
    }
    if (why)
        *why = w;
    return m;
}

IPX_API const char *ipx_os_name(int is_nt, unsigned long major, unsigned long minor)
{
    if (!is_nt)
        return minor >= 90 ? "Windows ME" : minor >= 10 ? "Windows 98" : "Windows 95";
    if (major == 5)
        return minor == 0 ? "Windows 2000" : minor == 1 ? "Windows XP" : "Windows Server 2003";
    if (major == 6)
        return minor == 0 ? "Windows Vista" : minor == 1 ? "Windows 7" : "Windows 8";
    if (major >= 10)
        return "Windows 10/11";
    return "Windows NT";
}

/* ---- frame types -------------------------------------------------------- */
enum { IPX_FRAME_AUTO = 0, IPX_FRAME_8022, IPX_FRAME_8023, IPX_FRAME_ETHII,
       IPX_FRAME_SNAP, IPX_FRAME_UNKNOWN };

IPX_API const char *ipx_frame_name(int f)
{
    switch (f) {
    case IPX_FRAME_AUTO:  return "auto";
    case IPX_FRAME_8022:  return "802.2";
    case IPX_FRAME_8023:  return "802.3";
    case IPX_FRAME_ETHII: return "ethernet_ii";
    case IPX_FRAME_SNAP:  return "snap";
    default:              return "unknown";
    }
}

/* Win98 NETTRANS.INF [NWLINK.ndi.reg] Frame_Type enum. Absent = the INF's
 * default "4" (Auto). */
IPX_API int ipx_frame_from_9x(const char *v)
{
    if (!v || !*v)
        return IPX_FRAME_AUTO;
    if (!strcmp(v, "4")) return IPX_FRAME_AUTO;
    if (!strcmp(v, "1")) return IPX_FRAME_8022;
    if (!strcmp(v, "0")) return IPX_FRAME_8023;
    if (!strcmp(v, "2")) return IPX_FRAME_ETHII;
    return IPX_FRAME_UNKNOWN;               /* 5 / 6: Token Ring */
}

IPX_API const char *ipx_frame_to_9x(int f)
{
    switch (f) {
    case IPX_FRAME_AUTO:  return "4";
    case IPX_FRAME_8022:  return "1";
    case IPX_FRAME_8023:  return "0";
    case IPX_FRAME_ETHII: return "2";
    default:              return NULL;
    }
}

/* XP Services\NwlnkIpx\Parameters\Adapters\<a>\PktType (first string of a
 * MULTI_SZ, hex). Absent = Auto. */
IPX_API int ipx_frame_from_nt(const char *v)
{
    if (!v || !*v)
        return IPX_FRAME_AUTO;
    if (!strcmp(v, "ff") || !strcmp(v, "FF") || !strcmp(v, "Ff") || !strcmp(v, "fF"))
        return IPX_FRAME_AUTO;
    if (!strcmp(v, "2")) return IPX_FRAME_8022;
    if (!strcmp(v, "1")) return IPX_FRAME_8023;
    if (!strcmp(v, "0")) return IPX_FRAME_ETHII;
    if (!strcmp(v, "3")) return IPX_FRAME_SNAP;
    return IPX_FRAME_UNKNOWN;
}

/* What the fleet wants: 802.2 pinned on Win98 (by the template), Auto left on XP. */
IPX_API int ipx_frame_wanted(int mech)
{
    return mech == IPX_MECH_9X ? IPX_FRAME_8022 : IPX_FRAME_AUTO;
}

/* ---- observe -> decide -> plan ------------------------------------------ */
typedef struct {
    int mech;                   /* IPX_MECH_* */
    int managed;                /* host_manages_this_box() */
    int enabled;                /* IpxSetup absent or non-zero */
    int winsock_ipx;            /* socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX) worked: THE post-condition */
    int component_present;      /* NT: a NetTrans instance ComponentId=ms_nwipx;
                                   9x: an Enum\Network\NWLINK devnode + its class key */
    int stack_loaded;           /* NT: NwlnkIpx running; 9x: a live NWLINK devnode, problem 0 */
    int files_present;          /* NT: netnwlnk.inf + nwlnkipx.sys (or driver.cab); 9x: both VxDs in SYSTEM */
    unsigned long installed_at; /* IpxInstalledAt (0 = never by the agent) */
    unsigned long boot_time;    /* time() - uptime */
    int attempts;               /* IpxSetupAttempts */
    int hung;                   /* an install never returned in this agent process */
    int frame_configured;       /* IPX_FRAME_* the OS is set to */
} ipx_obs_t;

enum {
    IPX_ST_NOT_SUPPORTED = 0,   /* no mechanism for this Windows */
    IPX_ST_POLICY,              /* a modern host the agent does not manage */
    IPX_ST_ACTIVE,              /* Winsock opens an IPX socket */
    IPX_ST_FRAME_MISMATCH,      /* active, but not on the frame the fleet uses */
    IPX_ST_PENDING_REBOOT,      /* installed during THIS boot, not live yet */
    IPX_ST_BROKEN,              /* installed before this boot and still not live */
    IPX_ST_HUNG,                /* an install never returned - until the agent restarts */
    IPX_ST_DISABLED,            /* not installed, and IpxSetup=0 */
    IPX_ST_GAVE_UP,             /* not installed after IPX_ATTEMPT_CAP tries */
    IPX_ST_PAYLOAD_MISSING,     /* NT: the in-box NWLink files are not on this disk */
    IPX_ST_NOT_INSTALLED,
    IPX_ST_COUNT
};

IPX_API const char *ipx_state_name(int s)
{
    static const char *const n[IPX_ST_COUNT] = {
        "not_supported", "policy", "active", "frame_mismatch", "pending_reboot",
        "broken", "hung", "disabled", "gave_up", "payload_missing", "not_installed"
    };
    return s >= 0 && s < IPX_ST_COUNT ? n[s] : "unknown";
}

IPX_API int ipx_decide(const ipx_obs_t *o)
{
    if (o->mech == IPX_MECH_NONE)
        return IPX_ST_NOT_SUPPORTED;
    if (!o->managed)
        return IPX_ST_POLICY;
    if (o->hung)
        return IPX_ST_HUNG;
    if (o->winsock_ipx) {
        int want = ipx_frame_wanted(o->mech);
        if (want != IPX_FRAME_AUTO && o->frame_configured != want)
            return IPX_ST_FRAME_MISMATCH;
        return IPX_ST_ACTIVE;
    }
    if (o->component_present) {
        /* Installed by the agent after this boot began: the reboot that makes
         * it live has not happened. Anything else installed is broken. */
        if (o->installed_at && o->boot_time && o->installed_at >= o->boot_time)
            return IPX_ST_PENDING_REBOOT;
        return IPX_ST_BROKEN;
    }
    if (!o->enabled)
        return IPX_ST_DISABLED;
    if (o->attempts >= IPX_ATTEMPT_CAP)
        return IPX_ST_GAVE_UP;
    if (o->mech == IPX_MECH_NETCFG && !o->files_present)
        return IPX_ST_PAYLOAD_MISSING;
    return IPX_ST_NOT_INSTALLED;
}

#define IPX_DO_NOTHING      0x00
#define IPX_DO_NETCFG       0x01    /* NT: INetCfg Install(MS_NWIPX) + Apply */
#define IPX_DO_STAGE_FILES  0x02    /* 9x: NWLINK.VXD + WSIPX.VXD into SYSTEM */
#define IPX_DO_REGISTRY     0x04    /* 9x: the template, binding LAST */

/* What to do about `state`. force = ignore IpxSetup=0; retry = ignore the
 * attempts cap and re-run an install that is in place but broken (both
 * mechanisms are idempotent: FindComponent first / additive registry). */
IPX_API int ipx_plan(int state, const ipx_obs_t *o, int force, int retry)
{
    if (state != IPX_ST_NOT_INSTALLED && state != IPX_ST_DISABLED &&
        state != IPX_ST_GAVE_UP && !(state == IPX_ST_BROKEN && retry))
        return IPX_DO_NOTHING;
    if (!o->enabled && !force)
        return IPX_DO_NOTHING;
    if (o->attempts >= IPX_ATTEMPT_CAP && !retry)
        return IPX_DO_NOTHING;
    if (o->mech == IPX_MECH_NETCFG)
        return o->files_present ? IPX_DO_NETCFG : IPX_DO_NOTHING;
    if (o->mech == IPX_MECH_9X)
        return IPX_DO_STAGE_FILES | IPX_DO_REGISTRY;
    return IPX_DO_NOTHING;
}

/* The 9x writes are refused until the orchestrator has validated the template
 * against a golden install (IpxSetup9xTemplateOk=1). Nothing else opens them. */
IPX_API int ipx_9x_writes_allowed(int template_ok_present, unsigned long template_ok)
{
    return template_ok_present && template_ok == 1;
}

/* The agent NEVER reboots an NT box. On 9x it reboots only when the operator
 * armed it (IpxSetupReboot=1), an install of its own is pending, it has not
 * already rebooted for THIS install, the last IPXSETUP reboot is at least
 * min_gap seconds ago, and no library sync is copying. secs_since_last < 0 =
 * never rebooted. Same shape as postskip's 1Bh reboot (ps_ide2_should_reboot). */
IPX_API int ipx_should_reboot(int mech, int allow, int state, unsigned long installed_at,
                              unsigned long reboot_last, long secs_since_last,
                              long min_gap, int sync_busy)
{
    if (mech != IPX_MECH_9X)
        return 0;
    if (!allow || state != IPX_ST_PENDING_REBOOT || !installed_at)
        return 0;
    if (reboot_last && reboot_last >= installed_at)
        return 0;                           /* one reboot per install */
    if (secs_since_last >= 0 && secs_since_last < min_gap)
        return 0;
    if (sync_busy)
        return 0;
    return 1;
}

/* ---- command words ------------------------------------------------------ */
#define IPX_ARG_STATUS 0x01
#define IPX_ARG_APPLY  0x02
#define IPX_ARG_FORCE  0x04
#define IPX_ARG_RETRY  0x08
#define IPX_ARG_BAD    0x80

IPX_API int ipx_word_is(const char *w, size_t n, const char *lit)
{
    size_t i;
    if (strlen(lit) != n)
        return 0;
    for (i = 0; i < n; i++) {
        int a = (unsigned char)w[i], b = (unsigned char)lit[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (a != b)
            return 0;
    }
    return 1;
}

/* IPXSETUP [status] | IPXSETUP apply [force] [retry]. Whole words, any case;
 * an unknown word - or force/retry without apply - is IPX_ARG_BAD, never read
 * as "apply" (the DRIVERS UPDATE lesson). */
IPX_API int ipx_parse_args(const char *a)
{
    int f = 0, words = 0;
    while (a && *a) {
        const char *w;
        size_t n;
        while (*a == ' ' || *a == '\t')
            a++;
        if (!*a)
            break;
        w = a;
        while (*a && *a != ' ' && *a != '\t')
            a++;
        n = (size_t)(a - w);
        words++;
        if (ipx_word_is(w, n, "status") && words == 1)
            f |= IPX_ARG_STATUS;
        else if (ipx_word_is(w, n, "apply") && words == 1)
            f |= IPX_ARG_APPLY;
        else if (ipx_word_is(w, n, "force") && (f & IPX_ARG_APPLY))
            f |= IPX_ARG_FORCE;
        else if (ipx_word_is(w, n, "retry") && (f & IPX_ARG_APPLY))
            f |= IPX_ARG_RETRY;
        else
            return IPX_ARG_BAD;
    }
    if ((f & IPX_ARG_STATUS) && words > 1)
        return IPX_ARG_BAD;
    if (!(f & IPX_ARG_APPLY))
        f |= IPX_ARG_STATUS;
    return f;
}

/* ---- the Win98 payload -------------------------------------------------- */
typedef struct {
    const char   *name;         /* 8.3, as it sits in %windir%\SYSTEM */
    unsigned long size;
    unsigned long crc32;
    const char   *md5;          /* for the host-side stager and MANIFEST.TXT */
} ipx_file_t;

/* Windows 98 SE (faXcooL_win_98_se_bootable.iso): nwlink.vxd from NET9.CAB,
 * wsipx.vxd from NET10.CAB, both stamped 4.10.1998 inside the 4.10.2222 set
 * (R3 section 1.1). NWNBLINK.VXD (NetBIOS over IPX) is deliberately NOT here:
 * games do not need it and it adds a transport to Client for MS Networks. */
static const ipx_file_t ipx9x_payload[] = {
    { "NWLINK.VXD", 51010UL, 0x5979320AUL, "d92a994f76be8ce6469affa6f3530300" },
    { "WSIPX.VXD",  14526UL, 0x27ACFFF1UL, "ec91ebf166e66bf7108dd93d374c630d" },
};
#define IPX9X_NPAYLOAD ((int)(sizeof(ipx9x_payload) / sizeof(ipx9x_payload[0])))

/* CRC-32 (IEEE, reflected 0xEDB88320) - zlib's. Start with 0. */
IPX_API unsigned long ipx_crc32(unsigned long crc, const unsigned char *p, size_t n)
{
    size_t i;
    int k;
    crc = ~crc & 0xFFFFFFFFUL;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
    return ~crc & 0xFFFFFFFFUL;
}

/* A staged or already-present payload file is acceptable only if it is
 * exactly the build the template was captured with. */
IPX_API int ipx_payload_ok(const ipx_file_t *f, unsigned long size, unsigned long crc)
{
    return size == f->size && crc == f->crc32;
}

/* ---- network-configuration COM ids (spelled out: no libuuid) ------------- */
typedef struct {
    unsigned long  d1;
    unsigned short d2, d3;
    unsigned char  d4[8];
} ipx_guid_t;

/* CLSID_CNetCfg {5B035261-40F9-11D1-AAEC-00805FC1270E} (netcfgx.dll) */
#define IPX_CLSID_CNETCFG      { 0x5B035261UL, 0x40F9, 0x11D1, { 0xAA, 0xEC, 0x00, 0x80, 0x5F, 0xC1, 0x27, 0x0E } }
/* IID_INetCfg {C0E8AE93-306E-11D1-AACF-00805FC1270E} */
#define IPX_IID_INETCFG        { 0xC0E8AE93UL, 0x306E, 0x11D1, { 0xAA, 0xCF, 0x00, 0x80, 0x5F, 0xC1, 0x27, 0x0E } }
/* IID_INetCfgLock {C0E8AE9F-306E-11D1-AACF-00805FC1270E} */
#define IPX_IID_INETCFGLOCK    { 0xC0E8AE9FUL, 0x306E, 0x11D1, { 0xAA, 0xCF, 0x00, 0x80, 0x5F, 0xC1, 0x27, 0x0E } }
/* IID_INetCfgClassSetup {C0E8AE9D-306E-11D1-AACF-00805FC1270E} */
#define IPX_IID_INETCFGCLASSSETUP { 0xC0E8AE9DUL, 0x306E, 0x11D1, { 0xAA, 0xCF, 0x00, 0x80, 0x5F, 0xC1, 0x27, 0x0E } }
/* GUID_DEVCLASS_NETTRANS {4D36E975-E325-11CE-BFC1-08002BE10318} */
#define IPX_GUID_DEVCLASS_NETTRANS { 0x4D36E975UL, 0xE325, 0x11CE, { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 } }

#define IPX_NT_COMPONENT    "ms_nwipx"      /* netnwlnk.inf's component id */
/* WHERE XP KEEPS A PROTOCOL'S ComponentId: NetCfg's own store,
 * Control\Network\{4D36E975-...}\{instance GUID}. Control\Class\{4D36E975-...}
 * has NO instance subkeys on XP (measured on XPBUILD 2026-10-01, reg query /s:
 * the class values only), so a check there reported component_present:false
 * with NWLink installed and live. The subkey names are GUIDs - 38 characters. */
#define IPX_NT_NETTRANS_KEY "SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E975-E325-11CE-BFC1-08002BE10318}"
#define IPX_NT_SERVICE      "NwlnkIpx"
#define IPX_NSF_POSTSYSINSTALL 0x00000002UL /* netcfgx.h: not in mingw's headers */

/* A 4-digit registry index, "0000".."9999" (the form NetTrans\NNNN and
 * Enum\Network\NWLINK\NNNN take). */
IPX_API int ipx_index_ok(const char *s)
{
    int i;
    for (i = 0; i < 4; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return s[4] == 0;
}

/* The lowest 4-digit index not in used[0..n) (each a value 0..9999), or -1. */
IPX_API int ipx_first_free(const int *used, int n)
{
    int c, i;
    for (c = 0; c <= 9999; c++) {
        for (i = 0; i < n; i++)
            if (used[i] == c)
                break;
        if (i == n)
            return c;
    }
    return -1;
}

/* ======================================================================== *
 * ==================  THE WINDOWS 98 SE REGISTRY TEMPLATE  ================== *
 * ======================================================================== *
 *
 * >>> ADJUST HERE after the golden capture (Network applet install of
 * >>> "IPX/SPX-compatible Protocol" in the Win98 build VM, then `regedit /e`
 * >>> before/after diffed) - and only then set IpxSetup9xTemplateOk=1.
 *
 * REGEDIT4 text, parsed with agent/shared/regmerge.h. Sources: NETTRANS.INF
 * [NWLINK.ndi.reg] / [NWLINK.AddReg] / [NWLINK.Ins.WSock2.AddReg] (Win98 SE,
 * md5 7b42b0f30239b10268080138d27cea46), laid out the way the build VM's own
 * MSTCP class key and devnode are (W98BUILD SYSTEM.DAT, read offline).
 * COMPARED 2026-10-01 02:15 with W98BUILD's SYSTEM.DAT read offline after the
 * orchestrator's Network-applet install (802.2, no client over IPX, Dial-Up
 * left alone): parts 1 and 3 match it value for value (six values were added
 * from it: DriverDate, Network_Id, InstallInf, Ndi StaticVxD, VxD cachesize and
 * the params' @ current-value mirrors). Part 2 could NOT be seen there - the
 * image was read after the first boot had run FirstBootCall (QueuedAPI and
 * RunOnce empty) - but its catalog shows the result part 2 must produce:
 * MSWSOSP's one hidden entry replaced by the four NWLINK blocks (spx,
 * spx/seq, ipx, osp). Confirm part 2 against a capture taken BEFORE that boot.
 *
 * Placeholders (ipx9x_render): %K% class index "0002", %I% devnode instance
 * "0000", %Q% QueuedAPI item number, %FRAME% Win98 Frame_Type ("1" = 802.2),
 * %NIC% the ONE physical adapter's Enum path (key headers only),
 * %SYSDIR:Q% the system directory escaped for a quoted string.
 *
 * Rules the agent ENFORCES on whatever is pasted here (ipx9x_template_check):
 *   - only HKEY_LOCAL_MACHINE values are SET - no [-key], no "name"=- , no
 *     line the reader cannot parse;
 *   - nothing names VREDIR / NWREDIR / NWNBLINK / VSERVER / NWSERVER: Client
 *     for Microsoft Networks is NEVER bound over IPX (the Network applet offers
 *     it, because NWLINK's DefUpper matches VREDIR's DefLower);
 *   - the LAST value of the last part is the NIC binding "NWLINK\%I%" - it is
 *     what turns the protocol on, so a failure anywhere before leaves it off;
 *   - at run time, ADDITIVE ONLY: a value that already exists with different
 *     data is a refusal of the whole install, before anything is written.
 * ------------------------------------------------------------------------ */

/* Part 1: the protocol - its class key, its devnode and the VxD keys. */
static const char ipx9x_tmpl_stack[] =
    "REGEDIT4\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%]\r\n"
    "\"DriverDesc\"=\"IPX/SPX-compatible Protocol\"\r\n"
    "\"InfSection\"=\"NWLINK.ndi\"\r\n"
    "\"InfPath\"=\"NETTRANS.INF\"\r\n"
    "\"ProviderName\"=\"Microsoft\"\r\n"
    "\"DriverDate\"=\" 4-23-1999\"\r\n"
    "\"DevLoader\"=\"*ndis\"\r\n"
    "\"DeviceVxDs\"=\"nwlink.vxd\"\r\n"
    "\"Network_Id\"=\"0\"\r\n"
    "\"Frame_Type\"=\"%FRAME%\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi]\r\n"
    "\"DeviceID\"=\"NWLINK\"\r\n"
    "\"MaxInstance\"=\"8\"\r\n"
    "\"NdiInstaller\"=\"netdi.dll,NwlinkNdiProc\"\r\n"
    "\"HelpText\"=\"The IPX/SPX-compatible protocol is a protocol NetWare and Windows NT servers, and Windows 95 computers use to communicate.\"\r\n"
    "\"InstallInf\"=\"\"\r\n"
    "\"StaticVxD\"=\"nwlink.vxd\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\Interfaces]\r\n"
    "\"DefUpper\"=\"ipx,ipxDHost,winsock\"\r\n"
    "\"DefLower\"=\"ndis2,ndis3,odi\"\r\n"
    "\"UpperRange\"=\"ipx,ipxDHost,winsock\"\r\n"
    "\"LowerRange\"=\"ndis2,ndis3,odi\"\r\n"
    "\"Upper\"=\"ipx,ipxDHost,winsock\"\r\n"
    "\"Lower\"=\"ndis2,ndis3,odi\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\Install]\r\n"
    "@=\"NWLINK.Install\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\Remove]\r\n"
    "@=\"NWLINK.Remove\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\params\\Network_Id]\r\n"
    "\"flag\"=hex:20,00,00,00\r\n"
    "\"default\"=\"0\"\r\n"
    "\"ParamDesc\"=\"Network Address\"\r\n"
    "\"type\"=\"dword\"\r\n"
    "\"base\"=\"16\"\r\n"
    "@=\"0\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\params\\Frame_Type]\r\n"
    "\"ParamDesc\"=\"Frame Type\"\r\n"
    "\"default\"=\"4\"\r\n"
    "\"type\"=\"enum\"\r\n"
    "@=\"%FRAME%\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\params\\Frame_Type\\enum]\r\n"
    "\"4\"=\"Auto\"\r\n"
    "\"1\"=\"Ethernet 802.2\"\r\n"
    "\"0\"=\"Ethernet 802.3\"\r\n"
    "\"2\"=\"Ethernet II\"\r\n"
    "\"5\"=\"Token Ring\"\r\n"
    "\"6\"=\"Token Ring SNAP\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\params\\maxconnect]\r\n"
    "\"location\"=\"System\\\\CurrentControlSet\\\\Services\\\\Vxd\\\\NWLink\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\params\\maxsockets]\r\n"
    "\"location\"=\"System\\\\CurrentControlSet\\\\Services\\\\Vxd\\\\NWLink\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\params\\forceeven]\r\n"
    "\"location\"=\"System\\\\CurrentControlSet\\\\Services\\\\Vxd\\\\NWLink\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\Ndi\\params\\cachesize]\r\n"
    "\"location\"=\"System\\\\CurrentControlSet\\\\Services\\\\Vxd\\\\NWLink\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\NDIS]\r\n"
    "\"LogDriverName\"=\"NWLINK\"\r\n"
    "\"MajorNdisVersion\"=hex:03\r\n"
    "\"MinorNdisVersion\"=hex:0a\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\NetTrans\\%K%\\NDIS\\NDIS2]\r\n"
    "\"DriverName\"=\"nwlink$\"\r\n"
    "\"FileName\"=\"*nwlink\"\r\n"
    "\r\n"
    /* NETTRANS.INF: "The NDI proc now does this" - the StaticVxD key, shaped
     * like VNETSUP's and VREDIR's in the build VM (Start 00, NetClean 01). */
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\VxD\\NWLINK]\r\n"
    "\"StaticVxD\"=\"nwlink.vxd\"\r\n"
    "\"Start\"=hex:00\r\n"
    "\"NetClean\"=hex:01\r\n"
    "\"cachesize\"=\"0\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\VxD\\NWLINK\\Ndi\\params\\maxconnect]\r\n"
    "\"ParamDesc\"=\"Maximum Connections\"\r\n"
    "\"optional\"=\"1\"\r\n"
    "\"type\"=\"int\"\r\n"
    "\"min\"=\"1\"\r\n"
    "\"max\"=\"128\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\VxD\\NWLINK\\Ndi\\params\\maxsockets]\r\n"
    "\"ParamDesc\"=\"Maximum Sockets\"\r\n"
    "\"optional\"=\"1\"\r\n"
    "\"type\"=\"int\"\r\n"
    "\"min\"=\"2\"\r\n"
    "\"max\"=\"255\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\VxD\\NWLINK\\Ndi\\params\\forceeven]\r\n"
    "\"ParamDesc\"=\"Force Even Length Packets\"\r\n"
    "\"optional\"=\"1\"\r\n"
    "\"default\"=\"0\"\r\n"
    "\"type\"=\"enum\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\VxD\\NWLINK\\Ndi\\params\\forceeven\\enum]\r\n"
    "\"0\"=\"No\"\r\n"
    "\"1\"=\"Yes\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\VxD\\NWLINK\\Ndi\\params\\cachesize]\r\n"
    "\"ParamDesc\"=\"Source Routing\"\r\n"
    "\"type\"=\"enum\"\r\n"
    "\"default\"=\"0\"\r\n"
    "@=\"0\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\VxD\\NWLINK\\Ndi\\params\\cachesize\\enum]\r\n"
    "\"0\"=\"Off\"\r\n"
    "\"16\"=\"16 entry cache (recommended)\"\r\n"
    "\"32\"=\"32 entry cache\"\r\n"
    "\"64\"=\"64 entry cache\"\r\n"
    "\r\n"
    /* The devnode, shaped exactly like the build VM's Enum\Network\MSTCP\0001.
     * Its Bindings key stays EMPTY: nothing (no client, no server) rides IPX. */
    "[HKEY_LOCAL_MACHINE\\Enum\\Network\\NWLINK\\%I%]\r\n"
    "\"Class\"=\"NetTrans\"\r\n"
    "\"Driver\"=\"NetTrans\\\\%K%\"\r\n"
    "\"MasterCopy\"=\"Enum\\\\Network\\\\NWLINK\\\\%I%\"\r\n"
    "\"DeviceDesc\"=\"IPX/SPX-compatible Protocol\"\r\n"
    "\"CompatibleIDs\"=\"NWLINK\"\r\n"
    "\"Mfg\"=\"Microsoft\"\r\n"
    "\"ClassGUID\"=\"{4d36e975-e325-11ce-bfc1-08002be10318}\"\r\n"
    "\"ConfigFlags\"=hex:10,00,00,00\r\n"
    "\"Capabilities\"=hex:14,00,00,00\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\Enum\\Network\\NWLINK\\%I%\\Bindings]\r\n"
    "\r\n"
    /* [NWLINK.Ins.WSock2.AddReg]: the Winsock 1.1 VxD provider for IPX/SPX. */
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\VxD\\Winsock]\r\n"
    "\"IPX/SPX Winsock Provider\"=\"wsipx.vxd\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Winsock2\\Providers\\IPX]\r\n"
    "\"ProviderName\"=\"Microsoft IPX\"\r\n";

/* Part 2: the Winsock 2 catalog entries - Microsoft's own route: NETDI queues
 * WSCInstallProvider(MSWSOSP {FF017DE1-CAE9-11CF-8A99-00AA0062C609}) with the
 * four protocol blocks of [NWLINK.Ins.WSock2.AddReg] (spx, spx/seq, ipx,
 * hidden osp), and `rundll.exe netdi.dll,FirstBootCall` runs the queue at the
 * next boot (WSock2Force.addreg's own RunOnce line). Written only when the
 * catalog has no AF_IPX entry yet. */
static const char ipx9x_tmpl_ws2[] =
    "REGEDIT4\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item%Q%\\1]\r\n"
    "@=\"WSCInstallProvider\"\r\n"
    "\"GUID\"=hex:e1,7d,01,ff,e9,ca,cf,11,8a,99,00,aa,00,62,c6,09\r\n"
    "\"Provider\"=\"%SYSDIR:Q%\\\\mswsosp.dll\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item%Q%\\1\\ProtocolInfo\\1]\r\n"
    "\"ServiceFlags1\"=hex:1e,00,02,00\r\n"
    "\"ServiceFlags2\"=hex:00,00,00,00\r\n"
    "\"ServiceFlags3\"=hex:00,00,00,00\r\n"
    "\"ServiceFlags4\"=hex:00,00,00,00\r\n"
    "\"ProviderFlags\"=hex:08,00,00,00\r\n"
    "\"ChainLen\"=hex:01,00,00,00\r\n"
    "\"Version\"=hex:01,00,00,00\r\n"
    "\"AddressFamily\"=hex:06,00,00,00\r\n"
    "\"MaxSockAddr\"=hex:0e,00,00,00\r\n"
    "\"MinSockAddr\"=hex:10,00,00,00\r\n"
    "\"SocketType\"=hex:01,00,00,00\r\n"
    "\"Protocol\"=hex:e8,04,00,00\r\n"
    "\"ProtocolMaxOffset\"=hex:00,00,00,00\r\n"
    "\"NetworkByteOrder\"=hex:00,00,00,00\r\n"
    "\"SecurityScheme\"=hex:00,00,00,00\r\n"
    "\"MessageSize\"=hex:ff,ff,ff,ff\r\n"
    "\"ProviderReserved\"=hex:00,00,00,00\r\n"
    "\"ProtocolString\"=\"MS.w95.spi.spx\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item%Q%\\1\\ProtocolInfo\\2]\r\n"
    "\"ServiceFlags1\"=hex:3e,00,02,00\r\n"
    "\"ServiceFlags2\"=hex:00,00,00,00\r\n"
    "\"ServiceFlags3\"=hex:00,00,00,00\r\n"
    "\"ServiceFlags4\"=hex:00,00,00,00\r\n"
    "\"ProviderFlags\"=hex:08,00,00,00\r\n"
    "\"ChainLen\"=hex:01,00,00,00\r\n"
    "\"Version\"=hex:01,00,00,00\r\n"
    "\"AddressFamily\"=hex:06,00,00,00\r\n"
    "\"MaxSockAddr\"=hex:0e,00,00,00\r\n"
    "\"MinSockAddr\"=hex:10,00,00,00\r\n"
    "\"SocketType\"=hex:05,00,00,00\r\n"
    "\"Protocol\"=hex:e8,04,00,00\r\n"
    "\"ProtocolMaxOffset\"=hex:00,00,00,00\r\n"
    "\"NetworkByteOrder\"=hex:00,00,00,00\r\n"
    "\"SecurityScheme\"=hex:00,00,00,00\r\n"
    "\"MessageSize\"=hex:ff,ff,ff,ff\r\n"
    "\"ProviderReserved\"=hex:00,00,00,00\r\n"
    "\"ProtocolString\"=\"MS.w95.spi.spx/seq\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item%Q%\\1\\ProtocolInfo\\3]\r\n"
    "\"ServiceFlags1\"=hex:09,06,02,00\r\n"
    "\"ServiceFlags2\"=hex:00,00,00,00\r\n"
    "\"ServiceFlags3\"=hex:00,00,00,00\r\n"
    "\"ServiceFlags4\"=hex:00,00,00,00\r\n"
    "\"ProviderFlags\"=hex:08,00,00,00\r\n"
    "\"ChainLen\"=hex:01,00,00,00\r\n"
    "\"Version\"=hex:01,00,00,00\r\n"
    "\"AddressFamily\"=hex:06,00,00,00\r\n"
    "\"MaxSockAddr\"=hex:10,00,00,00\r\n"
    "\"MinSockAddr\"=hex:0e,00,00,00\r\n"
    "\"SocketType\"=hex:02,00,00,00\r\n"
    "\"Protocol\"=hex:e8,03,00,00\r\n"
    "\"ProtocolMaxOffset\"=hex:ff,00,00,00\r\n"
    "\"NetworkByteOrder\"=hex:00,00,00,00\r\n"
    "\"SecurityScheme\"=hex:00,00,00,00\r\n"
    "\"MessageSize\"=hex:40,02,00,00\r\n"
    "\"ProviderReserved\"=hex:00,00,00,00\r\n"
    "\"ProtocolString\"=\"MS.w95.spi.ipx\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI\\Item%Q%\\1\\ProtocolInfo\\4]\r\n"
    "\"ServiceFlags1\"=hex:09,06,02,00\r\n"
    "\"ServiceFlags2\"=hex:00,00,00,00\r\n"
    "\"ServiceFlags3\"=hex:00,00,00,00\r\n"
    "\"ServiceFlags4\"=hex:00,00,00,00\r\n"
    "\"ProviderFlags\"=hex:0c,00,00,00\r\n"
    "\"ChainLen\"=hex:01,00,00,00\r\n"
    "\"Version\"=hex:01,00,00,00\r\n"
    "\"AddressFamily\"=hex:ce,fa,ce,fa\r\n"
    "\"MaxSockAddr\"=hex:10,00,00,00\r\n"
    "\"MinSockAddr\"=hex:0e,00,00,00\r\n"
    "\"SocketType\"=hex:ce,fa,ce,fa\r\n"
    "\"Protocol\"=hex:ce,fa,ce,fa\r\n"
    "\"ProtocolMaxOffset\"=hex:00,00,00,00\r\n"
    "\"NetworkByteOrder\"=hex:00,00,00,00\r\n"
    "\"SecurityScheme\"=hex:00,00,00,00\r\n"
    "\"MessageSize\"=hex:40,02,00,00\r\n"
    "\"ProviderReserved\"=hex:00,00,00,00\r\n"
    "\"ProtocolString\"=\"MS.w95.spi.osp\"\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce]\r\n"
    "\"NetSetup\"=\"rundll.exe netdi.dll,FirstBootCall\"\r\n";

/* Part 3, LAST: the line that turns it on - the adapter -> protocol binding. */
static const char ipx9x_tmpl_bind[] =
    "REGEDIT4\r\n"
    "\r\n"
    "[HKEY_LOCAL_MACHINE\\Enum\\%NIC%\\Bindings]\r\n"
    "\"NWLINK\\\\%I%\"=\"\"\r\n";

/* ======================  END OF THE WIN98 TEMPLATE  ====================== */

/* The template's parts, in the order they are applied. */
enum { IPX9X_PART_STACK = 0, IPX9X_PART_WS2, IPX9X_PART_BIND, IPX9X_NPARTS };
IPX_API const char *ipx9x_part(int i)
{
    return i == IPX9X_PART_STACK ? ipx9x_tmpl_stack
         : i == IPX9X_PART_WS2 ? ipx9x_tmpl_ws2
         : i == IPX9X_PART_BIND ? ipx9x_tmpl_bind : NULL;
}

/* Names that must never appear in a rendered template (see the rules above):
 * Client for Microsoft Networks, the NetWare client, NetBIOS over IPX, and the
 * two File and Printer Sharing services. */
static const char *const ipx9x_forbidden[] = {
    "VREDIR", "NWREDIR", "NWNBLINK", "VSERVER", "NWSERVER"
};
#define IPX9X_NFORBIDDEN ((int)(sizeof(ipx9x_forbidden) / sizeof(ipx9x_forbidden[0])))

IPX_API int ipx_has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < n; i++) {
            int a = (unsigned char)hay[i], b = (unsigned char)needle[i];
            if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
            if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
            if (a != b)
                break;
        }
        if (i == n)
            return 1;
    }
    return 0;
}

/*
 * Check RENDERED parts against the rules above. `ps`/`e` are caller scratch
 * (they are large; the agent heap-allocates them). nic = the Enum path,
 * inst = the devnode instance ("0000"). Returns the number of values the
 * parts set (> 0), or -1 with *why saying which rule broke.
 */
IPX_API int ipx9x_template_check(const char *const *parts, int nparts, const char *nic,
                                 const char *inst, rm_parser_t *ps, rm_entry_t *e,
                                 const char **why)
{
    int p, total = 0, i, last_is_bind = 0, bind_entries = 0;
    char bind_key[RM_KEY_MAX], bind_name[32];

    *why = "";
    if (!nic || !nic[0] || !inst || !ipx_index_ok(inst)) {
        *why = "no adapter / instance to bind";
        return -1;
    }
    if (strlen(nic) + 16 >= sizeof(bind_key)) {
        *why = "the adapter path is too long";
        return -1;
    }
    memcpy(bind_key, "Enum\\", 5);
    memcpy(bind_key + 5, nic, strlen(nic) + 1);
    memcpy(bind_key + strlen(bind_key), "\\Bindings", 10);
    memcpy(bind_name, "NWLINK\\", 7);
    memcpy(bind_name + 7, inst, 5);

    for (p = 0; p < nparts; p++) {
        const char *t = parts[p];
        if (!t)
            continue;
        for (i = 0; i < IPX9X_NFORBIDDEN; i++)
            if (ipx_has_ci(t, ipx9x_forbidden[i])) {
                *why = "the template names a client or server that must never ride IPX "
                       "(VREDIR / NWREDIR / NWNBLINK / VSERVER / NWSERVER)";
                return -1;
            }
        rm_init(ps, t, strlen(t));
        while (rm_next(ps, e)) {
            if (e->op != RM_OP_SET) {
                *why = e->op == RM_OP_UNKNOWN
                     ? "the template has a line the REGEDIT4 reader cannot parse"
                     : "the template deletes something - it must only ADD";
                return -1;
            }
            if (e->root != RM_HKLM) {
                *why = "the template writes outside HKEY_LOCAL_MACHINE";
                return -1;
            }
            total++;
            last_is_bind = rm_prefix_ci(e->key, bind_key) && strlen(e->key) == strlen(bind_key) &&
                           rm_prefix_ci(e->name, bind_name) && strlen(e->name) == strlen(bind_name) &&
                           e->type == RM_REG_SZ && e->len == 0;
            if (last_is_bind)
                bind_entries++;
        }
        if (ps->dialect != RM_DIALECT_REGEDIT4) {
            *why = "a template part is not REGEDIT4";
            return -1;
        }
    }
    if (!total) {
        *why = "the template sets nothing";
        return -1;
    }
    if (!last_is_bind || bind_entries != 1) {
        *why = "the adapter binding NWLINK\\nnnn must be the LAST value written, exactly once";
        return -1;
    }
    return total;
}

typedef struct {
    const char *name;           /* "K" for %K%; "SYSDIR:Q" for %SYSDIR:Q% */
    const char *value;
} ipx_var_t;

/* Substitute %NAME% placeholders. A placeholder with no var is an error, and
 * a value carrying a character that could break the REGEDIT4 line it lands in
 * (CR, LF, ']' or '"') is refused. Returns the rendered length, -1 on error. */
IPX_API int ipx9x_render(const char *tmpl, const ipx_var_t *vars, int nvars,
                         char *out, size_t outsz)
{
    size_t o = 0;
    const char *p = tmpl;
    if (!tmpl || !out || !outsz)
        return -1;
    while (*p) {
        if (*p == '%') {
            const char *e = strchr(p + 1, '%');
            int i, found = -1;
            size_t n;
            if (!e)
                return -1;
            n = (size_t)(e - (p + 1));
            for (i = 0; i < nvars; i++)
                if (strlen(vars[i].name) == n && !memcmp(vars[i].name, p + 1, n))
                    found = i;
            if (found < 0 || !vars[found].value)
                return -1;
            {
                const char *v = vars[found].value;
                for (; *v; v++) {
                    if (*v == '\r' || *v == '\n' || *v == ']' || *v == '"')
                        return -1;
                    if (o + 1 >= outsz)
                        return -1;
                    out[o++] = *v;
                }
            }
            p = e + 1;
            continue;
        }
        if (o + 1 >= outsz)
            return -1;
        out[o++] = *p++;
    }
    out[o] = 0;
    return (int)o;
}

/* The next "[HKEY_LOCAL_MACHINE\<key>]" header of a rendered part, as <key>,
 * in file order - so the agent can create every key the template names, the
 * EMPTY ones too (the devnode's Bindings), before it writes a single value.
 * Returns 0 at the end. */
IPX_API int ipx9x_next_key(const char **pp, char *key, size_t keysz)
{
    static const char hdr[] = "[HKEY_LOCAL_MACHINE\\";
    const char *p = *pp;
    while (p && *p) {
        const char *eol = p, *close;
        while (*eol && *eol != '\n')
            eol++;
        if (rm_prefix_ci(p, hdr)) {
            const char *k = p + sizeof(hdr) - 1;
            for (close = k; close < eol && *close != ']'; close++)
                ;
            if (close < eol && (size_t)(close - k) < keysz) {
                memcpy(key, k, (size_t)(close - k));
                key[close - k] = 0;
                *pp = *eol ? eol + 1 : eol;
                return 1;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    *pp = p;
    return 0;
}

/* Double every backslash, for a value placed inside a REGEDIT4 quoted string
 * (%SYSDIR:Q%). Returns 0, -1 if it does not fit. */
IPX_API int ipx_quote_backslashes(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *in; in++) {
        if (o + (*in == '\\' ? 2 : 1) >= outsz)
            return -1;
        if (*in == '\\')
            out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
    return 0;
}

/* Is this a physical LAN adapter the IPX binding may go on? A Win98 network
 * devnode: Class=Net, live with problem 0, TCP/IP already bound to it (that is
 * what makes it THE LAN adapter), and not the Dial-Up Adapter (*PNP8387) or the
 * Microsoft VPN adapter (*PNP8395). Ids are Win98's comma-separated REG_SZ. */
IPX_API int ipx9x_nic_candidate(const char *cls, unsigned long problem, int has_mstcp_binding,
                                const char *hwids, const char *compat, const char *desc)
{
    const char *virt[] = { "*PNP8387", "*PNP8395" };
    int i;
    if (!cls || !(cls[0] == 'N' || cls[0] == 'n') || !(cls[1] == 'e' || cls[1] == 'E') ||
        !(cls[2] == 't' || cls[2] == 'T') || cls[3])
        return 0;
    if (problem != 0 || !has_mstcp_binding)
        return 0;
    for (i = 0; i < 2; i++) {
        size_t n = strlen(virt[i]);
        const char *lists[2];
        int l;
        lists[0] = hwids;
        lists[1] = compat;
        for (l = 0; l < 2; l++) {
            const char *s = lists[l];
            for (; s && *s; s++) {
                size_t k;
                for (k = 0; k < n; k++) {
                    int a = (unsigned char)s[k], b = (unsigned char)virt[i][k];
                    if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
                    if (a != b)
                        break;
                }
                if (k == n)
                    return 0;
            }
        }
    }
    if (desc) {
        const char *d;
        for (d = desc; *d; d++)
            if ((d[0] == 'D' || d[0] == 'd') && (d[1] == 'i' || d[1] == 'I') &&
                (d[2] == 'a' || d[2] == 'A') && (d[3] == 'l' || d[3] == 'L') && d[4] == '-')
                return 0;                       /* "Dial-Up Adapter" */
    }
    return 1;
}

#endif /* RETRO_IPXPLAN_H */

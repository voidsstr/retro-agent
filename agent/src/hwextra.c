/*
 * hwextra.c - the two facts about a machine that HWPROFILE's single "active
 * adapter" answer deliberately does not carry, plus its network identity.
 *
 * hwprofile.c reports the ONE adapter that is drawing the screen, and that is
 * the right answer for the capability gate: a game runs on the card the
 * desktop is on.  For DOCUMENTING a machine it is not enough, because on this
 * fleet the interesting card is very often not that one:
 *
 *   - .143 renders on a GeForce 6800 and has its Voodoo5 5500 sitting behind
 *     it as a SECOND adapter.  Report only the active one and the box reads as
 *     an nVidia machine, which is how a Voodoo5 test matrix got sized at two
 *     boxes when only one of them had the card.
 *
 *   - A VOODOO 2 IS NOT A DISPLAY ADAPTER AT ALL.  Its INF is Class=MEDIA, so
 *     it appears in no display-class enumeration, EnumDisplayDevices never
 *     mentions it, and .171's card looked absent to every scan we had.  The
 *     only reliable evidence is the PCI enumerator itself: a physically fitted
 *     card enumerates under HKLM\SYSTEM\CurrentControlSet\Enum\PCI even with
 *     no driver bound to it, which is also what proved .133's V5 6000 is
 *     genuinely gone rather than merely undriven.
 *
 * So this module reports:
 *
 *   video_cards[]  every display-class instance (active or not), each marked
 *                  with whether it is the one attached to the desktop
 *   accelerators[] every VEN_121A device found in the PCI enumerator, however
 *                  it is classed - this is the 3dfx question answered by the
 *                  one source that cannot be fooled by a driver class
 *   network        IPv4 addresses and MAC, so a record on the share can be
 *                  matched back to the box that wrote it
 *
 * NB VEN_1102&DEV_0002 is a Creative SB Live!, NOT a Voodoo.  Matching on the
 * device id alone has already sent someone hunting for a 3dfx card in a sound
 * card's registry key; the vendor id is the whole of the test and 121A is the
 * only 3dfx vendor id.
 *
 * Everything here is read-only registry plus one IP Helper call.  It is
 * deliberately tolerant: any step that fails contributes an empty array rather
 * than failing the profile, because a hardware inventory that refuses to
 * report anything when one probe fails is how a box ends up undocumented.
 */

#include "handlers.h"
#include "hwextra.h"
#include "util.h"
#include "log.h"
#include "../shared/hwpub.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG_HWX "HWEXTRA"

#ifndef DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
#define DISPLAY_DEVICE_ATTACHED_TO_DESKTOP 0x00000001
#endif

typedef BOOL (WINAPI *PFN_EnumDisplayDevicesA_x)(LPCSTR, DWORD,
                                                 PDISPLAY_DEVICEA, DWORD);

/* ------------------------------------------------------------------ */
/* small registry helpers (local: hwprofile.c's are static there)       */
/* ------------------------------------------------------------------ */

static int hx_reg_str(HKEY h, const char *name, char *out, DWORD outsz)
{
    DWORD ty = REG_SZ, n = outsz;
    out[0] = 0;
    if (RegQueryValueExA(h, name, NULL, &ty, (BYTE *)out, &n) != ERROR_SUCCESS)
        return 0;
    if (n >= outsz) n = outsz - 1;
    out[n] = 0;
    return out[0] != 0;
}

/* Pull VEN_xxxx / DEV_yyyy out of any hardware-id-shaped string, case
 * insensitively.  Windows writes these uppercase, but we are a Linux-hosted
 * project reasoning about a case-insensitive filesystem and registry, and a
 * case-sensitive match here has already produced a confident "we have no
 * driver for this NIC" three separate times for drivers we shipped. */
static int hx_parse_ven_dev(const char *s, unsigned *ven, unsigned *dev)
{
    const char *p;
    *ven = *dev = 0;
    if (!s || !*s) return 0;
    for (p = s; *p; p++) {
        if ((p[0] == 'V' || p[0] == 'v') && _strnicmp(p, "VEN_", 4) == 0) {
            *ven = (unsigned)strtoul(p + 4, NULL, 16);
            break;
        }
    }
    for (p = s; *p; p++) {
        if ((p[0] == 'D' || p[0] == 'd') && _strnicmp(p, "DEV_", 4) == 0) {
            *dev = (unsigned)strtoul(p + 4, NULL, 16);
            break;
        }
    }
    return *ven != 0;
}

/* ------------------------------------------------------------------ */
/* video_cards[] - EVERY display-class instance                         */
/* ------------------------------------------------------------------ */

/* Is this VEN:DEV the adapter currently attached to the desktop? Asked of
 * EnumDisplayDevices, which is the only thing that knows - so a stale class
 * key is reported as present-but-inactive rather than mistaken for the card
 * doing the work (VIDEODIAG's adapters[0] bug, from the other direction). */
static int hx_is_active(unsigned ven, unsigned dev)
{
    PFN_EnumDisplayDevicesA_x pfn;
    DISPLAY_DEVICEA dd;
    DWORD i;

    if (!ven) return 0;
    pfn = (PFN_EnumDisplayDevicesA_x)GetProcAddress(
              GetModuleHandleA("user32.dll"), "EnumDisplayDevicesA");
    if (!pfn) return 0;

    for (i = 0; i < 16; i++) {
        unsigned v = 0, d = 0;
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!pfn(NULL, i, &dd, 0))
            break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        hx_parse_ven_dev(dd.DeviceID, &v, &d);
        if (v == ven && d == dev)
            return 1;
    }
    return 0;
}

void hwextra_emit_video_cards(json_t *j)
{
    HKEY  hbase;
    DWORD index;
    int   is_nt = (GetVersion() < 0x80000000u);
    const char *base = is_nt
        ? "SYSTEM\\CurrentControlSet\\Control\\Class\\"
          "{4D36E968-E325-11CE-BFC1-08002BE10318}"
        : "System\\CurrentControlSet\\Services\\Class\\Display";

    json_key(j, "video_cards");
    json_array_start(j);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &hbase)
            == ERROR_SUCCESS) {
        for (index = 0; index < 32; index++) {
            char  sub[64], full[512], match[256], desc[256], ver[64], date[64];
            char  buf[16];
            DWORD cch = sizeof(sub);
            HKEY  h;
            unsigned ven = 0, dev = 0;

            if (RegEnumKeyExA(hbase, index, sub, &cch, NULL, NULL, NULL, NULL)
                    != ERROR_SUCCESS)
                break;
            /* Skip the class key's own "Properties" style subkeys - only
             * numbered instance keys are adapters. */
            if (sub[0] < '0' || sub[0] > '9')
                continue;

            _snprintf(full, sizeof(full) - 1, "%s\\%s", base, sub);
            full[sizeof(full) - 1] = 0;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, full, 0, KEY_READ, &h)
                    != ERROR_SUCCESS)
                continue;

            hx_reg_str(h, "MatchingDeviceId", match, sizeof(match));
            hx_reg_str(h, "DriverDesc", desc, sizeof(desc));
            hx_reg_str(h, "DriverVersion", ver, sizeof(ver));
            hx_reg_str(h, "DriverDate", date, sizeof(date));
            RegCloseKey(h);
            hx_parse_ven_dev(match, &ven, &dev);

            json_object_start(j);
            json_kv_str(j, "instance", sub);
            json_kv_str(j, "name", desc);
            format_hex16(buf, ven);
            json_kv_str(j, "pci_ven", buf);
            format_hex16(buf, dev);
            json_kv_str(j, "pci_dev", buf);
            json_kv_str(j, "hardware_id", match);
            json_kv_str(j, "driver_version", ver);
            json_kv_str(j, "driver_date", date);
            json_kv_bool(j, "attached_to_desktop", hx_is_active(ven, dev));
            json_object_end(j);
        }
        RegCloseKey(hbase);
    }

    json_array_end(j);
}

/* ------------------------------------------------------------------ */
/* glide_cards[] - the PCI enumerator, which cannot be fooled by class  */
/* ------------------------------------------------------------------ */

#define VEN_3DFX 0x121A

/*
 * Count the instance subkeys under one Enum\PCI device key.
 *
 * TWO IDENTICAL CARDS SHARE ONE DEVICE KEY.  A pair of SLI Voodoo 2s appears
 * as a single VEN_121A&DEV_0002 key with two instance subkeys beneath it, so
 * anything that counts device keys reports one card where there are two.
 */
static int hx_count_instances(HKEY hdev, char *first, DWORD firstsz)
{
    DWORD i;
    int   n = 0;
    if (first && firstsz) first[0] = 0;
    for (i = 0; i < 64; i++) {
        char  sub[128];
        DWORD cch = sizeof(sub);
        if (RegEnumKeyExA(hdev, i, sub, &cch, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
            break;
        if (n == 0 && first && firstsz)
            safe_strncpy(first, sub, (int)firstsz);
        n++;
    }
    return n;
}

void hwextra_emit_accelerators(json_t *j)
{
    HKEY  hpci;
    DWORD index;

    json_key(j, "accelerators");
    json_array_start(j);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Enum\\PCI", 0,
                      KEY_READ, &hpci) == ERROR_SUCCESS) {
        for (index = 0; index < 512; index++) {
            char  dkey[128], full[512], inst[128], desc[256], buf[16];
            DWORD cch = sizeof(dkey);
            HKEY  hdev, hinst;
            unsigned ven = 0, dev = 0;
            int   count;

            if (RegEnumKeyExA(hpci, index, dkey, &cch, NULL, NULL, NULL, NULL)
                    != ERROR_SUCCESS)
                break;
            if (!hx_parse_ven_dev(dkey, &ven, &dev))
                continue;
            /* The vendor id IS the test. Matching on DEV_0002 alone finds a
             * Creative SB Live! (VEN_1102&DEV_0002) and calls it a Voodoo. */
            if (ven != VEN_3DFX)
                continue;

            _snprintf(full, sizeof(full) - 1,
                      "SYSTEM\\CurrentControlSet\\Enum\\PCI\\%s", dkey);
            full[sizeof(full) - 1] = 0;
            desc[0] = 0;
            count = 0;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, full, 0, KEY_READ, &hdev)
                    == ERROR_SUCCESS) {
                count = hx_count_instances(hdev, inst, sizeof(inst));
                if (count > 0) {
                    char ipath[700];
                    _snprintf(ipath, sizeof(ipath) - 1, "%s\\%s", full, inst);
                    ipath[sizeof(ipath) - 1] = 0;
                    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ipath, 0, KEY_READ,
                                      &hinst) == ERROR_SUCCESS) {
                        if (!hx_reg_str(hinst, "DeviceDesc", desc, sizeof(desc)))
                            hx_reg_str(hinst, "Class", desc, sizeof(desc));
                        RegCloseKey(hinst);
                    }
                }
                RegCloseKey(hdev);
            }

            json_object_start(j);
            json_kv_str(j, "device_key", dkey);
            format_hex16(buf, ven);
            json_kv_str(j, "pci_ven", buf);
            format_hex16(buf, dev);
            json_kv_str(j, "pci_dev", buf);
            json_kv_str(j, "description", desc);
            /* Instances, not device keys - see hx_count_instances. */
            json_kv_int(j, "count", count > 0 ? count : 1);
            json_object_end(j);
        }
        RegCloseKey(hpci);
    }

    json_array_end(j);
}

/* ------------------------------------------------------------------ */
/* network - so a record on the share names the box that wrote it       */
/* ------------------------------------------------------------------ */

/* IP Helper's IP_ADAPTER_INFO, declared here rather than pulled in from
 * iphlpapi.h: the agent builds at WINVER=0x0410 against old mingw headers and
 * this is a stable ABI struct. Resolved by GetProcAddress so a box without
 * iphlpapi.dll (some Win95 installs) degrades to an empty network object
 * instead of failing to load the whole agent. */
#define HX_MAX_ADAPTER_NAME_LENGTH        256
#define HX_MAX_ADAPTER_DESCRIPTION_LENGTH 128
#define HX_MAX_ADAPTER_ADDRESS_LENGTH     8

typedef struct _HX_IP_ADDR_STRING {
    struct _HX_IP_ADDR_STRING *Next;
    char   IpAddress[16];
    char   IpMask[16];
    DWORD  Context;
} HX_IP_ADDR_STRING;

typedef struct _HX_IP_ADAPTER_INFO {
    struct _HX_IP_ADAPTER_INFO *Next;
    DWORD  ComboIndex;
    char   AdapterName[HX_MAX_ADAPTER_NAME_LENGTH + 4];
    char   Description[HX_MAX_ADAPTER_DESCRIPTION_LENGTH + 4];
    UINT   AddressLength;
    BYTE   Address[HX_MAX_ADAPTER_ADDRESS_LENGTH];
    DWORD  Index;
    UINT   Type;
    UINT   DhcpEnabled;
    HX_IP_ADDR_STRING *CurrentIpAddress;
    HX_IP_ADDR_STRING  IpAddressList;
    HX_IP_ADDR_STRING  GatewayList;
    HX_IP_ADDR_STRING  DhcpServer;
    BOOL   HaveWins;
    HX_IP_ADDR_STRING  PrimaryWinsServer;
    HX_IP_ADDR_STRING  SecondaryWinsServer;
    time_t LeaseObtained;
    time_t LeaseExpires;
} HX_IP_ADAPTER_INFO;

typedef DWORD (WINAPI *PFN_GetAdaptersInfo)(HX_IP_ADAPTER_INFO *, ULONG *);

void hwextra_emit_network(json_t *j)
{
    HMODULE hip;
    PFN_GetAdaptersInfo pfn = NULL;
    int emitted = 0;

    json_key(j, "network");
    json_object_start(j);
    json_key(j, "interfaces");
    json_array_start(j);

    hip = LoadLibraryA("iphlpapi.dll");
    if (hip)
        pfn = (PFN_GetAdaptersInfo)GetProcAddress(hip, "GetAdaptersInfo");

    if (pfn) {
        ULONG size = 0;
        /* First call sizes the buffer; ERROR_BUFFER_OVERFLOW (111) is the
         * success path here, not a failure. */
        pfn(NULL, &size);
        if (size > 0 && size < 256 * 1024) {
            HX_IP_ADAPTER_INFO *ai = (HX_IP_ADAPTER_INFO *)
                HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
            if (ai && pfn(ai, &size) == NO_ERROR) {
                HX_IP_ADAPTER_INFO *a;
                for (a = ai; a; a = a->Next) {
                    char mac[32];
                    HX_IP_ADDR_STRING *ip;

                    /* Skip the loopback pseudo-adapter: it identifies nothing
                     * and every box would report the same 127.0.0.1. */
                    if (a->IpAddressList.IpAddress[0] == 0 ||
                        strcmp(a->IpAddressList.IpAddress, "0.0.0.0") == 0)
                        continue;

                    /* Offset arithmetic that looks right and is not - see
                     * hwpub_format_mac(); tests/native/test_hwpublish.c
                     * asserts it against the truncating form. */
                    hwpub_format_mac(a->Address, a->AddressLength,
                                     mac, sizeof(mac));

                    json_object_start(j);
                    json_kv_str(j, "description", a->Description);
                    json_kv_str(j, "mac", mac);
                    json_key(j, "ipv4");
                    json_array_start(j);
                    for (ip = &a->IpAddressList; ip; ip = ip->Next) {
                        if (ip->IpAddress[0] &&
                            strcmp(ip->IpAddress, "0.0.0.0") != 0)
                            json_str(j, ip->IpAddress);
                    }
                    json_array_end(j);
                    json_object_end(j);
                    emitted++;
                }
            }
            if (ai)
                HeapFree(GetProcessHeap(), 0, ai);
        }
    }

    /* Fallback: no IP Helper (or it told us nothing). gethostbyname on our own
     * name gives the addresses without a MAC, which is still enough to match a
     * record on the share back to a box. Winsock is already started by the
     * agent's listener, so this costs nothing extra. */
    if (!emitted) {
        char host[128];
        DWORD cch = sizeof(host);
        struct hostent *he;
        if (GetComputerNameA(host, &cch) && (he = gethostbyname(host)) != NULL
            && he->h_addrtype == AF_INET) {
            int n;
            json_object_start(j);
            json_kv_str(j, "description", "gethostbyname");
            json_kv_str(j, "mac", "");
            json_key(j, "ipv4");
            json_array_start(j);
            for (n = 0; he->h_addr_list[n] && n < 8; n++) {
                struct in_addr in;
                memcpy(&in, he->h_addr_list[n], sizeof(in));
                json_str(j, inet_ntoa(in));
            }
            json_array_end(j);
            json_object_end(j);
        }
    }

    json_array_end(j);
    json_kv_str(j, "source", pfn ? "GetAdaptersInfo" : "gethostbyname");
    json_object_end(j);

    if (hip)
        FreeLibrary(hip);
}

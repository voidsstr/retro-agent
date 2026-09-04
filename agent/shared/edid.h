/*
 * edid.h - the PANEL's native mode, read from EDID.
 *
 * WHY EDID AND NOT THE CURRENT DESKTOP MODE. A game that exits without
 * restoring the mode leaves the desktop at whatever it was using, and on this
 * fleet that really happens: .123 and .240 were both sitting at 640x480 from a
 * DOSBox leftover while driving 1080p panels. A tool that trusts the live mode
 * reads "this box is a 640x480 box", and if it then WRITES that conclusion
 * anywhere it pins the machine there for good. EnumDisplaySettings with
 * ENUM_CURRENT_SETTINGS answers "what is it showing", which is a different
 * question from "what can the panel do" - and only the second one is a
 * property of the hardware.
 *
 * This is a port of the probe in provisioning/fleetres/fleetres.c, which was
 * developed against the real fleet; the two hard-won details below are the
 * reason it is not three lines long. Keep them in step.
 */
#ifndef RETRO_EDID_H
#define RETRO_EDID_H

#include <windows.h>
#include <string.h>
#include <stdio.h>

#ifndef EDID_FN
#define EDID_FN static
#endif

typedef struct {
    int ok;
    int native_w, native_h, native_hz;
    int digital;                /* 1 = a digital panel (LCD), 0 = analogue */
    /*
     * THE NEXT THREE DECIDE LCD-vs-CRT AND THE TUBE'S SHAPE, and neither
     * question can be answered from native_w/native_h alone.
     *
     *  vmax    maximum vertical refresh from the 0xFD range descriptor. The
     *          LCD test is `digital OR (vmax <= 76 AND preferred <= 61)`,
     *          validated against all eight fleet panels: every CRT here
     *          quotes 85-180 Hz, every LCD quotes <= 76 at 60. The digital
     *          bit alone misses an analogue-input flat panel, and .246's HP
     *          2511 is the only fleet LCD that sets it.
     *  hcm/vcm the PHYSICAL screen size in centimetres. This is what says a
     *          tube is 4:3 - .133 and .171 are 4:3 CRTs that were being
     *          driven at 1280x1024, i.e. 5:4, and the MODE cannot tell you
     *          that because 1280x1024 is a perfectly real mode on a 4:3
     *          tube. Only the physical ratio separates "this panel is 5:4"
     *          from "this panel is 4:3 and the mode is wrong".
     */
    int vmax;                   /* max vertical refresh, 0xFD descriptor     */
    int hcm, vcm;               /* physical size in cm, 0 when not stated    */
    char name[16];              /* monitor name from the 0xFC descriptor */
    char pnpid[16];             /* the Enum\DISPLAY node it came from        */
} edid_panel_t;

EDID_FN int edid_is_valid(const BYTE *b, DWORD n)
{
    static const BYTE hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    return (n >= 128 && memcmp(b, hdr, 8) == 0);
}

/*
 * The FIRST detailed timing descriptor is the preferred (native) mode - that
 * is what the EDID spec means by preferred, and for an LCD it is the only mode
 * that is not a scaled approximation.
 */
EDID_FN void edid_parse(const BYTE *b, edid_panel_t *p)
{
    int off, i = 0;

    memset(p, 0, sizeof(*p));
    p->digital = (b[20] & 0x80) ? 1 : 0;
    p->hcm = b[21];
    p->vcm = b[22];

    for (off = 54; off <= 108; off += 18) {
        const BYTE *d = b + off;
        int px = d[0] | (d[1] << 8);
        if (px == 0) {
            if (d[3] == 0xFD) {                      /* display range limits */
                p->vmax = d[6];
            } else if (d[3] == 0xFC && !p->name[0]) { /* monitor name */
                for (i = 0; i < 13; i++) {
                    char c = (char)d[5 + i];
                    if (c == '\n' || c == 0) break;
                    p->name[i] = c;
                }
                p->name[i] = 0;
                while (i > 0 && p->name[i - 1] == ' ')
                    p->name[--i] = 0;
            }
            continue;
        }
        if (!p->native_w) {
            int hact = d[2] | ((d[4] & 0xF0) << 4);
            int hbl  = d[3] | ((d[4] & 0x0F) << 8);
            int vact = d[5] | ((d[7] & 0xF0) << 4);
            int vbl  = d[6] | ((d[7] & 0x0F) << 8);
            int htot = hact + hbl, vtot = vact + vbl;
            double clk = px * 10000.0;
            p->native_w = hact;
            p->native_h = vact;
            p->native_hz = (htot && vtot)
                         ? (int)(clk / (htot * (double)vtot) + 0.5) : 0;
        }
    }
    p->ok = (p->native_w > 0 && p->native_h > 0);
}

/* Read the EDID for ONE monitor PnP id out of
 * HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<pnpid>\<instance>.
 * The ACTIVE instance is the one carrying a "Control" subkey. */
EDID_FN int edid_from_pnp(const char *pnp, edid_panel_t *p, int require_active)
{
    HKEY hk, hi, hdp;
    char path[512], inst[256];
    DWORD idx, len, elen, type;
    BYTE ed[512];
    int got = 0, best = 0;

    sprintf(path, "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s", pnp);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hk)
        != ERROR_SUCCESS)
        return 0;

    for (idx = 0; ; idx++) {
        int active;
        HKEY hc;
        len = sizeof(inst);
        if (RegEnumKeyExA(hk, idx, inst, &len, NULL, NULL, NULL, NULL)
            != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExA(hk, inst, 0, KEY_READ, &hi) != ERROR_SUCCESS)
            continue;
        active = (RegOpenKeyExA(hi, "Control", 0, KEY_READ, &hc)
                  == ERROR_SUCCESS);
        if (active)
            RegCloseKey(hc);
        if (RegOpenKeyExA(hi, "Device Parameters", 0, KEY_READ, &hdp)
            == ERROR_SUCCESS) {
            elen = sizeof(ed);
            if (RegQueryValueExA(hdp, "EDID", NULL, &type, ed, &elen)
                    == ERROR_SUCCESS
                && edid_is_valid(ed, elen) && (active || !require_active)) {
                if (!got || (active && !best)) {
                    edid_panel_t tmp;
                    edid_parse(ed, &tmp);
                    if (tmp.ok) {
                        strncpy(tmp.pnpid, pnp, sizeof(tmp.pnpid) - 1);
                        tmp.pnpid[sizeof(tmp.pnpid) - 1] = 0;
                        *p = tmp;
                        got = 1;
                        if (active)
                            best = 1;
                    }
                }
            }
            RegCloseKey(hdp);
        }
        RegCloseKey(hi);
        if (best)
            break;
    }
    RegCloseKey(hk);
    return got;
}

/*
 * Walk EVERY attached adapter and EVERY monitor on it, not just adapter 0 /
 * monitor 0 - measured on .171, where two monitor nodes are enumerated (a real
 * Gateway VX1120 and a Default_Monitor carrying no EDID) and WHICH ONE COMES
 * BACK FIRST IS NOT STABLE BETWEEN RUNS. A single-shot probe therefore read the
 * panel correctly on some runs and silently fell back on others, which is far
 * worse than failing outright because it looks like the panel changed.
 */
EDID_FN int edid_probe_panel(edid_panel_t *p)
{
    DISPLAY_DEVICEA ad, mo;
    DWORD a, m;
    char pnp[64];

    memset(p, 0, sizeof(*p));

    for (a = 0; a < 8; a++) {
        memset(&ad, 0, sizeof(ad));
        ad.cb = sizeof(ad);
        if (!EnumDisplayDevicesA(NULL, a, &ad, 0))
            break;
        if (!(ad.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        for (m = 0; m < 8; m++) {
            char *s2, *e2;
            memset(&mo, 0, sizeof(mo));
            mo.cb = sizeof(mo);
            if (!EnumDisplayDevicesA(ad.DeviceName, m, &mo, 0))
                break;
            s2 = strchr(mo.DeviceID, '\\');
            if (!s2)
                continue;
            s2++;
            strncpy(pnp, s2, sizeof(pnp) - 1);
            pnp[sizeof(pnp) - 1] = 0;
            e2 = strchr(pnp, '\\');
            if (e2)
                *e2 = 0;
            if (!pnp[0] || _stricmp(pnp, "Default_Monitor") == 0)
                continue;
            if (edid_from_pnp(pnp, p, 0))
                return 1;
        }
    }

    /* Last resort: EnumDisplayDevices can hand back Default_Monitor for a panel
     * whose EDID the registry still holds. Scan Enum\DISPLAY directly and take
     * any non-default node with a valid EDID on an ACTIVE instance. */
    {
        HKEY hk;
        DWORD i2, l2;
        char key[256];
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY", 0,
                          KEY_READ, &hk) == ERROR_SUCCESS) {
            for (i2 = 0; ; i2++) {
                l2 = sizeof(key);
                if (RegEnumKeyExA(hk, i2, key, &l2, NULL, NULL, NULL, NULL)
                    != ERROR_SUCCESS)
                    break;
                if (_stricmp(key, "Default_Monitor") == 0)
                    continue;
                if (edid_from_pnp(key, p, 1)) {
                    RegCloseKey(hk);
                    return 1;
                }
            }
            RegCloseKey(hk);
        }
    }
    return 0;
}

#endif /* RETRO_EDID_H */

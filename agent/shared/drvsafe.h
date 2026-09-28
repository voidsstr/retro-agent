/*
 * drvsafe.h - the 3dfx rule every automatic driver path obeys (agent 1.87.0).
 *
 * User directive, 2026-09-27: the agent keeps a box's drivers correct "except
 * for 3dfx since those drivers should not be touched unless explicitly asked
 * by the chat". Until 1.87.0 nothing enforced that: the XP missing-driver pass
 * listed every device with a problem code, in every class, and the PXE image's
 * C:\D carries four 3dfx INFs (I001-I003 from the DriverPacks graphics pack,
 * V001 AmigaMerlin) - run against the real tree, the agent's own matcher chose
 * I003\3dfxvs2k.inf for a Voodoo5 and I002\voodoo2.inf for a Voodoo 2.
 *
 * What counts as 3dfx, all case-insensitive:
 *   - a device whose hardware/compatible ids carry the VEN_121A token or the
 *     word 3DFX (the Voodoo 2 is Class=MEDIA, and the V3 TV-out child is
 *     DISPLAY\3dfxV3TV with no VEN_121A at all - a class or vendor filter alone
 *     misses both);
 *   - a device BELOW a 3dfx device (TV-out children, monitors);
 *   - a PCI-to-PCI bridge with a 3dfx device below it (the V5 6000's HiNT
 *     bridge - our driver drives the board clock through its GPIO);
 *   - a device whose BOUND driver names 3dfx (provider, maker, description,
 *     INF, service): our vcr-kmd and the vintage lane included;
 *   - and, for every device, any candidate INF whose text names 3dfx - I001,
 *     I003 and V001 also register a global OpenGLdrivers\3dfx ICD, so they must
 *     not be installed onto ANY id.
 * A false positive costs a reported skip; a false negative costs the Voodoo.
 *
 * "Explicitly asked by the chat" = the command carries the literal token
 * ALLOW3DFX and names ONE device. There is deliberately no registry switch:
 * a persisted "allow" is exactly the automatic behaviour the user forbade.
 *
 * Win32-free: tests/native/test_drvsafe.c compiles this exact code.
 */
#ifndef RETRO_DRVSAFE_H
#define RETRO_DRVSAFE_H

#include <string.h>

#define DRVSAFE_ALLOW_TOKEN "ALLOW3DFX"

/* Includers use a few helpers each; none should warn about the rest. */
#if defined(__GNUC__)
#define DRVSAFE_API static __attribute__((unused))
#else
#define DRVSAFE_API static
#endif

enum {
    DRVSAFE_OK = 0,
    DRVSAFE_3DFX_ID,          /* the device's own ids */
    DRVSAFE_3DFX_ANCESTOR,    /* below a 3dfx device */
    DRVSAFE_3DFX_BRIDGE,      /* a PCI bridge with a 3dfx device below it */
    DRVSAFE_3DFX_DRIVER,      /* its bound driver is 3dfx */
    DRVSAFE_3DFX_INF          /* the candidate INF is 3dfx */
};

DRVSAFE_API int drvsafe_up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* Case-insensitive strstr for ASCII. */
DRVSAFE_API const char *drvsafe_stristr(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i;
    if (!hay || !n) return NULL;
    for (; *hay; hay++) {
        for (i = 0; i < n && hay[i] && drvsafe_up((unsigned char)hay[i]) == drvsafe_up((unsigned char)needle[i]); i++)
            ;
        if (i == n) return hay;
    }
    return NULL;
}

DRVSAFE_API int drvsafe_is_hex(int c)
{
    c = drvsafe_up(c);
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

/* One id: the VEN_121A token (not followed by another hex digit, so a longer
 * number is not mistaken for it) or the word 3DFX anywhere. */
DRVSAFE_API int drvsafe_id_is_3dfx(const char *id)
{
    const char *p = id;
    if (!id) return 0;
    while ((p = drvsafe_stristr(p, "VEN_121A")) != NULL) {
        if (!drvsafe_is_hex((unsigned char)p[8])) return 1;
        p += 8;
    }
    return drvsafe_stristr(id, "3DFX") != NULL;
}

/* A list of ids: NUL-separated and double-NUL terminated (NT REG_MULTI_SZ), or
 * comma-separated inside one string (Win9x Enum HardwareID), or both. NULL is
 * an empty list. Every id in every string is checked. */
DRVSAFE_API int drvsafe_list_3dfx(const char *list)
{
    char one[256];
    const char *p;
    size_t k;
    if (!list) return 0;
    for (; *list; list += strlen(list) + 1) {           /* each NUL-terminated string */
        for (p = list; *p; ) {                          /* each comma-separated id */
            for (k = 0; *p && *p != ','; p++)
                if (k + 1 < sizeof(one)) one[k++] = *p;
            one[k] = 0;
            if (drvsafe_id_is_3dfx(one)) return 1;
            if (*p == ',') p++;
        }
    }
    return 0;
}

/* Does any string of a list contain `word` (case-insensitive)? */
DRVSAFE_API int drvsafe_list_has(const char *list, const char *word)
{
    if (!list) return 0;
    for (; *list; list += strlen(list) + 1)
        if (drvsafe_stristr(list, word)) return 1;
    return 0;
}

DRVSAFE_API int drvsafe_ids_3dfx(const char *hw, const char *compat)
{
    return drvsafe_list_3dfx(hw) || drvsafe_list_3dfx(compat);
}

/* Words that mark a driver, a provider string or an INF as 3dfx. GLIDE alone is
 * NOT one of them: Alps "GlidePoint" touchpads are not ours to skip. */
DRVSAFE_API const char *const drvsafe_words[] = {
    "VEN_121A", "3DFX", "VOODOO", "AMIGAMERLIN", "GLIDE2X", "GLIDE3X",
    "VCR-KMD", "VCRMP", "VCRDD", "FXGPIO", NULL
};

/* The first 3dfx word in `s`, or NULL. */
DRVSAFE_API const char *drvsafe_text_hit(const char *s)
{
    int i;
    if (!s) return NULL;
    for (i = 0; drvsafe_words[i]; i++)
        if (drvsafe_stristr(s, drvsafe_words[i])) {
            if (i == 0) {                            /* the vendor token must be bounded */
                const char *p = s;
                while ((p = drvsafe_stristr(p, "VEN_121A")) != NULL) {
                    if (!drvsafe_is_hex((unsigned char)p[8])) return drvsafe_words[0];
                    p += 8;
                }
                continue;
            }
            return drvsafe_words[i];
        }
    return NULL;
}

DRVSAFE_API int drvsafe_text_3dfx(const char *s) { return drvsafe_text_hit(s) != NULL; }

/* A bound driver's strings: any 3dfx word in any of them. */
DRVSAFE_API int drvsafe_driver_is_3dfx(const char *provider, const char *mfg, const char *desc,
                                  const char *infpath, const char *service)
{
    return drvsafe_text_3dfx(provider) || drvsafe_text_3dfx(mfg) || drvsafe_text_3dfx(desc)
        || drvsafe_text_3dfx(infpath) || drvsafe_text_3dfx(service);
}

/* Is this device a PCI-to-PCI bridge (class 0604 - AGP bridges included)? Host
 * bridges (0600) are not: nothing hangs a 3dfx board off one on its own. */
DRVSAFE_API int drvsafe_is_pci_bridge(const char *hw, const char *compat)
{
    return drvsafe_list_has(hw, "CC_0604") || drvsafe_list_has(compat, "CC_0604");
}

/* Does the command explicitly allow touching 3dfx? The token must stand alone
 * (space-delimited), and it never counts together with ALL. */
DRVSAFE_API int drvsafe_args_allow(const char *args)
{
    const char *p = args;
    size_t n = strlen(DRVSAFE_ALLOW_TOKEN);
    int allow = 0, all = 0;
    if (!args) return 0;
    while (*p) {
        const char *s;
        size_t k;
        while (*p == ' ' || *p == '\t') p++;
        s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        k = (size_t)(p - s);
        if (k == n) {
            size_t i;
            for (i = 0; i < n && drvsafe_up((unsigned char)s[i]) == DRVSAFE_ALLOW_TOKEN[i]; i++)
                ;
            if (i == n) allow = 1;
        }
        if (k == 3 && drvsafe_up((unsigned char)s[0]) == 'A' && drvsafe_up((unsigned char)s[1]) == 'L'
                && drvsafe_up((unsigned char)s[2]) == 'L')
            all = 1;
    }
    return allow && !all;
}

DRVSAFE_API const char *drvsafe_reason_name(int reason)
{
    switch (reason) {
    case DRVSAFE_3DFX_ID:       return "3dfx device";
    case DRVSAFE_3DFX_ANCESTOR: return "below a 3dfx device";
    case DRVSAFE_3DFX_BRIDGE:   return "PCI bridge above a 3dfx device";
    case DRVSAFE_3DFX_DRIVER:   return "bound to a 3dfx driver";
    case DRVSAFE_3DFX_INF:      return "3dfx INF";
    default:                    return "ok";
    }
}

#endif

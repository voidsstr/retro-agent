/*
 * hostpolicy.c - "Is this a fleet box, or somebody's actual PC?"
 *
 * See hostpolicy.h for why GetVersionEx cannot answer this and why ntdll is
 * LoadLibrary'd rather than imported.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "hostpolicy.h"
#include "log.h"

#define LOG_POLICY "POLICY"

/* RtlGetVersion's argument. Declared here rather than pulled from winternl.h:
 * the agent builds with WINVER=0x0410 against MinGW headers that do not carry
 * a usable RTL_OSVERSIONINFOW, and this layout is fixed ABI. */
typedef struct {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} HP_RTL_OSVERSIONINFOW;

typedef LONG (WINAPI *RtlGetVersion_t)(HP_RTL_OSVERSIONINFOW *);

/* -1 = not yet computed. */
static int  g_modern    = -1;
static int  g_managed   = -1;
static DWORD g_major    = 0;
static DWORD g_minor    = 0;
static DWORD g_build    = 0;

int host_os_version(DWORD *major, DWORD *minor, DWORD *build)
{
    OSVERSIONINFOA  osvi;
    HMODULE         ntdll;
    RtlGetVersion_t pRtlGetVersion;
    int             got = 0;

    if (g_major) {                      /* answered once already */
        if (major) *major = g_major;
        if (minor) *minor = g_minor;
        if (build) *build = g_build;
        return 1;
    }

    /* Preferred: the unshimmed truth. LoadLibrary, never a static import -
     * ntdll is not in the Win98-proven DLL set (hostpolicy.h). */
    ntdll = LoadLibraryA("ntdll.dll");
    if (ntdll) {
        pRtlGetVersion = (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
        if (pRtlGetVersion) {
            HP_RTL_OSVERSIONINFOW rovi;
            memset(&rovi, 0, sizeof(rovi));
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (pRtlGetVersion(&rovi) == 0) {   /* STATUS_SUCCESS */
                g_major = rovi.dwMajorVersion;
                g_minor = rovi.dwMinorVersion;
                g_build = rovi.dwBuildNumber;
                got = 1;
            }
        }
        FreeLibrary(ntdll);
    }

    /* Fallback: Win9x (no ntdll at all) and anything else that refused.
     *
     * This path CANNOT distinguish 8.1/10/11 - they all say 6.2 here - but it
     * is only reached where RtlGetVersion is missing, i.e. 9x/NT4-era Windows,
     * which is never modern. A modern box always takes the branch above. */
    if (!got) {
        memset(&osvi, 0, sizeof(osvi));
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        if (GetVersionExA(&osvi)) {
            g_major = osvi.dwMajorVersion;
            g_minor = osvi.dwMinorVersion;
            g_build = osvi.dwBuildNumber;
            got = 1;
        }
    }

    if (major) *major = g_major;
    if (minor) *minor = g_minor;
    if (build) *build = g_build;
    return got;
}

int host_is_modern_windows(void)
{
    DWORD major = 0;

    if (g_modern >= 0)
        return g_modern;

    if (!host_os_version(&major, NULL, NULL)) {
        /* Cannot tell. Assume the fleet's own default - a retro box - because
         * that is what every machine this agent has ever run on has been, and
         * because the alternative silently disables the agent's whole purpose
         * on a box whose version call merely hiccuped. A modern box answers
         * RtlGetVersion reliably; it does not end up here. */
        g_modern = 0;
        return g_modern;
    }

    g_modern = (major >= HOSTPOLICY_MODERN_MAJOR) ? 1 : 0;
    return g_modern;
}

/* HKLM\Software\RetroAgent\ManageModernWindows == 1 */
static int override_set(void)
{
    HKEY  k;
    DWORD val = 0, sz = sizeof(val), type = 0;
    int   on = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, HOSTPOLICY_OVERRIDE_KEY, 0,
                      KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(k, HOSTPOLICY_OVERRIDE_VALUE, NULL, &type,
                         (LPBYTE)&val, &sz) == ERROR_SUCCESS &&
        type == REG_DWORD && val == 1)
        on = 1;
    RegCloseKey(k);
    return on;
}

int host_manages_this_box(void)
{
    if (g_managed >= 0)
        return g_managed;

    if (!host_is_modern_windows()) {
        g_managed = 1;
    } else if (override_set()) {
        log_msg(LOG_POLICY, "modern Windows (%lu.%lu build %lu) but HKLM\\%s\\%s=1 "
                            "- managing this box as a fleet box on purpose",
                (unsigned long)g_major, (unsigned long)g_minor,
                (unsigned long)g_build,
                HOSTPOLICY_OVERRIDE_KEY, HOSTPOLICY_OVERRIDE_VALUE);
        g_managed = 1;
    } else {
        log_msg(LOG_POLICY, "modern Windows (%lu.%lu build %lu) - this box will "
                            "NOT be themed, skinned or reconfigured. Set HKLM\\%s\\%s "
                            "(DWORD) to 1 to manage it anyway.",
                (unsigned long)g_major, (unsigned long)g_minor,
                (unsigned long)g_build,
                HOSTPOLICY_OVERRIDE_KEY, HOSTPOLICY_OVERRIDE_VALUE);
        g_managed = 0;
    }
    return g_managed;
}

/*
 * Log each distinct `what` once. The wallpaper keep-loop asks every 5 minutes
 * and command handlers ask on every request; without this the log becomes the
 * same line forever and the interesting ones scroll away. Small fixed table -
 * no allocation, and overflow just means a repeat, never a miss.
 */
#define SEEN_MAX 24
static const char *g_seen[SEEN_MAX];
static int         g_seen_n = 0;

static int already_logged(const char *what)
{
    int i;
    for (i = 0; i < g_seen_n; i++) {
        if (g_seen[i] && _stricmp(g_seen[i], what) == 0)
            return 1;
    }
    if (g_seen_n < SEEN_MAX)
        g_seen[g_seen_n++] = what;   /* callers pass string literals */
    return 0;
}

int host_policy_skip(const char *what)
{
    if (host_manages_this_box())
        return 0;
    if (!already_logged(what ? what : "(unnamed)"))
        log_msg(LOG_POLICY, "skipping %s - modern Windows, host management is off",
                what ? what : "(unnamed)");
    return 1;
}

void host_policy_describe(char *buf, int bufsize)
{
    DWORD major = 0, minor = 0, build = 0;

    if (!buf || bufsize <= 0)
        return;
    host_os_version(&major, &minor, &build);
    _snprintf(buf, bufsize - 1, "Windows %lu.%lu build %lu - host management %s",
              (unsigned long)major, (unsigned long)minor, (unsigned long)build,
              host_manages_this_box() ? "enabled" : "DISABLED (modern OS)");
    buf[bufsize - 1] = 0;
}

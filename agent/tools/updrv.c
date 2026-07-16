/*
 * updrv.c - scriptable PnP driver install/update tool for Windows 2000/XP
 *
 * Installs (or force-updates) the driver for an already-present PnP device
 * from an INF, exactly like Device Manager's "Update Driver" but with no GUI.
 * Built for driving over the retro agent (EXEC/EXECW) - single result line,
 * meaningful exit code.
 *
 * Usage:
 *   updrv <full-path-to-inf> <hardware-id>
 *   e.g. updrv C:\RETRO_AGENT\3dfx\voodoo3.inf "PCI\VEN_121A&DEV_0005"
 *
 * Output (one line):
 *   UPDRV: OK                    driver installed/updated        (exit 0)
 *   UPDRV: OK REBOOT REQUIRED    installed, reboot to activate   (exit 2)
 *   UPDRV: FAIL code=0x........  failed, + meaning if known      (exit 1)
 *
 * Implementation notes:
 *   - newdev.dll!UpdateDriverForPlugAndPlayDevicesA is loaded DYNAMICALLY
 *     (LoadLibrary/GetProcAddress) so the exe itself imports only
 *     kernel32/user32 and loads on any Win32; the call needs Win2000+.
 *   - INSTALLFLAG_FORCE makes the given INF win even if Windows thinks the
 *     currently-installed driver is a "better" match.
 *   - Built freestanding (-nostdlib) so there is no msvcrt import; console
 *     output goes through WriteFile, formatting through wsprintfA.
 *
 * Build:
 *   i686-w64-mingw32-gcc -Wall -Wextra -Os -s -nostdlib \
 *     -DWIN32_LEAN_AND_MEAN -DWINVER=0x0500 -D_WIN32_WINNT=0x0500 \
 *     -march=i586 -mtune=pentium3 -fno-stack-protector \
 *     -o updrv.exe updrv.c -Wl,-e,_mainCRTStartup -lkernel32 -luser32
 */

#include <windows.h>

#ifndef INSTALLFLAG_FORCE
#define INSTALLFLAG_FORCE 0x00000001
#endif

typedef BOOL (WINAPI *UpdateDriverForPnPDevsA_t)(
    HWND hwndParent,
    LPCSTR HardwareId,
    LPCSTR FullInfPath,
    DWORD InstallFlags,
    PBOOL bRebootRequired);

static void out(const char *s)
{
    DWORD written;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h != NULL)
        WriteFile(h, s, (DWORD)lstrlenA(s), &written, NULL);
}

/* Human meaning for the failure codes actually seen in the field. */
static const char *err_meaning(DWORD e)
{
    switch (e) {
    case 0x00000002: return "ERROR_FILE_NOT_FOUND: INF, or a file its CopyFiles section names, is missing";
    case 0x00000003: return "ERROR_PATH_NOT_FOUND: bad path";
    case 0x00000005: return "ERROR_ACCESS_DENIED: must run as Administrator";
    case 0x0000000D: return "ERROR_INVALID_DATA: INF unreadable or malformed";
    case 0x00000057: return "ERROR_INVALID_PARAMETER: bad argument (INF path must be a FULL path)";
    case 0x00000103: return "ERROR_NO_MORE_ITEMS: no present device matches that hardware ID (or driver not a better match without FORCE)";
    case 0xE0000101: return "ERROR_SECTION_NOT_FOUND: INF section missing (trimmed INF broke a reference?)";
    case 0xE0000102: return "ERROR_LINE_NOT_FOUND: INF line/directive missing";
    case 0xE0000200: return "ERROR_NO_ASSOCIATED_CLASS: INF has no Class/ClassGUID";
    case 0xE0000201: return "ERROR_CLASS_MISMATCH: INF class does not match the device's class";
    case 0xE0000203: return "ERROR_NO_DRIVER_SELECTED: no model line in the INF matches that hardware ID";
    case 0xE000020B: return "ERROR_NO_SUCH_DEVINST: device with that hardware ID is not present";
    case 0xE000022F: return "ERROR_NO_CATALOG_FOR_OEM_INF: unsigned driver blocked - set driver-signing policy to Ignore first";
    case 0xE0000235: return "ERROR_IN_WOW64: wrong platform binary for this OS (use the 32-bit tool on 32-bit XP)";
    case 0xE0000244: return "ERROR_SIGNATURE_OSATTRIBUTE_MISMATCH: signature does not cover this OS";
    default:         return "";
    }
}

/*
 * Pull the next command-line token out of *pp into dst (NUL-terminated),
 * honoring double quotes (quotes are stripped, content kept verbatim so
 * "PCI\VEN_121A&DEV_0005" survives). Returns 1 if a token was found.
 */
static int next_arg(char **pp, char *dst, int dstlen)
{
    char *p = *pp;
    int n = 0, inq = 0;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0') {
        *pp = p;
        return 0;
    }
    while (*p != '\0' && (inq || (*p != ' ' && *p != '\t'))) {
        if (*p == '"') {
            inq = !inq;
        } else if (n < dstlen - 1) {
            dst[n++] = *p;
        }
        p++;
    }
    dst[n] = '\0';
    *pp = p;
    return 1;
}

void __cdecl mainCRTStartup(void)
{
    char inf[520];
    char hwid[512];
    char full[520];
    char msg[1024];
    char *cmdline;
    HMODULE hNewDev;
    UpdateDriverForPnPDevsA_t pUpdate;
    BOOL ok, reboot = FALSE;
    DWORD err, attrs;

    cmdline = GetCommandLineA();

    /* skip argv[0], then take the two real args */
    next_arg(&cmdline, msg, sizeof(msg));           /* program name (reuse msg) */
    if (!next_arg(&cmdline, inf, sizeof(inf)) ||
        !next_arg(&cmdline, hwid, sizeof(hwid))) {
        out("updrv - install/update a PnP driver from an INF (Win2000/XP)\r\n"
            "usage: updrv <full-path-to-inf> <hardware-id>\r\n"
            "  e.g. updrv C:\\RETRO_AGENT\\3dfx\\voodoo3.inf \"PCI\\VEN_121A&DEV_0005\"\r\n");
        ExitProcess(1);
    }

    /* the API demands a full path - normalize whatever we were given */
    if (GetFullPathNameA(inf, sizeof(full), full, NULL) == 0)
        lstrcpyA(full, inf);

    attrs = GetFileAttributesA(full);
    if (attrs == 0xFFFFFFFF || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        wsprintfA(msg, "UPDRV: FAIL code=0x%08X INF not found: %s\r\n",
                  (unsigned)ERROR_FILE_NOT_FOUND, full);
        out(msg);
        ExitProcess(1);
    }

    hNewDev = LoadLibraryA("newdev.dll");
    if (hNewDev == NULL) {
        wsprintfA(msg, "UPDRV: FAIL code=0x%08X newdev.dll not available "
                       "(needs Windows 2000 or later)\r\n",
                  (unsigned)GetLastError());
        out(msg);
        ExitProcess(1);
    }

    pUpdate = (UpdateDriverForPnPDevsA_t)
        GetProcAddress(hNewDev, "UpdateDriverForPlugAndPlayDevicesA");
    if (pUpdate == NULL) {
        out("UPDRV: FAIL code=0x0000007F UpdateDriverForPlugAndPlayDevicesA "
            "not exported by newdev.dll\r\n");
        ExitProcess(1);
    }

    SetLastError(0);
    ok = pUpdate(NULL, hwid, full, INSTALLFLAG_FORCE, &reboot);

    if (ok) {
        if (reboot) {
            out("UPDRV: OK REBOOT REQUIRED\r\n");
            ExitProcess(2);
        }
        out("UPDRV: OK\r\n");
        ExitProcess(0);
    }

    err = GetLastError();
    wsprintfA(msg, "UPDRV: FAIL code=0x%08X %s\r\n",
              (unsigned)err, err_meaning(err));
    out(msg);
    ExitProcess(1);
}

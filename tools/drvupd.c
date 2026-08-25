/* drvupd.exe <inf> <hwid>  - headless devcon-update equivalent (SetupAPI/newdev) */
#include <windows.h>
#include <stdio.h>
#include <cfgmgr32.h>

typedef BOOL (WINAPI *PUDFPNPD)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: drvupd <inf> <hwid>\n"); return 2; }

    /* Re-enumerate the whole device tree first so a freshly fitted card appears. */
    DEVINST root;
    if (CM_Locate_DevNodeA(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
        CONFIGRET cr = CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
        printf("rescan: %s (0x%lx)\n", cr == CR_SUCCESS ? "ok" : "failed", (unsigned long)cr);
    }

    HMODULE nd = LoadLibraryA("newdev.dll");
    PUDFPNPD upd = nd ? (PUDFPNPD)GetProcAddress(nd, "UpdateDriverForPlugAndPlayDevicesA") : NULL;
    if (!upd) { printf("ERROR: newdev.dll/UpdateDriverForPlugAndPlayDevicesA unavailable\n"); return 3; }

    BOOL reboot = FALSE;
    /* 1 = INSTALLFLAG_FORCE */
    if (upd(NULL, argv[2], argv[1], 1, &reboot)) {
        printf("INSTALLED %s via %s%s\n", argv[2], argv[1], reboot ? " (reboot needed)" : "");
        return 0;
    }
    printf("FAILED err=%lu\n", (unsigned long)GetLastError());
    return 1;
}

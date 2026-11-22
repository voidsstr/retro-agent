/*
 * devctl.exe - enable / disable / list a device node on an NT fleet box.
 *
 * WHY: Windows 7 x86 on .186 ships no pnputil.exe, the agent has no
 * device-disable command, and a display adapter stuck in code 43 keeps the
 * output dead - so the only documented way back to a picture was a reboot.
 * This is the devcon-equivalent (SetupDi + DIF_PROPERTYCHANGE) so a devnode
 * can be cycled live. The Win9x boxes already have devctl9x; this is the NT
 * half.
 *
 * Build:  i686-w64-mingw32-gcc -O2 -Wall -s -o devctl.exe devctl.c -lsetupapi
 *
 *   devctl list [<substring>]        list devices, with status/problem
 *   devctl disable <hwid-substring>
 *   devctl enable  <hwid-substring>
 *   devctl restart <hwid-substring>  disable then enable
 *
 * It prints the POST-CONDITION (the device's status and problem code read
 * back afterwards), never just "OK" - a PropertyChange that returns success
 * and leaves the device untouched is exactly the failure this project keeps
 * paying for.
 */
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <string.h>

static int icontains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle), i;
    if (!nl) return 1;
    for (i = 0; hay[i]; i++) {
        size_t j = 0;
        while (j < nl && hay[i + j] &&
               toupper((unsigned char)hay[i + j]) == toupper((unsigned char)needle[j]))
            j++;
        if (j == nl) return 1;
    }
    return 0;
}

static void show(HDEVINFO h, SP_DEVINFO_DATA *d, const char *tag)
{
    char desc[512] = "", id[512] = "";
    ULONG status = 0, problem = 0;
    CONFIGRET cr;

    SetupDiGetDeviceRegistryPropertyA(h, d, SPDRP_DEVICEDESC, NULL,
                                      (PBYTE)desc, sizeof(desc), NULL);
    SetupDiGetDeviceInstanceIdA(h, d, id, sizeof(id), NULL);
    cr = CM_Get_DevNode_Status(&status, &problem, d->DevInst, 0);
    if (cr == CR_SUCCESS)
        printf("%s%-40.40s problem=%lu%s  [%s]\n", tag, desc, problem,
               (status & DN_HAS_PROBLEM) ? " HAS_PROBLEM" : "", id);
    else
        printf("%s%-40.40s status unavailable (cr=%lu)  [%s]\n",
               tag, desc, (unsigned long)cr, id);
}

static int change(HDEVINFO h, SP_DEVINFO_DATA *d, DWORD state)
{
    SP_PROPCHANGE_PARAMS p;
    ZeroMemory(&p, sizeof(p));
    p.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    p.StateChange = state;
    p.Scope = DICS_FLAG_CONFIGSPECIFIC;   /* this hardware profile */
    p.HwProfile = 0;
    if (!SetupDiSetClassInstallParamsA(h, d, (SP_CLASSINSTALL_HEADER *)&p,
                                       sizeof(p)))
        return (int)GetLastError();
    if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, h, d))
        return (int)GetLastError();
    return 0;
}

int main(int argc, char **argv)
{
    const char *op = (argc > 1) ? argv[1] : "list";
    const char *match = (argc > 2) ? argv[2] : "";
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i;
    int hits = 0, rc = 0;

    h = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed: %lu\n", GetLastError());
        return 2;
    }
    d.cbSize = sizeof(d);
    for (i = 0; SetupDiEnumDeviceInfo(h, i, &d); i++) {
        char id[512] = "", hw[2048] = "";
        SetupDiGetDeviceInstanceIdA(h, &d, id, sizeof(id), NULL);
        SetupDiGetDeviceRegistryPropertyA(h, &d, SPDRP_HARDWAREID, NULL,
                                          (PBYTE)hw, sizeof(hw), NULL);
        if (!icontains(id, match) && !icontains(hw, match)) continue;
        hits++;

        if (!strcmp(op, "list")) { show(h, &d, "  "); continue; }

        show(h, &d, "before: ");
        if (!strcmp(op, "disable") || !strcmp(op, "restart")) {
            int e = change(h, &d, DICS_DISABLE);
            printf("  disable -> %s\n", e ? "FAILED" : "ok");
            if (e) { printf("  error %d\n", e); rc = 3; }
            Sleep(1500);
        }
        if (!strcmp(op, "enable") || !strcmp(op, "restart")) {
            int e = change(h, &d, DICS_ENABLE);
            printf("  enable  -> %s\n", e ? "FAILED" : "ok");
            if (e) { printf("  error %d\n", e); rc = 3; }
            Sleep(1500);
        }
        show(h, &d, "after:  ");
    }
    SetupDiDestroyDeviceInfoList(h);
    if (!hits) { printf("no device matched '%s'\n", match); return 1; }
    return rc;
}

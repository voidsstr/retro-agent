/* regmode - print the live and persisted display modes; with "restore",
 * ChangeDisplaySettingsA(NULL, 0): return to the mode stored in the registry.
 * Never writes the registry. */
#include <windows.h>
#include <stdio.h>
#ifndef ENUM_REGISTRY_SETTINGS
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#endif
static void show(const char *tag) {
    DEVMODEA c, r; memset(&c,0,sizeof c); memset(&r,0,sizeof r); c.dmSize=r.dmSize=sizeof(DEVMODEA);
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &c);
    EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &r);
    printf("%s live %lux%lux%lu@%lu  persisted %lux%lux%lu@%lu\n", tag,
      c.dmPelsWidth, c.dmPelsHeight, c.dmBitsPerPel, c.dmDisplayFrequency,
      r.dmPelsWidth, r.dmPelsHeight, r.dmBitsPerPel, r.dmDisplayFrequency);
}
int main(int argc, char **argv) {
    show("before");
    if (argc > 1 && !strcmp(argv[1], "restore")) {
        LONG rc = ChangeDisplaySettingsA(NULL, 0);
        printf("ChangeDisplaySettingsA(NULL,0) -> %ld\n", rc);
        Sleep(1500);
        show("after ");
    }
    return 0;
}

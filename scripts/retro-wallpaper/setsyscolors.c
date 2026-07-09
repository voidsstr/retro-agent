/*
 * setsyscolors.exe - read HKCU\Control Panel\Colors and apply them LIVE via
 * SetSysColors, then broadcast WM_SYSCOLORCHANGE so running apps repaint.
 *
 * Writing the registry alone only takes effect on next logon; XP has no built-in
 * CLI to push classic colors live. This does it. Pair with a registry write (so
 * the scheme also persists) and stopping the Themes service (classic mode).
 *
 * Cross-build:  i686-w64-mingw32-gcc -O2 -o setsyscolors.exe setsyscolors.c \
 *                   -luser32 -lgdi32 -ladvapi32
 */
#include <windows.h>
#include <stdio.h>

static COLORREF parse_rgb(const char *s) {
    int r = 0, g = 0, b = 0;
    sscanf(s, "%d %d %d", &r, &g, &b);
    return RGB(r, g, b);
}

int main(void) {
    struct { const char *name; int idx; } map[] = {
        {"Scrollbar", COLOR_SCROLLBAR},
        {"Background", COLOR_BACKGROUND},
        {"ActiveTitle", COLOR_ACTIVECAPTION},
        {"InactiveTitle", COLOR_INACTIVECAPTION},
        {"Menu", COLOR_MENU},
        {"Window", COLOR_WINDOW},
        {"WindowFrame", COLOR_WINDOWFRAME},
        {"MenuText", COLOR_MENUTEXT},
        {"WindowText", COLOR_WINDOWTEXT},
        {"TitleText", COLOR_CAPTIONTEXT},
        {"ActiveBorder", COLOR_ACTIVEBORDER},
        {"InactiveBorder", COLOR_INACTIVEBORDER},
        {"AppWorkspace", COLOR_APPWORKSPACE},
        {"Hilight", COLOR_HIGHLIGHT},
        {"HilightText", COLOR_HIGHLIGHTTEXT},
        {"ButtonFace", COLOR_BTNFACE},
        {"ButtonShadow", COLOR_BTNSHADOW},
        {"GrayText", COLOR_GRAYTEXT},
        {"ButtonText", COLOR_BTNTEXT},
        {"InactiveTitleText", COLOR_INACTIVECAPTIONTEXT},
        {"ButtonHilight", COLOR_BTNHIGHLIGHT},
        {"ButtonDkShadow", COLOR_3DDKSHADOW},
        {"ButtonLight", COLOR_3DLIGHT},
        {"InfoText", COLOR_INFOTEXT},
        {"InfoWindow", COLOR_INFOBK},
        {"HotTrackingColor", COLOR_HOTLIGHT},
        {"GradientActiveTitle", COLOR_GRADIENTACTIVECAPTION},
        {"GradientInactiveTitle", COLOR_GRADIENTINACTIVECAPTION},
        {"MenuHilight", COLOR_MENUHILIGHT},
        {"MenuBar", COLOR_MENUBAR},
    };
    int n = (int)(sizeof(map) / sizeof(map[0]));
    int idx[64];
    COLORREF col[64];
    int c = 0;

    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Control Panel\\Colors", 0,
                      KEY_READ, &k) != ERROR_SUCCESS) {
        printf("cannot open Control Panel\\Colors\n");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        char buf[64];
        DWORD sz = sizeof(buf), type = 0;
        if (RegQueryValueExA(k, map[i].name, NULL, &type,
                             (BYTE *)buf, &sz) == ERROR_SUCCESS) {
            buf[sz < sizeof(buf) ? sz : sizeof(buf) - 1] = 0;
            idx[c] = map[i].idx;
            col[c] = parse_rgb(buf);
            c++;
        }
    }
    RegCloseKey(k);

    if (!SetSysColors(c, idx, col)) {
        printf("SetSysColors failed: %lu\n", GetLastError());
        return 2;
    }
    SendMessageTimeoutA(HWND_BROADCAST, WM_SYSCOLORCHANGE, 0, 0,
                        SMTO_ABORTIFHUNG, 2000, NULL);
    printf("applied %d system colors\n", c);
    return 0;
}

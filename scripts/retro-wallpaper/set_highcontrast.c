/* set_highcontrast.exe - toggle Windows High Contrast on/off (per-user, no admin).
 * High Contrast forces classic (non-visual-style) rendering everywhere, INCLUDING
 * the Win7/Vista Explorer folder view, so the HKCU\Control Panel\Colors scheme
 * (our green-on-black) actually paints the file-list background black. This is the
 * only unelevated way to make Explorer black on an Aero box (stopping the Themes
 * service needs admin).
 *   set_highcontrast.exe        -> ON  (uses the active HKCU color scheme)
 *   set_highcontrast.exe off    -> OFF
 * Cross-build: i686-w64-mingw32-gcc -O2 -o set_highcontrast.exe set_highcontrast.c -luser32
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    HIGHCONTRASTA hc;
    int on = !(argc > 1 && _stricmp(argv[1], "off") == 0);
    memset(&hc, 0, sizeof(hc));
    hc.cbSize = sizeof(hc);
    hc.dwFlags = on ? HCF_HIGHCONTRASTON : 0;
    /* NULL scheme = keep the CURRENT HKCU\Control Panel\Colors (our green-on-black)
     * instead of loading a built-in white-on-black scheme. */
    hc.lpszDefaultScheme = NULL;
    if (SystemParametersInfoA(SPI_SETHIGHCONTRAST, sizeof(hc), &hc,
                              SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE)) {
        printf("high contrast %s\n", on ? "ON" : "OFF");
        return 0;
    }
    printf("SPI_SETHIGHCONTRAST failed (%lu)\n", (unsigned long)GetLastError());
    return 1;
}

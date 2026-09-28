/* gammaprobe.c - does SetDeviceGammaRamp reach the display driver?
 * (vcr-kmd, 2026-09-28: Jedi Academy logged "SetDeviceGammaRamp failed."
 * on .124 and the driver's flight recorder saw no call.) Tries, on the screen
 * DC and on a window DC: Get, identity, a gamma-1.3 ramp, an id Tech 3
 * overbright ramp (doubled, clamped); prints each result + GetLastError, and
 * restores what Get returned. One JSON line. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static WORD saved[3][256], ramp[3][256];

static void fill(double gamma, int doubled)
{
    int i, c;
    for (i = 0; i < 256; i++) {
        double v = pow(i / 255.0, 1.0 / gamma) * 255.0;
        if (doubled)
            v = v * 2;
        if (v > 255)
            v = 255;
        for (c = 0; c < 3; c++)
            ramp[c][i] = (WORD)((int)(v + 0.5) << 8 | (int)(v + 0.5));
    }
}

static void try_dc(HDC dc, const char *name)
{
    BOOL g, s1, s2, s3, r;
    DWORD e1, e2, e3;
    g = GetDeviceGammaRamp(dc, saved);
    fill(1.0, 0); SetLastError(0); s1 = SetDeviceGammaRamp(dc, ramp); e1 = GetLastError();
    fill(1.3, 0); SetLastError(0); s2 = SetDeviceGammaRamp(dc, ramp); e2 = GetLastError();
    fill(1.0, 1); SetLastError(0); s3 = SetDeviceGammaRamp(dc, ramp); e3 = GetLastError();
    r = g ? SetDeviceGammaRamp(dc, saved) : 0;
    printf("{\"dc\":\"%s\",\"get\":%d,\"saved128\":%u,\"identity\":%d,\"err1\":%lu,"
           "\"g13\":%d,\"err2\":%lu,\"overbright\":%d,\"err3\":%lu,\"restore\":%d,"
           "\"bpp\":%d}\n", name, g, (unsigned)saved[0][128], s1, e1, s2, e2, s3, e3, r,
           GetDeviceCaps(dc, BITSPIXEL));
}

int main(int argc, char **argv)
{
    HDC dc = GetDC(NULL);
    HWND w;
    if (argc > 1 && !strcmp(argv[1], "identity")) {
        /* put the screen back to a straight ramp, and say what Get reads */
        BOOL s;
        fill(1.0, 0);
        s = SetDeviceGammaRamp(dc, ramp);
        GetDeviceGammaRamp(dc, saved);
        printf("{\"identity\":%d,\"err\":%lu,\"now128\":%u}\n", s, GetLastError(),
               (unsigned)saved[0][128]);
        ReleaseDC(NULL, dc);
        return s ? 0 : 1;
    }
    try_dc(dc, "screen");
    ReleaseDC(NULL, dc);
    w = CreateWindowA("STATIC", "gammaprobe", WS_POPUP | WS_VISIBLE, 0, 0, 64, 64,
                      NULL, NULL, GetModuleHandleA(NULL), NULL);
    dc = GetDC(w);
    try_dc(dc, "window");
    ReleaseDC(w, dc);
    DestroyWindow(w);
    return 0;
}

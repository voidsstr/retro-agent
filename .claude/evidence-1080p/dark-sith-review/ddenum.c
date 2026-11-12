/* ddenum - replicate Jedi Knight / MotS stdDisplay mode enumeration
 * (JK.EXE 0x425b60: SetCooperativeLevel 0x51 + EnumDisplayModes(0) keeping only
 *  320-wide modes [ModeX pass], then SetCooperativeLevel 0x11 + EnumDisplayModes(0)
 *  keeping every mode; one shared counter, hard cap 64).
 * Default: NO cooperative level at all (non-intrusive enumeration only).
 * Arg "x": replicate the exclusive-mode sequence with a hidden window.
 * Prints each stored mode as JK would index it. Read-only: never sets a mode. */
#include <windows.h>
#include <stdio.h>
#include <ddraw.h>

typedef struct { int modex; int w, h, bpp; DWORD pfflags; } mode_t;
static mode_t g_modes[256];
static int g_n = 0;       /* JK's shared counter (uncapped here, cap reported) */
static int g_pass = 0;

static HRESULT WINAPI cb(LPDDSURFACEDESC d, LPVOID ctx)
{
    int bpp = 0;
    if (d->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8) bpp = 8;
    else if (d->ddpfPixelFormat.dwFlags & DDPF_RGB) bpp = (int)d->ddpfPixelFormat.dwRGBBitCount;
    if (g_pass == 1 && d->dwWidth != 320) return DDENUMRET_OK;
    if (g_n < 256) {
        g_modes[g_n].modex = (g_pass == 1);
        g_modes[g_n].w = (int)d->dwWidth;
        g_modes[g_n].h = (int)d->dwHeight;
        g_modes[g_n].bpp = bpp;
        g_modes[g_n].pfflags = d->ddpfPixelFormat.dwFlags;
    }
    g_n++;
    return DDENUMRET_OK;
}

int main(int argc, char **argv)
{
    LPDIRECTDRAW dd = NULL;
    HRESULT hr;
    HWND hwnd = NULL;
    int excl = (argc > 1 && argv[1][0] == 'x');
    int i, pos1080 = -1, n16 = 0, listed16 = 0;

    hr = DirectDrawCreate(NULL, &dd, NULL);
    if (FAILED(hr) || !dd) { printf("DirectDrawCreate failed 0x%08lx\n", (unsigned long)hr); return 1; }
    if (excl) {
        WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "ddenum_probe";
        RegisterClassA(&wc);
        hwnd = CreateWindowA("ddenum_probe", "ddenum", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
        hr = IDirectDraw_SetCooperativeLevel(dd, hwnd, 0x51);
        printf("coop 0x51 -> 0x%08lx\n", (unsigned long)hr);
    }
    g_pass = 1;
    hr = IDirectDraw_EnumDisplayModes(dd, 0, NULL, NULL, cb);
    printf("pass1 enum -> 0x%08lx, stored so far %d\n", (unsigned long)hr, g_n);
    if (excl) {
        hr = IDirectDraw_SetCooperativeLevel(dd, hwnd, 0x11);
        printf("coop 0x11 -> 0x%08lx\n", (unsigned long)hr);
    }
    g_pass = 2;
    hr = IDirectDraw_EnumDisplayModes(dd, 0, NULL, NULL, cb);
    printf("pass2 enum -> 0x%08lx, total %d (JK keeps the first 64)\n", (unsigned long)hr, g_n);
    if (excl) {
        hr = IDirectDraw_SetCooperativeLevel(dd, hwnd, 8);
        printf("coop 0x08 -> 0x%08lx\n", (unsigned long)hr);
    }
    for (i = 0; i < g_n && i < 256; i++) {
        mode_t *m = &g_modes[i];
        int kept = i < 64;
        printf("%3d %s %4dx%-4d %2dbpp pf=0x%04lx%s\n", i, m->modex ? "X" : " ", m->w, m->h, m->bpp,
               (unsigned long)m->pfflags, kept ? "" : "  <-- DROPPED by JK's 64 cap");
        if (m->bpp == 16) { n16++; if (kept) listed16++; }
        if (m->w == 1920 && m->h == 1080 && m->bpp == 16 && !m->modex && pos1080 < 0) pos1080 = i;
    }
    printf("SUMMARY total=%d 16bpp_total=%d 16bpp_listed_with_3daccel=%d 1920x1080x16_index=%d %s\n",
           g_n, n16, listed16, pos1080,
           pos1080 < 0 ? "NOT ENUMERATED" : (pos1080 < 64 ? "LISTED" : "CUT BY 64 CAP"));
    IDirectDraw_Release(dd);
    if (hwnd) DestroyWindow(hwnd);
    return 0;
}

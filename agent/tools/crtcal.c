/*
 * crtcal.c - CRT calibration toolkit for the retro fleet (Win98/2K/XP)
 *
 * Two things you CAN automate/assist for CRT calibration without a colorimeter:
 *
 *   1) Full-screen TEST PATTERNS for eyeball adjustment of the monitor's OSD:
 *        - crosshatch grid   : geometry (pincushion, trapezoid, linearity) + convergence
 *        - circle+border     : geometry roundness / edge straightness / overscan
 *        - grayscale ramp    : brightness/contrast/gamma tracking
 *        - pluge             : black-level (brightness) set point
 *        - solid R/G/B/W/K   : purity, convergence per gun, white uniformity, degauss check
 *        - SMPTE-ish bars    : color
 *        - gamma match       : 1px b/w lines vs 50% gray patch -> dial gamma to match
 *
 *   2) SOFTWARE color correction via SetDeviceGammaRamp (the video card LUT):
 *        live gamma / brightness / contrast, per the on-screen readout. Works on
 *        ANY XP GPU (NVIDIA/ATI/Intel) with no monitor DDC/CI needed. Persists
 *        after exit until changed or a mode switch (reset with R before quitting
 *        if you don't want to keep it).
 *
 * Controls:
 *   SPACE / Right = next pattern     Left = previous     1..9,0 = pick pattern
 *   Up/Down = gamma   +/- = brightness   [ ] = contrast   R = reset color
 *   G = toggle on-screen readout       ESC / Q = quit
 *
 * Remote/agent use: `crtcal.exe crosshatch` starts on a named pattern; the agent
 * can LAUNCH it, UIKEY to cycle, and SCREENSHOT the (ideal) pattern. Geometry/
 * convergence errors only show on the physical CRT, so a human relays those.
 *
 * Build:
 *   i686-w64-mingw32-gcc -O2 -s -o crtcal.exe crtcal.c -lgdi32 -luser32 -mwindows \
 *     -DWINVER=0x0410 -D_WIN32_WINNT=0x0410 -march=i586 -mtune=pentium3
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { P_GRAY, P_CROSS, P_CIRCLE, P_PLUGE, P_BARS, P_GAMMA,
       P_WHITE, P_RED, P_GREEN, P_BLUE, P_BLACK, P_COUNT };
static const char *PNAMES[P_COUNT] = {
    "grayscale","crosshatch","circle","pluge","colorbars","gamma",
    "white","red","green","blue","black" };

static int    g_pat      = P_CROSS;
static double g_gamma    = 1.0;   /* 0.3 .. 3.0 */
static double g_bright   = 0.0;   /* -0.5 .. 0.5 */
static double g_contrast = 1.0;   /* 0.3 .. 2.0  */
static int    g_readout  = 1;
static int    g_w = 640, g_h = 480;

/* ---- software color via the video card LUT ---- */
static void apply_color(void)
{
    HDC dc = GetDC(NULL);
    WORD ramp[3][256];
    int i, c;
    for (i = 0; i < 256; i++) {
        double v = i / 255.0;
        v = (v - 0.5) * g_contrast + 0.5 + g_bright;   /* contrast about mid, then bias */
        if (v < 0) v = 0; if (v > 1) v = 1;
        v = pow(v, 1.0 / g_gamma);
        WORD w = (WORD)(v * 65535.0 + 0.5);
        for (c = 0; c < 3; c++) ramp[c][i] = w;
    }
    SetDeviceGammaRamp(dc, ramp);
    ReleaseDC(NULL, dc);
}
static void reset_color(void)
{
    g_gamma = 1.0; g_bright = 0.0; g_contrast = 1.0; apply_color();
}

/* ---- pattern drawing ---- */
static void fill(HDC dc, int x, int y, int w, int h, COLORREF col)
{
    RECT r = { x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(col);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void draw_grayscale(HDC dc)
{
    int steps = 16, i;
    int bw = g_w / steps;
    for (i = 0; i < steps; i++) {
        int g = (int)(255.0 * i / (steps - 1));
        fill(dc, i * bw, 0, bw + 1, g_h, RGB(g, g, g));
    }
}
static void draw_crosshatch(HDC dc)
{
    int step = g_h / 12, x, y;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255,255,255)), old;
    fill(dc, 0, 0, g_w, g_h, RGB(0,0,0));
    old = SelectObject(dc, pen);
    for (x = 0; x <= g_w; x += step) { MoveToEx(dc,x,0,NULL); LineTo(dc,x,g_h); }
    for (y = 0; y <= g_h; y += step) { MoveToEx(dc,0,y,NULL); LineTo(dc,g_w,y); }
    /* corner brackets to judge overscan/edges */
    SelectObject(dc, old); DeleteObject(pen);
}
static void draw_circle(HDC dc)
{
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255,255,255)), old;
    int d = (g_h < g_w ? g_h : g_w) - 4;
    int cx = g_w/2, cy = g_h/2;
    fill(dc, 0, 0, g_w, g_h, RGB(0,0,0));
    old = SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx-d/2, cy-d/2, cx+d/2, cy+d/2);         /* should look round, not egg */
    Rectangle(dc, 2, 2, g_w-2, g_h-2);                    /* edges straight, no bow */
    MoveToEx(dc,cx,0,NULL); LineTo(dc,cx,g_h);
    MoveToEx(dc,0,cy,NULL); LineTo(dc,g_w,cy);
    SelectObject(dc, old); DeleteObject(pen);
}
static void draw_pluge(HDC dc)
{
    /* black field; bars just below/above black + a white ref. Raise BRIGHTNESS
       until the +bars are just visible and the -bars vanish. */
    int bw = g_w/8, y = g_h/4, hh = g_h/2;
    fill(dc, 0, 0, g_w, g_h, RGB(0,0,0));
    fill(dc, bw*2, y, bw, hh, RGB(2,2,2));      /* blacker-than-black (should stay hidden) */
    fill(dc, bw*3, y, bw, hh, RGB(16,16,16));   /* just-above-black (should be faintly visible) */
    fill(dc, bw*5, y, bw, hh, RGB(235,235,235));/* white reference */
}
static void draw_bars(HDC dc)
{
    COLORREF c[8] = { RGB(255,255,255),RGB(255,255,0),RGB(0,255,255),RGB(0,255,0),
                      RGB(255,0,255),RGB(255,0,0),RGB(0,0,255),RGB(0,0,0) };
    int bw = g_w/8, i;
    for (i = 0; i < 8; i++) fill(dc, i*bw, 0, bw+1, g_h, c[i]);
}
static void draw_gamma(HDC dc)
{
    /* left: 1px black/white lines -> perceived ~50%. right: solid mid patches at
       several levels. Adjust GAMMA (Up/Down) until the solid matches the lines. */
    int x, half = g_w/2;
    for (x = 0; x < half; x++)
        fill(dc, x, 0, 1, g_h, (x & 1) ? RGB(255,255,255) : RGB(0,0,0));
    fill(dc, half, 0, g_w-half, g_h, RGB(188,188,188));  /* ~50% in gamma 2.2 space */
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    /* draw into a memory DC then blit — avoids flicker on slow CRT GPUs */
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, g_w, g_h);
    HBITMAP oldb = SelectObject(dc, bmp);

    switch (g_pat) {
    case P_GRAY:   draw_grayscale(dc); break;
    case P_CROSS:  draw_crosshatch(dc); break;
    case P_CIRCLE: draw_circle(dc); break;
    case P_PLUGE:  draw_pluge(dc); break;
    case P_BARS:   draw_bars(dc); break;
    case P_GAMMA:  draw_gamma(dc); break;
    case P_WHITE:  fill(dc,0,0,g_w,g_h,RGB(255,255,255)); break;
    case P_RED:    fill(dc,0,0,g_w,g_h,RGB(255,0,0)); break;
    case P_GREEN:  fill(dc,0,0,g_w,g_h,RGB(0,255,0)); break;
    case P_BLUE:   fill(dc,0,0,g_w,g_h,RGB(0,0,255)); break;
    case P_BLACK:  fill(dc,0,0,g_w,g_h,RGB(0,0,0)); break;
    }

    if (g_readout) {
        char buf[160];
        int solids = (g_pat >= P_WHITE);
        COLORREF fg = solids ? RGB(0,0,0) : RGB(0,255,0);
        _snprintf(buf, sizeof(buf),
                  " %s  [%d/%d]   gamma %.2f  bright %+.2f  contrast %.2f ",
                  PNAMES[g_pat], g_pat+1, P_COUNT, g_gamma, g_bright, g_contrast);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, solids ? RGB(255,255,255) : RGB(0,0,0));
        SetTextColor(dc, fg);
        TextOutA(dc, 8, 8, buf, (int)strlen(buf));
        TextOutA(dc, 8, 26,
                 " SPACE/<-> pattern  Up/Dn gamma  +/- bright  [ ] contrast  R reset  ESC quit ",
                 79);
    }

    BitBlt(wdc, 0, 0, g_w, g_h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldb);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: paint(h); return 0;
    case WM_KEYDOWN: {
        int redraw = 1, color = 0;
        switch (w) {
        case VK_ESCAPE: case 'Q': PostQuitMessage(0); return 0;
        case VK_SPACE: case VK_RIGHT: g_pat=(g_pat+1)%P_COUNT; break;
        case VK_LEFT: g_pat=(g_pat+P_COUNT-1)%P_COUNT; break;
        case VK_UP:   g_gamma+=0.05; if(g_gamma>3)g_gamma=3; color=1; break;
        case VK_DOWN: g_gamma-=0.05; if(g_gamma<0.3)g_gamma=0.3; color=1; break;
        case VK_OEM_PLUS: case VK_ADD:  g_bright+=0.02; if(g_bright>0.5)g_bright=0.5; color=1; break;
        case VK_OEM_MINUS: case VK_SUBTRACT: g_bright-=0.02; if(g_bright<-0.5)g_bright=-0.5; color=1; break;
        case VK_OEM_4: g_contrast-=0.05; if(g_contrast<0.3)g_contrast=0.3; color=1; break; /* [ */
        case VK_OEM_6: g_contrast+=0.05; if(g_contrast>2.0)g_contrast=2.0; color=1; break; /* ] */
        case 'R': reset_color(); break;
        case 'G': g_readout=!g_readout; break;
        default:
            if (w>='1'&&w<='9') { int p=w-'1'; if(p<P_COUNT) g_pat=p; }
            else if (w=='0') { if(P_COUNT>9) g_pat=9; }
            else redraw=0;
        }
        if (color) apply_color();
        if (redraw) InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    WNDCLASSA wc; HWND hwnd; MSG msg; int i;
    (void)hPrev; (void)show;

    /* Headless automation modes: set/reset the video-card LUT and exit with NO
     * window, so the agent can push software color correction fleet-wide:
     *   crtcal.exe setgamma <gamma> [bright] [contrast]
     *   crtcal.exe resetgamma
     */
    if (cmd && _strnicmp(cmd, "setgamma", 8) == 0) {
        double a = 1.0, b = 0.0, cc = 1.0;
        sscanf(cmd + 8, "%lf %lf %lf", &a, &b, &cc);
        g_gamma = a; g_bright = b; g_contrast = cc;
        apply_color();
        return 0;
    }
    if (cmd && _stricmp(cmd, "resetgamma") == 0) { reset_color(); return 0; }

    if (cmd && *cmd) {
        char a[64]; _snprintf(a,sizeof(a),"%s",cmd);
        for (i=0;i<P_COUNT;i++) if (_stricmp(a,PNAMES[i])==0) { g_pat=i; break; }
    }

    g_w = GetSystemMetrics(SM_CXSCREEN);
    g_h = GetSystemMetrics(SM_CYSCREEN);

    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "crtcal";
    RegisterClassA(&wc);

    hwnd = CreateWindowExA(WS_EX_TOPMOST, "crtcal", "crtcal",
                           WS_POPUP, 0, 0, g_w, g_h,
                           NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    ShowCursor(FALSE);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    ShowCursor(TRUE);
    return 0;
}

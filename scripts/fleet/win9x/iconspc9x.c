/* iconspc9x - read or set the desktop icon grid (ICONMETRICS spacing) on a
 * Win9x box, live and persisted (SPIF_UPDATEINIFILE | SPIF_SENDCHANGE).
 *
 *   iconspc9x            report only
 *   iconspc9x <h> <v>    set the grid cell to h x v pixels, then read it back
 *
 * Why (2026-10-01, .243): its desktop went to 800x600x16 (the 1 MB Cirrus
 * cannot hold 1024x768x16), and at Win98's default 75x75 grid an 800x572 work
 * area shows about 10 x 7 = 70 icons - .243 has 94, so two dozen game
 * shortcuts sat off the right edge of the screen, unreachable. Auto Arrange
 * repacks into whatever grid this sets. Log: C:\RETRO_AGENT\ICONSPC.TXT */
#include <windows.h>

static HANDLE out;
static char line[256];

static void w(const char *s)
{
    DWORD n;
    WriteFile(out, s, lstrlenA(s), &n, NULL);
    WriteFile(out, "\r\n", 2, &n, NULL);
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* the program token, quoted or not */
static const char *skip_prog(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"')
            p++;
        if (*p == '"')
            p++;
    } else {
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return skip_ws(p);
}

static int read_int(const char **pp, int *v)
{
    const char *p = skip_ws(*pp);
    int n = 0, any = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        p++;
        any = 1;
    }
    *pp = p;
    *v = n;
    return any;
}

static int get(ICONMETRICSA *im)
{
    ZeroMemory(im, sizeof(*im));
    im->cbSize = sizeof(*im);
    return SystemParametersInfoA(SPI_GETICONMETRICS, sizeof(*im), im, 0) != 0;
}

void __stdcall start(void)
{
    ICONMETRICSA im, back;
    const char *p;
    int h = 0, v = 0, rc = 0;
    RECT wa;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    out = CreateFileA("C:\\RETRO_AGENT\\ICONSPC.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (!get(&im)) {
        w("FAIL: SPI_GETICONMETRICS");
        CloseHandle(out);
        ExitProcess(2);
    }
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    wsprintfA(line, "before: grid %dx%d, title wrap %d, work area %dx%d, screen %dx%d",
              im.iHorzSpacing, im.iVertSpacing, im.iTitleWrap,
              (int)(wa.right - wa.left), (int)(wa.bottom - wa.top),
              GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    w(line);

    p = skip_prog(GetCommandLineA());
    if (*p) {
        if (!read_int(&p, &h) || !read_int(&p, &v) || h < 40 || h > 150 || v < 40 || v > 150) {
            w("FAIL: usage iconspc9x [<h> <v>], each 40..150 pixels");
            CloseHandle(out);
            ExitProcess(3);
        }
        im.iHorzSpacing = h;
        im.iVertSpacing = v;
        if (!SystemParametersInfoA(SPI_SETICONMETRICS, sizeof(im), &im,
                                   SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
            w("FAIL: SPI_SETICONMETRICS refused");
            rc = 4;
        }
        /* the post-condition, not the return value */
        if (!get(&back) || back.iHorzSpacing != h || back.iVertSpacing != v) {
            wsprintfA(line, "FAIL: asked %dx%d, reads back %dx%d", h, v,
                      back.iHorzSpacing, back.iVertSpacing);
            w(line);
            rc = 5;
        } else {
            wsprintfA(line, "after: grid %dx%d - about %d x %d icons fit the work area",
                      back.iHorzSpacing, back.iVertSpacing,
                      (int)(wa.right - wa.left) / back.iHorzSpacing,
                      (int)(wa.bottom - wa.top) / back.iVertSpacing);
            w(line);
        }
    }
    CloseHandle(out);
    ExitProcess(rc);
}

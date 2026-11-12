/* ddmprobe - read-only mode-list probe for the UE1/UE2 resolution-menu review.
 * Enumerates display modes exactly the way the staged renderers do and prints
 * what each GetRes would return. Changes NOTHING: no SetCooperativeLevel, no
 * device creation, no ChangeDisplaySettings. */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ddraw.h>

typedef HRESULT (WINAPI *PFN_DDCEX)(GUID*, LPVOID*, REFIID, IUnknown*);

#define MAXM 1024
typedef struct { int w, h, bpp, hz; } MODE;
static MODE dd[MAXM]; static int ndd;

static HRESULT WINAPI ddcb(LPDDSURFACEDESC2 d, LPVOID ctx) {
    (void)ctx;
    if (ndd < MAXM) {
        dd[ndd].w = (int)d->dwWidth; dd[ndd].h = (int)d->dwHeight;
        dd[ndd].bpp = (int)d->ddpfPixelFormat.dwRGBBitCount; dd[ndd].hz = (int)d->dwRefreshRate;
        ndd++;
    }
    return DDENUMRET_OK;
}

/* UE1 226/DeusEx D3DDrv GetRes: unique 16-bpp WxH in DD order, first `cap` */
static int ue1_d3d_list(int cap, MODE *out) {
    int n = 0, i, j;
    MODE rel[MAXM]; int nrel = 0;
    for (i = 0; i < ndd; i++) {
        if (dd[i].bpp != 16) continue;
        for (j = 0; j < nrel; j++) if (rel[j].w == dd[i].w && rel[j].h == dd[i].h) break;
        if (j == nrel) { rel[nrel] = dd[i]; nrel++; }
    }
    for (i = 0; i < nrel && i < cap; i++) out[n++] = rel[i];
    return n;
}
static int cmpres(const void *a, const void *b) {
    const MODE *x = a, *y = b;
    if (x->w != y->w) return x->w - y->w;
    return x->h - y->h;
}

int main(void) {
    HMODULE h; PFN_DDCEX pcreate; LPDIRECTDRAW7 dd7 = NULL; HRESULT hr;
    DDSURFACEDESC2 flt; MODE out[MAXM]; int n, i, j;
    DEVMODEA dm; OSVERSIONINFOA ov;

    ov.dwOSVersionInfoSize = sizeof(ov); GetVersionExA(&ov);
    printf("os %lu.%lu build %lu\n", ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
    memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm))
        printf("current %lux%lux%lu@%lu\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency);
    memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &dm))
        printf("registry %lux%lux%lu@%lu\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency);

    /* ---- DirectDraw 7, as the UG 226b / DeusEx 1112fm D3DDrv does ---- */
    h = LoadLibraryA("ddraw.dll");
    pcreate = h ? (PFN_DDCEX)GetProcAddress(h, "DirectDrawCreateEx") : NULL;
    if (!pcreate) { printf("ddraw: no DirectDrawCreateEx\n"); }
    else {
        hr = pcreate(NULL, (LPVOID*)&dd7, &IID_IDirectDraw7, NULL);
        if (FAILED(hr) || !dd7) printf("ddraw: create failed 0x%08lx\n", (unsigned long)hr);
        else {
            memset(&flt, 0, sizeof(flt)); flt.dwSize = sizeof(flt);
            flt.dwFlags = DDSD_CAPS; flt.ddsCaps.dwCaps = DDSCAPS_3DDEVICE;
            ndd = 0;
            hr = IDirectDraw7_EnumDisplayModes(dd7, 0, &flt, NULL, ddcb);
            printf("dd7 EnumDisplayModes hr=0x%08lx n=%d\n", (unsigned long)hr, ndd);
            printf("dd7 order:");
            for (i = 0; i < ndd; i++) printf(" %dx%dx%d", dd[i].w, dd[i].h, dd[i].bpp);
            printf("\n");
            n = ue1_d3d_list(MAXM, out);
            printf("dd7 unique16 (n=%d):", n);
            for (i = 0; i < n; i++) printf(" %dx%d", out[i].w, out[i].h);
            printf("\n");
            for (i = 0; i < n; i++) if (out[i].w == 1920 && out[i].h == 1080) break;
            printf("dd7 unique16 index_of_1920x1080=%d (1-based, 0=absent)\n", i < n ? i + 1 : 0);
            n = ue1_d3d_list(16, out);
            printf("UE1_D3DDRV_GETRES (cap16):");
            for (i = 0; i < n; i++) printf(" %dx%d", out[i].w, out[i].h);
            printf("\n");
            /* Deus Ex menu: first 16 tokens, drop width < 640 */
            printf("DEUSEX_MENU (cap16,tok16,>=640):");
            for (i = 0; i < n; i++) if (out[i].w >= 640) printf(" %dx%d", out[i].w, out[i].h);
            printf("\n");
            n = ue1_d3d_list(0x7f, out);
            printf("DEUSEX_MENU (cap127,tok40,>=640):");
            for (i = 0, j = 0; i < n && j < 40; i++, j++) if (out[i].w >= 640) printf(" %dx%d", out[i].w, out[i].h);
            printf("\n");
            /* 32-bpp set for comparison */
            printf("dd7 unique32:");
            { MODE r32[MAXM]; int k = 0, a;
              for (i = 0; i < ndd; i++) { if (dd[i].bpp != 32) continue;
                for (a = 0; a < k; a++) if (r32[a].w == dd[i].w && r32[a].h == dd[i].h) break;
                if (a == k) { r32[k++] = dd[i]; printf(" %dx%d", dd[i].w, dd[i].h); } } }
            printf("\n");
            IDirectDraw7_Release(dd7);
        }
    }

    /* ---- EnumDisplaySettingsA(NULL,i), as UT436 OpenGLDrv::Init does ---- */
    {
        static MODE eds[4096]; int ne = 0, k, bppv[2] = {16, 32}, b;
        for (i = 0; ne < 4096; i++) {
            memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsA(NULL, i, &dm)) break;
            eds[ne].w = dm.dmPelsWidth; eds[ne].h = dm.dmPelsHeight; eds[ne].bpp = dm.dmBitsPerPel; eds[ne].hz = dm.dmDisplayFrequency; ne++;
        }
        printf("eds raw entries=%d\n", ne);
        for (b = 0; b < 2; b++) {
            MODE r[MAXM]; int nr = 0, a;
            for (k = 0; k < ne; k++) {
                if (eds[k].bpp != bppv[b]) continue;
                if ((eds[k].w == 320 && eds[k].h == 200) || (eds[k].w == 640 && eds[k].h == 400)) continue;
                for (a = 0; a < nr; a++) if (r[a].w == eds[k].w && r[a].h == eds[k].h) break;
                if (a == nr && nr < MAXM) r[nr++] = eds[k];
            }
            qsort(r, nr, sizeof(MODE), cmpres);
            printf("UT436_OGL_GETRES_%d (n=%d):", bppv[b], nr);
            for (a = 0; a < nr; a++) printf(" %dx%d", r[a].w, r[a].h);
            printf("\n");
        }
    }
    return 0;
}

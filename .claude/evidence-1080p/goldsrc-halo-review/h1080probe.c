/* h1080probe - read-only: prints what DirectDraw and Direct3D 9 enumerate on
 * this box, and replicates Halo 1.10's video-memory measurement
 * (halo.exe 0x57dae3..0x57dc44). Changes no display mode, writes nothing. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <ddraw.h>
#include <d3d9.h>

typedef struct { GUID g; int has; char desc[128]; char name[64]; } DEV;
static DEV devs[8]; static int ndev = 0;

static BOOL WINAPI enum_dev(GUID *g, LPSTR desc, LPSTR name, LPVOID ctx, HMONITOR hm)
{
    (void)ctx; (void)hm;
    if (ndev >= 8) return FALSE;
    devs[ndev].has = g != NULL;
    if (g) devs[ndev].g = *g;
    lstrcpynA(devs[ndev].desc, desc ? desc : "", 128);
    lstrcpynA(devs[ndev].name, name ? name : "", 64);
    ndev++;
    return TRUE;
}

typedef struct { int w, h, bpp; } MODE;
static MODE dm[256]; static int ndm = 0;

static HRESULT WINAPI enum_mode(LPDDSURFACEDESC2 sd, LPVOID ctx)
{
    (void)ctx;
    if (ndm < 256) {
        dm[ndm].w = sd->dwWidth; dm[ndm].h = sd->dwHeight;
        dm[ndm].bpp = sd->ddpfPixelFormat.dwRGBBitCount; ndm++;
    }
    return DDENUMRET_OK;
}

static unsigned halo_round(unsigned v)
{
    if (v <= 0x1000000u) return (v + 0x7fffffu) & 0xff800000u;
    if (v <= 0x4000000u) return (v + 0x1ffffffu) & 0xfe000000u;
    if (v > 0x80000000u) return 0x80000000u;
    return (v + 0x3ffffffu) & 0xfc000000u;
}

int main(void)
{
    int i, j;
    typedef HRESULT (WINAPI *PDDCEX)(GUID*, LPVOID*, REFIID, IUnknown*);
    typedef HRESULT (WINAPI *PDDENUMEX)(LPDDENUMCALLBACKEXA, LPVOID, DWORD);
    typedef IDirect3D9 *(WINAPI *PD3DC9)(UINT);
    HMODULE hdd = LoadLibraryA("ddraw.dll");
    HMODULE hd3 = LoadLibraryA("d3d9.dll");
    PDDCEX pcreate = hdd ? (PDDCEX)GetProcAddress(hdd, "DirectDrawCreateEx") : NULL;
    PDDENUMEX penum = hdd ? (PDDENUMEX)GetProcAddress(hdd, "DirectDrawEnumerateExA") : NULL;
    PD3DC9 pd3d = hd3 ? (PD3DC9)GetProcAddress(hd3, "Direct3DCreate9") : NULL;
    unsigned halo_vram = 0;

    printf("h1080probe 1\n");
    if (!pcreate || !penum) { printf("ddraw: unavailable\n"); }
    else {
        penum(enum_dev, NULL, DDENUM_ATTACHEDSECONDARYDEVICES);
        for (i = 0; i < ndev; i++) {
            IDirectDraw7 *dd = NULL;
            HRESULT hr = pcreate(devs[i].has ? &devs[i].g : NULL, (LPVOID*)&dd, &IID_IDirectDraw7, NULL);
            DWORD caps[4] = { 0x4200, 0x10007000, 0x10005000, 0x10004040 };
            unsigned mn = 0xffffffffu;
            printf("dd[%d] desc=\"%s\" name=\"%s\" create=0x%08lx\n", i, devs[i].desc, devs[i].name, (unsigned long)hr);
            if (FAILED(hr) || !dd) continue;
            IDirectDraw7_SetCooperativeLevel(dd, NULL, DDSCL_NORMAL);
            for (j = 0; j < 4; j++) {
                DDSCAPS2 c; DWORD tot = 0, fr = 0;
                ZeroMemory(&c, sizeof(c)); c.dwCaps = caps[j];
                hr = IDirectDraw7_GetAvailableVidMem(dd, &c, &tot, &fr);
                printf("  vidmem caps=0x%08lx hr=0x%08lx total=%lu free=%lu\n", caps[j], (unsigned long)hr, (unsigned long)tot, (unsigned long)fr);
                if (SUCCEEDED(hr) && tot > 0 && tot < 0xffffffffu && tot < mn) mn = tot;
            }
            printf("  halo_value=%u MB (min total %u)\n", halo_round(mn) >> 20, mn);
            if (i == 0) halo_vram = halo_round(mn);
            if (i == 0) {
                ndm = 0;
                IDirectDraw7_EnumDisplayModes(dd, 0, NULL, NULL, enum_mode);
                printf("  ddmodes n=%d:", ndm);
                for (j = 0; j < ndm; j++) printf(" %dx%dx%d", dm[j].w, dm[j].h, dm[j].bpp);
                printf("\n");
                /* WON hl.exe 1.1.0.8 callback 0x413a20 at its default 16 bpp */
                printf("  won16:");
                for (j = 0; j < ndm; j++) {
                    int w = dm[j].w, h = dm[j].h, b = dm[j].bpp;
                    if (!(b == 15 || b == 16)) continue;
                    if ((double)h / (double)w != 0.75) continue;
                    if (h > 1024 || w < 400) continue;
                    printf(" %dx%d", w, h);
                }
                printf("\n  gs4554_32:");
                for (j = 0; j < ndm; j++) {
                    int w = dm[j].w, h = dm[j].h, b = dm[j].bpp;
                    if (b != 32 || h < 480) continue;
                    if (!(w*9 == h*16 || w*3 == h*4 || w*4 == h*5 || w*5 == h*8)) continue;
                    printf(" %dx%d", w, h);
                }
                printf("\n");
            }
            IDirectDraw7_Release(dd);
        }
    }
    if (!pd3d) { printf("d3d9: unavailable\n"); return 0; }
    {
        IDirect3D9 *d3 = pd3d(D3D_SDK_VERSION);
        D3DADAPTER_IDENTIFIER9 id; D3DDISPLAYMODE cur;
        D3DFORMAT fmts[2] = { D3DFMT_X8R8G8B8, D3DFMT_R5G6B5 };
        static const int wl[10][2] = {{800,600},{1024,768},{1152,720},{1280,720},{1280,960},{1280,1024},{1600,1200},{1680,1050},{1920,1080},{1920,1200}};
        int f;
        if (!d3) { printf("d3d9: create failed\n"); return 0; }
        ZeroMemory(&id, sizeof(id));
        IDirect3D9_GetAdapterIdentifier(d3, 0, 0, &id);
        IDirect3D9_GetAdapterDisplayMode(d3, 0, &cur);
        printf("d3d9 adapters=%u dev=\"%s\" desc=\"%s\" ven=%04lx devid=%04lx desktop=%ux%u@%u fmt=%d\n",
               IDirect3D9_GetAdapterCount(d3), id.DeviceName, id.Description,
               (unsigned long)id.VendorId, (unsigned long)id.DeviceId, cur.Width, cur.Height, cur.RefreshRate, (int)cur.Format);
        for (f = 0; f < 2; f++) {
            UINT n = IDirect3D9_GetAdapterModeCount(d3, 0, fmts[f]), k;
            int has1080 = 0, maxhz1080 = 0;
            printf("d3d9 fmt=%d count=%u:", (int)fmts[f], n);
            for (k = 0; k < n; k++) {
                D3DDISPLAYMODE m;
                if (FAILED(IDirect3D9_EnumAdapterModes(d3, 0, fmts[f], k, &m))) continue;
                printf(" %ux%u@%u", m.Width, m.Height, m.RefreshRate);
                if (m.Width == 1920 && m.Height == 1080) { has1080 = 1; if ((int)m.RefreshRate > maxhz1080) maxhz1080 = m.RefreshRate; }
            }
            printf("\n  has1920x1080=%d maxhz=%d\n", has1080, maxhz1080);
            if (fmts[f] == D3DFMT_X8R8G8B8) {
                /* Halo 1.10 0x4baba0 fullscreen filter with the VRAM cap (-vidmode given) */
                printf("  halo1_list:");
                for (k = n; k-- > 0;) {
                    D3DDISPLAYMODE m; unsigned w, h;
                    if (FAILED(IDirect3D9_EnumAdapterModes(d3, 0, fmts[f], k, &m))) continue;
                    w = m.Width; h = m.Height;
                    if (halo_vram <= 0x2000000u && w > 1024) continue;
                    if (halo_vram <= 0x4000000u && w > 1280) continue;
                    if (halo_vram <= 0x8000000u && w > 1600) continue;
                    if (w > 0x1000 || w < 640 || h < 480 || w > 4800 || h > 3600 || m.RefreshRate > 120) continue;
                    if ((w == 720 || w == 848) && (h == 480 || h == 576)) continue;
                    printf(" %ux%u@%u", w, h, m.RefreshRate);
                }
                printf("\n  halo2_list:");
                for (j = 0; j < 10; j++) {
                    int seen = 0;
                    for (k = 0; k < n; k++) {
                        D3DDISPLAYMODE m;
                        if (FAILED(IDirect3D9_EnumAdapterModes(d3, 0, fmts[f], k, &m))) continue;
                        if ((int)m.Width == wl[j][0] && (int)m.Height == wl[j][1]) { seen = 1; break; }
                    }
                    if (seen) printf(" %dx%d", wl[j][0], wl[j][1]);
                }
                printf("\n");
            }
        }
        IDirect3D9_Release(d3);
    }
    return 0;
}

/*
 * glide-mac: 3dfx Voodoo GPU backend (M5) — binary/XNOR GEMM over the Glide3
 * fixed-function pipe. The Voodoo has no shaders; we use it as a massively
 * parallel XNOR-popcount array:
 *
 *   match-count[i,j] = sum_k XNOR(A[i,k], B[k,j])     (A,B in {0,1})
 *   signed dot       = 2*match - K                     (for {-1,+1} nets)
 *
 * Realization per k-step (2 draws):
 *   TMU0 <- column-texture pair, TMU1 <- row-texture pair, both A_8 format
 *   (values only 0x00/0xFF, so the TMU multiply A*B/255 is EXACT: 0 or 255)
 *   alpha test GEQUAL 0x80 gates surviving pixels
 *   color combine emits constant RGB(1,1,1); blend ONE:ONE accumulates +1
 *   draw 1: (A, B) textures     -> +1 where both bits are 1
 *   draw 2: (~A, ~B) textures   -> +1 where both bits are 0
 * After <=255 k-steps the 8-bit framebuffer holds exact match counts; we
 * grLfbReadRegion them out and continue accumulating in CPU int32.
 *
 * Texture geometry: TA[t=k][s=i] = A[i,k] (A transposed), TB[t=k][s=j] =
 * B[k,j]; one quad per k-step sources row k of both textures via t=k, with
 * s varying along x for TMU1 (j) and along y for TMU0 (i). So the SAME two
 * 256x256 textures serve all 256 k-steps — no re-download inside a tile.
 *
 * Everything is bound dynamically (LoadLibrary glide3x.dll) so retro-infer
 * still runs on boxes without a Voodoo. Fullscreen 640x480 context while
 * computing; callers restore the desktop afterwards (setmode).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../infer.h"
#include "glide_mac.h"

#ifndef _WIN32

int glide_available(void) { return 0; }
int glide_init(char *err, size_t errlen)
{
    (void)err; (void)errlen;
    if (err && errlen) strncpy(err, "glide backend is Win32-only", errlen - 1);
    return 1;
}
void glide_shutdown(void) {}
int glide_bgemm(int M, int N, int K, const unsigned char *A,
                const unsigned char *B, int *C_matches, char *err,
                size_t errlen)
{
    (void)M; (void)N; (void)K; (void)A; (void)B; (void)C_matches;
    if (err && errlen) strncpy(err, "glide backend is Win32-only", errlen - 1);
    return 1;
}

#else /* _WIN32 */

#include <windows.h>

/* ---- minimal Glide3 ABI (from retro3dfx/out/sdk/include/glide.h) ---- */
typedef unsigned long FxU32;
typedef long FxI32;
typedef int FxBool;
typedef unsigned char FxU8;
typedef FxU32 GrColor_t;
typedef FxU8 GrAlpha_t;
typedef FxU32 GrContext_t;
typedef FxI32 GrScreenResolution_t, GrScreenRefresh_t, GrColorFormat_t,
    GrOriginLocation_t, GrBuffer_t, GrChipID_t, GrCombineFunction_t,
    GrCombineFactor_t, GrCombineLocal_t, GrCombineOther_t, GrAlphaBlendFnc_t,
    GrCmpFnc_t, GrTextureFilterMode_t, GrTextureClampMode_t, GrMipMapMode_t,
    GrLOD_t, GrAspectRatio_t, GrTextureFormat_t, GrLfbWriteMode_t,
    GrOriginLocation_t2, GrCoordinateSpaceMode_t;

typedef struct {
    GrLOD_t smallLodLog2, largeLodLog2;
    GrAspectRatio_t aspectRatioLog2;
    GrTextureFormat_t format;
    void *data;
} GrTexInfo;

/* Constants verified against retro3dfx/out/sdk/include/glide.h + sst1vid.h */
#define GR_RESOLUTION_640x480 0x7
#define GR_REFRESH_60Hz 0x0
#define GR_COLORFORMAT_ARGB 0x0
#define GR_ORIGIN_UPPER_LEFT 0x0
#define GR_BUFFER_BACKBUFFER 0x1
#define GR_TMU0 0x0
#define GR_TMU1 0x1
#define GR_COMBINE_FUNCTION_ZERO 0x0
#define GR_COMBINE_FUNCTION_LOCAL 0x1
#define GR_COMBINE_FUNCTION_SCALE_OTHER 0x3
#define GR_COMBINE_FACTOR_NONE 0x0
#define GR_COMBINE_FACTOR_ZERO 0x0
#define GR_COMBINE_FACTOR_ONE 0x8
#define GR_COMBINE_FACTOR_LOCAL 0x1
#define GR_COMBINE_LOCAL_CONSTANT 0x1
#define GR_COMBINE_LOCAL_NONE 0x1      /* == CONSTANT in glide3 */
#define GR_COMBINE_OTHER_CONSTANT 0x2
#define GR_COMBINE_OTHER_NONE 0x2      /* == CONSTANT in glide3 */
#define GR_COMBINE_OTHER_TEXTURE 0x1
#define GR_BLEND_ZERO 0x0
#define GR_BLEND_ONE 0x4
#define GR_CMP_ALWAYS 0x7
#define GR_CMP_GEQUAL 0x6
#define GR_TEXTUREFILTER_POINT_SAMPLED 0x0
#define GR_TEXTURECLAMP_CLAMP 0x1
#define GR_MIPMAP_DISABLE 0x0
#define GR_LOD_LOG2_256 0x8
#define GR_ASPECT_LOG2_1x1 0x0
#define GR_TEXFMT_ALPHA_8 0x2
#define GR_PARAM_XY 0x01
#define GR_PARAM_Q 0x04
#define GR_PARAM_ST0 0x40
#define GR_PARAM_ST1 0x41
#define GR_PARAM_ENABLE 0x01
#define GR_PARAM_DISABLE 0x00
#define GR_WINDOW_COORDS 0x00
#define GR_DITHER_DISABLE 0x0

typedef void (__stdcall *p_grGlideInit)(void);
typedef void (__stdcall *p_grGlideShutdown)(void);
typedef void (__stdcall *p_grSstSelect)(int);
typedef GrContext_t (__stdcall *p_grSstWinOpen)(FxU32, GrScreenResolution_t,
    GrScreenRefresh_t, GrColorFormat_t, GrOriginLocation_t, int, int);
typedef FxBool (__stdcall *p_grSstWinClose)(GrContext_t);
typedef void (__stdcall *p_grBufferClear)(GrColor_t, GrAlpha_t, FxU32);
typedef void (__stdcall *p_grRenderBuffer)(GrBuffer_t);
typedef void (__stdcall *p_grColorCombine)(GrCombineFunction_t,
    GrCombineFactor_t, GrCombineLocal_t, GrCombineOther_t, FxBool);
typedef void (__stdcall *p_grAlphaCombine)(GrCombineFunction_t,
    GrCombineFactor_t, GrCombineLocal_t, GrCombineOther_t, FxBool);
typedef void (__stdcall *p_grTexCombine)(GrChipID_t, GrCombineFunction_t,
    GrCombineFactor_t, GrCombineFunction_t, GrCombineFactor_t, FxBool, FxBool);
typedef void (__stdcall *p_grConstantColorValue)(GrColor_t);
typedef void (__stdcall *p_grAlphaBlendFunction)(GrAlphaBlendFnc_t,
    GrAlphaBlendFnc_t, GrAlphaBlendFnc_t, GrAlphaBlendFnc_t);
typedef void (__stdcall *p_grAlphaTestFunction)(GrCmpFnc_t);
typedef void (__stdcall *p_grAlphaTestReferenceValue)(GrAlpha_t);
typedef FxU32 (__stdcall *p_grTexMinAddress)(GrChipID_t);
typedef FxU32 (__stdcall *p_grTexMaxAddress)(GrChipID_t);
typedef void (__stdcall *p_grTexDownloadMipMap)(GrChipID_t, FxU32, FxU32,
                                                GrTexInfo *);
typedef void (__stdcall *p_grTexSource)(GrChipID_t, FxU32, FxU32, GrTexInfo *);
typedef void (__stdcall *p_grTexFilterMode)(GrChipID_t,
    GrTextureFilterMode_t, GrTextureFilterMode_t);
typedef void (__stdcall *p_grTexClampMode)(GrChipID_t, GrTextureClampMode_t,
                                           GrTextureClampMode_t);
typedef void (__stdcall *p_grTexMipMapMode)(GrChipID_t, GrMipMapMode_t,
                                            FxBool);
typedef void (__stdcall *p_grVertexLayout)(FxU32, FxI32, FxU32);
typedef void (__stdcall *p_grCoordinateSpace)(GrCoordinateSpaceMode_t);
typedef void (__stdcall *p_grDrawTriangle)(const void *, const void *,
                                           const void *);
typedef void (__stdcall *p_grDitherMode)(FxU32);
typedef FxBool (__stdcall *p_grLfbReadRegion)(GrBuffer_t, FxU32, FxU32, FxU32,
                                              FxU32, FxU32, void *);
typedef void (__stdcall *p_grBufferSwap)(FxU32);
typedef void (__stdcall *p_grFinish)(void);

static struct {
    HMODULE dll;
    GrContext_t ctx;
    int inited;
    FxU32 tex_base[2];
    p_grGlideInit GlideInit;
    p_grGlideShutdown GlideShutdown;
    p_grSstSelect SstSelect;
    p_grSstWinOpen SstWinOpen;
    p_grSstWinClose SstWinClose;
    p_grBufferClear BufferClear;
    p_grRenderBuffer RenderBuffer;
    p_grColorCombine ColorCombine;
    p_grAlphaCombine AlphaCombine;
    p_grTexCombine TexCombine;
    p_grConstantColorValue ConstantColorValue;
    p_grAlphaBlendFunction AlphaBlendFunction;
    p_grAlphaTestFunction AlphaTestFunction;
    p_grAlphaTestReferenceValue AlphaTestReferenceValue;
    p_grTexMinAddress TexMinAddress;
    p_grTexMaxAddress TexMaxAddress;
    p_grTexDownloadMipMap TexDownloadMipMap;
    p_grTexSource TexSource;
    p_grTexFilterMode TexFilterMode;
    p_grTexClampMode TexClampMode;
    p_grTexMipMapMode TexMipMapMode;
    p_grVertexLayout VertexLayout;
    p_grCoordinateSpace CoordinateSpace;
    p_grDrawTriangle DrawTriangle;
    p_grLfbReadRegion LfbReadRegion;
    p_grBufferSwap BufferSwap;
    p_grFinish Finish;
    p_grDitherMode DitherMode;
} G;

static HWND g_hwnd;

static LRESULT CALLBACK glide_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

/* Our glide3x requires a real window handle for grSstWinOpen. */
static HWND make_window(void)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = glide_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "retro-infer-glide";
    wc.hCursor = LoadCursorA(NULL, (const char *)32512 /* IDC_ARROW */);
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "retro-infer-glide", "retro-infer glide-mac",
                           WS_POPUP, 0, 0, 640, 480, NULL, NULL,
                           wc.hInstance, NULL);
    if (!hwnd)
        return NULL;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    while (PeekMessageA(&msg, NULL, 0, 0, 1 /* PM_REMOVE */)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return hwnd;
}

static FARPROC bind1(const char *plain, const char *decorated)
{
    FARPROC p = GetProcAddress(G.dll, plain);
    if (!p)
        p = GetProcAddress(G.dll, decorated);
    return p;
}

#define BIND(field, name, args) \
    do { \
        G.field = (void *)bind1(#name, "_" #name "@" #args); \
        if (!G.field) { \
            if (err && errlen) \
                _snprintf(err, errlen, "glide3x.dll missing %s", #name); \
            return 1; \
        } \
    } while (0)

int glide_available(void)
{
    HMODULE h = LoadLibraryA("glide3x.dll");
    if (!h)
        return 0;
    FreeLibrary(h);
    return 1;
}

/* vertex: x, y, q, s0, t0, s1, t1 */
typedef struct {
    float x, y, q, s0, t0, s1, t1;
} gvtx_t;

int glide_init(char *err, size_t errlen)
{
    if (G.inited)
        return 0;
    G.dll = LoadLibraryA("glide3x.dll");
    if (!G.dll) {
        if (err && errlen)
            strncpy(err, "glide3x.dll not found", errlen - 1);
        return 1;
    }
    BIND(GlideInit, grGlideInit, 0);
    BIND(GlideShutdown, grGlideShutdown, 0);
    BIND(SstSelect, grSstSelect, 4);
    BIND(SstWinOpen, grSstWinOpen, 28);
    BIND(SstWinClose, grSstWinClose, 4);
    BIND(BufferClear, grBufferClear, 12);
    BIND(RenderBuffer, grRenderBuffer, 4);
    BIND(ColorCombine, grColorCombine, 20);
    BIND(AlphaCombine, grAlphaCombine, 20);
    BIND(TexCombine, grTexCombine, 28);
    BIND(ConstantColorValue, grConstantColorValue, 4);
    BIND(AlphaBlendFunction, grAlphaBlendFunction, 16);
    BIND(AlphaTestFunction, grAlphaTestFunction, 4);
    BIND(AlphaTestReferenceValue, grAlphaTestReferenceValue, 4);
    BIND(TexMinAddress, grTexMinAddress, 4);
    BIND(TexMaxAddress, grTexMaxAddress, 4);
    BIND(TexDownloadMipMap, grTexDownloadMipMap, 16);
    BIND(TexSource, grTexSource, 16);
    BIND(TexFilterMode, grTexFilterMode, 12);
    BIND(TexClampMode, grTexClampMode, 12);
    BIND(TexMipMapMode, grTexMipMapMode, 12);
    BIND(VertexLayout, grVertexLayout, 12);
    BIND(CoordinateSpace, grCoordinateSpace, 4);
    BIND(DrawTriangle, grDrawTriangle, 12);
    BIND(LfbReadRegion, grLfbReadRegion, 28);
    BIND(BufferSwap, grBufferSwap, 4);
    G.Finish = (p_grFinish)bind1("grFinish", "_grFinish@0"); /* optional */
    G.DitherMode = (p_grDitherMode)bind1("grDitherMode", "_grDitherMode@4");

    g_hwnd = make_window();
    if (!g_hwnd) {
        if (err && errlen)
            strncpy(err, "CreateWindow failed", errlen - 1);
        FreeLibrary(G.dll);
        G.dll = NULL;
        return 1;
    }
    G.GlideInit();
    G.SstSelect(0);
    G.ctx = G.SstWinOpen((FxU32)(ULONG_PTR)g_hwnd, GR_RESOLUTION_640x480,
                         GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                         GR_ORIGIN_UPPER_LEFT, 2, 0);
    if (!G.ctx) {
        if (err && errlen)
            strncpy(err, "grSstWinOpen failed (no Voodoo?)", errlen - 1);
        G.GlideShutdown();
        FreeLibrary(G.dll);
        G.dll = NULL;
        return 1;
    }

    G.CoordinateSpace(GR_WINDOW_COORDS);
    G.VertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
    G.VertexLayout(GR_PARAM_Q, 8, GR_PARAM_ENABLE);
    G.VertexLayout(GR_PARAM_ST0, 12, GR_PARAM_ENABLE);
    G.VertexLayout(GR_PARAM_ST1, 20, GR_PARAM_ENABLE);

    G.RenderBuffer(GR_BUFFER_BACKBUFFER);
    /* dithering would destroy exact accumulation in the 565 framebuffer */
    if (G.DitherMode)
        G.DitherMode(GR_DITHER_DISABLE);
    {
        int tmu;
        for (tmu = 0; tmu < 2; tmu++) {
            G.TexFilterMode(tmu, GR_TEXTUREFILTER_POINT_SAMPLED,
                            GR_TEXTUREFILTER_POINT_SAMPLED);
            G.TexClampMode(tmu, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP);
            G.TexMipMapMode(tmu, GR_MIPMAP_DISABLE, 0);
            G.tex_base[tmu] = G.TexMinAddress(tmu);
        }
    }
    G.inited = 1;
    return 0;
}

void glide_shutdown(void)
{
    if (!G.inited)
        return;
    G.SstWinClose(G.ctx);
    G.GlideShutdown();
    FreeLibrary(G.dll);
    memset(&G, 0, sizeof(G));
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
}

/* Download a 256x256 ALPHA_8 texture; slot 0..3 within a TMU. */
static void tex_download(int tmu, int slot, const unsigned char *texels)
{
    GrTexInfo info;
    info.smallLodLog2 = GR_LOD_LOG2_256;
    info.largeLodLog2 = GR_LOD_LOG2_256;
    info.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
    info.format = GR_TEXFMT_ALPHA_8;
    info.data = (void *)texels;
    /* evenOdd = BOTH (0x3): with 0 no mipmap level is written at all */
    G.TexDownloadMipMap(tmu, G.tex_base[tmu] + (FxU32)slot * 65536, 3, &info);
}

static void tex_use(int tmu, int slot)
{
    GrTexInfo info;
    info.smallLodLog2 = GR_LOD_LOG2_256;
    info.largeLodLog2 = GR_LOD_LOG2_256;
    info.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
    info.format = GR_TEXFMT_ALPHA_8;
    info.data = NULL;
    G.TexSource(tmu, G.tex_base[tmu] + (FxU32)slot * 65536, 3, &info);
}

/* draw a wxh quad at origin sampling t=trow of TMU0 (s along y => i) and
 * TMU1 (s along x => j). Texel-center sampling => +0.5 offsets. */
static void draw_kquad(int w, int h, int trow)
{
    gvtx_t v[4];
    float t = (float)trow + 0.5f;
    int c;
    for (c = 0; c < 4; c++) {
        int right = (c == 1 || c == 2), bottom = (c >= 2);
        v[c].x = right ? (float)w : 0.0f;
        v[c].y = bottom ? (float)h : 0.0f;
        v[c].q = 1.0f;
        /* TMU0: s = i (vertical), TMU1: s = j (horizontal) */
        v[c].s0 = bottom ? (float)h : 0.0f;
        v[c].t0 = t;
        v[c].s1 = right ? (float)w : 0.0f;
        v[c].t1 = t;
    }
    G.DrawTriangle(&v[0], &v[1], &v[2]);
    G.DrawTriangle(&v[0], &v[2], &v[3]);
}

/* Pipeline state for the accumulation passes */
static void set_accum_state(void)
{
    /* TMU1 (upstream) passes its own texel; TMU0 multiplies by its local:
     * tex_out = TMU0_local * TMU1_out (exact for 0x00/0xFF alphas) */
    G.TexCombine(GR_TMU1, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                 GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, 0, 0);
    G.TexCombine(GR_TMU0, GR_COMBINE_FUNCTION_SCALE_OTHER,
                 GR_COMBINE_FACTOR_LOCAL, GR_COMBINE_FUNCTION_SCALE_OTHER,
                 GR_COMBINE_FACTOR_LOCAL, 0, 0);
    /* color = constant (1,1,1); alpha = texture product */
    /* +8 in 8-bit = exactly +1 in the stored 5-bit red per pass (see
     * readback_accum): replicate(n)+8 >> 3 == n+1 for n in 0..30 under
     * truncating 888->565 conversion. glide_selftest() verifies this rule
     * on the actual hardware before any real GEMM runs. */
    G.ConstantColorValue(0xFF080808UL);
    G.ColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, 0);
    G.AlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, 0);
    G.AlphaTestFunction(GR_CMP_GEQUAL);
    G.AlphaTestReferenceValue(0x80);
    G.AlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ONE, GR_BLEND_ONE,
                         GR_BLEND_ZERO);
}

/* Read back the w x h match-count region (565 fb: green has 6 bits; we use
 * the RED channel: counts <= 31 per chunk => chunk limit 31 k-steps).
 * Voodoo fb is 16-bit 565: red = bits 11..15, 5 bits => max 31. */
#define CHUNK_K 31

static int readback_accum(int w, int h, int *C, int N)
{
    static unsigned short lfb[256 * 256];
    int i, j;
    if (!G.LfbReadRegion(GR_BUFFER_BACKBUFFER, 0, 0, (FxU32)w, (FxU32)h,
                         (FxU32)(w * 2), lfb))
        return 1;
    for (i = 0; i < h; i++)
        for (j = 0; j < w; j++)
            C[i * N + j] += (lfb[i * w + j] >> 11) & 0x1F;
    return 0;
}

/*
 * Binary GEMM: A is [M,K] bits (one byte per element, 0/1), B is [K,N] bits.
 * C_matches[i,j] += number of k where A[i,k] == B[k,j]. M,N,K <= 256.
 *
 * Texture layout: TA[t=k][s=i] = A[i,k]*255, TB[t=k][s=j] = B[k,j]*255,
 * inverted copies in slot+1.
 */
int glide_bgemm(int M, int N, int K, const unsigned char *A,
                const unsigned char *B, int *C_matches, char *err,
                size_t errlen)
{
    static unsigned char ta[65536], tb[65536], tai[65536], tbi[65536];
    int i, j, k, kc;

    if (M > 256 || N > 256 || K > 256) {
        if (err && errlen)
            strncpy(err, "tile too large (max 256)", errlen - 1);
        return 1;
    }
    if (!G.inited) {
        if (err && errlen)
            strncpy(err, "glide not initialized", errlen - 1);
        return 1;
    }

    memset(ta, 0, sizeof(ta));
    memset(tb, 0, sizeof(tb));
    for (k = 0; k < K; k++) {
        for (i = 0; i < M; i++)
            ta[k * 256 + i] = A[i * K + k] ? 0xFF : 0x00;
        for (j = 0; j < N; j++)
            tb[k * 256 + j] = B[k * N + j] ? 0xFF : 0x00;
    }
    for (i = 0; i < 65536; i++) {
        tai[i] = (unsigned char)~ta[i];
        tbi[i] = (unsigned char)~tb[i];
    }
    tex_download(GR_TMU0, 0, ta);
    tex_download(GR_TMU0, 1, tai);
    tex_download(GR_TMU1, 0, tb);
    tex_download(GR_TMU1, 1, tbi);

    set_accum_state();

    for (kc = 0; kc < K; kc += CHUNK_K) {
        int kend = kc + CHUNK_K < K ? kc + CHUNK_K : K;
        G.BufferClear(0, 0, 0);
        for (k = kc; k < kend; k++) {
            tex_use(GR_TMU0, 0);
            tex_use(GR_TMU1, 0);
            draw_kquad(N, M, k);
            tex_use(GR_TMU0, 1);
            tex_use(GR_TMU1, 1);
            draw_kquad(N, M, k);
        }
        if (G.Finish)
            G.Finish();
        if (readback_accum(N, M, C_matches, N)) {
            if (err && errlen)
                strncpy(err, "grLfbReadRegion failed", errlen - 1);
            return 1;
        }
    }
    return 0;
}

#endif /* _WIN32 */

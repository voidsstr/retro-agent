/*
 * gl-combine: portable OpenGL 1.1 + ARB_multitexture binary/XNOR GEMM
 * backend (M6). Vendor-neutral by construction — runs on any card with
 * core multitexture (universal since ~1999: GeForce, Radeon, Intel
 * integrated), unlike glide-mac which is 3dfx-only. Mirrors the glide-mac
 * accumulation design (see glide_mac.c) but ported to plain GL:
 *
 *   per k-step: TEX0 = A-column row k, TEX1 = B-row row k, GL_INTENSITY8
 *     format (0 or 255, replicated to R=G=B=A so the value is visible to
 *     *both* the RGB and alpha modulate chain — this is the fix for the
 *     bug an earlier GL_ALPHA8 version had: alpha-format textures carry
 *     ZERO in their RGB channels by definition, so any RGB-based
 *     accumulation was silently multiplying by zero regardless of the
 *     texture's alpha value)
 *   TEXTURE_ENV_MODE = GL_MODULATE on both units chains the product:
 *     final_rgba = primaryColor * tex0 * tex1 (componentwise)
 *   primary color is glColor3ub(1,1,1) — i.e. 1/255 per channel — so a
 *     genuine match (both texels 255) contributes EXACTLY 1/255 = 1 count
 *     to the framebuffer once quantized to 8 bits; a non-match contributes
 *     0. No alpha test needed: the product IS the gate.
 *   blend ONE:ONE (additive) accumulates counts across the two AND-passes
 *     (positive tex pair + inverted tex pair = XNOR/match count) and
 *     across k-steps; chunked at 255 per readback so the 8-bit channel
 *     never overflows (matches Glide's chunking, but with the full 8 bits
 *     of a real RGB channel instead of the Voodoo's 5-bit 565 red).
 *   glReadPixels the RED channel, CPU int32 accumulation across chunks.
 *
 * STATUS: hardware-verified exact on 3 real GPUs — Radeon 9800 XT (.240),
 * Radeon HD 3850 AGP (.123), Intel HD Graphics (.145): --nv-check passes
 * (0 mismatches) across multiple tile sizes/seeds up to the full 256^3
 * tile, with FNV-1a result hashes matching the Glide backend bit-for-bit
 * on the same seed (independent cross-validation of both GPU paths + the
 * CPU reference), AND --nv-check-multi (varying-size calls in one session,
 * mirroring the BNN tiling pattern) passes. Bugs found + fixed getting
 * here (see docs/machines/ai-capability-profiles.md for the write-up):
 * (1) GL_ALPHA8 textures carry zero in their RGB channels by definition,
 * so the RGB-based accumulation never worked — fixed by switching to
 * GL_INTENSITY8; (2) the quad geometry used /128 instead of /256 in its
 * NDC math (a factor-of-2 viewport-to-texel mapping error); (3) the
 * default GL_PACK_ALIGNMENT of 4 pads each glReadPixels row to a multiple
 * of 4 bytes, but the readback indexing was tightly packed with no
 * padding — invisible whenever N*3 is already 4-aligned (every --nv-check
 * default of N=256 is), but wrong for real shapes like the BNN's 10-class
 * output layer (N=10) — fixed with glPixelStorei(GL_PACK_ALIGNMENT, 1);
 * (4) glGenTextures'ing 4 new texture objects on every bgemm call leaked
 * hundreds of objects during a full BNN eval — fixed by allocating the 4
 * textures once in init and reusing them (good hygiene; wasn't the actual
 * cause of (3)'s symptom, isolated via --nv-check-multi). No GeForce box
 * has been online to date; the design has no NVIDIA-specific dependency
 * (register combiners were considered for a faster int8 path but this
 * portable multitexture route already gives exact results, so it's the
 * one actually shipped).
 *
 * Everything is bound dynamically (opengl32.dll) so retro-infer runs on
 * boxes without GL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv_gl.h"

#ifndef _WIN32

int nvgl_available(void) { return 0; }
int nvgl_init(char *err, size_t errlen)
{
    if (err && errlen)
        strncpy(err, "nv backend is Win32-only", errlen - 1);
    return 1;
}
void nvgl_shutdown(void) {}
int nvgl_bgemm(int M, int N, int K, const unsigned char *A,
               const unsigned char *B, int *C_matches, char *err,
               size_t errlen)
{
    (void)M; (void)N; (void)K; (void)A; (void)B; (void)C_matches;
    if (err && errlen)
        strncpy(err, "nv backend is Win32-only", errlen - 1);
    return 1;
}

#else

#include <windows.h>

#define GL_TEXTURE_2D 0x0DE1
#define GL_INTENSITY8 0x804B
#define GL_LUMINANCE 0x1909
#define GL_UNSIGNED_BYTE 0x1401
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP 0x2900
#define GL_MODULATE 0x2100
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_BLEND 0x0BE2
#define GL_ONE 1
#define GL_QUADS 0x0007
#define GL_RGB 0x1907
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TEXTURE0_ARB 0x84C0
#define GL_TEXTURE1_ARB 0x84C1
#define GL_EXTENSIONS 0x1F03
#define GL_DITHER 0x0BD0
#define GL_BACK 0x0405
#define GL_PACK_ALIGNMENT 0x0D05

typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef int GLsizei;
typedef unsigned char GLboolean;

typedef void (__stdcall *pfn_v)(void);
static HMODULE g_gl;
static HWND g_wnd;
static HDC g_dc;
static GLuint g_tex[4];   /* allocated once in init, reused every bgemm call
                           * — a per-call glGenTextures leaked hundreds of
                           * texture objects during a full BNN eval (~128
                           * bgemm calls) and produced wrong results on the
                           * Radeon's driver under that resource churn. */
static HGLRC g_rc;

#define GLF(ret, name, args) static ret(__stdcall *p_##name) args
GLF(void, glEnable, (GLenum));
GLF(void, glDisable, (GLenum));
GLF(void, glBindTexture, (GLenum, GLuint));
GLF(void, glGenTextures, (GLsizei, GLuint *));
GLF(void, glDeleteTextures, (GLsizei, const GLuint *));
GLF(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                         GLenum, GLenum, const void *));
GLF(void, glTexParameteri, (GLenum, GLenum, GLint));
GLF(void, glTexEnvi, (GLenum, GLenum, GLint));
GLF(void, glBlendFunc, (GLenum, GLenum));
GLF(void, glColor3ub, (GLubyte, GLubyte, GLubyte));
GLF(void, glBegin, (GLenum));
GLF(void, glEnd, (void));
GLF(void, glVertex2f, (GLfloat, GLfloat));
GLF(void, glClear, (unsigned));
GLF(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat));
GLF(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                         void *));
GLF(void, glReadBuffer, (GLenum));
GLF(void, glDrawBuffer, (GLenum));
GLF(void, glPixelStorei, (GLenum, GLint));
GLF(void, glFinish, (void));
GLF(const GLubyte *, glGetString, (GLenum));
GLF(void, glOrtho, (double, double, double, double, double, double));
GLF(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
/* multitexture via wglGetProcAddress */
static void(__stdcall *p_glActiveTextureARB)(GLenum);
static void(__stdcall *p_glMultiTexCoord2fARB)(GLenum, GLfloat, GLfloat);

typedef PROC(__stdcall *p_wglGetProcAddress_t)(const char *);
typedef HGLRC(__stdcall *p_wglCreateContext_t)(HDC);
typedef BOOL(__stdcall *p_wglMakeCurrent_t)(HDC, HGLRC);
typedef BOOL(__stdcall *p_wglDeleteContext_t)(HGLRC);
static p_wglGetProcAddress_t p_wglGetProcAddress;
static p_wglCreateContext_t p_wglCreateContext;
static p_wglMakeCurrent_t p_wglMakeCurrent;
static p_wglDeleteContext_t p_wglDeleteContext;

static int bind_gl(char *err, size_t errlen)
{
#define B(n) do { p_##n = (void *)GetProcAddress(g_gl, #n); \
    if (!p_##n) { if (err && errlen) _snprintf(err, errlen, "gl missing %s", #n); return 1; } } while (0)
    B(glEnable); B(glDisable); B(glBindTexture); B(glGenTextures);
    B(glDeleteTextures);
    B(glTexImage2D); B(glTexParameteri); B(glTexEnvi);
    B(glBlendFunc); B(glColor3ub); B(glBegin); B(glEnd); B(glVertex2f);
    B(glClear); B(glClearColor); B(glReadPixels); B(glFinish);
    B(glReadBuffer); B(glDrawBuffer); B(glPixelStorei);
    B(glGetString); B(glOrtho); B(glViewport);
#undef B
    p_wglGetProcAddress = (p_wglGetProcAddress_t)GetProcAddress(g_gl, "wglGetProcAddress");
    p_wglCreateContext = (p_wglCreateContext_t)GetProcAddress(g_gl, "wglCreateContext");
    p_wglMakeCurrent = (p_wglMakeCurrent_t)GetProcAddress(g_gl, "wglMakeCurrent");
    p_wglDeleteContext = (p_wglDeleteContext_t)GetProcAddress(g_gl, "wglDeleteContext");
    if (!p_wglGetProcAddress || !p_wglCreateContext || !p_wglMakeCurrent) {
        if (err && errlen)
            strncpy(err, "wgl entry points missing", errlen - 1);
        return 1;
    }
    return 0;
}

int nvgl_available(void)
{
    HMODULE h = LoadLibraryA("opengl32.dll");
    if (!h)
        return 0;
    FreeLibrary(h);
    return 1;
}

static LRESULT CALLBACK nv_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

int nvgl_init(char *err, size_t errlen)
{
    PIXELFORMATDESCRIPTOR pfd;
    int pf;
    WNDCLASSA wc;
    const GLubyte *ext;

    g_gl = LoadLibraryA("opengl32.dll");
    if (!g_gl) {
        if (err && errlen)
            strncpy(err, "opengl32.dll not found", errlen - 1);
        return 1;
    }
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = nv_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "retro-infer-nvgl";
    RegisterClassA(&wc);
    /* Topmost, foreground, on-screen: glReadPixels from a window's back
     * buffer is undefined if the window is occluded/offscreen (pixel-
     * ownership test). This isn't a pbuffer, so the window must genuinely be
     * the frontmost visible surface while we compute. Callers restore the
     * desktop afterwards. */
    g_wnd = CreateWindowExA(WS_EX_TOPMOST, "retro-infer-nvgl", "retro-infer nv",
                            WS_POPUP, 0, 0, 512, 512, NULL, NULL,
                            wc.hInstance, NULL);
    {
        MSG msg;
        ShowWindow(g_wnd, SW_SHOW);
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 512, 512,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
        BringWindowToTop(g_wnd);
        SetForegroundWindow(g_wnd);
        UpdateWindow(g_wnd);
        while (PeekMessageA(&msg, NULL, 0, 0, 1)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    g_dc = GetDC(g_wnd);
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pf = ChoosePixelFormat(g_dc, &pfd);
    if (!pf || !SetPixelFormat(g_dc, pf, &pfd)) {
        if (err && errlen)
            strncpy(err, "SetPixelFormat failed", errlen - 1);
        return 1;
    }
    if (bind_gl(err, errlen))
        return 1;
    g_rc = p_wglCreateContext(g_dc);
    if (!g_rc || !p_wglMakeCurrent(g_dc, g_rc)) {
        if (err && errlen)
            strncpy(err, "wglCreateContext failed", errlen - 1);
        return 1;
    }
    ext = p_glGetString(GL_EXTENSIONS);
    if (!ext || !strstr((const char *)ext, "GL_ARB_multitexture")) {
        if (err && errlen)
            strncpy(err, "no ARB_multitexture (software GL?)", errlen - 1);
        return 1;
    }
    p_glActiveTextureARB =
        (void *)p_wglGetProcAddress("glActiveTextureARB");
    p_glMultiTexCoord2fARB =
        (void *)p_wglGetProcAddress("glMultiTexCoord2fARB");
    if (!p_glActiveTextureARB || !p_glMultiTexCoord2fARB) {
        if (err && errlen)
            strncpy(err, "multitexture entry points missing", errlen - 1);
        return 1;
    }
    p_glDisable(GL_DITHER);
    p_glDrawBuffer(GL_BACK);
    p_glReadBuffer(GL_BACK);
    p_glViewport(0, 0, 512, 512);
    /* GL_PACK_ALIGNMENT defaults to 4: glReadPixels pads each row to a
     * multiple of 4 bytes, but our readback indexing (pix[(i*N+j)*3]) is
     * tightly packed with no row padding — mismatched whenever N*3 isn't a
     * multiple of 4 (e.g. N=10, a real shape: the BNN's 10-class output
     * layer). Set alignment to 1 so rows really are packed tightly. */
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glGenTextures(4, g_tex);
    return 0;
}

void nvgl_shutdown(void)
{
    if (g_rc) {
        if (g_tex[0] && p_glDeleteTextures)
            p_glDeleteTextures(4, g_tex);
        memset(g_tex, 0, sizeof(g_tex));
        p_wglMakeCurrent(NULL, NULL);
        p_wglDeleteContext(g_rc);
        g_rc = NULL;
    }
    if (g_wnd) {
        ReleaseDC(g_wnd, g_dc);
        DestroyWindow(g_wnd);
        g_wnd = NULL;
    }
    if (g_gl) {
        /* Deliberately NOT calling FreeLibrary(g_gl) here: some GPU driver
         * stacks (seen: NVIDIA's on a modern Windows box) load their real
         * ICD implementation underneath opengl32.dll and can hang a
         * worker/cleanup thread if that loader DLL is explicitly unloaded
         * mid-process rather than left for normal process teardown to
         * reclaim — every --nv-check/--bnn-eval one-shot invocation was
         * leaving a zombie retro-infer.exe process behind on WHITEBEAST
         * (RTX 4080) until this was removed. Leaking one DLL module
         * reference for the life of a short process is harmless and is
         * standard practice for GL applications; the OS unloads it at
         * real process exit either way. */
        g_gl = NULL;
    }
}

/* Same contract as glide_bgemm; chunks of 255 in the 8-bit backbuffer.
 * TEX0 t=k row = A column (s=i via y), TEX1 t=k row = B row (s=j via x);
 * TEXTURE_ENV MODULATE across units multiplies alphas (exact for 0/255). */
int nvgl_bgemm(int M, int N, int K, const unsigned char *A,
               const unsigned char *B, int *C_matches, char *err,
               size_t errlen)
{
    static unsigned char ta[65536], tb[65536], tai[65536], tbi[65536];
    static unsigned char pix[256 * 256 * 3];
    int i, j, k, kc;

    if (M > 256 || N > 256 || K > 256) {
        if (err && errlen)
            strncpy(err, "tile too large (max 256)", errlen - 1);
        return 1;
    }
    if (!g_rc) {
        if (err && errlen)
            strncpy(err, "nvgl not initialized", errlen - 1);
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

    /* reuse the 4 texture objects allocated once in nvgl_init — respecifying
     * via glTexImage2D on an existing name is well-defined and avoids
     * leaking a texture object per call (a full BNN eval calls bgemm ~128
     * times; leaking that many objects broke results on the Radeon). */
    {
        const unsigned char *src[4];
        src[0] = ta; src[1] = tai; src[2] = tb; src[3] = tbi;
        for (i = 0; i < 4; i++) {
            p_glActiveTextureARB((GLenum)(GL_TEXTURE0_ARB + (i >= 2)));
            p_glBindTexture(GL_TEXTURE_2D, g_tex[i]);
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_INTENSITY8, 256, 256, 0,
                           GL_LUMINANCE, GL_UNSIGNED_BYTE, src[i]);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        }
    }
    for (i = 0; i < 2; i++) {
        p_glActiveTextureARB((GLenum)(GL_TEXTURE0_ARB + i));
        p_glEnable(GL_TEXTURE_2D);
        p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }
    /* No alpha test needed: GL_INTENSITY8 * GL_INTENSITY8 modulate is
     * already exactly 0 (no match) or 1.0 (match) — the product IS the
     * gate. glColor3ub(1,1,1) = 1/255 per channel, so a match contributes
     * exactly 1 count (out of 255) once quantized; additive blend sums
     * matches across the two AND-passes and across k-steps in this chunk. */
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_ONE, GL_ONE);
    p_glColor3ub(1, 1, 1);

    for (kc = 0; kc < K; kc += 255) {
        int kend = kc + 255 < K ? kc + 255 : K;
        p_glClearColor(0, 0, 0, 0);
        p_glClear(GL_COLOR_BUFFER_BIT);
        for (k = kc; k < kend; k++) {
            int inv;
            float t = ((float)k + 0.5f) / 256.0f;
            for (inv = 0; inv < 2; inv++) {
                p_glActiveTextureARB(GL_TEXTURE0_ARB);
                p_glBindTexture(GL_TEXTURE_2D, g_tex[inv]);
                p_glActiveTextureARB(GL_TEXTURE1_ARB);
                p_glBindTexture(GL_TEXTURE_2D, g_tex[2 + inv]);
                p_glBegin(GL_QUADS);
                /* x -> j via TEX1.s, y -> i via TEX0.s */
                p_glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0f, t);
                p_glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.0f, t);
                p_glVertex2f(-1.0f, -1.0f);
                p_glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0f, t);
                p_glMultiTexCoord2fARB(GL_TEXTURE1_ARB, (float)N / 256.0f, t);
                p_glVertex2f((float)N / 256.0f - 1.0f, -1.0f);
                p_glMultiTexCoord2fARB(GL_TEXTURE0_ARB, (float)M / 256.0f, t);
                p_glMultiTexCoord2fARB(GL_TEXTURE1_ARB, (float)N / 256.0f, t);
                p_glVertex2f((float)N / 256.0f - 1.0f, (float)M / 256.0f - 1.0f);
                p_glMultiTexCoord2fARB(GL_TEXTURE0_ARB, (float)M / 256.0f, t);
                p_glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.0f, t);
                p_glVertex2f(-1.0f, (float)M / 256.0f - 1.0f);
                p_glEnd();
            }
        }
        p_glFinish();
        p_glReadPixels(0, 0, N, M, GL_RGB, GL_UNSIGNED_BYTE, pix);
        for (i = 0; i < M; i++)
            for (j = 0; j < N; j++)
                C_matches[i * N + j] += pix[(i * N + j) * 3];
    }
    return 0;
}

#endif /* _WIN32 */

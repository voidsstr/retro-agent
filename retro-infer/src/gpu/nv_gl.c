/*
 * nv-combiner: NVIDIA GeForce GPU backend (M6) — binary/XNOR GEMM via
 * OpenGL fixed-function + NV_register_combiners, mirroring the glide-mac
 * design (see glide_mac.c):
 *
 *   per k-step: TEX0 = A-column row k, TEX1 = B-row row k (ALPHA8, 0/255)
 *   combiner: alpha = tex0 * tex1 (exact for 0/255)
 *   alpha test GEQUAL 0.5 gates; blend ONE:ONE adds RGB(1,1,1)
 *   -> 8-bit backbuffer accumulates exact match counts (chunks of 255)
 *   glReadPixels readback, CPU int32 accumulation across chunks
 *
 * STATUS: compile-verified, NOT hardware-verified — no GeForce box is
 * currently online in the fleet (M6 acceptance pending hardware). The
 * general shape (exact-accumulation via alpha-tested additive blend) is
 * the one already proven on the Voodoo5.
 *
 * Everything is bound dynamically (opengl32.dll) so retro-infer runs on
 * boxes without GL. GeForce 2: NV_register_combiners (int8 path later);
 * GeForce 3/4: could add NV_texture_shader; GF-FX: float via NV_fragment.
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
#define GL_ALPHA8 0x803C
#define GL_ALPHA 0x1906
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
#define GL_ALPHA_TEST 0x0BC0
#define GL_BLEND 0x0BE2
#define GL_GEQUAL 0x0206
#define GL_ONE 1
#define GL_QUADS 0x0007
#define GL_RGB 0x1907
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TEXTURE0_ARB 0x84C0
#define GL_TEXTURE1_ARB 0x84C1
#define GL_EXTENSIONS 0x1F03
#define GL_DITHER 0x0BD0
#define GL_BACK 0x0405

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
static HGLRC g_rc;

#define GLF(ret, name, args) static ret(__stdcall *p_##name) args
GLF(void, glEnable, (GLenum));
GLF(void, glDisable, (GLenum));
GLF(void, glBindTexture, (GLenum, GLuint));
GLF(void, glGenTextures, (GLsizei, GLuint *));
GLF(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                         GLenum, GLenum, const void *));
GLF(void, glTexParameteri, (GLenum, GLenum, GLint));
GLF(void, glTexEnvi, (GLenum, GLenum, GLint));
GLF(void, glAlphaFunc, (GLenum, GLfloat));
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
    B(glTexImage2D); B(glTexParameteri); B(glTexEnvi); B(glAlphaFunc);
    B(glBlendFunc); B(glColor3ub); B(glBegin); B(glEnd); B(glVertex2f);
    B(glClear); B(glClearColor); B(glReadPixels); B(glFinish);
    B(glReadBuffer); B(glDrawBuffer);
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
    return 0;
}

void nvgl_shutdown(void)
{
    if (g_rc) {
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
        FreeLibrary(g_gl);
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
    GLuint tex[4];
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

    p_glGenTextures(4, tex);
    {
        const unsigned char *src[4];
        src[0] = ta; src[1] = tai; src[2] = tb; src[3] = tbi;
        for (i = 0; i < 4; i++) {
            p_glActiveTextureARB((GLenum)(GL_TEXTURE0_ARB + (i >= 2)));
            p_glBindTexture(GL_TEXTURE_2D, tex[i]);
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, 256, 256, 0, GL_ALPHA,
                           GL_UNSIGNED_BYTE, src[i]);
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
    p_glEnable(GL_ALPHA_TEST);
    p_glAlphaFunc(GL_GEQUAL, 0.5f);
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
                p_glBindTexture(GL_TEXTURE_2D, tex[inv]);
                p_glActiveTextureARB(GL_TEXTURE1_ARB);
                p_glBindTexture(GL_TEXTURE_2D, tex[2 + inv]);
                p_glBegin(GL_QUADS);
                /* x -> j via TEX1.s, y -> i via TEX0.s */
                p_glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0f, t);
                p_glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.0f, t);
                p_glVertex2f(-1.0f, -1.0f);
                p_glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0.0f, t);
                p_glMultiTexCoord2fARB(GL_TEXTURE1_ARB, (float)N / 256.0f, t);
                p_glVertex2f((float)N / 128.0f - 1.0f, -1.0f);
                p_glMultiTexCoord2fARB(GL_TEXTURE0_ARB, (float)M / 256.0f, t);
                p_glMultiTexCoord2fARB(GL_TEXTURE1_ARB, (float)N / 256.0f, t);
                p_glVertex2f((float)N / 128.0f - 1.0f, (float)M / 128.0f - 1.0f);
                p_glMultiTexCoord2fARB(GL_TEXTURE0_ARB, (float)M / 256.0f, t);
                p_glMultiTexCoord2fARB(GL_TEXTURE1_ARB, 0.0f, t);
                p_glVertex2f(-1.0f, (float)M / 128.0f - 1.0f);
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

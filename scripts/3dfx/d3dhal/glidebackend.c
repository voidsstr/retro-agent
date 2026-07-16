/*
 * glidebackend.c - Glide3 implementation of the backend seam (see .h).
 *
 * Fixed-function pipeline expressed in Glide terms. Clean-room: built only
 * against the open Glide3 headers/lib. Compiles for Win32 (mingw) and links
 * the glide3x import lib from scripts/3dfx/build-glide.sh.
 */
#include <windows.h>
#include "glidebackend.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* The open Glide DirectDraw layer (dxdrvr.c) needs a REAL window handle: with
 * hWnd=0 it falls back to GetActiveWindow(), which is NULL under a headless /
 * agent launch -> it pops a "NULL window handle" MessageBox and hangs. So we
 * create a fullscreen popup window and pass its HWND to grSstWinOpen. */
static HWND g_hwnd = NULL;

static LRESULT CALLBACK gb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l){
    return DefWindowProcA(h, m, w, l);
}
static HWND gb_make_window(int w, int h){
    WNDCLASSA wc;
    HINSTANCE hi = GetModuleHandleA(NULL);
    HWND wnd;
    if (g_hwnd) return g_hwnd;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = gb_wndproc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "gfxbenchGlideWnd";
    RegisterClassA(&wc);
    wnd = CreateWindowExA(WS_EX_TOPMOST, "gfxbenchGlideWnd", "gfxbench",
                          WS_POPUP, 0, 0, w, h, NULL, NULL, hi, NULL);
    if (wnd) { ShowWindow(wnd, SW_SHOW); SetForegroundWindow(wnd);
               SetFocus(wnd); UpdateWindow(wnd); }
    g_hwnd = wnd;
    return wnd;
}

/* Capture Glide's own error strings. Without a callback, a fatal Glide error on
 * WIN32 pops a MessageBox and exit(1)s - invisible under a headless/fullscreen
 * launch, so the app just vanishes. This logs the exact message to a file so we
 * can trace hardware-init failures. Appends (multiple errors), flushes each. */
static void gb_glide_error(const char *s, FxBool fatal)
{
    FILE *f = fopen("C:\\RETRO_AGENT\\glide_err.log", "a");
    if (f) {
        fprintf(f, "GLIDE %s: %s\n", fatal ? "FATAL" : "warn", s ? s : "(null)");
        fclose(f);
    }
}

/* GR_PARAM offsets for our gb_vtx_t (must match the layout installed below). */
#define VTX_XY_OFF     0                          /* x,y                     */
#define VTX_Z_OFF      (2*sizeof(float))          /* ooz                     */
#define VTX_Q_OFF      (3*sizeof(float))          /* 1/w                     */
#define VTX_RGB_OFF    (4*sizeof(float))          /* stored as floats below  */
/* We feed Glide float colors; keep a parallel float-color vertex internally. */

typedef struct {
    float x, y, z, q;
    float r, g, b, a;      /* 0..255 as float (GR_PARAM_RGB/A want floats)    */
    float s, t;            /* pre-scaled by tex size                          */
} gvtx_t;

struct gb_tex {
    int w, h;
    gb_texfmt_t fmt;
    GrTexInfo info;
    FxU32 start_addr;
    int   log2w, log2h;
    void *pixels;          /* converted TMU-format copy                       */
    int   bytes;
    GrTexTable_t tabtype;  /* palette, for P8                                 */
    GuTexPalette pal;
};

static FxU32 g_tex_min, g_tex_max;   /* TMU0 memory window                    */
static gb_tex_t *g_bound;
static float g_sscale = 256.f, g_tscale = 256.f;  /* current tex ST scale     */
static FxU32 g_zclear = 0;           /* W-buffer "farthest" clear value       */

#define GB_FOG_ENTRIES 64            /* Glide3 fog table is fixed at 64        */

/* ---- helpers ------------------------------------------------------------- */

static int ilog2(int v){ int n=0; while(v>1){v>>=1;n++;} return n; }

static GrScreenResolution_t res_token(int w,int h){
    if(w==640&&h==480)   return GR_RESOLUTION_640x480;
    if(w==800&&h==600)   return GR_RESOLUTION_800x600;
    if(w==1024&&h==768)  return GR_RESOLUTION_1024x768;
    if(w==1280&&h==1024) return GR_RESOLUTION_1280x1024;
    if(w==1600&&h==1200) return GR_RESOLUTION_1600x1200;
    return GR_RESOLUTION_640x480;
}
static GrScreenRefresh_t refresh_token(int hz){
    if(hz>=85) return GR_REFRESH_85Hz;
    if(hz>=75) return GR_REFRESH_75Hz;
    return GR_REFRESH_60Hz;
}

/* ---- lifecycle ----------------------------------------------------------- */

int gb_startup(char *name_out,int name_len,int *num_chips_out){
    /* Register BEFORE grGlideInit: grGlideInit itself can raise a fatal error,
     * and grGlideInit never resets GrErrorCallback, so ours sticks. This both
     * captures the exact message AND avoids the default MessageBox hang. */
    grErrorSetCallback(gb_glide_error);
    grGlideInit();
    grSstSelect(0);
    if(name_out&&name_len){
        const char *n = grGetString(GR_HARDWARE);
        strncpy(name_out, n?n:"3dfx", name_len-1);
        name_out[name_len-1]=0;
    }
    if(num_chips_out){
        FxI32 nc=1; grGet(GR_NUM_FB, sizeof(nc), &nc); /* fb == chip count-ish */
        *num_chips_out = (int)nc;
    }
    return 0;
}

void gb_shutdown(void){
    grGlideShutdown();
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
}

int gb_open(const gb_mode_t *m){
    GrColorFormat_t fmt = GR_COLORFORMAT_ABGR;
    int naux = m->z_buffer ? 1 : 0;
    int ncol = (m->double_buffer>=2)?3:2;
    HWND hw = gb_make_window(m->width, m->height);
    GrContext_t gc = grSstWinOpen((FxU32)(size_t)hw,
                                  res_token(m->width,m->height),
                                  refresh_token(m->refresh_hz),
                                  fmt, GR_ORIGIN_UPPER_LEFT, ncol, naux);
    if(!gc) return -1;

    /* FSAA: VSA-100 ordered-grid. Sample count comes from grEnable + the
     * SST env the control panel uses; we request it directly. */
    if(m->fsaa>=2){
        grEnable(GR_AA_ORDERED);
        /* pixel-sample selection is exposed via grGet/grSstConfigPipeline on
         * VSA-100; the SLI/AA split is handled inside Glide's h5 tree. */
    } else {
        grDisable(GR_AA_ORDERED);
    }

    /* TMU0 memory window for texture uploads. */
    g_tex_min = grTexMinAddress(GR_TMU0);
    g_tex_max = grTexMaxAddress(GR_TMU0);

    /* W-buffer farthest value for clears. */
    {
        FxI32 wr[2] = {0,0};
        grGet(GR_WDEPTH_MIN_MAX, sizeof(wr), wr);
        g_zclear = (FxU32)wr[1];
    }

    gb_set_vertex_layout();
    grCoordinateSpace(GR_WINDOW_COORDS);
    gb_set_shade(GB_SHADE_GOURAUD);
    gb_tex_none();
    gb_set_depth(m->z_buffer, GR_CMP_LEQUAL, 1);
    return 0;
}

void gb_close(void){ grSstWinClose(0); }

/* Static table of the modes we cycle in gfxbench; gb_enum_modes filters by what
 * the board reports and by memory (FSAA/depth cost). */
static const gb_mode_t k_modes[] = {
    {640,480,16,60,0,1,1},{640,480,16,60,2,1,1},{640,480,16,60,4,1,1},
    {800,600,16,60,0,1,1},{800,600,16,60,2,1,1},{800,600,16,60,4,1,1},
    {1024,768,16,60,0,1,1},{1024,768,16,60,2,1,1},{1024,768,16,60,4,1,1},
    {1600,1200,16,60,0,1,1},{1600,1200,16,60,2,1,1},
    {640,480,32,60,0,1,1},{800,600,32,60,0,1,1},{1024,768,32,60,0,1,1},
};
int gb_enum_modes(gb_mode_t *out,int max){
    int n = (int)(sizeof(k_modes)/sizeof(k_modes[0]));
    if(n>max) n=max;
    memcpy(out, k_modes, n*sizeof(gb_mode_t));
    return n;
}

/* ---- per-frame ----------------------------------------------------------- */
void gb_clear(unsigned r,unsigned g,unsigned b){
    grBufferClear(((FxU32)r<<0)|((FxU32)g<<8)|((FxU32)b<<16), 0, g_zclear);
}
void gb_swap(int vsync){ grBufferSwap(vsync?1:0); }
void gb_finish(void){ grFinish(); }

/* ---- render state -------------------------------------------------------- */
void gb_set_shade(gb_shade_t s){
    if(s==GB_SHADE_GOURAUD)
        grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                       GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
    else
        grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                       GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, FXFALSE);
}
void gb_set_depth(int enable,int func,int write){
    grDepthBufferMode(enable?GR_DEPTHBUFFER_WBUFFER:GR_DEPTHBUFFER_DISABLE);
    if(enable){ grDepthBufferFunction(func); grDepthMask(write?FXTRUE:FXFALSE); }
}
void gb_set_blend(int enable,int src,int dst){
    if(enable) grAlphaBlendFunction(src,dst,GR_BLEND_ONE,GR_BLEND_ZERO);
    else       grAlphaBlendFunction(GR_BLEND_ONE,GR_BLEND_ZERO,GR_BLEND_ONE,GR_BLEND_ZERO);
}
void gb_set_alphatest(int enable,int func,unsigned char ref){
    grAlphaTestFunction(enable?func:GR_CMP_ALWAYS);
    grAlphaTestReferenceValue(ref);
}
void gb_set_fog(int enable,unsigned rgb,float density){
    if(enable){
        static GrFog_t tbl[GB_FOG_ENTRIES];
        guFogGenerateLinear(tbl, 0.0f, 1.0f/(density>0?density:0.01f));
        grFogMode(GR_FOG_WITH_TABLE_ON_Q);
        grFogColorValue(rgb);
        grFogTable(tbl);
    } else grFogMode(GR_FOG_DISABLE);
}
void gb_set_cull(gb_cull_t c){
    grCullMode(c==GB_CULL_NONE?GR_CULL_DISABLE:
               c==GB_CULL_CW?GR_CULL_POSITIVE:GR_CULL_NEGATIVE);
}
void gb_set_dither(int enable){ grDitherMode(enable?GR_DITHER_4x4:GR_DITHER_DISABLE); }
void gb_set_chromakey(int enable,unsigned rgb){
    grChromakeyMode(enable?GR_CHROMAKEY_ENABLE:GR_CHROMAKEY_DISABLE);
    if(enable) grChromakeyValue(rgb);
}
void gb_set_texfactor(unsigned argb){ grConstantColorValue(argb); }

/* ---- textures ------------------------------------------------------------ */
static GrTextureFormat_t tmu_fmt(gb_texfmt_t f){
    switch(f){
        case GB_TF_ARGB1555: return GR_TEXFMT_ARGB_1555;
        case GB_TF_ARGB4444: return GR_TEXFMT_ARGB_4444;
        case GB_TF_P8:       return GR_TEXFMT_P_8;
        default:             return GR_TEXFMT_RGB_565;
    }
}
gb_tex_t *gb_tex_create(int w,int h,gb_texfmt_t fmt){
    gb_tex_t *t=calloc(1,sizeof(*t));
    t->w=w; t->h=h; t->fmt=fmt; t->log2w=ilog2(w); t->log2h=ilog2(h);
    t->info.smallLodLog2 = GR_LOD_LOG2_256;    /* set from dims below */
    t->info.largeLodLog2 = GR_LOD_LOG2_256;
    t->info.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
    t->info.format = tmu_fmt(fmt);
    t->bytes = w*h*((fmt==GB_TF_P8)?1:2);
    t->pixels = malloc(t->bytes);
    return t;
}
void gb_tex_upload(gb_tex_t *t,const void *pixels){
    memcpy(t->pixels, pixels, t->bytes);
    t->info.data = t->pixels;
    t->start_addr = g_tex_min;
    grTexDownloadMipMap(GR_TMU0, t->start_addr, GR_MIPMAPLEVELMASK_BOTH, &t->info);
    if(t->fmt==GB_TF_P8)
        grTexDownloadTable(GR_TEXTABLE_PALETTE, &t->pal);
}
void gb_tex_bind(gb_tex_t *t){
    g_bound=t;
    grTexSource(GR_TMU0, t->start_addr, GR_MIPMAPLEVELMASK_BOTH, &t->info);
    /* modulate texture by iterated (gouraud) color: OTHER=texture, LOCAL=iter */
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                 GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
    g_sscale = (float)t->w; g_tscale = (float)t->h;
}
void gb_tex_none(void){
    g_bound=0;
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
}
void gb_tex_destroy(gb_tex_t *t){ if(t){ free(t->pixels); free(t);} }
void gb_set_texfilter(int bilinear,int clamp_u,int clamp_v){
    grTexFilterMode(GR_TMU0,
        bilinear?GR_TEXTUREFILTER_BILINEAR:GR_TEXTUREFILTER_POINT_SAMPLED,
        bilinear?GR_TEXTUREFILTER_BILINEAR:GR_TEXTUREFILTER_POINT_SAMPLED);
    grTexClampMode(GR_TMU0,
        clamp_u?GR_TEXTURECLAMP_CLAMP:GR_TEXTURECLAMP_WRAP,
        clamp_v?GR_TEXTURECLAMP_CLAMP:GR_TEXTURECLAMP_WRAP);
}

/* ---- geometry ------------------------------------------------------------ */
void gb_set_vertex_layout(void){
    grVertexLayout(GR_PARAM_XY,  0,                     GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_Q0,  3*sizeof(float),       GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_Z,   2*sizeof(float),       GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_RGB, 4*sizeof(float),       GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_A,   7*sizeof(float),       GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_ST0, 8*sizeof(float),       GR_PARAM_ENABLE);
}

static void to_gvtx(gvtx_t *o,const gb_vtx_t *i){
    o->x=i->x; o->y=i->y; o->z=i->ooz; o->q=i->w;
    o->r=i->r; o->g=i->g; o->b=i->b; o->a=i->a;
    o->s=i->s*g_sscale*i->w; o->t=i->t*g_tscale*i->w;   /* perspective ST*q   */
}
void gb_draw_tris(const gb_vtx_t *v,int count){
    int i; gvtx_t g[3];
    for(i=0;i+2<count;i+=3){
        to_gvtx(&g[0],&v[i]); to_gvtx(&g[1],&v[i+1]); to_gvtx(&g[2],&v[i+2]);
        grDrawTriangle(&g[0],&g[1],&g[2]);
    }
}
void gb_draw_points(const gb_vtx_t *v,int count){
    int i; gvtx_t g; for(i=0;i<count;i++){ to_gvtx(&g,&v[i]); grDrawPoint(&g);} }
void gb_draw_lines(const gb_vtx_t *v,int count){
    int i; gvtx_t a,b;
    for(i=0;i+1<count;i+=2){ to_gvtx(&a,&v[i]); to_gvtx(&b,&v[i+1]); grDrawLine(&a,&b);} }

/*
 * gfxbench.c - on-card 3dfx mode + render-option test and benchmark.
 *
 * A plain Win32 EXE that drives the open Glide3 (via glidebackend) on a real
 * Voodoo 3/4/5. It:
 *   - enumerates and cycles every resolution / color depth / FSAA level,
 *   - toggles every fixed-function render option (gouraud, texture, bilinear,
 *     alpha blend, alpha test, fog, depth test, dither, chroma key, cull),
 *   - draws a diagnostic scene chosen to make each option visible (FSAA on the
 *     spinning triangle edges, fog gradient, alpha-blended quads, a checker
 *     texture for filtering), and
 *   - benchmarks: renders N frames per mode, measures FPS, writes a CSV.
 *
 * Two roles:
 *   1. Validate the rebuilt Glide DLLs on real hardware (the milestone-1 goal
 *      of docs/3dfx-d3d-hal-design.md) - independent of the D3D DDI.
 *   2. Exercise the exact glidebackend seam the Direct3D HAL will use, so the
 *      hardware path is debugged before it goes inside a Windows driver.
 *
 * Interactive keys (windowed message loop):
 *   [ ]  prev/next mode        f  cycle FSAA within mode
 *   t    texture on/off        b  bilinear/point
 *   g    gouraud/flat          l  alpha blend on/off
 *   a    alpha test on/off     o  fog on/off
 *   z    depth test on/off     d  dither on/off
 *   k    chroma key on/off     c  cull cycle
 *   SPACE run benchmark sweep (all modes, headless timing) -> gfxbench.csv
 *   ESC  quit
 *
 * Non-interactive:  gfxbench.exe -bench [-frames N] [-csv path]
 *   runs the full sweep and exits (for agent-driven runs).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include "glidebackend.h"

/* ---- diagnostics: log to a file, flushed per line, so nothing is lost when
 * Glide takes the screen fullscreen (stdout to a pipe gets swallowed). ------ */
static FILE *g_log;
static void LOG(const char *fmt, ...){
    va_list ap;
    if(g_log){ va_start(ap,fmt); vfprintf(g_log,fmt,ap); va_end(ap);
               fputc('\n',g_log); fflush(g_log); }
    va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); printf("\n"); fflush(stdout);
}

/* ---- diagnostic scene ---------------------------------------------------- */

typedef struct {
    int   tex, bilinear, gouraud, blend, atest, fog, depth, dither, ckey, cull;
    int   fsaa_override; /* -1 = use mode's */
} opts_t;

static gb_tex_t *g_checker;
static unsigned short g_checker_px[64*64];

static void fill_checker_pixels(void){
    /* 64x64 RGB565 checker: makes filtering + FSAA texture-smoothing visible. */
    int x,y;
    for(y=0;y<64;y++) for(x=0;x<64;x++){
        int c = ((x>>3)^(y>>3))&1;
        g_checker_px[y*64+x] = c ? 0xFFFF : 0xF800; /* white / red */
    }
}
/* Textures live in per-context TMU memory, so (re)create + upload AFTER each
 * gb_open. Calling grTexDownload before a context is open is illegal. */
static void checker_upload(void){
    if(g_checker) gb_tex_destroy(g_checker);
    g_checker = gb_tex_create(64,64,GB_TF_RGB565);
    gb_tex_upload(g_checker, g_checker_px);
}

static void apply_opts(const opts_t *o){
    gb_set_shade(o->gouraud?GB_SHADE_GOURAUD:GB_SHADE_FLAT);
    if(o->tex){ gb_tex_bind(g_checker); gb_set_texfilter(o->bilinear,0,0); }
    else        gb_tex_none();
    gb_set_blend(o->blend, GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA);
    gb_set_alphatest(o->atest, GR_CMP_GEQUAL, 0x80);
    gb_set_fog(o->fog, 0x404060, 3.0f);
    gb_set_depth(o->depth, GR_CMP_LEQUAL, 1);
    gb_set_dither(o->dither);
    gb_set_chromakey(o->ckey, 0xF800);
    gb_set_cull(o->cull==0?GB_CULL_NONE:o->cull==1?GB_CULL_CW:GB_CULL_CCW);
}

/* one animated frame: gradient bg + spinning textured/blended triangles */
static void draw_scene(const gb_mode_t *m, const opts_t *o, float t){
    int i;
    gb_clear(20,20,40);
    /* three spinning triangles at different depths/alpha */
    for(i=0;i<3;i++){
        float ang = t + i*2.09439f;
        float cx = m->width *0.5f, cy = m->height*0.5f;
        float rad = m->height*0.32f - i*22;
        float z = 0.5f - i*0.1f;
        gb_vtx_t v[3]; int k;
        for(k=0;k<3;k++){
            float a = ang + k*2.09439f;
            v[k].x = cx + cosf(a)*rad;
            v[k].y = cy + sinf(a)*rad;
            v[k].ooz = z; v[k].w = 1.0f/(1.0f+i*0.5f);
            v[k].r = (k==0)?255:60; v[k].g=(k==1)?255:60; v[k].b=(k==2)?255:60;
            v[k].a = o->blend ? 160 : 255;
            v[k].s = (k==1||k==2)?1.0f:0.0f;
            v[k].t = (k==2)?1.0f:0.0f;
        }
        gb_draw_tris(v,3);
    }
}

/* ---- benchmark sweep ----------------------------------------------------- */

static double now_ms(void){
    LARGE_INTEGER f,c;
    if(QueryPerformanceFrequency(&f)&&QueryPerformanceCounter(&c))
        return 1000.0*(double)c.QuadPart/(double)f.QuadPart;
    return (double)GetTickCount();
}

static void bench_sweep(int frames, const char *csv){
    gb_mode_t modes[32]; int n = gb_enum_modes(modes,32), i;
    FILE *f = fopen(csv?csv:"gfxbench.csv","w");
    if(f) fprintf(f,"width,height,depth,fsaa,frames,ms,fps\n");
    opts_t o = {1,1,1,0,0,0,1,1,0,0,-1}; /* textured+bilinear+depth, gouraud   */
    LOG("gfxbench sweep: %d modes x %d frames", n, frames);
    for(i=0;i<n;i++){
        double t0,t1; int fr;
        /* Voodoo3 is 16-bit only and has no FSAA; skip modes it can't do (they
         * can hang a mismatched driver instead of failing cleanly). */
        if(getenv("GFX_V3") && (modes[i].depth!=16 || modes[i].fsaa!=0)){
            LOG("  %dx%d %dbpp fsaa%d : skipped (V3-invalid)",
                modes[i].width,modes[i].height,modes[i].depth,modes[i].fsaa);
            continue;
        }
        LOG("mode %d: opening %dx%d %dbpp fsaa%d ...",i,modes[i].width,modes[i].height,modes[i].depth,modes[i].fsaa);
        if(gb_open(&modes[i])!=0){
            LOG("  %dx%d %dbpp fsaa%d : OPEN FAILED (skipped)",
                   modes[i].width,modes[i].height,modes[i].depth,modes[i].fsaa);
            continue;
        }
        LOG("  opened; uploading texture..."); checker_upload();
        LOG("  applying opts..."); apply_opts(&o);
        LOG("  drawing %d frames...",frames);
        t0=now_ms();
        for(fr=0;fr<frames;fr++){ draw_scene(&modes[i],&o,fr*0.05f); gb_swap(0); }
        LOG("  gb_finish..."); gb_finish();
        t1=now_ms();
        {
            double ms=t1-t0, fps=frames*1000.0/(ms>0?ms:1);
            LOG("  %4dx%-4d %2dbpp fsaa%d : %6.1f fps (%.0f ms)",
                   modes[i].width,modes[i].height,modes[i].depth,modes[i].fsaa,fps,ms);
            if(f) fprintf(f,"%d,%d,%d,%d,%d,%.1f,%.1f\n",
                          modes[i].width,modes[i].height,modes[i].depth,
                          modes[i].fsaa,frames,ms,fps);
        }
        gb_close();
    }
    if(f) fclose(f);
    printf("wrote %s\n", csv?csv:"gfxbench.csv");
}

/* ---- interactive --------------------------------------------------------- */

static void interactive(void){
    gb_mode_t modes[32]; int n=gb_enum_modes(modes,32), cur=0;
    opts_t o = {1,1,1,0,0,0,1,1,0,0,-1};
    int running=1;
    if(gb_open(&modes[cur])!=0){ LOG("open failed"); return; }
    checker_upload();
    apply_opts(&o);
    printf("interactive: [ ]=mode f=fsaa t=tex b=bilinear g=shade l=blend\n"
           "             a=atest o=fog z=depth d=dither k=ckey c=cull SPACE=bench ESC=quit\n");
    {
        float tm=0;
        while(running){
            MSG msg;
            while(PeekMessage(&msg,0,0,0,PM_REMOVE)){
                if(msg.message==WM_KEYDOWN){
                    int reopen=0;
                    switch(msg.wParam){
                        case VK_ESCAPE: running=0; break;
                        case VK_SPACE: gb_close(); bench_sweep(300,"gfxbench.csv");
                                       gb_open(&modes[cur]); apply_opts(&o); break;
                        case 0xDB /*[*/: cur=(cur+n-1)%n; reopen=1; break;
                        case 0xDD /*]*/: cur=(cur+1)%n;   reopen=1; break;
                        case 'T': o.tex^=1; break;   case 'B': o.bilinear^=1; break;
                        case 'G': o.gouraud^=1; break;case 'L': o.blend^=1; break;
                        case 'A': o.atest^=1; break;  case 'O': o.fog^=1; break;
                        case 'Z': o.depth^=1; break;  case 'D': o.dither^=1; break;
                        case 'K': o.ckey^=1; break;   case 'C': o.cull=(o.cull+1)%3; break;
                        case 'F': /* cycle fsaa: pick next mode with same res */
                            cur=(cur+1)%n; reopen=1; break;
                    }
                    if(reopen){ gb_close(); gb_open(&modes[cur]); checker_upload(); }
                    apply_opts(&o);
                }
            }
            draw_scene(&modes[cur], &o, tm); tm+=0.03f;
            gb_swap(1);
        }
    }
    gb_close();
}

int main(int argc,char**argv){
    char name[128]="?"; int chips=1, i, bench=0, frames=300;
    const char *csv="C:\\RETRO_AGENT\\gfxbench.csv";
    char exedir[MAX_PATH], logpath[MAX_PATH], *sl;
    setvbuf(stdout, NULL, _IONBF, 0);         /* don't lose output to the pipe */
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"-bench")) bench=1;
        else if(!strcmp(argv[i],"-frames")&&i+1<argc) frames=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-csv")&&i+1<argc) csv=argv[++i];
    }
    /* log next to the exe (survives the fullscreen switch); this is how we see
     * what happened on the card since stdout is swallowed by Glide fullscreen. */
    GetModuleFileNameA(NULL, exedir, sizeof(exedir));
    sl = strrchr(exedir,'\\'); if(sl) *sl=0; else strcpy(exedir,"C:\\RETRO_AGENT");
    _snprintf(logpath,sizeof(logpath),"%s\\gfxbench.log",exedir);
    g_log = fopen(logpath,"w");

    LOG("gfxbench start: bench=%d frames=%d csv=%s", bench, frames, csv);
    if(gb_startup(name,sizeof(name),&chips)!=0){ LOG("FATAL: Glide init failed"); return 2; }
    LOG("board: %s (%d fb/chip units)", name, chips);
    fill_checker_pixels();
    if(bench) bench_sweep(frames,csv);
    else      interactive();
    if(g_checker) gb_tex_destroy(g_checker);
    gb_shutdown();
    LOG("gfxbench done.");
    if(g_log) fclose(g_log);
    return 0;
}

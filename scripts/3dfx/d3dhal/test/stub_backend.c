/*
 * stub_backend.c - records backend calls instead of touching Glide.
 *
 * Lets the HAL translation logic (state + DP2 dispatch + primitives) run and be
 * asserted on the build host, with no Glide DLL and no hardware. Implements the
 * glidebackend.h surface as a call recorder.
 */
#include "glidebackend.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

char  g_log[8192];
int   g_loglen;
int   g_tris, g_points, g_lines;

static void logf_(const char *s){
    int n=(int)strlen(s);
    if(g_loglen+n < (int)sizeof(g_log)){ memcpy(g_log+g_loglen,s,n); g_loglen+=n; }
}
static void logkv(const char *k,int v){ char b[64]; sprintf(b,"%s=%d;",k,v); logf_(b); }

int  gb_startup(char *n,int l,int *c){ if(n&&l){strncpy(n,"stub",l);} if(c)*c=2; return 0; }
void gb_shutdown(void){}
int  gb_open(const gb_mode_t *m){ (void)m; return 0; }
void gb_close(void){}
int  gb_enum_modes(gb_mode_t *o,int m){ (void)o;(void)m; return 0; }
void gb_clear(unsigned r,unsigned g,unsigned b){(void)r;(void)g;(void)b;}
void gb_swap(int v){(void)v;}
void gb_finish(void){}

void gb_set_shade(gb_shade_t s){ logkv("shade",(int)s); }
void gb_set_depth(int e,int f,int w){ logkv("zen",e);logkv("zf",f);logkv("zw",w); }
void gb_set_blend(int e,int s,int d){ logkv("blend",e);logkv("bs",s);logkv("bd",d); }
void gb_set_alphatest(int e,int f,unsigned char r){ logkv("aten",e);logkv("atf",f);logkv("atr",r); }
void gb_set_fog(int e,unsigned c,float d){ logkv("fog",e);logkv("fogc",(int)c);(void)d; }
void gb_set_cull(gb_cull_t c){ logkv("cull",(int)c); }
void gb_set_dither(int e){ logkv("dither",e); }
void gb_set_chromakey(int e,unsigned k){ logkv("ckey",e);(void)k; }
void gb_set_texfactor(unsigned a){ logkv("tfactor",(int)(a&0xff)); }

gb_tex_t *gb_tex_create(int w,int h,gb_texfmt_t f){ logkv("texcreate",f);(void)w;(void)h; return (gb_tex_t*)0x1; }
void gb_tex_upload(gb_tex_t *t,const void *p){ (void)t;(void)p; logf_("texupload;"); }
void gb_tex_bind(gb_tex_t *t){ (void)t; logf_("texbind;"); }
void gb_tex_none(void){ logf_("texnone;"); }
void gb_tex_destroy(gb_tex_t *t){ (void)t; }
void gb_set_texfilter(int b,int u,int v){ logkv("bilinear",b);logkv("clu",u);logkv("clv",v); }

void gb_set_vertex_layout(void){}
void gb_draw_tris(const gb_vtx_t *v,int c){ (void)v; g_tris+=c/3; }
void gb_draw_points(const gb_vtx_t *v,int c){ (void)v; g_points+=c; }
void gb_draw_lines(const gb_vtx_t *v,int c){ (void)v; g_lines+=c/2; }

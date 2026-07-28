/*
 * oncard_validate.c - on-card validation of the fxD3D render core.
 *
 * Drives the REAL fxD3D DDI dispatcher (fxd_dp2_execute) against the REAL Glide
 * backend on a Voodoo3, from a synthetic DP2 command buffer - i.e. the exact
 * D3D->Glide path the kernel driver will run, minus the display-driver chassis.
 * Proves the translation core renders correctly on hardware before we sink
 * effort into the NT display driver. User-mode EXE (no DDK, no driver swap).
 */
#include "fxd3d.h"
extern void __stdcall Sleep(unsigned long);
#include <stdio.h>
#include <string.h>

/* D3DRENDERSTATE codes (match d3dhal_state.c) */
enum { RS_ZENABLE=7, RS_SHADEMODE=9, RS_ZWRITEENABLE=14, RS_ZFUNC=23,
       RS_DITHERENABLE=26, RS_CULLMODE=22 };
enum { CMP_LESSEQUAL=4, CULL_NONE=1, SHADE_GOURAUD=2 };

static unsigned char buf[8192];
static int off;
static void put_hdr(int cmd,int count){ fxd2_hdr h; memset(&h,0,sizeof(h));
  h.bCommand=(WORD)cmd; h.dwCount=(DWORD)count; memcpy(buf+off,&h,sizeof(h));
  off+=(int)sizeof(h); }
static void put32(DWORD v){ buf[off++]=v&0xff; buf[off++]=(v>>8)&0xff;
  buf[off++]=(v>>16)&0xff; buf[off++]=(v>>24)&0xff; }
static void putvtx(float x,float y,DWORD color){
  fxd_tlvertex v; memset(&v,0,sizeof(v));
  v.x=x; v.y=y; v.z=0.5f; v.rhw=1.0f; v.color=color; v.tu=0; v.tv=0;
  memcpy(buf+off,&v,sizeof(v)); off+=(int)sizeof(v); }

static void log_line(const char *s){
  FILE *f=fopen("C:\\RETRO_AGENT\\fxd3d_val.log","a");
  if(f){ fprintf(f,"%s\n",s); fclose(f);} }

int main(void){
  char name[128]={0}; int chips=0, rc; gb_mode_t m; fxd_device dev; int frame;

  log_line("== fxD3D on-card validate start ==");
  rc = gb_startup(name, sizeof(name), &chips);
  { char b[192]; sprintf(b,"gb_startup rc=%d board='%s' chips=%d",rc,name,chips); log_line(b); }
  if(rc!=0){ log_line("gb_startup FAILED"); return 2; }

  memset(&m,0,sizeof(m));
  m.width=640; m.height=480; m.depth=16; m.refresh_hz=60;
  m.double_buffer=1; m.z_buffer=1; m.fsaa=0;
  rc = gb_open(&m);
  { char b[64]; sprintf(b,"gb_open(640x480x16) rc=%d",rc); log_line(b); }
  if(rc!=0){ log_line("gb_open FAILED"); gb_shutdown(); return 3; }

  fxd_device_init(&dev, 640, 480, 16);
  dev.bound_tex = NULL;   /* untextured gouraud first */

  /* build the DP2 buffer once: render-state + a screen-filling gouraud quad
   * (RGBW corners) + a bright center triangle. */
  off=0;
  put_hdr(FXD2_RENDERSTATE, 6);
  put32(RS_SHADEMODE);    put32(SHADE_GOURAUD);
  put32(RS_CULLMODE);     put32(CULL_NONE);
  put32(RS_ZENABLE);      put32(1);
  put32(RS_ZWRITEENABLE); put32(1);
  put32(RS_ZFUNC);        put32(CMP_LESSEQUAL);
  put32(RS_DITHERENABLE); put32(1);

  put_hdr(FXD2_TRIANGLELIST, 9);   /* 3 triangles = 9 verts */
  /* quad tri 1 */
  putvtx(  0,  0, 0xFFFF0000);   /* red    */
  putvtx(640,  0, 0xFF00FF00);   /* green  */
  putvtx(  0,480, 0xFF0000FF);   /* blue   */
  /* quad tri 2 */
  putvtx(640,  0, 0xFF00FF00);   /* green  */
  putvtx(640,480, 0xFFFFFFFF);   /* white  */
  putvtx(  0,480, 0xFF0000FF);   /* blue   */
  /* center bright triangle */
  putvtx(320, 80, 0xFFFFFF00);   /* yellow */
  putvtx(200,400, 0xFF00FFFF);   /* cyan   */
  putvtx(440,400, 0xFFFF00FF);   /* magenta*/

  log_line("rendering ~12s ...");
  for(frame=0; frame<240; frame++){        /* ~12s at ~20fps with sleeps */
    gb_clear(20,20,40);
    fxd_dp2_execute(&dev, buf, off);
    gb_swap(0);
    Sleep(50);
  }

  gb_close();
  gb_shutdown();
  log_line("== done ==");
  return 0;
}

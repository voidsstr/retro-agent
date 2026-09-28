/*
 * vcrdd.h - vcr-kmd display driver (vcrdd.dll): private declarations.
 *
 * A GDI driver whose primary surface is the linear frame buffer itself
 * (EngCreateBitmap over the LFB, drawing punted to GDI - the NT "framebuf"
 * design). It imports only win32k.sys. Its jobs beyond the desktop:
 *   - answer the HWCEXT escapes our Glide sends (vcrdd_escape.c);
 *   - answer OPENGL_GETINFO, which is how XP's opengl32 finds an ICD;
 *   - forward the vcr-kmd harness escapes (log, registers, snapshots) to the
 *     miniport, and log its own steps into the miniport's flight recorder.
 */
#ifndef VCRDD_H
#define VCRDD_H

#include <stddef.h>
#include <stdarg.h>
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include <devioctl.h>
#include <ntddvdeo.h>

#include "../include/vcr_types.h"
#include "../include/vcr_events.h"
#include "../include/vcr_ioctl.h"
#include "../include/vcr_hwcext.h"
#include "../include/vcr_fmt.h"
#include "../include/vcr_flip.h"
#include "../include/vcr_text.h"

#define VCRDD_TAG           0x44524356      /* 'VCRD' */

typedef struct VCR_PDEV {
    HANDLE      hDriver;            /* the miniport */
    HDEV        hdevEng;
    HSURF       hsurfEng;           /* the primary: an opaque DEVICE surface */
    HSURF       hsurfBits;          /* the engine bitmap over the same frame buffer */
    SURFOBJ    *psoBits;            /* ... locked; where every hooked call draws */
    HPALETTE    hpalDefault;
    PALETTEENTRY *pPal;             /* 8 bpp only */
    ULONG       ulMode;             /* miniport mode index */
    ULONG       cx, cy, bpp, freq;
    LONG        lDelta;
    ULONG       iBitmapFormat;
    FLONG       flRed, flGreen, flBlue;
    PVOID       pvRamBase;          /* what MAP_VIDEO_MEMORY returned */
    PUCHAR      pjScreen;           /* the desktop: LFB + desktop offset */
    ULONG       cjFrameBuffer;
    ULONG       exclusive_pid;      /* a Glide process owns the chip */
    ULONG       restore_failed;     /* its HWCRLSEXCLUSIVE came, RESTORE_MODE failed (the rc):
                                     * the owner is kept, and DrvAssertMode(TRUE) says so */
    ULONG       hwc_requests;
    ULONG       cjVram;             /* all of video memory (MAP_VIDEO_MEMORY) */
    ULONG       dd_enabled, dd_exclusive, dd_flips, dd_blts;    /* DirectDraw HAL */
    ULONG       hw_pointer;         /* the miniport offers the hardware cursor */
    ULONG       ptr_on;
    LONG        xHot, yHot;
    /* the 2D engine (vcrdd_2d.c) */
    PUCHAR      pjRegs;             /* chip 0's register window (system space) */
    ULONG       g2d_ok;             /* the engine may be used */
    ULONG       g2d_busy;           /* an operation was queued since the last sync */
    ULONG       g2d_disabled;       /* Diag\Accel2D = 0 */
    ULONG       g2d_fifo_full;      /* status[4:0] with the PCI FIFO empty */
    ULONG       d3d_disabled;       /* Diag\\D3D = 0 */
    ULONG       no_texport;         /* Diag\\TexPortFlush = 0 */
    ULONG       napalm;             /* a VSA-100 (Voodoo 4/5): renderMode, stencil, 32 bpp 3D */
    ULONG       rt32;               /* 32 bpp D3D targets offered: napalm AND Diag\\D3D32 = 1 */
    ULONG       reset3d;            /* Glide 3D state reset at release: napalm AND Diag\\Reset3D = 1 */
    PVOID       pvmList;            /* DirectDraw's VIDEOMEMORY heap list (it fills lpHeap) */
    ULONG       fog_loaded[4];      /* the fog table on the chip: mode, start, end, density */
    ULONG       fog_valid;
    ULONG       g2d_ops, g2d_gdi_copies, g2d_gdi_fills;
    /* the text path (vcrdd_punt.c DrvTextOut, include/vcr_text.h) */
    ULONG       text_off;           /* not Diag\\Accel2DText = 1 (the default): EngTextOut as before */
    ULONG       pat_on, line_on;    /* Diag\\Accel2DPattern / Accel2DLine = 1 (default off) */
    ULONG       pat_fills, line_fills, pat_punts, line_punts;
    ULONG       text_calls, text_glyphs, text_clipped, text_rects, text_blits, text_punts;
    ULONG       text_fifo_waits;    /* a text write that had to wait for FIFO room */
    PUCHAR      text_mask;          /* VCR_TEXT_MASK_BYTES: a string's glyphs, OR-ed (NULL: per glyph) */
    ULONG       text_punt_why[VCR_TEXT_R_MAX];
    /* a DirectDraw flip the chip has not latched yet: the completion rule and
     * its counters (include/vcr_flip.h; vcrdd_ddraw.c is the glue) */
    vcr_flip_state flip;
    ULONG       flip_from;          /* the old front buffer (fpVidMem): not to be drawn on yet */
} VCR_PDEV;

/* vcrdd_log.c */
extern HANDLE g_hDriver;
void  VcrDd(ULONG level, ULONG code, ULONG a, ULONG b, ULONG c, ULONG d,
            const char *fmt, ...);
DWORD VcrIoctl(HANDLE h, DWORD code, PVOID in, DWORD cin, PVOID out, DWORD cout,
               DWORD *got);

/* vcrdd.c */
BOOL  VcrDdSetMode(VCR_PDEV *pd);

/* vcrdd_2d.c: the 2D engine. Sync before ANY CPU access to video memory. */
void  VcrDd2dInit(VCR_PDEV *pd);
void  VcrDd2dTerm(VCR_PDEV *pd);
void  VcrDd2dSync(VCR_PDEV *pd);
BOOL  VcrDdRoom(VCR_PDEV *pd, ULONG n);   /* n free PCI FIFO slots, bounded */
BOOL  VcrDd2dCopy(VCR_PDEV *pd, ULONG dst_off, LONG dst_stride, ULONG src_off, LONG src_stride,
                  ULONG bytespp, LONG sx, LONG sy, LONG dx, LONG dy, LONG w, LONG h,
                  ULONG ckey, ULONG ck_lo, ULONG ck_hi);
BOOL  VcrDd2dFill(VCR_PDEV *pd, ULONG dst_off, LONG dst_stride, ULONG bytespp, LONG x, LONG y,
                  LONG w, LONG h, ULONG color);
BOOL  VcrDd2dPatFill(VCR_PDEV *pd, ULONG dst_off, LONG dst_stride, ULONG bytespp, LONG x, LONG y,
                     LONG w, LONG h, ULONG pat0, ULONG pat1, ULONG fore, ULONG back, ULONG rop3,
                     BOOL transparent, ULONG patx, ULONG paty);
/* monochrome expansion (text): one per run of glyphs in one colour. Begin
 * points the engine at the surface; each Glyph is a host-to-screen blit of
 * one clipped glyph part (vcr_text.h), its data counted against the free PCI
 * FIFO slots the last status read reported (credit), every wait bounded. */
typedef struct VCR_MONO {
    VCR_PDEV   *pd;
    ULONG       xskip;              /* pixels between the 16-byte base and the surface */
    ULONG       credit;             /* FIFO slots known free */
} VCR_MONO;
BOOL  VcrDd2dMonoBegin(VCR_PDEV *pd, VCR_MONO *m, ULONG dst_off, LONG dst_stride, ULONG bytespp,
                       ULONG color);
BOOL  VcrDd2dMonoGlyph(VCR_MONO *m, const BYTE *bits, ULONG cx, const vcr_glyph_part *p);
/* the 3D state a Glide session leaves on chip 0, cleared when its owner gives
 * the chip back (VSA-100, Diag\\Reset3D = 1; vcr_3dseq.h) - only on an idle
 * chip with Glide's command FIFO off, bounded like the rest. Returns
 * VCR_R3D_*; *detail is logged with it. */
ULONG VcrDdGlideReset3d(VCR_PDEV *pd, ULONG *detail);

/* vcrdd_ddraw.c (with the public DDK's DirectDraw headers) */
#ifdef VCR_HAVE_DDI
BOOL APIENTRY DrvGetDirectDrawInfo(DHPDEV dhpdev, DD_HALINFO *hal, DWORD *nheaps,
                                   VIDEOMEMORY *vm, DWORD *nfourcc, DWORD *fourcc);
BOOL APIENTRY DrvEnableDirectDraw(DHPDEV dhpdev, DD_CALLBACKS *cb, DD_SURFACECALLBACKS *scb,
                                  DD_PALETTECALLBACKS *pcb);
VOID APIENTRY DrvDisableDirectDraw(DHPDEV dhpdev);
void  VcrDdFlipStatsLog(VCR_PDEV *pd);  /* the flip counters, if anything is new */
void  VcrDdFlipLocalGone(VCR_PDEV *pd); /* DestroyDDLocal: the counters, if the flips were this process's */
DWORD APIENTRY DdGetDriverInfo(PDD_GETDRIVERINFODATA p);
/* vcrdd_d3d.c: the Direct3D half of the HAL */
void  VcrDdD3dHalInfo(VCR_PDEV *pd, DD_HALINFO *hal);
int   VcrDdD3dDriverInfo(VCR_PDEV *pd, PDD_GETDRIVERINFODATA p);
void  VcrDdD3dSurfaceGone(PDD_SURFACE_LOCAL s);
int   VcrDdD3dCreateMipChain(VCR_PDEV *pd, PDD_CREATESURFACEDATA p);
int   VcrDdD3dFreeMipChain(VCR_PDEV *pd, PDD_SURFACE_LOCAL s);
void  VcrDdD3dTexWritten(PDD_SURFACE_LOCAL s);
#endif

/* vcrdd_punt.c: the hooked drawing calls */
#define VCRDD_HOOKS (HOOK_BITBLT | HOOK_COPYBITS | HOOK_TEXTOUT | HOOK_STROKEPATH | \
                     HOOK_FILLPATH | HOOK_LINETO | HOOK_STRETCHBLT | HOOK_STRETCHBLTROP | \
                     HOOK_ALPHABLEND | HOOK_GRADIENTFILL | HOOK_TRANSPARENTBLT)
BOOL APIENTRY DrvBitBlt(SURFOBJ *, SURFOBJ *, SURFOBJ *, CLIPOBJ *, XLATEOBJ *, RECTL *,
                        POINTL *, POINTL *, BRUSHOBJ *, POINTL *, ROP4);
BOOL APIENTRY DrvCopyBits(SURFOBJ *, SURFOBJ *, CLIPOBJ *, XLATEOBJ *, RECTL *, POINTL *);
BOOL APIENTRY DrvTextOut(SURFOBJ *, STROBJ *, FONTOBJ *, CLIPOBJ *, RECTL *, RECTL *,
                         BRUSHOBJ *, BRUSHOBJ *, POINTL *, MIX);
BOOL APIENTRY DrvStrokePath(SURFOBJ *, PATHOBJ *, CLIPOBJ *, XFORMOBJ *, BRUSHOBJ *, POINTL *,
                            LINEATTRS *, MIX);
BOOL APIENTRY DrvFillPath(SURFOBJ *, PATHOBJ *, CLIPOBJ *, BRUSHOBJ *, POINTL *, MIX, FLONG);
BOOL APIENTRY DrvLineTo(SURFOBJ *, CLIPOBJ *, BRUSHOBJ *, LONG, LONG, LONG, LONG, RECTL *, MIX);
BOOL APIENTRY DrvStretchBlt(SURFOBJ *, SURFOBJ *, SURFOBJ *, CLIPOBJ *, XLATEOBJ *,
                            COLORADJUSTMENT *, POINTL *, RECTL *, RECTL *, POINTL *, ULONG);
BOOL APIENTRY DrvStretchBltROP(SURFOBJ *, SURFOBJ *, SURFOBJ *, CLIPOBJ *, XLATEOBJ *,
                               COLORADJUSTMENT *, POINTL *, RECTL *, RECTL *, POINTL *, ULONG,
                               BRUSHOBJ *, DWORD);
BOOL APIENTRY DrvAlphaBlend(SURFOBJ *, SURFOBJ *, CLIPOBJ *, XLATEOBJ *, RECTL *, RECTL *,
                            BLENDOBJ *);
BOOL APIENTRY DrvGradientFill(SURFOBJ *, CLIPOBJ *, XLATEOBJ *, TRIVERTEX *, ULONG, PVOID,
                              ULONG, RECTL *, POINTL *, ULONG);
BOOL APIENTRY DrvTransparentBlt(SURFOBJ *, SURFOBJ *, CLIPOBJ *, XLATEOBJ *, RECTL *, RECTL *,
                                ULONG, ULONG);

/* vcrdd_pointer.c */
ULONG APIENTRY DrvSetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask, SURFOBJ *psoColor,
                                  XLATEOBJ *pxlo, LONG xHot, LONG yHot, LONG x, LONG y,
                                  RECTL *prcl, FLONG fl);
VOID APIENTRY DrvMovePointer(SURFOBJ *pso, LONG x, LONG y, RECTL *prcl);
void  VcrDdPointerProbe(VCR_PDEV *pd);

/* vcrdd_escape.c */
ULONG APIENTRY DrvEscape(SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn,
                         ULONG cjOut, PVOID pvOut);

#endif /* VCRDD_H */

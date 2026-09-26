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
    ULONG       hwc_requests;
    ULONG       cjVram;             /* all of video memory (MAP_VIDEO_MEMORY) */
    ULONG       dd_enabled, dd_exclusive, dd_flips, dd_blts;    /* DirectDraw HAL */
    ULONG       hw_pointer;         /* the miniport offers the hardware cursor */
    ULONG       ptr_on;
    LONG        xHot, yHot;
} VCR_PDEV;

/* vcrdd_log.c */
extern HANDLE g_hDriver;
void  VcrDd(ULONG level, ULONG code, ULONG a, ULONG b, ULONG c, ULONG d,
            const char *fmt, ...);
DWORD VcrIoctl(HANDLE h, DWORD code, PVOID in, DWORD cin, PVOID out, DWORD cout,
               DWORD *got);

/* vcrdd.c */
BOOL  VcrDdSetMode(VCR_PDEV *pd);

/* vcrdd_ddraw.c (with the public DDK's DirectDraw headers) */
#ifdef VCR_HAVE_DDI
BOOL APIENTRY DrvGetDirectDrawInfo(DHPDEV dhpdev, DD_HALINFO *hal, DWORD *nheaps,
                                   VIDEOMEMORY *vm, DWORD *nfourcc, DWORD *fourcc);
BOOL APIENTRY DrvEnableDirectDraw(DHPDEV dhpdev, DD_CALLBACKS *cb, DD_SURFACECALLBACKS *scb,
                                  DD_PALETTECALLBACKS *pcb);
VOID APIENTRY DrvDisableDirectDraw(DHPDEV dhpdev);
DWORD APIENTRY DdGetDriverInfo(PDD_GETDRIVERINFODATA p);
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

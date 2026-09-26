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
    HSURF       hsurfEng;
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

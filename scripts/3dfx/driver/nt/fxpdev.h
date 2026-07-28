/*
 * fxpdev.h - the per-PDEV state (our private DHPDEV) shared by the two halves
 * of the fxd3ddd display driver:
 *   - chassis.c  builds/tears it down (DrvEnablePDEV/Surface, modeset, the
 *     gbkernel attach), and
 *   - enable.c   reads it from the DirectDraw/D3D DDI (surface placement,
 *     Lock pointers, MapMemory, the M4c-2 present path).
 *
 * DDK-context header: include it AFTER windows.h/winddi.h/ntddvdeo.h (it uses
 * HANDLE/HDEV/VIDEO_MEMORY_INFORMATION etc. straight from them). The one
 * non-DDK dependency is the host-tested gbk pure-logic core (the DDraw
 * surface bump allocator lives in gbk/gbk_surf.c).
 *
 * Clean-room: only public DDK types + our own state.
 */
#ifndef FXD3D_FXPDEV_H
#define FXD3D_FXPDEV_H

#include "gbk/gbk.h"       /* gbk_surfheap_t (self-contained, C89)           */

/* Max public access ranges we query from the paired miniport (BAR0 register
 * aperture is among them - design "Miniport pairing"/gbkernel MMIO gap). */
#define FXD3D_MAX_PUBRANGES  8

typedef struct _FXPDEV
{
    HANDLE   hDriver;         /* miniport handle for EngDeviceIoControl        */
    HDEV     hdevEng;         /* GDI device handle (set by DrvCompletePDEV)    */
    HSURF    hsurfScreen;     /* the primary framebuffer surface (DrvEnableSurface) */

    LONG     cxScreen;        /* visible width  in pixels                      */
    LONG     cyScreen;        /* visible height in pixels                      */
    ULONG    ulBitsPerPel;    /* 16 or 32                                      */
    LONG     lDeltaScreen;    /* framebuffer stride in bytes                   */
    ULONG    ulBitmapType;    /* BMF_16BPP / BMF_32BPP                         */
    ULONG    iDitherFormat;   /* BMF_* GDI dither format for DEVINFO           */

    BYTE    *pjScreen;        /* mapped linear framebuffer base                */
    ULONG    ulFrequency;     /* refresh rate in Hz                            */
    ULONG    ulMode;          /* miniport mode index selected for this PDEV    */

    ULONG    flRed;           /* colour masks (16/32bpp bitfields)             */
    ULONG    flGreen;
    ULONG    flBlue;

    HPALETTE hpalDefault;     /* default palette (bitfields for hi/true colour)*/

    VIDEO_MEMORY_INFORMATION vmi;   /* result of IOCTL_VIDEO_MAP_VIDEO_MEMORY  */

    VOID    *pvBar0;          /* mapped BAR0 register aperture (public range)  */
    int      pubRangesMapped; /* 1 once QUERY_PUBLIC_ACCESS_RANGES succeeded    */
    int      gbkAttached;     /* 1 once gbkernel_attach succeeded (3D live)     */

    /* Every non-NULL VA the QUERY mapped, retained so DrvDisableSurface can
     * hand IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES the matching VIDEO_MEMORY[]
     * input - the miniport frees BY these VAs (a NULL/0 input is rejected with
     * ERROR_INSUFFICIENT_BUFFER and frees nothing -> aperture leak). BAR0 is
     * only one of them (samples map 2-3). */
    VOID    *apvPubRange[FXD3D_MAX_PUBRANGES];
    ULONG    cPubRangeVA;     /* count of valid VAs in apvPubRange[]            */

    /* --- DirectDraw / D3D surface state (enable.c, M4c-2) ----------------- *
     * All surface fpVidMem values are byte offsets into the mapped video
     * memory (the GDI primary is offset 0), the same space gbkernel's layout
     * speaks - kernel access is pjScreen + fpVidMem, user access is the
     * DdMapMemory fpProcess + fpVidMem.                                      */
    gbk_surfheap_t ddHeap;    /* bump allocator over the DDraw offscreen
                               * region gbkernel reserves (gbk_meminfo
                               * surfOffset/surfSize)                          */
    int      ddHeapLive;      /* 1 once ddHeap was seeded from gbkernel        */
    ULONG    fpRT3d;          /* board offset the app's screen-sized 3D back/
                               * RT surface was PLACED at (informational: the
                               * present source is identified by the
                               * FXNT_SURFTAG_RT3D tag in the surface GLOBAL,
                               * which survives the runtime's post-Flip
                               * fpVidMem rotation - review #5)                */
    int      rt3dValid;       /* a live surface carries FXNT_SURFTAG_RT3D      */
    int      ddExclusive;     /* DdSetExclusiveMode state (informational)      */
} FXPDEV, *PFXPDEV;

#endif /* FXD3D_FXPDEV_H */

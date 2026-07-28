/*
 * gbkdebug.c - DrvEscape handler for the fxD3D on-card BRING-UP LADDER (M4c-1).
 *
 * This is the DDK-facing half of the ladder: it unpacks the ExtEscape buffers
 * the retro agent sends (from a tiny user tool at M4d, via ExtEscape on the
 * display DC) and dispatches to the gbkernel_dbg_* implementations in
 * gbkernel.c, which actually touch the Voodoo3. The point of the ladder is to
 * validate the kernel Glide transport ONE RUNG AT A TIME on real hardware -
 * probe -> clear -> triangle -> texture -> readback - NONE of which needs
 * Direct3D, so the backend is proven safe before D3D ever drives it
 * (docs/3dfx-gbkernel-design.md sec 6 "Bring-up ladder").
 *
 * FP bracketing (M4b-2 minor #9): the two rungs that run x87 work - FXDBG_TRI
 * and FXDBG_TEX - are bracketed in a single EngSaveFloatingPointState region
 * INSIDE gbkernel_dbg_tri()/gbkernel_dbg_tex() (at the ladder entry, where the
 * (float) casts and gb_draw happen); this handler does no floating point, so it
 * adds no bracket of its own. FXDBG_PROBE / FXDBG_CLEAR / FXDBG_READBACK are
 * pure-integer paths.
 *
 * Clean-room: implements only the public NT DrvEscape DDI (winddi.h) plus our
 * private escape opcodes (gbkdebug.h). The escape buffers are captured by GDI
 * into kernel memory before this is called, so pvIn/pvOut are readable/writable
 * up to cjIn/cjOut; every access is still size-guarded here.
 */
#include "gbkdebug.h"

#ifdef HAVE_DDK

#include <windows.h>
#include <winddi.h>

#ifndef QUERYESCSUPPORT
#define QUERYESCSUPPORT 8      /* standard GDI escape (wingdi.h) */
#endif

/* Write the small status result rungs 2-4 share; returns bytes written. */
static ULONG
fxdbg_write_status(PVOID pvOut, ULONG cjOut, int ok)
{
    fxdbg_status_out_t *out = (fxdbg_status_out_t *)pvOut;
    if (out == NULL || cjOut < sizeof(fxdbg_status_out_t))
        return 0;
    out->magic   = FXDBG_MAGIC;
    out->ok      = (unsigned long)(ok ? 1 : 0);
    out->faulted = (unsigned long)gbkernel_dbg_faulted();
    return sizeof(fxdbg_status_out_t);
}

ULONG APIENTRY
DrvEscape(SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn,
          ULONG cjOut, PVOID pvOut)
{
    UNREFERENCED_PARAMETER(pso);

    switch (iEsc) {

    case QUERYESCSUPPORT: {
        ULONG q;
        if (pvIn == NULL || cjIn < sizeof(ULONG))
            return 0;
        q = *(ULONG *)pvIn;
        if (q == QUERYESCSUPPORT ||
            q == FXDBG_PROBE || q == FXDBG_CLEAR || q == FXDBG_TRI ||
            q == FXDBG_TEX   || q == FXDBG_READBACK)
            return 1;
        return 0;
    }

    /* Rung 1: probe - BAR0 status + cmdFifo0 regs + layout (no draw). */
    case FXDBG_PROBE: {
        fxdbg_probe_t *out = (fxdbg_probe_t *)pvOut;
        if (out == NULL || cjOut < sizeof(fxdbg_probe_t))
            return 0;
        gbkernel_dbg_probe(out);
        return sizeof(fxdbg_probe_t);
    }

    /* Rung 2: clear <rgb> + present. */
    case FXDBG_CLEAR: {
        unsigned long r = 0, g = 0, b = 0;
        int ok;
        if (pvIn != NULL && cjIn >= sizeof(fxdbg_clear_in_t)) {
            fxdbg_clear_in_t *in = (fxdbg_clear_in_t *)pvIn;
            r = in->r; g = in->g; b = in->b;
        }
        ok = gbkernel_dbg_clear(r, g, b);
        return fxdbg_write_status(pvOut, cjOut, ok);
    }

    /* Rung 3: one hardcoded gouraud triangle + present. */
    case FXDBG_TRI: {
        int ok = gbkernel_dbg_tri();
        return fxdbg_write_status(pvOut, cjOut, ok);
    }

    /* Rung 4: checker texture upload + textured quad + present. */
    case FXDBG_TEX: {
        int ok = gbkernel_dbg_tex();
        return fxdbg_write_status(pvOut, cjOut, ok);
    }

    /* Rung 5: framebuffer readback (BAR1) for host-side pixel verification. */
    case FXDBG_READBACK: {
        fxdbg_readback_in_t *in = (fxdbg_readback_in_t *)pvIn;
        if (in == NULL || cjIn < sizeof(fxdbg_readback_in_t) ||
            pvOut == NULL || cjOut == 0)
            return 0;
        return (ULONG)gbkernel_dbg_readback(in->which, in->x, in->y,
                                            in->w, in->h, pvOut,
                                            (unsigned long)cjOut);
    }

    default:
        /* not one of ours: 0 tells GDI the escape is unsupported. */
        return 0;
    }
}

#else  /* !HAVE_DDK - reviews on any host; the real handler is DDK-only. */

int fxnt_gbkdebug_is_ddk_only = 1;

#endif /* HAVE_DDK */

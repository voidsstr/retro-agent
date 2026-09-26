/*
 * vcrdd_3d.c - the 3D engine (Banshee / Voodoo3 / VSA-100), driven from the
 * display driver for its Direct3D HAL (vcrdd_d3d.c).
 *
 * Straight register writes through the PCI FIFO, the same transport as the
 * 2D engine (vcrdd_2d.c: VcrDdRoom / the sync rules). Triangles go through the
 * triangle setup unit: a vertex is written to the s* registers and the unit
 * computes the gradients itself - sBeginTriCMD after a triangle's first
 * vertex, sDrawTriCMD after each of the other two. Clears are fastfillCMD
 * (colour from c1, depth from zaColor) inside the clip rectangle.
 *
 * THIS FILE USES THE FPU. It is compiled without -mgeneral-regs-only (see
 * the Makefile), and every entry point is called between
 * EngSaveFloatingPointState and EngRestoreFloatingPointState by the HAL: a
 * display driver runs in kernel mode, where the FPU state belongs to the
 * interrupted thread.
 */
#include "vcrdd.h"
#include "vcrdd_3d.h"

static void w3(VCR_PDEV *pd, ULONG off, ULONG v)
{
    *(volatile ULONG *)(pd->pjRegs + V3D_BASE + off) = v;
}

static void wf(VCR_PDEV *pd, ULONG off, float f)
{
    union { float f; ULONG u; } x;
    x.f = f;
    w3(pd, off, x.u);
}

static float fbits(ULONG u)
{
    union { float f; ULONG u; } x;
    x.u = u;
    return x.f;
}

/* ---- the render target ----------------------------------------------------------- */

BOOL VcrDd3dTarget(VCR_PDEV *pd, const vcr3d_target *t)
{
    if (!VcrDdRoom(pd, 6))
        return FALSE;
    w3(pd, V3D_COLBUFFERADDR, t->rt_off);
    w3(pd, V3D_COLBUFFERSTRIDE, BS_LINEAR_STRIDE(t->rt_pitch));
    w3(pd, V3D_AUXBUFFERADDR, t->z_off);
    w3(pd, V3D_AUXBUFFERSTRIDE, BS_LINEAR_STRIDE(t->z_pitch ? t->z_pitch : t->rt_pitch));
    w3(pd, V3D_CLIPLEFTRIGHT, (0u << 16) | (t->width & 0xfff));
    w3(pd, V3D_CLIPBOTTOMTOP, (0u << 16) | (t->height & 0xfff));
    pd->g2d_busy = 1;
    return TRUE;
}

/* ---- state -------------------------------------------------------------------------- */

BOOL VcrDd3dState(VCR_PDEV *pd, const vcr3d_regs *r)
{
    if (!VcrDdRoom(pd, 12))
        return FALSE;
    w3(pd, V3D_FBZCOLORPATH, r->fbzColorPath);
    w3(pd, V3D_FBZMODE, r->fbzMode);
    w3(pd, V3D_ALPHAMODE, r->alphaMode);
    w3(pd, V3D_FOGMODE, r->fogMode);
    w3(pd, V3D_FOGCOLOR, r->fogColor);
    w3(pd, V3D_C0, r->c0);
    w3(pd, V3D_C1, r->c1);
    w3(pd, V3D_SSETUPMODE, r->setupMode);
    if (r->textured) {
        w3(pd, V3D_TMU0 + V3D_TEXTUREMODE, r->textureMode);
        w3(pd, V3D_TMU0 + V3D_TLOD, r->tLOD);
        w3(pd, V3D_TMU0 + V3D_TDETAIL, 0);
        w3(pd, V3D_TMU0 + V3D_TEXBASEADDR, r->texBaseAddr);
    }
    pd->g2d_busy = 1;
    return TRUE;
}

/* ---- clear --------------------------------------------------------------------------- */

/* fastfill every rectangle: colour from c1, depth from zaColor, only where
 * the fbzMode write masks allow. Leaves fbzMode, c1, the clip changed - the
 * HAL re-sends its state before the next draw. */
BOOL VcrDd3dClear(VCR_PDEV *pd, const vcr3d_target *t, ULONG what, ULONG argb, ULONG zbits,
                  const RECTL *rc, ULONG n)
{
    ULONG i, mode = FZ_RECTCLIP;
    float z;
    ULONG z16;
    if (what & VCR3D_CLEAR_COLOR)
        mode |= FZ_RGBWRITE;
    if ((what & VCR3D_CLEAR_Z) && t->z_off)
        mode |= FZ_ZAWRITE;
    z = fbits(zbits);
    z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
    z16 = (ULONG)(z * 65535.0f + 0.5f);
    if (!VcrDdRoom(pd, 3))
        return FALSE;
    w3(pd, V3D_FBZMODE, mode);
    w3(pd, V3D_C1, argb);
    w3(pd, V3D_ZACOLOR, z16);
    for (i = 0; i < n; i++) {
        LONG l = rc[i].left < 0 ? 0 : rc[i].left, tp = rc[i].top < 0 ? 0 : rc[i].top;
        LONG r = rc[i].right > (LONG)t->width ? (LONG)t->width : rc[i].right;
        LONG b = rc[i].bottom > (LONG)t->height ? (LONG)t->height : rc[i].bottom;
        if (r <= l || b <= tp)
            continue;
        if (!VcrDdRoom(pd, 3))
            return FALSE;
        w3(pd, V3D_CLIPLEFTRIGHT, ((ULONG)l << 16) | (ULONG)r);
        w3(pd, V3D_CLIPBOTTOMTOP, ((ULONG)tp << 16) | (ULONG)b);
        w3(pd, V3D_FASTFILLCMD, 0);
    }
    if (!VcrDdRoom(pd, 2))
        return FALSE;
    w3(pd, V3D_CLIPLEFTRIGHT, t->width & 0xfff);
    w3(pd, V3D_CLIPBOTTOMTOP, t->height & 0xfff);
    pd->g2d_busy = 1;
    return TRUE;
}

/* ---- triangles ------------------------------------------------------------------------ */

static BOOL vertex(VCR_PDEV *pd, const vcr3d_draw *d, const UCHAR *v)
{
    const float *p = (const float *)v;          /* x, y, z, rhw: D3DFVF_XYZRHW */
    float oow = p[3];
    if (!VcrDdRoom(pd, d->textured ? 9 : 6))
        return FALSE;
    wf(pd, V3D_SVX, p[0] + d->xy_bias);
    wf(pd, V3D_SVY, p[1] + d->xy_bias);
    w3(pd, V3D_SARGB, d->diff_off ? *(const ULONG *)(v + d->diff_off) : 0xffffffffu);
    wf(pd, V3D_SVZ, p[2] * 65535.0f);
    wf(pd, V3D_SOOWFBI, oow);
    if (d->textured) {
        const float *uv = (const float *)(v + d->tex_off);
        wf(pd, V3D_SOOW0, oow);
        wf(pd, V3D_SSOW0, uv[0] * d->s_scale * oow);
        wf(pd, V3D_STOW0, uv[1] * d->t_scale * oow);
    }
    return TRUE;
}

BOOL VcrDd3dTriangle(VCR_PDEV *pd, const vcr3d_draw *d, const UCHAR *a, const UCHAR *b,
                     const UCHAR *c)
{
    if (!vertex(pd, d, a))
        return FALSE;
    w3(pd, V3D_SBEGINTRICMD, 0);
    if (!vertex(pd, d, b))
        return FALSE;
    w3(pd, V3D_SDRAWTRICMD, 0);
    if (!vertex(pd, d, c))
        return FALSE;
    w3(pd, V3D_SDRAWTRICMD, 0);
    pd->g2d_busy = 1;
    return TRUE;
}

/* the S/T scales for a w x h texture: the wider side spans 256 */
void VcrDd3dTexScale(vcr3d_draw *d, ULONG w, ULONG h)
{
    ULONG big = w > h ? w : h;
    d->s_scale = 256.0f * (float)w / (float)big;
    d->t_scale = 256.0f * (float)h / (float)big;
}

void VcrDd3dDrawInit(vcr3d_draw *d)
{
    d->xy_bias = 0.0f;
    d->s_scale = d->t_scale = 256.0f;
}

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
#include "../include/vcr_fog.h"
#include "../include/vcr_3dseq.h"

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
    /* the writes are vcr_3dseq.h's, where the host test checks them one by
     * one. VSA-100: the 3D pixel size (16, or 32 with a 32-bit 24+8 aux
     * buffer - only with Diag\\D3D32) and every channel written, as
     * _grRenderMode does, then stencilMode = stencilOp = 0: the stencil byte
     * of a 32 bpp aux buffer is live, and whatever a Glide/OpenGL session
     * left there must neither fail nor rewrite our pixels. A Banshee/Voodoo3
     * has neither register and renders 16 bpp only. Any format but 16 (or 32
     * where it is allowed) is refused on every chip, before a write. */
    vcr_regw w[VCR_3D_TARGET_MAX];
    ULONG n = vcr_3d_target_seq(pd->napalm, pd->rt32, t->fmt, t->rt_off, t->rt_pitch, t->z_off,
                                t->z_pitch, t->width, t->height, w), i;
    if (!n)
        return FALSE;
    /* 7: what a Banshee/Voodoo3 has always waited for (6 writes); a VSA-100
     * waits for its 9 */
    if (!VcrDdRoom(pd, n > 7 ? n : 7))
        return FALSE;
    for (i = 0; i < n; i++)
        w3(pd, w[i].off, w[i].val);
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
    if ((r->fogMode & FM_ENABLE) &&
        (!pd->fog_valid || memcmp(pd->fog_loaded, r->fog_table, sizeof pd->fog_loaded))) {
        unsigned char t[VCR_FOG_TABLE_ENTRIES];
        ULONG i;
        vcr_fog_table((int)r->fog_table[0], fbits(r->fog_table[1]), fbits(r->fog_table[2]),
                      fbits(r->fog_table[3]), t);
        for (i = 0; i < 32; i++) {
            if ((i & 7) == 0 && !VcrDdRoom(pd, 8))
                return FALSE;
            w3(pd, V3D_FOGTABLE + 4 * i, vcr_fog_reg(t, (int)i));
        }
        memcpy(pd->fog_loaded, r->fog_table, sizeof pd->fog_loaded);
        pd->fog_valid = 1;
        if (!VcrDdRoom(pd, 4))
            return FALSE;
    }
    if (r->textured) {
        w3(pd, V3D_TMU0 + V3D_TEXTUREMODE, r->textureMode);
        w3(pd, V3D_TMU0 + V3D_TLOD, r->tLOD);
        w3(pd, V3D_TMU0 + V3D_TDETAIL, 0);
        w3(pd, V3D_TMU0 + V3D_TEXBASEADDR, r->texBaseAddr);
    }
    if (r->textured1) {
        if (!VcrDdRoom(pd, 4))
            return FALSE;
        w3(pd, V3D_TMU1 + V3D_TEXTUREMODE, r->textureMode1);
        w3(pd, V3D_TMU1 + V3D_TLOD, r->tLOD1);
        w3(pd, V3D_TMU1 + V3D_TDETAIL, 0);
        w3(pd, V3D_TMU1 + V3D_TEXBASEADDR, r->texBaseAddr1);
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
    ULONG zv;
    if (what & VCR3D_CLEAR_COLOR)
        mode |= FZ_RGBWRITE;
    if ((what & VCR3D_CLEAR_Z) && t->z_off)
        mode |= FZ_ZAWRITE;
    z = fbits(zbits);
    z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
    /* zaColor[23:0] is the depth (h3defs.h SST_ZACOLOR_DEPTH): 16 bits of it
     * at 16 bpp, all 24 at 32 bpp. [31:24] is SST_ZACOLOR_ALPHA - NOT the
     * stencil: with stencilMode 0 (VcrDd3dTarget) a clear leaves the stencil
     * byte of a 24+8 aux buffer untouched, and nothing here ever reads it */
    zv = t->fmt == VCR_RT_32 ? (ULONG)(z * 16777215.0f + 0.5f) : (ULONG)(z * 65535.0f + 0.5f);
    if (zv > vcr_rt_zmax(t->fmt))
        zv = vcr_rt_zmax(t->fmt);
    if (!VcrDdRoom(pd, 3))
        return FALSE;
    w3(pd, V3D_FBZMODE, mode);
    w3(pd, V3D_C1, argb);
    w3(pd, V3D_ZACOLOR, zv);
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
    ULONG argb = d->diff_off ? *(const ULONG *)(v + d->diff_off) : 0xffffffffu;
    if (!VcrDdRoom(pd, 9 + (d->textured ? 3 : 0) + (d->textured1 ? 3 : 0)))
        return FALSE;
    wf(pd, V3D_SVX, p[0] + d->xy_bias);
    wf(pd, V3D_SVY, p[1] + d->xy_bias);
    /* The colour as four FLOATS (0..255), as 3dfx's h5 Glide feeds the setup
     * unit (built GLIDE_PACKED_RGB=0: sRed/sGreen/sBlue/sAlpha, gxdraw.c);
     * the vendor's D3D HAL sends the packed D3DCOLOR, and both work. (A
     * gouraud failure on .124 was blamed on sARGB first; it was the missing
     * PARMADJUST - vcrdd_d3d.c compute_regs.) */
    wf(pd, V3D_SRED, (float)((argb >> 16) & 0xff));
    wf(pd, V3D_SGREEN, (float)((argb >> 8) & 0xff));
    wf(pd, V3D_SBLUE, (float)(argb & 0xff));
    wf(pd, V3D_SALPHA, (float)(argb >> 24));
    wf(pd, V3D_SVZ, p[2] * d->z_scale);
    if (d->fog_vertex) {
        /* the fog factor rides in the specular alpha (255 = no fog): the
         * chip's fog table is a ramp, and 1/W is chosen to land on it */
        ULONG sa = (*(const ULONG *)(v + d->spec_off)) >> 24;
        wf(pd, V3D_SOOWFBI, vcr_fog_ramp_oow(1.0f - (float)sa * (1.0f / 255.0f)));
    } else {
        wf(pd, V3D_SOOWFBI, oow);
    }
    if (d->textured) {
        const float *uv = (const float *)(v + d->tex_off);
        wf(pd, V3D_SOOW0, oow);
        wf(pd, V3D_SSOW0, uv[0] * d->s_scale * oow);
        wf(pd, V3D_STOW0, uv[1] * d->t_scale * oow);
    }
    if (d->textured1) {
        const float *uv = (const float *)(v + d->tex1_off);
        wf(pd, V3D_SOOW1, oow);
        wf(pd, V3D_SSOW1, uv[0] * d->s_scale1 * oow);
        wf(pd, V3D_STOW1, uv[1] * d->t_scale1 * oow);
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

/* The TMU's texture cache does not see the CPU write texture memory. Glide's
 * download-coherency sequence (h3 GR_TEX_FLUSH_PRE/POST): a 2D NOP, the TMUs
 * pointed away and back with a nopCMD between - what the silicon needs; then
 * one dword written back through the texture port at the texture's first
 * texel - the write 86Box's texture cache watches (it ignores LFB writes).
 * The port decodes linearly from TMU0's texBaseAddr on Banshee and later, so
 * the written-back dword lands exactly where it was read. */
BOOL VcrDd3dTexFlush(VCR_PDEV *pd, ULONG base, ULONG addr)
{
    if (!VcrDdRoom(pd, 7))
        return FALSE;
    *(volatile ULONG *)(pd->pjRegs + 0x100070) = 0x100;         /* 2D: NOP | GO */
    w3(pd, V3D_TEXBASEADDR, ~base & 0x00fffff0u);               /* chip field 0: every TMU */
    w3(pd, V3D_NOPCMD, 0);
    w3(pd, V3D_TEXBASEADDR, base);
    *(volatile ULONG *)(pd->pjRegs + 0x100070) = 0x100;
    if (!pd->no_texport && addr >= base && addr - base < 0x200000) {
        ULONG v = *(volatile ULONG *)((PUCHAR)pd->pvRamBase + addr);
        w3(pd, V3D_TMU0 + V3D_TEXBASEADDR, base);
        *(volatile ULONG *)(pd->pjRegs + 0x600000 + (addr - base)) = v;
    }
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

void VcrDd3dTexScale1(vcr3d_draw *d, ULONG w, ULONG h)
{
    ULONG big = w > h ? w : h;
    d->s_scale1 = 256.0f * (float)w / (float)big;
    d->t_scale1 = 256.0f * (float)h / (float)big;
}

/* the depth the setup unit iterates spans the aux buffer: 16 bits, or 24 in
 * 32 bpp (Glide's GR_ZDEPTH_MIN_MAX on a 4-byte pixel). Again at every
 * SETRENDERTARGET: the new target may be the other size. */
void VcrDd3dDrawTarget(vcr3d_draw *d, const vcr3d_target *t)
{
    d->z_scale = t && t->fmt == VCR_RT_32 ? 16777215.0f : 65535.0f;
}

void VcrDd3dDrawInit(vcr3d_draw *d, const vcr3d_target *t)
{
    d->xy_bias = 0.0f;
    d->s_scale = d->t_scale = 256.0f;
    VcrDd3dDrawTarget(d, t);
}

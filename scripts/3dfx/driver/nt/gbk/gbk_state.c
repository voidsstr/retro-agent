/*
 * gbk_state.c - shadow registers + lifted glide state math (pure bitops).
 * Design: docs/3dfx-gbkernel-design.md sec 3 (the gb_* -> glide lift table).
 * Every setter lifts the cited _gr* body verbatim, composed with the
 * gb_* -> gr* argument mapping validated on hardware by the user-mode
 * backend (d3dhal/glidebackend.c:211-249,291-309).
 *
 * Internal color format is GR_COLORFORMAT_ARGB, for which _grSwizzleColor
 * is the identity (diglide.c:233-241, ARGB case is a bare break) - so
 * color values pass through untouched (design sec 3 tail).
 * Kernel-clean: C89, no CRT, no floats.
 */
#include "gbk.h"

/* GR_* argument codes used by the lifted bodies (glide.h; numeric
 * convention documented in gbk.h - kept private to this module).        */
#define GBK_GR_CMP_LESS         0x1   /* glide.h:222                     */
#define GBK_GR_CMP_ALWAYS       0x7   /* glide.h:228                     */
#define GBK_GR_BLEND_ZERO       0x0   /* glide.h:167                     */
#define GBK_GR_BLEND_ONE        0x4   /* glide.h:172                     */
#define GBK_GR_FN_ZERO          0x0   /* GR_COMBINE_FUNCTION_* glide.h:92 */
#define GBK_GR_FN_LOCAL         0x1
#define GBK_GR_FN_SCALE_OTHER   0x3
#define GBK_GR_FAC_ZERO         0x0   /* GR_COMBINE_FACTOR_* glide.h:109 */
#define GBK_GR_FAC_LOCAL        0x1
#define GBK_GR_FAC_TEX_ALPHA    0x4
#define GBK_GR_FAC_TEX_RGB      0x5
#define GBK_GR_FAC_ONE          0x8
#define GBK_GR_LOCAL_ITERATED   0x0   /* GR_COMBINE_LOCAL_* glide.h:128  */
#define GBK_GR_LOCAL_CONSTANT   0x1
#define GBK_GR_OTHER_ITERATED   0x0   /* GR_COMBINE_OTHER_* glide.h:134  */
#define GBK_GR_OTHER_TEXTURE    0x1
#define GBK_GR_OTHER_CONSTANT   0x2

/* PKT3 pmask for our fixed vertex layouts: always Z + Wfbi, never W0
 * (design sec 2; pruning logic _grUpdateParamIndex gglide.c:2504-2536) */
#define GBK_PMASK_UNTEXTURED \
    (SST_SETUP_RGB | SST_SETUP_A | SST_SETUP_Z | SST_SETUP_Wfbi)
#define GBK_PMASK_TEXTURED   (GBK_PMASK_UNTEXTURED | SST_SETUP_ST0)

/* rebuild the precomputed independent-triangle headers after a cull /
 * layout change (_grUpdateTriPacketHdr gglide.c:2743-2745: cullStripHdr
 * completed with nVerts=3 and BDDBDD). The GLIDE_TRI_CULLING sw-cull
 * override (:2747-2755) is intentionally NOT lifted: the kernel uses
 * hardware culling so the draw path stays free of FP math (design sec 2).*/
static void
gbk_update_tri_hdrs(gbk_state_t *s)
{
    s->triHdrUntextured = gbk_pkt3_hdr(s->smode, GBK_PMASK_UNTEXTURED,
                                       3, SSTCP_PKT3_BDDBDD);
    s->triHdrTextured   = gbk_pkt3_hdr(s->smode, GBK_PMASK_TEXTURED,
                                       3, SSTCP_PKT3_BDDBDD);
}

/* _grColorCombine (gglide.c:1519-1640) lifted verbatim for the function/
 * factor/local/other combos the gb_* seam produces.                     */
static void
gbk_color_combine(gbk_state_t *s, h3u32 function, h3u32 factor,
                  h3u32 local, h3u32 other, int invert)
{
    h3u32 fbzColorPath = s->fbzColorPath;

    fbzColorPath &= ~(SST_ENTEXTUREMAP |
                      SST_RGBSELECT |
                      SST_LOCALSELECT |
                      SST_CC_ZERO_OTHER |
                      SST_CC_SUB_CLOCAL |
                      SST_CC_MSELECT |
                      SST_CC_REVERSE_BLEND |
                      SST_CC_ADD_CLOCAL |
                      SST_CC_ADD_ALOCAL |
                      SST_CC_INVERT_OUTPUT);

    /* "this is bogus, it should be done once" - glide always ORs it */
    fbzColorPath |= SST_PARMADJUST;

    /* reverse blending from the factor high bit, then strip it (:1564) */
    if ((factor & 0x8) == 0)
        fbzColorPath |= SST_CC_REVERSE_BLEND;
    factor &= 0x7;

    s->ccRequiresTex = (factor == GBK_GR_FAC_TEX_ALPHA)
                     | (factor == GBK_GR_FAC_TEX_RGB)
                     | (other == GBK_GR_OTHER_TEXTURE);

    fbzColorPath |= factor << SST_CC_MSELECT_SHIFT;
    fbzColorPath |= local << SST_LOCALSELECT_SHIFT;
    fbzColorPath |= other << SST_RGBSELECT_SHIFT;
    if (invert)
        fbzColorPath |= SST_CC_INVERT_OUTPUT;

    /* core color-combine unit bits (:1590-1633); only the cases the gb_*
     * seam reaches (ZERO, LOCAL, SCALE_OTHER) are needed               */
    switch (function) {
    case GBK_GR_FN_ZERO:
        fbzColorPath |= SST_CC_ZERO_OTHER;
        break;
    case GBK_GR_FN_LOCAL:
        fbzColorPath |= SST_CC_ZERO_OTHER | SST_CC_ADD_CLOCAL;
        break;
    case GBK_GR_FN_SCALE_OTHER:
    default:
        break;
    }

    /* "if either color or alpha combine requires texture then enable" */
    if (s->ccRequiresTex || s->acRequiresTex)
        fbzColorPath |= SST_ENTEXTUREMAP;
    s->fbzColorPath = fbzColorPath;
    s->dirty |= GBK_DIRTY_fbzColorPath;
}

/* _grAlphaCombine (gglide.c:520-624) lifted verbatim (same shape as the
 * color unit, CCA fields + ASELECT/ALOCALSELECT).                       */
static void
gbk_alpha_combine(gbk_state_t *s, h3u32 function, h3u32 factor,
                  h3u32 local, h3u32 other, int invert)
{
    h3u32 fbzColorPath = s->fbzColorPath;

    fbzColorPath &= ~(SST_ENTEXTUREMAP |
                      SST_ASELECT |
                      SST_ALOCALSELECT |
                      SST_CCA_ZERO_OTHER |
                      SST_CCA_SUB_CLOCAL |
                      SST_CCA_MSELECT |
                      SST_CCA_REVERSE_BLEND |
                      SST_CCA_ADD_CLOCAL |
                      SST_CCA_ADD_ALOCAL |
                      SST_CCA_INVERT_OUTPUT);

    if ((factor & 0x8) == 0)
        fbzColorPath |= SST_CCA_REVERSE_BLEND;
    factor &= 0x7;

    s->acRequiresTex = (factor == GBK_GR_FAC_TEX_ALPHA)
                     | (other == GBK_GR_OTHER_TEXTURE);

    fbzColorPath |= factor << SST_CCA_MSELECT_SHIFT;
    fbzColorPath |= local << SST_ALOCALSELECT_SHIFT;
    fbzColorPath |= other << SST_ASELECT_SHIFT;
    if (invert)
        fbzColorPath |= SST_CCA_INVERT_OUTPUT;

    switch (function) {
    case GBK_GR_FN_ZERO:
        fbzColorPath |= SST_CCA_ZERO_OTHER;
        break;
    case GBK_GR_FN_LOCAL:
        fbzColorPath |= SST_CCA_ZERO_OTHER | SST_CCA_ADD_CLOCAL;
        break;
    case GBK_GR_FN_SCALE_OTHER:
    default:
        break;
    }

    if (s->ccRequiresTex || s->acRequiresTex)
        fbzColorPath |= SST_ENTEXTUREMAP;
    s->fbzColorPath = fbzColorPath;
    s->dirty |= GBK_DIRTY_fbzColorPath;
}

/* _grDepthBufferMode (gglide.c:1823-1874); the FLOAT_SEL fog-coord path
 * (:1857-1859) never triggers: our vertex layout has no fog coord.      */
static void
gbk_depth_mode(gbk_state_t *s, int wbuffer_on)
{
    h3u32 fbzMode = s->fbzMode;

    fbzMode &= ~(SST_ENDEPTHBUFFER |
                 SST_WBUFFER |
                 SST_ENZBIAS |
                 SST_ZCOMPARE_TO_ZACOLOR |
                 SST_DEPTH_FLOAT_SEL);
    if (wbuffer_on)   /* GR_DEPTHBUFFER_WBUFFER case (:1848-1854) */
        fbzMode |= SST_ENDEPTHBUFFER | SST_WBUFFER | SST_ENZBIAS;

    s->fbzMode = fbzMode;
    s->dirty |= GBK_DIRTY_fbzMode;
}

/* _grDepthBufferFunction (gglide.c:1802-1817): func field [7:5] */
static void
gbk_depth_func(gbk_state_t *s, int func)
{
    s->fbzMode = (s->fbzMode & ~SST_ZFUNC)
               | (((h3u32)func & 0x7) << SST_ZFUNC_SHIFT);
    s->dirty |= GBK_DIRTY_fbzMode;
}

/* grDepthMask via the distate write-mask block (distate.c:986-1003):
 * enableDepthMask -> SST_ZAWRMASK (RGBWRMASK is managed separately)     */
static void
gbk_depth_writemask(gbk_state_t *s, int write)
{
    if (write)
        s->fbzMode |= SST_ZAWRMASK;
    else
        s->fbzMode &= ~SST_ZAWRMASK;
    s->dirty |= GBK_DIRTY_fbzMode;
}

/* grTexCombine (gtex.c:717-948) lifted for the two combos the seam uses:
 * decal (LOCAL,NONE,LOCAL,NONE - glidebackend.c:293-294) and the open
 * default (ZERO,NONE,ZERO,NONE - assertDefaultState gsst.c:664-666).    */
static void
gbk_tex_combine(gbk_state_t *s, h3u32 function)
{
    h3u32 texturemode = s->textureMode;

    texturemode &= ~(SST_TCOMBINE | SST_TACOMBINE);
    /* factor NONE (=ZERO, high bit clear) -> mselect 0 + reverse blend
     * for both units (gtex.c:726-735)                                   */
    texturemode |= SST_TC_REVERSE_BLEND | SST_TCA_REVERSE_BLEND;
    switch (function) {
    case GBK_GR_FN_ZERO:     /* rgb+alpha function ZERO (gtex.c:746-749) */
        texturemode |= SST_TC_ZERO_OTHER | SST_TCA_ZERO_OTHER;
        break;
    case GBK_GR_FN_LOCAL:    /* decal (gtex.c:751-755, alpha :811-815)   */
        texturemode |= SST_TC_ZERO_OTHER | SST_TC_ADD_CLOCAL |
                       SST_TCA_ZERO_OTHER | SST_TCA_ADD_CLOCAL;
        break;
    default:
        break;
    }

    s->textureMode = texturemode;
    s->dirty |= GBK_DIRTY_textureMode;
}

/* assertDefaultState() (gsst.c:602-672) replayed through the lifted
 * bodies in glide's call order, on top of the initial register state
 * fbzMode = ENRECTCLIP|ENZBIAS (gsst.c:1499).                           */
void
gbk_state_defaults(gbk_state_t *s, int hasDepthBuffer)
{
    s->fbzColorPath = 0;
    s->fogMode = 0;
    s->alphaMode = 0;
    s->fbzMode = SST_ENRECTCLIP | SST_ENZBIAS;   /* gsst.c:1499 */
    s->fogColor = 0;
    s->zaColor = 0;
    s->chromaKey = 0;
    s->c0 = 0;
    s->c1 = 0;
    s->textureMode = 0;
    s->tLOD = 0;
    s->texBaseAddr = 0;
    s->smode = 0;
    s->dirty = 0;
    s->texEnabled = 0;
    s->hasAux = hasDepthBuffer;
    s->ccRequiresTex = 0;
    s->acRequiresTex = 0;

    /* grAlphaBlendFunction(ONE, ZERO, ONE, ZERO)   (gsst.c:608-609) */
    gbk_state_set_blend(s, 0, GBK_GR_BLEND_ONE, GBK_GR_BLEND_ZERO);
    /* grAlphaTestFunction(ALWAYS) + ReferenceValue(0)  (:610-611)   */
    gbk_state_set_alphatest(s, 0, GBK_GR_CMP_ALWAYS, 0);
    /* grChromakeyMode(DISABLE)                     (:612)           */
    gbk_state_set_chromakey(s, 0, 0);
    /* grConstantColorValue(~0)                     (:615)           */
    gbk_state_set_texfactor(s, 0xFFFFFFFFUL);
    /* grColorCombine(SCALE_OTHER, ONE, ITERATED, ITERATED, 0) (:616-620) */
    gbk_color_combine(s, GBK_GR_FN_SCALE_OTHER, GBK_GR_FAC_ONE,
                      GBK_GR_LOCAL_ITERATED, GBK_GR_OTHER_ITERATED, 0);
    /* grAlphaCombine(SCALE_OTHER, ONE, LOCAL_NONE, OTHER_CONSTANT, 0)
     * (:621-625; LOCAL_NONE==LOCAL_CONSTANT, OTHER_CONSTANT glide.h:130,136) */
    gbk_alpha_combine(s, GBK_GR_FN_SCALE_OTHER, GBK_GR_FAC_ONE,
                      GBK_GR_LOCAL_CONSTANT, GBK_GR_OTHER_CONSTANT, 0);
    /* grColorMask(TRUE, FALSE): RGBWRMASK on (distate.c:986)        */
    s->fbzMode |= SST_RGBWRMASK;
    /* grCullMode(DISABLE)                          (:627)           */
    gbk_state_set_cull(s, 0);
    /* grDepthBiasLevel(0): zaColor depth field = 0  (:628, already 0) */
    /* grDepthMask(FALSE)                           (:629)           */
    gbk_depth_writemask(s, 0);
    /* grDepthBufferMode(DISABLE)                   (:630)           */
    gbk_depth_mode(s, 0);
    /* grDepthBufferFunction(LESS)                  (:631)           */
    gbk_depth_func(s, GBK_GR_CMP_LESS);
    /* grDitherMode(GR_DITHER_4x4)                  (:633)           */
    gbk_state_set_dither(s, 1);
    /* grFogMode(DISABLE) + grFogColorValue(0)      (:634-635)       */
    gbk_state_set_fog(s, 0, 0);
    s->fogColor = 0;
    /* TMU0 defaults (:658-667): clamp CLAMP/CLAMP, filter POINT/POINT */
    gbk_state_set_texfilter(s, 0, 1, 1);
    /* grTexCombine(TMU0, ZERO, NONE, ZERO, NONE, 0, 0) (:664-666)   */
    gbk_tex_combine(s, GBK_GR_FN_ZERO);

    /* everything must reach the hardware on the first flush */
    s->dirty = GBK_DIRTY_fbzColorPath | GBK_DIRTY_fogMode |
               GBK_DIRTY_alphaMode | GBK_DIRTY_fbzMode |
               GBK_DIRTY_fogColor | GBK_DIRTY_zaColor |
               GBK_DIRTY_chromaKey | GBK_DIRTY_c0 | GBK_DIRTY_c1 |
               GBK_DIRTY_textureMode | GBK_DIRTY_tLOD |
               GBK_DIRTY_texBaseAddr;
}

/* gb_set_depth (glidebackend.c:219-222):
 * grDepthBufferMode(enable ? WBUFFER : DISABLE) then, when enabled,
 * grDepthBufferFunction(func) + grDepthMask(write). Enabling without an
 * allocated aux buffer is refused - "they could stomp on the cmd fifo"
 * (gglide.c:1830-1833; design sec 6 risk 2).                            */
void
gbk_state_set_depth(gbk_state_t *s, int enable, int func, int write)
{
    if (enable && !s->hasAux)
        enable = 0;
    gbk_depth_mode(s, enable);
    if (enable) {
        gbk_depth_func(s, func);
        gbk_depth_writemask(s, write);
    } else {
        gbk_depth_writemask(s, 0);   /* no aux writes while disabled */
    }
}

/* gb_set_blend (glidebackend.c:223-226) -> _grAlphaBlendFunction
 * (gglide.c:472-514): alpha channel factors fixed at ONE/ZERO (the only
 * ones h3 supports, :483-491); ENALPHABLEND cleared exactly for the
 * pass-through combo (:493-497); four 4-bit factor fields (:499-504).   */
void
gbk_state_set_blend(gbk_state_t *s, int enable, int srcFact, int dstFact)
{
    h3u32 alphamode = s->alphaMode;
    h3u32 rgb_sf, rgb_df;

    if (!enable) {
        rgb_sf = GBK_GR_BLEND_ONE;
        rgb_df = GBK_GR_BLEND_ZERO;
    } else {
        rgb_sf = (h3u32)srcFact;
        rgb_df = (h3u32)dstFact;
    }

    if (rgb_sf == GBK_GR_BLEND_ONE && rgb_df == GBK_GR_BLEND_ZERO)
        alphamode &= ~SST_ENALPHABLEND;
    else
        alphamode |= SST_ENALPHABLEND;

    alphamode &= ~(SST_RGBSRCFACT | SST_RGBDSTFACT |
                   SST_ASRCFACT | SST_ADSTFACT);
    alphamode |= (rgb_sf << SST_RGBSRCFACT_SHIFT)
               | (rgb_df << SST_RGBDSTFACT_SHIFT)
               | ((h3u32)GBK_GR_BLEND_ONE << SST_ASRCFACT_SHIFT)
               | ((h3u32)GBK_GR_BLEND_ZERO << SST_ADSTFACT_SHIFT);

    s->alphaMode = alphamode;
    s->dirty |= GBK_DIRTY_alphaMode;
}

/* gb_set_alphatest (glidebackend.c:227-230) -> _grAlphaTestFunction
 * (gglide.c:662-682: ALWAYS clears ENALPHAFUNC, anything else sets
 * func[3:1] + enable) and _grAlphaTestReferenceValue (:688-707).        */
void
gbk_state_set_alphatest(gbk_state_t *s, int enable, int func, h3u32 ref)
{
    h3u32 alphamode = s->alphaMode;
    h3u32 fnc = enable ? (h3u32)func : (h3u32)GBK_GR_CMP_ALWAYS;

    alphamode &= ~(SST_ALPHAFNC | SST_ENALPHAFUNC);
    if (fnc != GBK_GR_CMP_ALWAYS)
        alphamode |= (fnc << SST_ALPHAFNC_SHIFT) | SST_ENALPHAFUNC;

    alphamode &= ~SST_ALPHAREF;
    alphamode |= (ref & 0xFFUL) << SST_ALPHAREF_SHIFT;

    s->alphaMode = alphamode;
    s->dirty |= GBK_DIRTY_alphaMode;
}

/* gb_set_fog (glidebackend.c:231-239) -> _grFogMode (gglide.c:2012-2075):
 * enable = GR_FOG_WITH_TABLE_ON_W (table on 1/w, ENFOGGING only,
 * :2057-2059), disable = GR_FOG_DISABLE; FOG_DITHER|FOG_ZONES are always
 * ORed in ("always safe to enable", :2064-2066); plus _grFogColorValue
 * (:1673-1689, swizzle = identity). The fog TABLE itself arrives
 * precomputed from user mode (design sec 3) via gbk_fog_pack_entry.     */
void
gbk_state_set_fog(gbk_state_t *s, int enable, h3u32 rgb)
{
    h3u32 fogmode = s->fogMode;

    fogmode &= ~(SST_ENFOGGING | SST_FOGADD | SST_FOGMULT |
                 SST_FOG_Z | SST_FOG_CONSTANT);
    if (enable)
        fogmode |= SST_ENFOGGING;
    fogmode |= SST_FOG_DITHER | SST_FOG_ZONES;

    s->fogMode = fogmode;
    s->dirty |= GBK_DIRTY_fogMode;
    if (enable) {
        s->fogColor = rgb;
        s->dirty |= GBK_DIRTY_fogColor;
    }
}

/* fogTable word packing (grFogTable gglide.c:2096-2113):
 * d0 = (e1-e0)<<2 (.2 format), d1 = (eNext-e1)<<2, both truncated to the
 * 8-bit GrFog_t; word = (e1<<24)|(d1<<16)|(e0<<8)|d0. Last word: eNext
 * passed == e1 so d1 = 0 (":don't access beyond end of table", :2103).  */
h3u32
gbk_fog_pack_entry(h3u32 e0, h3u32 e1, h3u32 eNext)
{
    h3u32 d0 = ((e1 - e0) << 2) & 0xFFUL;
    h3u32 d1 = ((eNext - e1) << 2) & 0xFFUL;

    return ((e1 & 0xFFUL) << 24) | (d1 << 16) | ((e0 & 0xFFUL) << 8) | d0;
}

/* gb_set_cull (glidebackend.c:240-243: NONE->DISABLE, CW->POSITIVE,
 * CCW->NEGATIVE) -> grCullMode + _grUpdateTriPacketHdr (gglide.c:2717-
 * 2726): culling off = kSetupPingPongDisable alone; on = kSetupCullEnable
 * | (POSITIVE -> kSetupCullPositive(0) : kSetupCullNegative). PKT3
 * header smode bits, NOT a register write (design sec 3).               */
void
gbk_state_set_cull(gbk_state_t *s, int cull)
{
    h3u32 smode;

    if (cull == 0) {
        smode = kSetupPingPongDisable;
    } else {
        smode = kSetupCullEnable;
        if (cull != 1)               /* CCW = GR_CULL_NEGATIVE */
            smode |= kSetupCullNegative;
    }
    s->smode = smode;
    gbk_update_tri_hdrs(s);
}

/* gb_set_dither (glidebackend.c:244) -> _grDitherMode (gglide.c:1965-
 * 2006): h3 forces the 2x2 matrix + dither subtraction for any enabled
 * mode (:1984-1991; disableDitherSub env defaults off).                 */
void
gbk_state_set_dither(gbk_state_t *s, int enable)
{
    h3u32 fbzMode = s->fbzMode;

    fbzMode &= ~(SST_ENDITHER | SST_DITHER2x2 | SST_ENDITHERSUBTRACT);
    if (enable)
        fbzMode |= SST_ENDITHER | SST_DITHER2x2 | SST_ENDITHERSUBTRACT;

    s->fbzMode = fbzMode;
    s->dirty |= GBK_DIRTY_fbzMode;
}

/* gb_set_chromakey (glidebackend.c:245-248) -> _grChromakeyMode
 * (gglide.c:1392-1407) + _grChromakeyValue (:1735-1745, swizzle = id).  */
void
gbk_state_set_chromakey(gbk_state_t *s, int enable, h3u32 rgb)
{
    if (enable)
        s->fbzMode |= SST_ENCHROMAKEY;
    else
        s->fbzMode &= ~SST_ENCHROMAKEY;
    s->dirty |= GBK_DIRTY_fbzMode;

    if (enable) {
        s->chromaKey = rgb;
        s->dirty |= GBK_DIRTY_chromaKey;
    }
}

/* gb_set_shade (glidebackend.c:211-218): grColorCombine(LOCAL, NONE,
 * gouraud ? ITERATED : CONSTANT, OTHER_NONE, 0).                        */
void
gbk_state_set_shade(gbk_state_t *s, int gouraud)
{
    gbk_color_combine(s, GBK_GR_FN_LOCAL, GBK_GR_FAC_ZERO,
                      gouraud ? GBK_GR_LOCAL_ITERATED
                              : GBK_GR_LOCAL_CONSTANT,
                      GBK_GR_OTHER_CONSTANT /* == OTHER_NONE */, 0);
}

/* gb_set_texfactor (glidebackend.c:249) -> _grConstantColorValue
 * (gglide.c:1753-1775): c0 AND c1 both take the color (swizzle = id).   */
void
gbk_state_set_texfactor(gbk_state_t *s, h3u32 argb)
{
    s->c0 = argb;
    s->c1 = argb;
    s->dirty |= GBK_DIRTY_c0 | GBK_DIRTY_c1;
}

/* gb_set_texfilter (glidebackend.c:303-309) -> grTexFilterMode
 * (gtex.c:970-991: min/mag bilinear bits) + grTexClampMode (gtex.c:636-
 * 673: clamp_u -> S, clamp_v -> T; no mirror, so tLOD untouched).       */
void
gbk_state_set_texfilter(gbk_state_t *s, int bilinear, int clampU, int clampV)
{
    h3u32 texMode = s->textureMode;

    texMode &= ~(SST_TMINFILTER | SST_TMAGFILTER);
    if (bilinear)
        texMode |= SST_TMINFILTER | SST_TMAGFILTER;

    texMode &= ~(SST_TCLAMPS | SST_TCLAMPT);
    if (clampU)
        texMode |= SST_TCLAMPS;
    if (clampV)
        texMode |= SST_TCLAMPT;

    s->textureMode = texMode;
    s->dirty |= GBK_DIRTY_textureMode;
}

/* Texturing on/off = the color-combine change gb_tex_bind/gb_tex_none
 * make (glidebackend.c:291-300):
 *  on:  grColorCombine(SCALE_OTHER, FACTOR_LOCAL, ITERATED, TEXTURE, 0)
 *       + grTexCombine decal (LOCAL, NONE, LOCAL, NONE)
 *  off: grColorCombine(LOCAL, NONE, ITERATED, OTHER_NONE, 0).
 * Returns nonzero when SST_ENTEXTUREMAP flipped (info; the nopCMD=0 the
 * transition requires - distate.c:910-915 - is emitted by flush).       */
int
gbk_state_set_texenable(gbk_state_t *s, int enable)
{
    h3u32 before = s->fbzColorPath & SST_ENTEXTUREMAP;

    if (enable) {
        gbk_color_combine(s, GBK_GR_FN_SCALE_OTHER, GBK_GR_FAC_LOCAL,
                          GBK_GR_LOCAL_ITERATED, GBK_GR_OTHER_TEXTURE, 0);
        gbk_tex_combine(s, GBK_GR_FN_LOCAL);
    } else {
        gbk_color_combine(s, GBK_GR_FN_LOCAL, GBK_GR_FAC_ZERO,
                          GBK_GR_LOCAL_ITERATED, GBK_GR_OTHER_CONSTANT, 0);
    }

    return (before ^ (s->fbzColorPath & SST_ENTEXTUREMAP)) != 0;
}

/* current PKT3 triangle header (design sec 3 tail): the precomputed
 * 3-vertex header for the common case, re-encoded for batches.          */
h3u32
gbk_state_tri_hdr(const gbk_state_t *s, int textured, h3u32 nVerts)
{
    if (nVerts == 3)
        return textured ? s->triHdrTextured : s->triHdrUntextured;
    return gbk_pkt3_hdr(s->smode,
                        textured ? GBK_PMASK_TEXTURED : GBK_PMASK_UNTEXTURED,
                        nVerts, SSTCP_PKT3_BDDBDD);
}

/* append one PKT4 group to out[]: header + one value per set mask bit,
 * ascending (h3gdefs.h:240-243). vals[] is indexed by register slot.    */
static int
gbk_emit_pkt4(h3u32 *out, int pos, int maxWords,
              h3u32 baseRegByteOff, h3u32 mask, h3u32 chipField,
              const h3u32 *vals, h3u32 nSlots)
{
    h3u32 slot;

    if (mask == 0)
        return pos;
    if (pos + 1 + (int)gbk_pkt4_nwords(mask) > maxWords)
        return -1;
    out[pos++] = gbk_pkt4_hdr(baseRegByteOff, mask, chipField, 0);
    for (slot = 0; slot < nSlots; slot++) {
        if (mask & (1UL << slot))
            out[pos++] = vals[slot];
    }
    return pos;
}

/* Serialize the dirty shadow as FIFO words (design sec 2 "State flush"):
 *  - PKT1 nopCMD=0 first when ENTEXTUREMAP flipped since the last flush
 *    (distate.c:910-915: "transition into/out of texturing"),
 *  - PKT4 group base fbzColorPath for the common FBI registers
 *    (_grFlushCommonStateRegs gglide.c:2210-2231; register slots within
 *    the group = (SST3D_OFF_reg - SST3D_OFF_fbzColorPath) >> 2),
 *  - PKT4 group base c0 for the constant colors (glide uses base stipple
 *    :2233-2239; base c0 is the same packet mechanism minus the
 *    never-dirty stipple slot),
 *  - PKT4 TMU group base textureMode, chip-field TMU0
 *    (gglide.c:2287-2289; slots textureMode=0, tLOD=1, texBaseAddr=3).  */
int
gbk_state_flush(gbk_state_t *s, h3u32 *out, int maxWords)
{
    h3u32 common[14] = {0};
    h3u32 colors[2] = {0};
    h3u32 tmu[4] = {0};
    h3u32 commonMask = 0, colorMask = 0, tmuMask = 0;
    int texNow, pos = 0;

    if (s->dirty == 0)
        return 0;

    /* texture-enable transition needs a pipeline drain before the new
     * fbzColorPath lands (distate.c:910-915)                            */
    texNow = (s->fbzColorPath & SST_ENTEXTUREMAP) != 0;
    if (texNow != s->texEnabled) {
        if (pos + 2 > maxWords)
            return -1;
        out[pos++] = gbk_pkt1_hdr(SST3D_OFF_nopCMD, 1, eChipBroadcast, 0);
        out[pos++] = 0;
    }

    /* common FBI group, base fbzColorPath (0x104): slot = (off-0x104)>>2 */
    if (s->dirty & GBK_DIRTY_fbzColorPath) {
        commonMask |= 1UL << 0;  common[0]  = s->fbzColorPath;
    }
    if (s->dirty & GBK_DIRTY_fogMode) {
        commonMask |= 1UL << 1;  common[1]  = s->fogMode;
    }
    if (s->dirty & GBK_DIRTY_alphaMode) {
        commonMask |= 1UL << 2;  common[2]  = s->alphaMode;
    }
    if (s->dirty & GBK_DIRTY_fbzMode) {
        commonMask |= 1UL << 3;  common[3]  = s->fbzMode;
    }
    if (s->dirty & GBK_DIRTY_fogColor) {
        commonMask |= 1UL << 10; common[10] = s->fogColor;
    }
    if (s->dirty & GBK_DIRTY_zaColor) {
        commonMask |= 1UL << 11; common[11] = s->zaColor;
    }
    if (s->dirty & GBK_DIRTY_chromaKey) {
        commonMask |= 1UL << 12; common[12] = s->chromaKey;
    }
    pos = gbk_emit_pkt4(out, pos, maxWords, SST3D_OFF_fbzColorPath,
                        commonMask, eChipBroadcast, common, 14);
    if (pos < 0)
        return -1;

    /* constant colors, base c0 (0x144): slots c0=0, c1=1 */
    if (s->dirty & GBK_DIRTY_c0) { colorMask |= 1UL << 0; colors[0] = s->c0; }
    if (s->dirty & GBK_DIRTY_c1) { colorMask |= 1UL << 1; colors[1] = s->c1; }
    pos = gbk_emit_pkt4(out, pos, maxWords, SST3D_OFF_c0,
                        colorMask, eChipBroadcast, colors, 2);
    if (pos < 0)
        return -1;

    /* TMU group, base textureMode (0x300), chip-field TMU0 */
    if (s->dirty & GBK_DIRTY_textureMode) {
        tmuMask |= 1UL << 0; tmu[0] = s->textureMode;
    }
    if (s->dirty & GBK_DIRTY_tLOD) {
        tmuMask |= 1UL << 1; tmu[1] = s->tLOD;
    }
    if (s->dirty & GBK_DIRTY_texBaseAddr) {
        tmuMask |= 1UL << 3; tmu[3] = s->texBaseAddr;
    }
    pos = gbk_emit_pkt4(out, pos, maxWords, SST3D_OFF_textureMode,
                        tmuMask, eChipTMU0, tmu, 4);
    if (pos < 0)
        return -1;

    s->texEnabled = texNow;
    s->dirty = 0;
    return pos;
}

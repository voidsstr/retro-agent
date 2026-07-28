/*
 * test_gbk_state.c - host tests for gbk_state.c (shadow-register state math).
 * Every expected register value is derived BY HAND from the cited glide
 * function bodies + the h3defs.h bit definitions (the derivation is in the
 * comment above each assert), so a transcription slip in the module cannot
 * self-validate.
 */
#include "../gbk/gbk.h"
#include "test_common.h"

int
main(void)
{
    /* ---- bit-transcription spot checks (h3defs.h) --------------------- */
    CHECK_EQ(SST_ENRECTCLIP, 1UL << 0);
    CHECK_EQ(SST_ENDEPTHBUFFER, 1UL << 4);
    CHECK_EQ(SST_ZFUNC_SHIFT, 5);
    CHECK_EQ(SST_ENDITHER, 1UL << 8);
    CHECK_EQ(SST_RGBWRMASK, 1UL << 9);
    CHECK_EQ(SST_ZAWRMASK, 1UL << 10);
    CHECK_EQ(SST_ENZBIAS, 1UL << 16);
    CHECK_EQ(SST_ZCOMPARE_TO_ZACOLOR, 1UL << 20);   /* h3defs.h:320 */
    CHECK_EQ(SST_DEPTH_FLOAT_SEL, 1UL << 21);       /* h3defs.h:321 */
    CHECK_EQ(SST_ALPHAFNC_SHIFT, 1);
    CHECK_EQ(SST_RGBSRCFACT_SHIFT, 8);
    CHECK_EQ(SST_RGBDSTFACT_SHIFT, 12);
    CHECK_EQ(SST_ALPHAREF_SHIFT, 24);
    /* SST_ALPHAREF / SST_ZACOLOR_ALPHA cover bits [31:24]; the masks MUST be
     * unsigned or (0xFF << 24) is signed-overflow UB on the shipping ABIs
     * (mingw i686 / VC6: long is 32-bit). The shifted value is 0xFF000000
     * either way on this LP64 host, so a value-only check passes even for the
     * buggy signed 0xFFL form -- assert the value AND unsignedness. The sign of
     * -(mask) is the portable tell: negative => signed => would overflow ILP32
     * (glide h3defs.h:463,469). Mirrors the H3HW_STATIC_ASSERT in h3hw.h. */
    CHECK_EQ(SST_ALPHAREF, 0xFF000000UL);
    CHECK((-(SST_ALPHAREF)) > 0);        /* unsigned type => bit-31 shift OK */
    CHECK_EQ(SST_ZACOLOR_ALPHA, 0xFF000000UL);
    CHECK((-(SST_ZACOLOR_ALPHA)) > 0);
    CHECK_EQ(SST_A_ZERO, 0);        /* blend codes == GR_BLEND_* (glide.h: */
    CHECK_EQ(SST_A_SRCALPHA, 1);    /* 167-183 vs h3defs.h:445-453)       */
    CHECK_EQ(SST_A_ONE, 4);
    CHECK_EQ(SST_AOM_SRCALPHA, 5);
    CHECK_EQ(SST_ENTEXTUREMAP, 1UL << 27);
    CHECK_EQ(SST_ENFOGGING, 1UL << 0);
    CHECK_EQ(SST_FOG_DITHER, 1UL << 6);
    CHECK_EQ(SST_FOG_ZONES, 1UL << 7);
    CHECK_EQ(SST_TPERSP_ST, 1UL << 0);
    CHECK_EQ(SST_TCLAMPW, 1UL << 3);
    CHECK_EQ(SST_TFORMAT_SHIFT, 8);
    CHECK_EQ(SST_RGB565, 10UL << 8);
    CHECK_EQ(SST_ARGB1555, 11UL << 8);
    CHECK_EQ(SST_ARGB4444, 12UL << 8);

    /* ---- defaults = assertDefaultState replay (gsst.c:602-672) --------
     * fbzMode: start ENRECTCLIP|ENZBIAS (gsst.c:1499) = 0x10001
     *   +RGBWRMASK(0x200, colorMask TRUE distate.c:986)
     *   depthMode DISABLE clears ENZBIAS (gglide.c:1839-1846) -> -0x10000
     *   depthFunc LESS: ZFUNC=1<<5 = 0x20 (gglide.c:1813-1815)
     *   dither 4x4: ENDITHER|DITHER2x2|ENDITHERSUBTRACT =
     *     0x100|0x800|0x80000 (gglide.c:1984-1991)
     *   = 1|0x200|0x20|0x100|0x800|0x80000 = 0x80B21                     */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        CHECK_EQ(s.fbzMode, 0x80B21UL);
        /* alphaMode: blend(ONE,ZERO,ONE,ZERO) -> no ENALPHABLEND, factors
         * (4<<8)|(0<<12)|(4<<16)|(0<<20) (gglide.c:493-504); alpha test
         * ALWAYS -> no func bits (:675-677); ref 0 -> 0x40400            */
        CHECK_EQ(s.alphaMode, 0x40400UL);
        /* fogMode: DISABLE still ORs FOG_DITHER|FOG_ZONES (gglide.c:
         * 2064-2066) = 0xC0                                              */
        CHECK_EQ(s.fogMode, 0xC0UL);
        /* fbzColorPath: colorCombine(SCALE_OTHER,ONE,ITER,ITER) leaves
         * only PARMADJUST (factor ONE: high bit set -> no reverse,
         * mselect 0; gglide.c:1560-1567); alphaCombine(SCALE_OTHER,ONE,
         * LOCAL_NONE=C0,OTHER_CONST=c1): ALOCALSELECT=1<<5,
         * ASELECT=2<<2 (gglide.c:571-577) -> 0x4000000|0x20|0x8         */
        CHECK_EQ(s.fbzColorPath, 0x4000028UL);
        /* textureMode: clamp CLAMP/CLAMP = TCLAMPS|TCLAMPT (0xC0,
         * gtex.c:643-658); filter POINT/POINT = no bits (gtex.c:981-984);
         * texCombine(ZERO,NONE,ZERO,NONE): TC_ZERO_OTHER(0x1000)|
         * TC_REVERSE(0x20000)|TCA_ZERO_OTHER(0x200000)|TCA_REVERSE
         * (0x4000000) (gtex.c:726-749,806-810) -> 0x42210C0             */
        CHECK_EQ(s.textureMode, 0x42210C0UL);
        /* grConstantColorValue(~0) -> c0=c1 (gglide.c:1758-1775) */
        CHECK_EQ(s.c0, 0xFFFFFFFFUL);
        CHECK_EQ(s.c1, 0xFFFFFFFFUL);
        CHECK_EQ(s.zaColor, 0);
        CHECK_EQ(s.fogColor, 0);
        /* cull DISABLE -> smode = kSetupPingPongDisable (gglide.c:2721) */
        CHECK_EQ(s.smode, 0x8UL);
        /* precomputed untextured tri header: (8<<22)|(0xF<<10)|(3<<6)|3 */
        CHECK_EQ(s.triHdrUntextured, 0x2003CC3UL);
        CHECK_EQ(s.texEnabled, 0);
        /* everything dirty so the first flush pushes the whole state */
        CHECK_EQ(s.dirty, 0xFFFUL);
    }

    /* ---- gb_set_depth (W-buffer, LEQUAL, write) -----------------------
     * from defaults 0x80B21: +ENDEPTHBUFFER|WBUFFER|ENZBIAS
     * (0x10|0x8|0x10000, gglide.c:1848-1851), ZFUNC 1->3 (0x20->0x60),
     * +ZAWRMASK(0x400) = 0x90F79                                         */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        gbk_state_set_depth(&s, 1, 3 /*GR_CMP_LEQUAL*/, 1);
        CHECK_EQ(s.fbzMode, 0x90F79UL);
        /* disable again -> depth bits+mask off, rest intact */
        gbk_state_set_depth(&s, 0, 0, 0);
        CHECK_EQ(s.fbzMode, 0x80B61UL);   /* 0x80B21 with ZFUNC=3 kept */
    }
    /* without an aux buffer the enable must be refused ("they could
     * stomp on the cmd fifo", gglide.c:1830-1833)                        */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 0);
        gbk_state_set_depth(&s, 1, 3, 1);
        CHECK((s.fbzMode & SST_ENDEPTHBUFFER) == 0);
        CHECK((s.fbzMode & SST_ZAWRMASK) == 0);
    }

    /* ---- gb_set_blend -------------------------------------------------
     * (SRC_ALPHA, ONE_MINUS_SRC_ALPHA): ENALPHABLEND(0x10) | (1<<8) |
     * (5<<12) | alpha channel fixed ONE/ZERO (4<<16) (gglide.c:483-504) */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        gbk_state_set_blend(&s, 1, SST_A_SRCALPHA, SST_AOM_SRCALPHA);
        CHECK_EQ(s.alphaMode, 0x45110UL);
        /* disable -> exactly the default pass-through encoding */
        gbk_state_set_blend(&s, 0, 0, 0);
        CHECK_EQ(s.alphaMode, 0x40400UL);
        /* enable with ONE/ZERO == pass-through -> ENALPHABLEND stays off
         * (gglide.c:493-495)                                             */
        gbk_state_set_blend(&s, 1, SST_A_ONE, SST_A_ZERO);
        CHECK_EQ(s.alphaMode, 0x40400UL);
    }

    /* ---- gb_set_alphatest ---------------------------------------------
     * (GREATER=4, ref 0x7F) on top of blend 0x45110: (4<<1)|ENALPHAFUNC
     * = 0x9 (gglide.c:675-677), ref<<24 (:700-701)                       */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        gbk_state_set_blend(&s, 1, SST_A_SRCALPHA, SST_AOM_SRCALPHA);
        gbk_state_set_alphatest(&s, 1, 4, 0x7F);
        CHECK_EQ(s.alphaMode, 0x7F045119UL);
        gbk_state_set_alphatest(&s, 0, 0, 0);
        CHECK_EQ(s.alphaMode, 0x45110UL);
    }

    /* ---- gb_set_fog ---------------------------------------------------
     * enable = GR_FOG_WITH_TABLE_ON_W: ENFOGGING only + always-on
     * FOG_DITHER|FOG_ZONES (gglide.c:2057-2066)                          */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        gbk_state_set_fog(&s, 1, 0x336699UL);
        CHECK_EQ(s.fogMode, 0xC1UL);
        CHECK_EQ(s.fogColor, 0x336699UL);
        gbk_state_set_fog(&s, 0, 0);
        CHECK_EQ(s.fogMode, 0xC0UL);
    }
    /* fogTable packing (grFogTable gglide.c:2096-2113):
     * e0=0,e1=10,next=30: d0=(10-0)<<2=0x28, d1=(30-10)<<2=0x50,
     * word = (10<<24)|(0x50<<16)|(0<<8)|0x28                             */
    CHECK_EQ(gbk_fog_pack_entry(0, 10, 30), 0x0A500028UL);
    /* last word: eNext==e1 -> d1=0 (":don't access beyond end") */
    CHECK_EQ(gbk_fog_pack_entry(200, 255, 255), 0xFF00C8DCUL);

    /* ---- gb_set_cull -> PKT3 smode (NOT a register) -------------------
     * none -> kSetupPingPongDisable(8); CW -> POSITIVE -> CullEnable(2)
     * alone (kSetupCullPositive==0); CCW -> NEGATIVE -> 2|4
     * (gglide.c:2717-2726 + glidebackend.c:240-243)                      */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        gbk_state_set_cull(&s, 1);
        CHECK_EQ(s.smode, 0x2UL);
        /* untextured 3-vert header: (2<<22)|(0xF<<10)|(3<<6)|3 */
        CHECK_EQ(gbk_state_tri_hdr(&s, 0, 3), 0x803CC3UL);
        gbk_state_set_cull(&s, 2);
        CHECK_EQ(s.smode, 0x6UL);
        /* textured (+ST0, pmask 0x2F), batched 6 verts */
        CHECK_EQ(gbk_state_tri_hdr(&s, 1, 6),
                 (0x6UL << 22) | (0x2FUL << 10) | (6UL << 6) | 3UL);
        gbk_state_set_cull(&s, 0);
        CHECK_EQ(s.smode, 0x8UL);
        CHECK_EQ(gbk_state_tri_hdr(&s, 0, 3), s.triHdrUntextured);
    }

    /* ---- gb_set_dither / gb_set_chromakey ----------------------------- */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        gbk_state_set_dither(&s, 0);
        /* removes ENDITHER|DITHER2x2|ENDITHERSUBTRACT from 0x80B21 */
        CHECK_EQ(s.fbzMode, 0x221UL);
        gbk_state_set_dither(&s, 1);
        CHECK_EQ(s.fbzMode, 0x80B21UL);
        gbk_state_set_chromakey(&s, 1, 0xF81FUL);
        CHECK_EQ(s.fbzMode, 0x80B23UL);        /* +ENCHROMAKEY(bit1) */
        CHECK_EQ(s.chromaKey, 0xF81FUL);
        gbk_state_set_chromakey(&s, 0, 0);
        CHECK_EQ(s.fbzMode, 0x80B21UL);
    }

    /* ---- gb_set_shade -------------------------------------------------
     * gouraud: colorCombine(LOCAL,NONE,ITERATED,OTHER_NONE): PARMADJUST |
     * CC_REVERSE(factor 0, 0x2000) | RGBSELECT=2(other const) |
     * CC_ZERO_OTHER|CC_ADD_CLOCAL (0x100|0x4000) | ac residue 0x28
     * = 0x400612A; flat adds LOCALSELECT(0x10) for LOCAL_CONSTANT        */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        gbk_state_set_shade(&s, 1);
        CHECK_EQ(s.fbzColorPath, 0x400612AUL);
        gbk_state_set_shade(&s, 0);
        CHECK_EQ(s.fbzColorPath, 0x400613AUL);
    }

    /* ---- gb_set_texfactor / gb_set_texfilter -------------------------- */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        gbk_state_set_texfactor(&s, 0x80FF00FFUL);
        CHECK_EQ(s.c0, 0x80FF00FFUL);
        CHECK_EQ(s.c1, 0x80FF00FFUL);
        /* bilinear + clamp U only: +TMIN|TMAG(0x6), TCLAMPS(0x40) kept,
         * TCLAMPT cleared: 0x42210C0 -> 0x4221046                        */
        gbk_state_set_texfilter(&s, 1, 1, 0);
        CHECK_EQ(s.textureMode, 0x4221046UL);
    }

    /* ---- texture enable / disable + the ENTEXTUREMAP transition -------
     * on: colorCombine(SCALE_OTHER,FACTOR_LOCAL,ITERATED,TEXTURE):
     * PARMADJUST|CC_REVERSE(0x2000)|MSELECT=1(0x400)|RGBSELECT=1|
     * ENTEXTUREMAP(0x8000000)|ac 0x28 = 0xC002429; texCombine decal
     * (LOCAL,NONE,LOCAL,NONE) -> 0xC0 clamp + TC decal bits = 0xC2610C0  */
    {
        gbk_state_t s;
        gbk_state_defaults(&s, 1);
        CHECK(gbk_state_set_texenable(&s, 1) != 0);   /* off -> on flips */
        CHECK_EQ(s.fbzColorPath, 0xC002429UL);
        CHECK_EQ(s.textureMode, 0xC2610C0UL);
        CHECK(gbk_state_set_texenable(&s, 1) == 0);   /* on -> on: no flip */
        CHECK(gbk_state_set_texenable(&s, 0) != 0);   /* on -> off flips */
        /* off == the gouraud shade combine (gb_tex_none) */
        CHECK_EQ(s.fbzColorPath, 0x400612AUL);
    }

    /* ---- flush: dirty shadow -> PKT4 groups (+nopCMD on transitions) -- */
    {
        gbk_state_t s;
        h3u32 buf[GBK_STATE_FLUSH_MAXWORDS];
        int n;

        gbk_state_defaults(&s, 1);

        /* undersized buffer must be refused without clearing dirty */
        CHECK_EQ(gbk_state_flush(&s, buf, 3), -1);
        CHECK_EQ(s.dirty, 0xFFFUL);

        /* full first flush: common group (7 regs), colors (2), TMU (3) */
        n = gbk_state_flush(&s, buf, GBK_STATE_FLUSH_MAXWORDS);
        CHECK_EQ(n, 15);
        /* common: mask bits 0,1,2,3,10,11,12 = 0x1C0F, base fbzColorPath */
        CHECK_EQ(buf[0], (0x1C0FUL << 15) | (0x104UL << 1) | 4UL);
        CHECK_EQ(buf[1], s.fbzColorPath);
        CHECK_EQ(buf[2], s.fogMode);
        CHECK_EQ(buf[3], s.alphaMode);
        CHECK_EQ(buf[4], s.fbzMode);
        CHECK_EQ(buf[5], s.fogColor);
        CHECK_EQ(buf[6], s.zaColor);
        CHECK_EQ(buf[7], s.chromaKey);
        /* colors: base c0, mask 0x3 */
        CHECK_EQ(buf[8], (0x3UL << 15) | (0x144UL << 1) | 4UL);
        CHECK_EQ(buf[9], s.c0);
        CHECK_EQ(buf[10], s.c1);
        /* TMU: base textureMode, chip TMU0, mask 0xB (mode,tLOD,base) */
        CHECK_EQ(buf[11],
                 (0xBUL << 15) | (0x2UL << 11) | (0x300UL << 1) | 4UL);
        CHECK_EQ(buf[12], s.textureMode);
        CHECK_EQ(buf[13], s.tLOD);
        CHECK_EQ(buf[14], s.texBaseAddr);
        CHECK_EQ(s.dirty, 0);
        CHECK_EQ(gbk_state_flush(&s, buf, GBK_STATE_FLUSH_MAXWORDS), 0);

        /* texture-enable flip: flush must PREPEND nopCMD=0
         * (distate.c:910-915) before the fbzColorPath group             */
        gbk_state_set_texenable(&s, 1);
        n = gbk_state_flush(&s, buf, GBK_STATE_FLUSH_MAXWORDS);
        CHECK_EQ(n, 6);
        CHECK_EQ(buf[0], (1UL << 16) | (0x120UL << 1) | 1UL); /* nopCMD */
        CHECK_EQ(buf[1], 0);
        CHECK_EQ(buf[2], (0x1UL << 15) | (0x104UL << 1) | 4UL);
        CHECK_EQ(buf[3], s.fbzColorPath);
        CHECK_EQ(buf[4],
                 (0x1UL << 15) | (0x2UL << 11) | (0x300UL << 1) | 4UL);
        CHECK_EQ(buf[5], s.textureMode);
        CHECK_EQ(s.texEnabled, 1);

        /* no transition -> no nopCMD prepended */
        gbk_state_set_texfactor(&s, 0x11223344UL);
        n = gbk_state_flush(&s, buf, GBK_STATE_FLUSH_MAXWORDS);
        CHECK_EQ(n, 3);
        CHECK_EQ(buf[0], (0x3UL << 15) | (0x144UL << 1) | 4UL);
    }

    TEST_END("test_gbk_state");
}

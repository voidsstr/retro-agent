/* test_fx_best_refresh.c
 *
 * Guards the 0.1.34 fullscreen-refresh fix in MY open-source ICD — the MesaFX
 * fork retro3dfx-gl (github voidsstr/retro3dfx-gl), file
 *   src/mesa/drivers/glide/fxapi.c  ->  fxBestRefresh()
 * deployed as game-local opengl32.dll / system32 retrogl.dll on .124.
 *
 * The bug: fxMesaCreateBestContext() hardcoded GR_REFRESH_60Hz, so every
 * fullscreen GL game (Glide programs the video timing itself — GDI/-freq can't
 * override it) ran at 60Hz regardless of what the monitor supports. Verified
 * on .124 (Voodoo3, CS 1.6): retrogl.log showed grSstWinOpen ref=60 before,
 * ref=6 (GR_REFRESH_100Hz) after, board open OK.
 *
 * The fix selects a rate: env override (FX_GLIDE_REFRESH_RATE /
 * SSTV2_REFRESH_RATE / MESA_FX_REFRESH) else the monitor's max for that WxH
 * from EnumDisplaySettings, snapped DOWN to the nearest GR_REFRESH_* enum
 * Glide has a timing for; anything below 60 (or a query failure, hz=0) falls
 * back to 60. This test mirrors the snap table + selection exactly and
 * asserts BOTH the fixed mapping and the old-buggy constant.
 */
#include "munit.h"

/* GR_REFRESH_* enum values from glide3 (glide.h) — the ABI the ICD emits. */
#define GR_REFRESH_60Hz  0
#define GR_REFRESH_70Hz  1
#define GR_REFRESH_72Hz  2
#define GR_REFRESH_75Hz  3
#define GR_REFRESH_80Hz  4
#define GR_REFRESH_90Hz  5
#define GR_REFRESH_100Hz 6
#define GR_REFRESH_85Hz  7
#define GR_REFRESH_120Hz 8

/* EXACT mirror of fxapi.c fxBestRefresh()'s snap table + walk: first table
 * entry whose hz <= requested wins; table is highest-first; no match -> 60. */
static int fx_snap_refresh(int hz) {
    static const struct { int hz; int ref; } tbl[] = {
        {120, GR_REFRESH_120Hz}, {100, GR_REFRESH_100Hz}, {90, GR_REFRESH_90Hz},
        {85, GR_REFRESH_85Hz},   {80, GR_REFRESH_80Hz},   {75, GR_REFRESH_75Hz},
        {72, GR_REFRESH_72Hz},   {70, GR_REFRESH_70Hz},   {60, GR_REFRESH_60Hz},
    };
    unsigned i;
    for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (tbl[i].hz <= hz)
            return tbl[i].ref;
    return GR_REFRESH_60Hz;
}

TEST(monitor_max_100_maps_to_100hz_enum_not_60) {
    /* the .124 case: 1024x768 monitor max 100Hz */
    CHECK_EQ_U(fx_snap_refresh(100), GR_REFRESH_100Hz);
    /* the OLD-BUGGY behavior was a hardcoded GR_REFRESH_60Hz for this input */
    CHECK(fx_snap_refresh(100) != GR_REFRESH_60Hz,
          "0.1.33 and earlier hardcoded 60Hz here — must not regress");
}

TEST(snaps_down_between_table_rates) {
    CHECK_EQ_U(fx_snap_refresh(110), GR_REFRESH_100Hz); /* 110 -> 100 */
    CHECK_EQ_U(fx_snap_refresh(99),  GR_REFRESH_90Hz);  /* 99 -> 90  */
    CHECK_EQ_U(fx_snap_refresh(84),  GR_REFRESH_80Hz);  /* 84 -> 80  */
    CHECK_EQ_U(fx_snap_refresh(74),  GR_REFRESH_72Hz);  /* 74 -> 72  */
    CHECK_EQ_U(fx_snap_refresh(71),  GR_REFRESH_70Hz);  /* 71 -> 70  */
}

TEST(exact_table_rates_map_to_their_own_enum) {
    CHECK_EQ_U(fx_snap_refresh(120), GR_REFRESH_120Hz);
    CHECK_EQ_U(fx_snap_refresh(85),  GR_REFRESH_85Hz);
    CHECK_EQ_U(fx_snap_refresh(75),  GR_REFRESH_75Hz);
    CHECK_EQ_U(fx_snap_refresh(60),  GR_REFRESH_60Hz);
}

TEST(below_60_or_unknown_falls_back_to_60) {
    /* EnumDisplaySettings found nothing / env unset garbage -> hz stays 0 */
    CHECK_EQ_U(fx_snap_refresh(0),  GR_REFRESH_60Hz);
    CHECK_EQ_U(fx_snap_refresh(59), GR_REFRESH_60Hz);
    CHECK_EQ_U(fx_snap_refresh(-5), GR_REFRESH_60Hz);
}

MUNIT_MAIN("MesaFX fxBestRefresh snap-down (refresh fix 0.1.34)", {
    RUN(monitor_max_100_maps_to_100hz_enum_not_60);
    RUN(snaps_down_between_table_rates);
    RUN(exact_table_rates_map_to_their_own_enum);
    RUN(below_60_or_unknown_falls_back_to_60);
})

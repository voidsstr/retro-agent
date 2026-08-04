/* test_glide2x_mapboard_guards.c
 *
 * Guards the glide2x XP bring-up fix in MY open Glide fork — retro3dfx-glide
 * (github voidsstr/retro3dfx-glide), file
 *   glide2x/h3/minihwc/minihwc.c  ->  hwcMapBoard() / hwcInit()  (commit 79ee51e)
 * deployed as game-local + system32 glide2x.dll on .124.
 *
 * The bug (found 2026-08-04 via Unreal Gold's 3dfx/GlideDrv renderer): the H5
 * 3dfxv3d display driver only MAPS per-process linear addresses on its
 * new-GLIDESTATE path, and hwcInit sent ALLOCCONTEXT first — so
 * HWCEXT_GETLINEARADDR returned resStatus=1 with base0=0, hwcMapBoard used it
 * unguarded, marked the mapping initialized, and hwcInitRegisters faulted
 * reading dramInit1 off a NULL register base (GPF inside grGlideInit; with
 * the GOG nGlide glide2x it was even worse — a hard chip wedge).
 *
 * This test mirrors the guard decision logic exactly: the escape result is
 * accepted ONLY if (rv > 0) && (resStatus == 1) && (base0 != 0); any
 * rejection must also clear the initialized flag so hwcInitRegisters bails
 * instead of dereferencing.
 */
#include "munit.h"

typedef struct {
    int resStatus;
    unsigned long base0;
} escres_t;

/* EXACT mirror of the hwcMapBoard guard chain (minihwc.c, commit 79ee51e):
 * returns 1 = mapping accepted, 0 = rejected; *initialized mirrors
 * linearInfo.initialized after the call. */
static int map_board(int rv, escres_t *res, int *initialized)
{
    *initialized = 1;                    /* set optimistically, as the code does */
    if (rv <= 0) res->resStatus = 0;     /* failed escape -> failure */
    if (res->base0 == 0) res->resStatus = 0;  /* zero register base -> failure */
    if (res->resStatus != 1) {
        *initialized = 0;                /* the new un-mark guard */
        return 0;
    }
    return 1;
}

TEST(good_mapping_accepted) {
    escres_t r = { 1, 0x07090000UL };
    int init = 0;
    CHECK(map_board(1, &r, &init) == 1, "healthy escape accepted");
    CHECK(init == 1, "mapping marked initialized");
}

TEST(zero_base_rejected_even_with_success_status) {
    /* THE .124 case: driver reports success but base0==0 (unprimed map).
     * OLD-BUGGY behavior: accepted -> hwcInitRegisters GPF at NULL+IO_OFFSET. */
    escres_t r = { 1, 0 };
    int init = 0;
    CHECK(map_board(1, &r, &init) == 0, "resStatus=1 + base0=0 must be rejected");
    CHECK(init == 0, "initialized cleared so hwcInitRegisters bails");
}

TEST(failed_escape_rejected_regardless_of_stale_buffer) {
    /* rv<=0 left res uninitialized pre-fix; garbage base must not be used */
    escres_t r = { 1, 0xDEADBEEFUL };
    int init = 0;
    CHECK(map_board(0, &r, &init) == 0, "rv=0 rejected");
    CHECK(map_board(-1, &r, &init) == 0, "rv<0 rejected");
    CHECK(init == 0, "initialized cleared");
}

TEST(status_zero_rejected) {
    escres_t r = { 0, 0x07090000UL };
    int init = 0;
    CHECK(map_board(1, &r, &init) == 0, "driver-reported failure rejected");
    CHECK(init == 0, "initialized cleared");
}

MUNIT_MAIN("glide2x hwcMapBoard linear-base guards (fix 79ee51e)", {
    RUN(good_mapping_accepted);
    RUN(zero_base_rejected_even_with_success_status);
    RUN(failed_escape_rejected_regardless_of_stale_buffer);
    RUN(status_zero_rejected);
})

/* test_icd_exit_shutdown.c
 *
 * Guards the 0.1.62 fix in OUR clean-room ICD (MesaFX fork retro3dfx-gl,
 * src/mesa/drivers/glide/fxapi.c -> cleangraphics() / fxCloseHardware(),
 * carried in voodoo-cleanroom/patches/mesafx-voodoo2-icd.patch). Clean-room
 * lane, NOT the vintage SGL ICD.
 *
 * 0.1.31 kept Glide initialised across a context destroy so an idTech
 * vid_restart does not do grGlideShutdown -> grGlideInit mid-flight (that
 * wedged the Voodoo 3). But the same rule applied at PROCESS EXIT, so
 * grGlideShutdown was never called at all. On the Voodoo 5 6000 over
 * AmigaMerlin's glide3x the next Glide process then intermittently faulted in
 * grGlideInit on a dead board mapping (.124, 2026-09-24).
 *
 * The contract, mirrored exactly:
 *   - a context destroy while the process runs keeps Glide up (vid_restart);
 *   - process exit shuts Glide down, whether or not a context is still current;
 *   - FX_GLIDE_SHUTDOWN=1 still forces shutdown on the last context.
 */
#include "munit.h"

struct icd {
    int glide_up;        /* glbGlideInitialized */
    int nctx;            /* glbTotNumCtx */
    int have_current;    /* fxMesaCurrentCtx != NULL */
    int exiting;         /* glbProcessExiting */
    int env_shutdown;    /* getenv("FX_GLIDE_SHUTDOWN") */
    int shutdowns;       /* grGlideShutdown() calls */
};

/* fxCloseHardware(), 0.1.62 */
static void close_hw(struct icd *s) {
    if (s->glide_up && s->nctx == 0 && (s->exiting || s->env_shutdown)) {
        s->shutdowns++;
        s->glide_up = 0;
    }
}

/* fxMesaDestroyContext() as far as this contract goes */
static void destroy_ctx(struct icd *s) {
    s->nctx--;
    s->have_current = 0;
    close_hw(s);
}

/* cleangraphics(), 0.1.62 - the _onexit handler */
static void at_exit(struct icd *s) {
    s->exiting = 1;
    if (s->have_current) { s->nctx = 1; destroy_ctx(s); }
    if (s->glide_up) { s->shutdowns++; s->glide_up = 0; }
}

/* cleangraphics(), 0.1.61 and earlier: no exiting flag, no fallback */
static void at_exit_old(struct icd *s) {
    s->nctx = 1;
    s->have_current = 0;          /* old code destroyed unconditionally */
    s->nctx--;
    if (s->glide_up && s->nctx == 0 && s->env_shutdown) { s->shutdowns++; s->glide_up = 0; }
}

TEST(vid_restart_keeps_glide_up) {
    struct icd s = {1, 1, 1, 0, 0, 0};
    destroy_ctx(&s);                       /* the game's vid_restart */
    CHECK_EQ_U(s.glide_up, 1);
    CHECK_EQ_U(s.shutdowns, 0);
}

TEST(exit_with_a_current_context_shuts_glide_down_once) {
    struct icd s = {1, 1, 1, 0, 0, 0};
    at_exit(&s);
    CHECK_EQ_U(s.glide_up, 0);
    CHECK_EQ_U(s.shutdowns, 1);
}

TEST(exit_after_the_game_deleted_its_context_still_shuts_down) {
    struct icd s = {1, 1, 1, 0, 0, 0};
    destroy_ctx(&s);                       /* wglDeleteContext before quit */
    at_exit(&s);
    CHECK_EQ_U(s.glide_up, 0);
    CHECK_EQ_U(s.shutdowns, 1);
}

TEST(old_code_never_shut_glide_down_at_exit) {
    struct icd s = {1, 1, 1, 0, 0, 0};
    at_exit_old(&s);
    CHECK_EQ_U(s.glide_up, 1);             /* the 0.1.61 bug */
    CHECK_EQ_U(s.shutdowns, 0);
}

TEST(env_override_still_forces_shutdown_on_last_context) {
    struct icd s = {1, 1, 1, 0, 1, 0};
    destroy_ctx(&s);
    CHECK_EQ_U(s.glide_up, 0);
    CHECK_EQ_U(s.shutdowns, 1);
    at_exit(&s);
    CHECK_EQ_U(s.shutdowns, 1);            /* never twice */
}

MUNIT_MAIN("MesaFX ICD shuts Glide down at process exit (fix 0.1.62)", {
    RUN(vid_restart_keeps_glide_up);
    RUN(exit_with_a_current_context_shuts_glide_down_once);
    RUN(exit_after_the_game_deleted_its_context_still_shuts_down);
    RUN(old_code_never_shut_glide_down_at_exit);
    RUN(env_override_still_forces_shutdown_on_last_context);
})

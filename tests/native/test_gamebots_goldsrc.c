/* True-source tests for the GoldSrc adapter's engine-independent core.
 *
 * Includes scripts/gamebots/adapters/goldsrc/retrobot_core.c directly, same
 * pattern as tests/native/test_gamebots_client.c for gb_client.c: this tests
 * the code that actually packs a bot's observation, not a copy of it.
 *
 * retrobot_core.c has zero HLSDK and zero -m32 dependency by design (see its
 * header comment), so this builds and runs with the same plain
 *     gcc -std=c11 -O0 -g -Wall -I tests/native
 * every other native test in this suite uses -- no 32-bit toolchain, no
 * game-server headers, no network. The Metamod glue that calls into this
 * (retrobot_engine.cpp) is NOT exercised here; see
 * scripts/gamebots/adapters/goldsrc/README.md for why that half is untested
 * on this host.
 *
 * Built and run by tests/run_all.sh section [2] (it globs test_*.c).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../../scripts/gamebots/adapters/goldsrc/retrobot_core.c"

static int failures;
static int checks;

static void ok(const char *name, int cond)
{
    checks++;
    if (cond) {
        printf("  [ ok ] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
        failures++;
    }
}

static void ok_close(const char *name, float got, float want, float tol)
{
    checks++;
    if (fabsf(got - want) <= tol) {
        printf("  [ ok ] %s (%.6f)\n", name, (double)got);
    } else {
        printf("  [FAIL] %s (got %.6f, want %.6f)\n", name, (double)got, (double)want);
        failures++;
    }
}

static void test_clamp(void)
{
    ok("in-range passes through", rb_clamp(3.0f, -1.0f, 5.0f) == 3.0f);
    ok("below lo clamps to lo", rb_clamp(-9.0f, -1.0f, 5.0f) == -1.0f);
    ok("above hi clamps to hi", rb_clamp(9.0f, -1.0f, 5.0f) == 5.0f);
    /* The one that matters: NaN < lo and NaN > hi are both false, so a plain
     * min/max chain returns NaN unchanged -- the same trap gb_clamp() in
     * gb_client.c documents on the action side. */
    ok("NaN maps to lo, not passed through",
       rb_clamp((float)NAN, -1.0f, 5.0f) == -1.0f);
    ok("+inf clamps to hi", rb_clamp((float)INFINITY, -1.0f, 5.0f) == 5.0f);
    ok("-inf clamps to lo", rb_clamp((float)-INFINITY, -1.0f, 5.0f) == -1.0f);
}

static void test_norm01(void)
{
    ok_close("mid-range normalises to 0.5", rb_norm01(50.0f, 0.0f, 100.0f), 0.5f, 1e-6f);
    ok("value below lo clamps to 0", rb_norm01(-10.0f, 0.0f, 100.0f) == 0.0f);
    ok("value above hi clamps to 1", rb_norm01(200.0f, 0.0f, 100.0f) == 1.0f);
    /* An "unknown maximum" (engine could not determine e.g. clip size) must
     * read as 0, never divide-by-zero into NaN/inf that then contaminates a
     * whole observation. */
    ok("hi <= lo (unknown max) yields 0, not NaN/inf", rb_norm01(5.0f, 0.0f, 0.0f) == 0.0f);
    ok("negative span yields 0", rb_norm01(5.0f, 10.0f, 0.0f) == 0.0f);
    ok("NaN numerator yields 0 (clamped to lo of 0..1)",
       rb_norm01((float)NAN, 0.0f, 100.0f) == 0.0f);
}

static void test_world_to_local(void)
{
    float local[3];

    /* yaw = 0: forward = world +X, right = world -Y, up = world +Z. */
    rb_world_to_local((float[3]){ 1.0f, 0.0f, 0.0f }, 0.0f, local);
    ok_close("yaw=0: +X is fully forward", local[0], 1.0f, 1e-5f);
    ok_close("yaw=0: +X has no right component", local[1], 0.0f, 1e-5f);

    rb_world_to_local((float[3]){ 0.0f, 0.0f, 7.0f }, 0.0f, local);
    ok_close("up passes through unrotated regardless of yaw", local[2], 7.0f, 1e-5f);

    /* yaw = 90: forward now points along world +Y. */
    rb_world_to_local((float[3]){ 0.0f, 1.0f, 0.0f }, 90.0f, local);
    ok_close("yaw=90: world +Y is fully forward", local[0], 1.0f, 1e-4f);
    ok_close("yaw=90: world +Y has no right component", local[1], 0.0f, 1e-4f);

    /* Rotation must preserve length -- otherwise "1.0 = max speed" drifts
     * with view angle, which would make the same physical speed read
     * differently depending on which way the bot is looking. */
    {
        float w[3] = { 3.0f, -4.0f, 2.0f };
        float len_w = sqrtf(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
        float len_l;
        rb_world_to_local(w, 37.0f, local);
        len_l = sqrtf(local[0]*local[0] + local[1]*local[1] + local[2]*local[2]);
        ok_close("rotation preserves vector length", len_l, len_w, 1e-4f);
    }
}

static rb_entity_raw_t mk_entity(float x, int is_teammate, int visible)
{
    rb_entity_raw_t e;
    memset(&e, 0, sizeof(e));
    e.valid = 1;
    e.is_teammate = is_teammate;
    e.visible = visible;
    e.pos[0] = x;
    e.health_frac = 1.0f;
    return e;
}

static void test_threat_sort(void)
{
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    int out[GB_MAX_ENTITIES];
    rb_entity_raw_t ents[6];

    /* [0] far visible enemy, [1] near visible enemy, [2] near visible
     * teammate, [3] very-near hidden enemy, [4] invalid slot (must be
     * skipped entirely), [5] near hidden teammate. */
    ents[0] = mk_entity(500.0f, 0, 1);
    ents[1] = mk_entity(100.0f, 0, 1);
    ents[2] = mk_entity(50.0f, 1, 1);
    ents[3] = mk_entity(10.0f, 0, 0);
    ents[4] = mk_entity(1.0f, 0, 1);
    ents[4].valid = 0;
    ents[5] = mk_entity(20.0f, 1, 0);

    rb_sort_entities_by_threat(ents, 6, origin, out);

    ok("nearest VISIBLE enemy sorts first even though closer entities exist",
       out[0] == 1);
    ok("farther visible enemy sorts before any visible teammate", out[1] == 0);
    ok("visible teammate sorts before any hidden entity", out[2] == 2);
    ok("hidden enemy sorts before hidden teammate", out[3] == 3);
    ok("hidden teammate is last of the valid entries", out[4] == 5);
    ok("invalid entity slot never appears in the ordering",
       out[0] != 4 && out[1] != 4 && out[2] != 4 && out[3] != 4 && out[4] != 4);
    ok("unfilled trailing slots are -1", out[5] == -1);

    /* Fewer real entities than GB_MAX_ENTITIES: the rest must read as
     * "absent" (-1), never garbage or a repeated index. */
    {
        rb_entity_raw_t one[1];
        int out2[GB_MAX_ENTITIES];
        int i, all_absent_after_first = 1;
        one[0] = mk_entity(42.0f, 0, 1);
        rb_sort_entities_by_threat(one, 1, origin, out2);
        ok("single entity lands in slot 0", out2[0] == 0);
        for (i = 1; i < GB_MAX_ENTITIES; i++)
            if (out2[i] != -1) all_absent_after_first = 0;
        ok("every other slot is -1 when only one entity exists",
           all_absent_after_first);
    }

    /* Stability: two equally-ranked entities (same group, same distance)
     * keep their input order -- this is what makes the ordering
     * reproducible rather than "whatever the sort felt like". */
    {
        rb_entity_raw_t tied[2];
        int out3[GB_MAX_ENTITIES];
        tied[0] = mk_entity(30.0f, 0, 1);
        tied[1] = mk_entity(30.0f, 0, 1);
        rb_sort_entities_by_threat(tied, 2, origin, out3);
        ok("equal-rank ties keep their original relative order",
           out3[0] == 0 && out3[1] == 1);
    }

    /* n outside [0, RB_MAX_ENTITIES_RAW] must not read out of bounds or
     * crash -- this runs inside a game server, where that would corrupt its
     * memory, not just this test's. */
    {
        int out4[GB_MAX_ENTITIES];
        rb_sort_entities_by_threat(ents, -3, origin, out4);
        ok("negative n is treated as zero entities", out4[0] == -1);
    }

    /* A NaN position must not crash the sort (NaN fails every comparison) --
     * it should simply sort to the back rather than corrupting the order of
     * every entity after it. */
    {
        rb_entity_raw_t withnan[2];
        int out5[GB_MAX_ENTITIES];
        withnan[0] = mk_entity(10.0f, 0, 1);
        withnan[1] = mk_entity(10.0f, 0, 1);
        withnan[1].pos[0] = (float)NAN;
        rb_sort_entities_by_threat(withnan, 2, origin, out5);
        ok("a NaN-positioned entity does not crash the sort and is not first",
           out5[0] == 0);
    }
}

static rb_raw_obs_t mk_raw(void)
{
    rb_raw_obs_t raw;
    memset(&raw, 0, sizeof(raw));
    raw.far_plane = 4096.0f;
    raw.max_speed = 320.0f;
    raw.max_health = 100.0f;
    raw.max_armor = 100.0f;
    return raw;
}

static void test_build_observation_self(void)
{
    rb_raw_obs_t raw = mk_raw();
    float obs[GB_OBS_DIM];

    raw.health = 75.0f;
    raw.armor = 50.0f;
    raw.ammo_clip = 15.0f; raw.ammo_clip_max = 30.0f;
    raw.ammo_reserve = 60.0f; raw.ammo_reserve_max = 120.0f;
    raw.weapon_id = 8; raw.weapon_id_max = 32;
    raw.angles[1] = 0.0f;   /* yaw */
    raw.vel[0] = 160.0f;    /* half max_speed, straight along +X == forward at yaw 0 */
    raw.on_ground = 1;
    raw.crouching = 0;
    raw.in_water = 1;
    raw.reloading = 0;
    raw.alive = 1;

    rb_build_observation(&raw, obs);

    ok_close("health_frac", obs[GB_OBS_HEALTH_FRAC], 0.75f, 1e-5f);
    ok_close("armor_frac", obs[GB_OBS_ARMOR_FRAC], 0.5f, 1e-5f);
    ok_close("ammo_frac", obs[GB_OBS_AMMO_FRAC], 0.5f, 1e-5f);
    ok_close("ammo_reserve_frac", obs[GB_OBS_AMMO_RESERVE_FRAC], 0.5f, 1e-5f);
    ok_close("weapon_id_norm", obs[GB_OBS_WEAPON_ID_NORM], 8.0f / 32.0f, 1e-5f);
    ok_close("vel_local forward component", obs[GB_OBS_VEL_LOCAL + 0], 0.5f, 1e-4f);
    ok_close("speed_frac", obs[GB_OBS_SPEED_FRAC], 0.5f, 1e-4f);
    ok("on_ground is 1.0", obs[GB_OBS_ON_GROUND] == 1.0f);
    ok("crouching is 0.0", obs[GB_OBS_CROUCHING] == 0.0f);
    ok("in_water is 1.0", obs[GB_OBS_IN_WATER] == 1.0f);
    ok("alive is 1.0", obs[GB_OBS_ALIVE] == 1.0f);

    /* Unknown maxima (the honest zero-fill case documented in the adapter's
     * README: fields the engine genuinely could not determine) must read as
     * 0, not NaN -- a NaN here would poison every downstream computation the
     * policy does with this observation. */
    {
        rb_raw_obs_t raw2 = mk_raw();
        float obs2[GB_OBS_DIM];
        raw2.ammo_clip_max = 0.0f;       /* "unknown" */
        raw2.ammo_reserve_max = 0.0f;
        raw2.weapon_id_max = 0.0f;
        rb_build_observation(&raw2, obs2);
        ok("unknown ammo max reads as 0, not NaN", obs2[GB_OBS_AMMO_FRAC] == 0.0f);
        ok("unknown reserve max reads as 0, not NaN",
           obs2[GB_OBS_AMMO_RESERVE_FRAC] == 0.0f);
        ok("unknown weapon max reads as 0, not NaN",
           obs2[GB_OBS_WEAPON_ID_NORM] == 0.0f);
    }
}

static void test_build_observation_rays_and_reserved(void)
{
    rb_raw_obs_t raw = mk_raw();
    float obs[GB_OBS_DIM];
    int i;

    raw.ray_h[0] = 2048.0f;              /* half far_plane */
    raw.ray_h[1] = 999999.0f;            /* beyond far_plane: must clamp to 1 */
    raw.ray_h[2] = (float)NAN;           /* must not propagate */
    raw.ray_up = 4096.0f;
    raw.ray_down = 0.0f;

    rb_build_observation(&raw, obs);

    ok_close("a ray at half far_plane normalises to 0.5",
             obs[GB_OBS_RAY_H + 0], 0.5f, 1e-5f);
    ok("a ray beyond far_plane clamps to 1.0", obs[GB_OBS_RAY_H + 1] == 1.0f);
    ok("a NaN ray reads as 0, not NaN", obs[GB_OBS_RAY_H + 2] == 0.0f);
    ok("ray_up at exactly far_plane is 1.0", obs[GB_OBS_RAY_UP] == 1.0f);
    ok("ray_down at 0 is 0.0", obs[GB_OBS_RAY_DOWN] == 0.0f);

    /* The reserved intent slot and the alignment pad are contractually
     * zero -- the planner injects intent server-side (see
     * docs/game-ai-bots-plan.md Sec 4.3); an adapter that writes anything
     * there would be silently fighting the planner. */
    {
        int intent_all_zero = 1, pad_all_zero = 1;
        for (i = 0; i < GB_INTENT_DIM; i++)
            if (obs[GB_OBS_INTENT + i] != 0.0f) intent_all_zero = 0;
        for (i = GB_OBS_PAD; i < GB_OBS_DIM; i++)
            if (obs[i] != 0.0f) pad_all_zero = 0;
        ok("the reserved intent slot is left zero by the adapter", intent_all_zero);
        ok("the alignment padding is left zero", pad_all_zero);
    }
}

static void test_build_observation_entities_and_events(void)
{
    rb_raw_obs_t raw = mk_raw();
    float obs[GB_OBS_DIM];

    raw.pos[0] = 0.0f; raw.pos[1] = 0.0f; raw.pos[2] = 0.0f;
    raw.angles[1] = 0.0f;

    raw.n_entities = 2;
    raw.entities[0] = mk_entity(0.0f, 0, 1);   /* overwritten below */
    memset(&raw.entities[0], 0, sizeof(raw.entities[0]));
    raw.entities[0].valid = 1;
    raw.entities[0].is_teammate = 0;
    raw.entities[0].visible = 1;
    raw.entities[0].pos[0] = 100.0f;           /* straight ahead at yaw 0 */
    raw.entities[0].health_frac = 0.4f;

    raw.entities[1].valid = 0;                 /* must be skipped */

    raw.took_damage = 25.0f;
    raw.damage_dir_world[0] = -1.0f;           /* damage came from behind */
    raw.killed_someone = 1;
    raw.died = 0;
    raw.round_time_frac = 1.5f;                /* out of range: must clamp */
    raw.score_diff_norm = -5.0f;               /* out of range: must clamp */
    raw.teammates_alive_frac = 0.75f;
    raw.enemies_alive_frac = 0.25f;
    raw.objective[0] = 1.0f;

    rb_build_observation(&raw, obs);

    ok("slot 0 present", obs[GB_OBS_E0_PRESENT] == 1.0f);
    ok("slot 0 is not a teammate", obs[GB_OBS_E0_IS_TEAMMATE] == 0.0f);
    ok_close("slot 0 is directly ahead in the bot's frame",
             obs[GB_OBS_E0_DIR + 0], 1.0f, 1e-4f);
    ok_close("slot 0 distance normalises against far_plane",
             obs[GB_OBS_E0_DIST_NORM], 100.0f / 4096.0f, 1e-4f);
    ok_close("slot 0 health_frac", obs[GB_OBS_E0_HEALTH_FRAC], 0.4f, 1e-6f);
    ok("slot 0 visible", obs[GB_OBS_E0_VISIBLE] == 1.0f);
    ok("the invalid entity did not consume slot 1",
       obs[GB_OBS_E1_PRESENT] == 0.0f);

    ok_close("took_damage normalises against max health",
             obs[GB_OBS_TOOK_DAMAGE], 0.25f, 1e-5f);
    ok("killed_someone is 1.0", obs[GB_OBS_KILLED_SOMEONE] == 1.0f);
    ok("died is 0.0", obs[GB_OBS_DIED] == 0.0f);
    ok("round_time_frac > 1 clamps to 1.0", obs[GB_OBS_ROUND_TIME_FRAC] == 1.0f);
    ok("score_diff_norm < -1 clamps to -1.0", obs[GB_OBS_SCORE_DIFF_NORM] == -1.0f);
    ok_close("teammates_alive_frac", obs[GB_OBS_TEAMMATES_ALIVE_FRAC], 0.75f, 1e-6f);
    ok_close("enemies_alive_frac", obs[GB_OBS_ENEMIES_ALIVE_FRAC], 0.25f, 1e-6f);
    ok_close("objective[0]", obs[GB_OBS_OBJECTIVE + 0], 1.0f, 1e-6f);
}

int main(void)
{
    printf("== gamebots GoldSrc adapter core (true-source) ==\n");
    test_clamp();
    test_norm01();
    test_world_to_local();
    test_threat_sort();
    test_build_observation_self();
    test_build_observation_rays_and_reserved();
    test_build_observation_entities_and_events();
    printf("-- gamebots goldsrc core: %d/%d checks passed --\n",
           checks - failures, checks);
    return failures ? 1 : 0;
}

/* retrobot_core.c — see retrobot_core.h. Plain C11, no HLSDK, no engine, no
 * sockets: this is the part of the GoldSrc adapter that is honest to unit
 * test without a game server, a 32-bit toolchain, or the HLSDK checkout. */

#include "retrobot_core.h"

#include <math.h>
#include <string.h>

/* -std=c11 (no GNU/POSIX extensions) does not expose <math.h>'s M_PI, so this
 * doesn't rely on it -- a build under a stricter -std shouldn't need to know
 * that trick. */
#define RB_PI 3.14159265358979323846f

/* GB_MAX_ENTITIES is unrolled by name in gamebots_schema.h (GB_OBS_E0_* ..
 * GB_OBS_E7_*), not generated from a loop, so RB_ENT_OFF below is hand-listed
 * to match. If the schema ever changes the entity count this fails loudly at
 * compile time instead of silently packing the wrong floats. */
#if GB_MAX_ENTITIES != 8
#error "retrobot_core.c hardcodes 8 entity-slot offsets (GB_OBS_E0.. GB_OBS_E7); update RB_ENT_OFF to match the new GB_MAX_ENTITIES"
#endif

typedef struct {
    int present, is_teammate, dir, dist_norm, rel_vel, health_frac, visible;
} rb_ent_off_t;

static const rb_ent_off_t RB_ENT_OFF[GB_MAX_ENTITIES] = {
    { GB_OBS_E0_PRESENT, GB_OBS_E0_IS_TEAMMATE, GB_OBS_E0_DIR, GB_OBS_E0_DIST_NORM, GB_OBS_E0_REL_VEL, GB_OBS_E0_HEALTH_FRAC, GB_OBS_E0_VISIBLE },
    { GB_OBS_E1_PRESENT, GB_OBS_E1_IS_TEAMMATE, GB_OBS_E1_DIR, GB_OBS_E1_DIST_NORM, GB_OBS_E1_REL_VEL, GB_OBS_E1_HEALTH_FRAC, GB_OBS_E1_VISIBLE },
    { GB_OBS_E2_PRESENT, GB_OBS_E2_IS_TEAMMATE, GB_OBS_E2_DIR, GB_OBS_E2_DIST_NORM, GB_OBS_E2_REL_VEL, GB_OBS_E2_HEALTH_FRAC, GB_OBS_E2_VISIBLE },
    { GB_OBS_E3_PRESENT, GB_OBS_E3_IS_TEAMMATE, GB_OBS_E3_DIR, GB_OBS_E3_DIST_NORM, GB_OBS_E3_REL_VEL, GB_OBS_E3_HEALTH_FRAC, GB_OBS_E3_VISIBLE },
    { GB_OBS_E4_PRESENT, GB_OBS_E4_IS_TEAMMATE, GB_OBS_E4_DIR, GB_OBS_E4_DIST_NORM, GB_OBS_E4_REL_VEL, GB_OBS_E4_HEALTH_FRAC, GB_OBS_E4_VISIBLE },
    { GB_OBS_E5_PRESENT, GB_OBS_E5_IS_TEAMMATE, GB_OBS_E5_DIR, GB_OBS_E5_DIST_NORM, GB_OBS_E5_REL_VEL, GB_OBS_E5_HEALTH_FRAC, GB_OBS_E5_VISIBLE },
    { GB_OBS_E6_PRESENT, GB_OBS_E6_IS_TEAMMATE, GB_OBS_E6_DIR, GB_OBS_E6_DIST_NORM, GB_OBS_E6_REL_VEL, GB_OBS_E6_HEALTH_FRAC, GB_OBS_E6_VISIBLE },
    { GB_OBS_E7_PRESENT, GB_OBS_E7_IS_TEAMMATE, GB_OBS_E7_DIR, GB_OBS_E7_DIST_NORM, GB_OBS_E7_REL_VEL, GB_OBS_E7_HEALTH_FRAC, GB_OBS_E7_VISIBLE },
};

float rb_clamp(float v, float lo, float hi)
{
    /* NaN fails every comparison; a plain min/max chain would pass it
     * through unchanged, which is exactly the bug gb_client.c's gb_clamp
     * documents on the action side. */
    if (v != v)
        return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float rb_norm01(float v, float lo, float hi)
{
    float span = hi - lo;
    if (span <= 0.0f)
        return 0.0f;               /* "unknown maximum" -> 0, not a NaN/inf */
    return rb_clamp((v - lo) / span, 0.0f, 1.0f);
}

void rb_world_to_local(const float world[3], float yaw_deg, float out_local[3])
{
    float yaw_rad = yaw_deg * (RB_PI / 180.0f);
    float c = cosf(yaw_rad), s = sinf(yaw_rad);
    /* forward = (c, s, 0), right = (s, -c, 0), up = (0, 0, 1) -- see the
     * frame-convention comment in retrobot_core.h. */
    out_local[0] = world[0] * c + world[1] * s;
    out_local[1] = world[0] * s - world[1] * c;
    out_local[2] = world[2];
}

void rb_sort_entities_by_threat(const rb_entity_raw_t *entities, int n,
                                 const float bot_pos[3],
                                 int out_idx[GB_MAX_ENTITIES])
{
    int idx[RB_MAX_ENTITIES_RAW];
    float key[RB_MAX_ENTITIES_RAW];
    int i, j, m;

    if (n > RB_MAX_ENTITIES_RAW) n = RB_MAX_ENTITIES_RAW;
    if (n < 0) n = 0;

    m = 0;
    for (i = 0; i < n; i++) {
        const rb_entity_raw_t *e = &entities[i];
        float dx, dy, dz, dist, group;
        if (!e->valid)
            continue;
        dx = e->pos[0] - bot_pos[0];
        dy = e->pos[1] - bot_pos[1];
        dz = e->pos[2] - bot_pos[2];
        dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist != dist)          /* NaN position never sorts to the front */
            dist = 1.0e9f;
        /* group 0: visible enemy, 1: visible teammate,
         *       2: hidden enemy,  3: hidden teammate */
        group = (e->visible ? 0.0f : 2.0f) + (e->is_teammate ? 1.0f : 0.0f);
        idx[m] = i;
        key[m] = group * 1.0e7f + dist;
        m++;
    }

    /* Stable insertion sort. m is at most RB_MAX_ENTITIES_RAW (32), so O(m^2)
     * is irrelevant, and stability is what makes ties reproducible in a
     * test rather than "whatever qsort felt like today". */
    for (i = 1; i < m; i++) {
        float k = key[i];
        int id = idx[i];
        j = i - 1;
        while (j >= 0 && key[j] > k) {
            key[j + 1] = key[j];
            idx[j + 1] = idx[j];
            j--;
        }
        key[j + 1] = k;
        idx[j + 1] = id;
    }

    for (i = 0; i < GB_MAX_ENTITIES; i++)
        out_idx[i] = (i < m) ? idx[i] : -1;
}

void rb_build_observation(const rb_raw_obs_t *raw, float out_obs[GB_OBS_DIM])
{
    int i;
    int order[GB_MAX_ENTITIES];
    float fp = raw->far_plane > 1e-3f ? raw->far_plane : 1.0f;
    float ms = raw->max_speed > 1e-3f ? raw->max_speed : 1.0f;
    float local[3];

    memset(out_obs, 0, sizeof(float) * GB_OBS_DIM);

    /* --- self --- */
    out_obs[GB_OBS_HEALTH_FRAC] = rb_norm01(raw->health, 0.0f, raw->max_health);
    out_obs[GB_OBS_ARMOR_FRAC] = rb_norm01(raw->armor, 0.0f, raw->max_armor);
    out_obs[GB_OBS_AMMO_FRAC] = rb_norm01(raw->ammo_clip, 0.0f, raw->ammo_clip_max);
    out_obs[GB_OBS_AMMO_RESERVE_FRAC] = rb_norm01(raw->ammo_reserve, 0.0f, raw->ammo_reserve_max);
    out_obs[GB_OBS_WEAPON_ID_NORM] = rb_norm01((float)raw->weapon_id, 0.0f, (float)raw->weapon_id_max);

    rb_world_to_local(raw->vel, raw->angles[1], local);
    out_obs[GB_OBS_VEL_LOCAL + 0] = rb_clamp(local[0] / ms, -1.0f, 1.0f);
    out_obs[GB_OBS_VEL_LOCAL + 1] = rb_clamp(local[1] / ms, -1.0f, 1.0f);
    out_obs[GB_OBS_VEL_LOCAL + 2] = rb_clamp(local[2] / ms, -1.0f, 1.0f);
    {
        float speed = sqrtf(raw->vel[0] * raw->vel[0] + raw->vel[1] * raw->vel[1] +
                             raw->vel[2] * raw->vel[2]);
        out_obs[GB_OBS_SPEED_FRAC] = rb_clamp(speed / ms, 0.0f, 1.0f);
    }
    out_obs[GB_OBS_PITCH_NORM] = rb_clamp(raw->angles[0] / 90.0f, -1.0f, 1.0f);
    out_obs[GB_OBS_ON_GROUND] = raw->on_ground ? 1.0f : 0.0f;
    out_obs[GB_OBS_CROUCHING] = raw->crouching ? 1.0f : 0.0f;
    out_obs[GB_OBS_IN_WATER] = raw->in_water ? 1.0f : 0.0f;
    out_obs[GB_OBS_RELOADING] = raw->reloading ? 1.0f : 0.0f;
    out_obs[GB_OBS_ALIVE] = raw->alive ? 1.0f : 0.0f;

    /* --- local geometry --- */
    for (i = 0; i < GB_NUM_RAYS_H; i++)
        out_obs[GB_OBS_RAY_H + i] = rb_clamp(raw->ray_h[i] / fp, 0.0f, 1.0f);
    out_obs[GB_OBS_RAY_UP] = rb_clamp(raw->ray_up / fp, 0.0f, 1.0f);
    out_obs[GB_OBS_RAY_DOWN] = rb_clamp(raw->ray_down / fp, 0.0f, 1.0f);

    /* --- other entities, threat-sorted --- */
    rb_sort_entities_by_threat(raw->entities, raw->n_entities, raw->pos, order);
    for (i = 0; i < GB_MAX_ENTITIES; i++) {
        int idx = order[i];
        const rb_ent_off_t *o = &RB_ENT_OFF[i];
        const rb_entity_raw_t *e;
        float rel[3], dist, dir_world[3], dir_local[3];
        float relvel_world[3], relvel_local[3];

        if (idx < 0)
            continue;               /* slot stays all-zero: "not present" */
        e = &raw->entities[idx];

        rel[0] = e->pos[0] - raw->pos[0];
        rel[1] = e->pos[1] - raw->pos[1];
        rel[2] = e->pos[2] - raw->pos[2];
        dist = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        if (dist > 1e-4f) {
            dir_world[0] = rel[0] / dist;
            dir_world[1] = rel[1] / dist;
            dir_world[2] = rel[2] / dist;
        } else {
            dir_world[0] = dir_world[1] = dir_world[2] = 0.0f;
        }
        rb_world_to_local(dir_world, raw->angles[1], dir_local);

        relvel_world[0] = e->vel[0] - raw->vel[0];
        relvel_world[1] = e->vel[1] - raw->vel[1];
        relvel_world[2] = e->vel[2] - raw->vel[2];
        rb_world_to_local(relvel_world, raw->angles[1], relvel_local);

        out_obs[o->present] = 1.0f;
        out_obs[o->is_teammate] = e->is_teammate ? 1.0f : 0.0f;
        out_obs[o->dir + 0] = dir_local[0];
        out_obs[o->dir + 1] = dir_local[1];
        out_obs[o->dir + 2] = dir_local[2];
        out_obs[o->dist_norm] = rb_clamp(dist / fp, 0.0f, 1.0f);
        out_obs[o->rel_vel + 0] = rb_clamp(relvel_local[0] / ms, -1.0f, 1.0f);
        out_obs[o->rel_vel + 1] = rb_clamp(relvel_local[1] / ms, -1.0f, 1.0f);
        out_obs[o->health_frac] = rb_clamp(e->health_frac, 0.0f, 1.0f);
        out_obs[o->visible] = e->visible ? 1.0f : 0.0f;
    }

    /* --- what just happened to us --- */
    out_obs[GB_OBS_TOOK_DAMAGE] = rb_norm01(raw->took_damage, 0.0f, raw->max_health);
    {
        float dir_local[3];
        rb_world_to_local(raw->damage_dir_world, raw->angles[1], dir_local);
        out_obs[GB_OBS_DAMAGE_DIR + 0] = dir_local[0];
        out_obs[GB_OBS_DAMAGE_DIR + 1] = dir_local[1];
    }
    out_obs[GB_OBS_KILLED_SOMEONE] = raw->killed_someone ? 1.0f : 0.0f;
    out_obs[GB_OBS_DIED] = raw->died ? 1.0f : 0.0f;

    /* --- match context --- */
    out_obs[GB_OBS_ROUND_TIME_FRAC] = rb_clamp(raw->round_time_frac, 0.0f, 1.0f);
    out_obs[GB_OBS_SCORE_DIFF_NORM] = rb_clamp(raw->score_diff_norm, -1.0f, 1.0f);
    out_obs[GB_OBS_TEAMMATES_ALIVE_FRAC] = rb_clamp(raw->teammates_alive_frac, 0.0f, 1.0f);
    out_obs[GB_OBS_ENEMIES_ALIVE_FRAC] = rb_clamp(raw->enemies_alive_frac, 0.0f, 1.0f);
    out_obs[GB_OBS_OBJECTIVE + 0] = rb_clamp(raw->objective[0], -1.0f, 1.0f);
    out_obs[GB_OBS_OBJECTIVE + 1] = rb_clamp(raw->objective[1], -1.0f, 1.0f);

    /* GB_OBS_INTENT and GB_OBS_PAD are left zero: the intent vector is
     * injected server-side by the planner (see docs/game-ai-bots-plan.md
     * Sec 4.3 / scripts/gamebots/planner.py) -- the adapter sending zeros
     * IS the contract, not an omission. */
}

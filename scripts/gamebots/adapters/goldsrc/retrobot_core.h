/* retrobot_core — engine-independent observation packing for the GoldSrc
 * adapter (retrobot.so).
 *
 * Everything in this file/its .c is PURE LOGIC: no HLSDK, no engine calls, no
 * sockets. It takes raw numbers the engine glue (retrobot_engine.cpp) reads
 * out of edict_t/entvars_t/clientdata_t/weapon_data_t and TraceLine, and packs
 * them into the schema's GB_OBS_DIM float layout -- rotation into the bot's
 * own frame, normalisation, clamping, and threat-sorted entity slots.
 *
 * Splitting it out this way means:
 *   - it builds and is testable on ANY host with a plain C11 compiler, no
 *     32-bit toolchain and no HLSDK checkout required (see
 *     tests/native/test_gamebots_goldsrc.c);
 *   - the one place that turns "field name" into "float index" is this file,
 *     using the GB_OBS_* macros gamebots_schema.h generates -- never a bare
 *     `base + 7`, which is exactly the bug the schema's own README calls out
 *     (policyd.py once read `visible` at the wrong offset and nobody saw an
 *     error, just a policy that silently never used it).
 *
 * Frame convention: ego-centric, YAW ONLY (no roll, no pitch tilt) --
 *     forward = (cos(yaw), sin(yaw), 0)
 *     right   = (sin(yaw), -cos(yaw), 0)
 *     up      = (0, 0, 1)
 * This is a convention of ours, not a copy of the engine's AngleVectors(): it
 * only has to be self-consistent between every vector we rotate, which it is.
 * Pitch is not applied because the schema's "own frame" is for ground
 * movement and relative geometry, where the up axis is world-up regardless of
 * where the bot is looking.
 */
#ifndef RETROBOT_CORE_H
#define RETROBOT_CORE_H

#include "../../gamebots_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The engine glue may hand us more candidate entities than fit in the
 * observation (e.g. every player + a few grenades); core picks the best
 * GB_MAX_ENTITIES by threat and drops the rest. */
#define RB_MAX_ENTITIES_RAW 32

typedef struct {
    int   valid;        /* 0 = empty slot, ignored */
    int   is_teammate;  /* 1 teammate, 0 enemy/neutral */
    int   visible;       /* 1 if a TraceLine from the bot's eye to this entity
                           * is unobstructed (engine-computed; core never
                           * traces anything itself) */
    float pos[3];         /* world position */
    float vel[3];         /* world velocity */
    float health_frac;    /* 0..1; 0 if the engine could not determine it */
} rb_entity_raw_t;

typedef struct {
    /* --- self, all engine units (world position/velocity, degrees) --- */
    float pos[3];               /* world position, used only to compute
                                  * entity-relative vectors -- never written
                                  * to the observation itself (ego-centric) */
    float vel[3];                /* world velocity */
    float angles[3];             /* pitch, yaw, roll, degrees */

    float health, max_health;
    float armor, max_armor;
    float ammo_clip, ammo_clip_max;         /* <=0 max means "unknown" -> 0 */
    float ammo_reserve, ammo_reserve_max;   /* <=0 max means "unknown" -> 0 */
    int   weapon_id, weapon_id_max;          /* <=0 max means "unknown" -> 0 */

    int   on_ground, crouching, in_water, reloading, alive;

    /* --- local geometry: world-unit distances, RAY_FAR-ish if no hit --- */
    float ray_h[GB_NUM_RAYS_H];
    float ray_up;
    float ray_down;

    /* --- other entities: engine fills [0, n_entities) --- */
    rb_entity_raw_t entities[RB_MAX_ENTITIES_RAW];
    int   n_entities;

    /* --- what just happened to us --- */
    float took_damage;             /* absolute HP lost last tick */
    float damage_dir_world[3];     /* world-space unit direction it came from;
                                     * zero vector if unknown/no damage */
    int   killed_someone, died;

    /* --- match context: pre-normalised by the engine glue (cheap, and
     * genuinely engine/gamerules-specific -- nothing to rotate or clamp
     * generically here beyond the final safety clamp) --- */
    float round_time_frac;
    float score_diff_norm;
    float teammates_alive_frac;
    float enemies_alive_frac;
    float objective[2];

    /* --- normalisation constants; engine-supplied so tests can vary them
     * without hardcoding a map's scale into this file --- */
    float far_plane;    /* world units representing "no data" / ray max range */
    float max_speed;    /* world units/s representing a local-frame value of 1.0 */
} rb_raw_obs_t;

/* Build the packed, schema-ordered observation. out_obs must have room for
 * GB_OBS_DIM floats; every one is written (including the reserved intent
 * slot and the alignment pad, both left zero), so no uninitialised float
 * ever reaches gb_add(). */
void rb_build_observation(const rb_raw_obs_t *raw, float out_obs[GB_OBS_DIM]);

/* Rotate a world-space vector into the bot's own yaw-only frame
 * (forward, right, up) -- see the frame convention above. */
void rb_world_to_local(const float world[3], float yaw_deg, float out_local[3]);

/* Pick, order and return up to GB_MAX_ENTITIES indices into `entities`
 * (0..n-1), threat-sorted: visible enemies first, then visible teammates,
 * then non-visible enemies, then non-visible teammates; ascending distance
 * within each group. Unfilled trailing slots are -1. Invalid (valid==0)
 * entries are skipped entirely. Ties (equal group and distance) keep their
 * original relative order (stable), which is what makes this testable. */
void rb_sort_entities_by_threat(const rb_entity_raw_t *entities, int n,
                                 const float bot_pos[3],
                                 int out_idx[GB_MAX_ENTITIES]);

/* NaN-safe clamp: NaN (which fails every comparison) maps to `lo`, not
 * "passed through" -- see gb_client.c's gb_clamp for why this matters: a
 * half-trained net emits NaN long before it emits good play, and this is the
 * observation side of the same discipline. */
float rb_clamp(float v, float lo, float hi);

/* NaN-safe clamp-and-normalise: (v - lo) / (hi - lo), clamped to 0..1.
 * hi <= lo (an "unknown maximum", e.g. clip size the engine could not
 * determine) returns 0 rather than dividing by zero or a negative range. */
float rb_norm01(float v, float lo, float hi);

#ifdef __cplusplus
}
#endif

#endif /* RETROBOT_CORE_H */

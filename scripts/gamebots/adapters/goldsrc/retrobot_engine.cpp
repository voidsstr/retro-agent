/* retrobot_engine.cpp — the Metamod plugin half of the GoldSrc adapter.
 *
 * *** STATUS: BUILDS AND DLOPENS; NEVER RUN AGAINST A LIVE HLDS. See
 * *** README.md "Build status" before trusting anything here. It compiles,
 * *** links, and loads cleanly as a 32-bit .so (verified: `file` reports
 * *** ELF 32-bit Intel 80386; a standalone dlopen()/dlsym() harness finds
 * *** every Metamod entry point with no undefined symbols; a second harness
 * *** calls the real Meta_Query() and gets back the correct plugin_info_t).
 * *** Getting this far required a 32-bit toolchain this host didn't have
 * *** installed and fixing three real bugs a compiler found that
 * *** header-matching alone had missed -- see README.md "Bugs a compiler
 * *** found" for what they were and why each one was invisible without a
 * *** compiler. What has NOT run: Meta_Attach, GiveFnptrsToDll, the
 * *** pfnStartFrame hook, pfnCreateFakeClient, or anything else that
 * *** touches g_engfuncs/gpGlobals -- those need a real engine behind them.
 * *** Do not deploy this to cs16-server / cs16-noblood / specialists-server
 * *** without testing it against a throwaway HLDS instance first.
 *
 * What this does, once built and loaded as a Metamod plugin:
 *   - a server console command creates fakeclient bots (pfnCreateFakeClient,
 *     FL_FAKECLIENT) and auto-joins them (jointeam/joinclass "auto" — the
 *     CS 1.6 convention of team/class id 5 meaning "let the server pick",
 *     used the same way by every public CS bot plugin; not from any leaked
 *     SDK, just how a real player's menu selection maps to a client command);
 *   - once a frame (pfnStartFrame, POST) it builds an rb_raw_obs_t per bot
 *     from real, generically-available HLSDK state, hands it to
 *     retrobot_core.c's rb_build_observation() for the schema packing, and
 *     round-trips the whole batch through gb_client;
 *   - on GB_OK it applies the returned action via pfnRunPlayerMove, which is
 *     the same entry point the engine uses to run a real client's usercmd
 *     through the game's own player-movement code;
 *   - on GB_FALLBACK (no policy server, timeout, schema mismatch) each bot
 *     holds its current view angle and stands still. That is a deliberate,
 *     honest fallback, not a placeholder for a missing feature: vanilla
 *     HLSDK/CS ships no bot AI of its own to fall back to (RealBot/HPB-bot
 *     bring their own), and "stand still" is exactly what the task brief
 *     allows for an engine with none.
 *
 * What is genuinely NOT extracted, and why (see README.md "Zero-filled
 * fields" for the full list and the reasoning):
 *   - backup/reserve ammo per ammo type (CS tracks this in CBasePlayer's
 *     private m_rgAmmo[], which a Metamod plugin cannot see without the
 *     mod's own SDK headers -- which were not fetched, per the "don't grab
 *     dubious sources" instruction);
 *   - who damaged us and from where (needs a TakeDamage/TraceAttack
 *     interception this plugin does not install; only the HP delta is
 *     observable generically, so `took_damage` is real but `damage_dir` is
 *     zero);
 *   - round timer, score, and objective state (bomb planted, flag carried)
 *     -- CS's gamerules keeps these privately; nothing in the public
 *     dllapi/engine interface exposes them generically.
 * Every one of these is written as 0.0f, not a guess, and the fields that
 * ARE filled are lain out in a comment above build_raw_obs() with the exact
 * HLSDK source they came from.
 */

#include <extdll.h>
/* dlls/util.h has NO include guard (`#ifndef UTIL_H`) -- the real HLSDK
 * convention is that a .cpp includes it exactly once, directly, and nothing
 * else redundantly re-includes it. <meta_api.h> below already pulls it in
 * transitively (meta_api.h -> dllapi.h -> sdk_util.h -> <util.h>), so this
 * file must NOT also include it directly -- doing so double-parses a header
 * with no guard and every default-argument / class declaration in it
 * becomes a hard "redefinition" error. Confirmed by hitting exactly that
 * error the first time a compiler actually saw this file. */
#include <in_buttons.h>
/* entity_state.h declares clientdata_t (struct clientdata_s) AND pulls in
 * weaponinfo.h for weapon_data_t -- neither is reachable through extdll.h's
 * own include chain (confirmed: extdll.h -> eiface.h never includes
 * entity_state.h). Missing this was the actual first build failure: the
 * compiler read "clientdata_t" as an undeclared identifier, not as a
 * mistyped API call. */
#include <entity_state.h>

#include <meta_api.h>
#include "sdk_util.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "retrobot_core.h"
}
extern "C" {
#include "../gb_client.h"
}

/* ------------------------------------------------------------------ */
/* engine function table -- same pattern as metamod-hl1's own
 * stub_plugin/h_export.cpp: a Metamod plugin is handed the real
 * enginefuncs_t/globalvars_t exactly like a normal mod DLL is, via
 * GiveFnptrsToDll, which metamod forwards on to every loaded plugin. This
 * is the ONE place g_engfuncs/gpGlobals are populated; everything above
 * uses them assuming this has already run, which the engine guarantees by
 * calling GiveFnptrsToDll before anything else in the DLL.               */
/* ------------------------------------------------------------------ */

enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;

extern "C" void WINAPI GiveFnptrsToDll(enginefuncs_t *pengfuncsFromEngine, globalvars_t *pGlobals)
{
    memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
    gpGlobals = pGlobals;
}

/* Avoid pulling in libstdc++ for a plugin that uses only a handful of C++
 * conveniences (Vector's operator overloads, none of which need RTTI or
 * exceptions) -- verbatim convention from stub_plugin/h_export.cpp. */
#if defined(linux) || defined(__linux__)
extern "C" void __cxa_pure_virtual(void) { }
void *operator new(size_t size)   { return malloc(size); }
void *operator new[](size_t size) { return malloc(size); }
void operator delete(void *ptr)   { free(ptr); }
void operator delete[](void *ptr) { free(ptr); }
#endif

/* dlls/util.h declares `UTIL_LogPrintf(char *fmt, ...)` (non-const), but
 * metamod-hl1/metamod/sdk_util.cpp -- the actual, only implementation any
 * plugin links against, since metamod ships no shared runtime plugins link
 * a copy of it (see this Makefile's sdk_util.o rule) -- DEFINES it as
 * `UTIL_LogPrintf(const char *fmt, ...)`. In C++ those are two DIFFERENT
 * overloads with two different mangled names, not the same function
 * redeclared. A call through dlls/util.h's declaration therefore builds and
 * *links* "successfully" (a .so is allowed unresolved symbols at build
 * time) but fails to dlopen at runtime: "undefined symbol:
 * _Z14UTIL_LogPrintfPcz". Confirmed by hitting exactly that with a dlopen
 * smoke test. Redeclaring the REAL (const) signature here makes every
 * string-literal call site in this file bind to the overload that exists,
 * since an exact `const char*` match is preferred over the non-const
 * overload's deprecated string-literal conversion. */
extern void UTIL_LogPrintf(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* constants                                                           */
/* ------------------------------------------------------------------ */

#define RB_FAR_PLANE       4096.0f   /* world units; matches a typical CS map's scale */
#define RB_DEFAULT_MAXSPEED 320.0f   /* CS default ground speed, used only until the
                                       * real per-player value arrives via
                                       * pfnUpdateClientData (see g_cd_cache) */
#define RB_DEFAULT_MAXARMOR 100.0f   /* CS's armor cap; entvars_t has no generic
                                       * "max armor" field, so this is a documented
                                       * constant, not a discovered one */
#define RB_MAX_TRACKED_EDICTS 65     /* 1-based edict index, generous over a 32-slot
                                       * server so a player-slot-count cvar change
                                       * mid-map can't index out of bounds */
#define RB_CACHE_STALE_FRAMES 4      /* a weapon/clientdata cache entry older than
                                       * this many StartFrames reads as "unknown"
                                       * rather than stale data from a player who
                                       * disconnected */

/* CS 1.6's "auto-assign" id for both `jointeam` and `joinclass` -- the
 * server picks the least-populated side/class. Documented public convention
 * (every CS bot plugin does this), not from a leaked SDK. */
#define RB_CS_AUTO_JOIN 5

/* Not relying on <math.h>'s M_PI: it's a common extension, not standard C++,
 * and retrobot_core.c already makes the same call for the same reason. */
#define RB_PI 3.14159265358979323846f

/* ------------------------------------------------------------------ */
/* per-edict caches, filled by POST hooks on functions the engine calls
 * to collect network-update data -- see pfnGetWeaponData_Post and
 * pfnUpdateClientData_Post below. This is the standard, publicly
 * documented way a Metamod plugin observes a player's current weapon and
 * ammo without the mod's own private CBasePlayer layout: read what the
 * game DLL already computed for the network snapshot.               */
/* ------------------------------------------------------------------ */

struct rb_wdata_cache_t {
    int    valid;
    int    frame_stamp;
    weapon_data_t data[32];   /* indexed by weapon id, matches weapon_data_t[64]
                                * cap in entity_state.h; 32 covers every real
                                * HLSDK/CS weapon id with room to spare */
};

struct rb_cdata_cache_t {
    int    valid;
    int    frame_stamp;
    clientdata_t cd;
};

static rb_wdata_cache_t g_wdata_cache[RB_MAX_TRACKED_EDICTS];
static rb_cdata_cache_t g_cdata_cache[RB_MAX_TRACKED_EDICTS];
static int g_frame_counter = 0;

/* ------------------------------------------------------------------ */
/* bot registry                                                        */
/* ------------------------------------------------------------------ */

#define RB_MAX_BOTS 32   /* one per possible server slot; GB_MAX_BOTS (256) is
                           * far larger, this cap is the GoldSrc server's own
                           * maxplayers ceiling, not a gb_client limit */

struct rb_bot_t {
    int      in_use;
    edict_t *ed;
    uint16_t gb_id;          /* stable id sent on the gamebots wire; NOT the
                               * edict index, which is reused across a
                               * disconnect/reconnect */
    float    prev_health;
    int      prev_frags;
    int      prev_deadflag;
};

static rb_bot_t g_bots[RB_MAX_BOTS];
static uint16_t g_next_gb_id = 1;
static gb_client_t g_gb;
static int g_gb_inited = 0;

static rb_bot_t *rb_find_bot(edict_t *ed)
{
    for (int i = 0; i < RB_MAX_BOTS; i++)
        if (g_bots[i].in_use && g_bots[i].ed == ed)
            return &g_bots[i];
    return NULL;
}

static rb_bot_t *rb_free_bot_slot(void)
{
    for (int i = 0; i < RB_MAX_BOTS; i++)
        if (!g_bots[i].in_use)
            return &g_bots[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* extraction                                                           */
/* ------------------------------------------------------------------ */

/* One horizontal ray every 22.5 degrees, starting straight ahead (the
 * bot's current yaw) and going clockwise -- matches
 * scripts/gamebots/schema.py's GB_NUM_RAYS_H doc string exactly. */
static void rb_cast_rays(edict_t *bot, float yaw_deg, float out_h[GB_NUM_RAYS_H],
                          float *out_up, float *out_down)
{
    Vector eye = bot->v.origin + bot->v.view_ofs;
    for (int i = 0; i < GB_NUM_RAYS_H; i++) {
        float a = (yaw_deg + i * (360.0f / GB_NUM_RAYS_H)) * (RB_PI / 180.0f);
        Vector dir(cosf((float)a), sinf((float)a), 0.0f);
        Vector end = eye + dir * RB_FAR_PLANE;
        TraceResult tr;
        g_engfuncs.pfnTraceLine((float *)&eye, (float *)&end, ignore_monsters, bot, &tr);
        Vector hit = Vector(tr.vecEndPos) - eye;
        out_h[i] = hit.Length();
    }
    {
        Vector up_end = eye + Vector(0, 0, RB_FAR_PLANE);
        Vector down_end = eye - Vector(0, 0, RB_FAR_PLANE);
        TraceResult tr;
        g_engfuncs.pfnTraceLine((float *)&eye, (float *)&up_end, ignore_monsters, bot, &tr);
        *out_up = (Vector(tr.vecEndPos) - eye).Length();
        g_engfuncs.pfnTraceLine((float *)&eye, (float *)&down_end, ignore_monsters, bot, &tr);
        *out_down = (eye - Vector(tr.vecEndPos)).Length();
    }
}

static int rb_is_visible(edict_t *from, edict_t *target)
{
    Vector eye = from->v.origin + from->v.view_ofs;
    Vector their_eye = target->v.origin + target->v.view_ofs;
    TraceResult tr;
    g_engfuncs.pfnTraceLine((float *)&eye, (float *)&their_eye, ignore_monsters, from, &tr);
    return (tr.flFraction >= 0.99f) || (tr.pHit == target);
}

/* Gather every other connected player as a raw entity candidate.
 * is_teammate uses entvars_t.team -- a GENERIC field (see progdefs.h), but
 * whether a given mod's game DLL keeps it in sync with its own team concept
 * is mod-specific. This is real engine data, not a guess; if a live test
 * shows CS 1.6 doesn't populate it the way expected, that is a finding to
 * record, not a reason to fabricate a substitute. */
static int rb_gather_entities(edict_t *bot, rb_entity_raw_t *out, int max_out)
{
    int n = 0;
    for (int i = 1; i <= gpGlobals->maxClients && n < max_out; i++) {
        edict_t *ed = g_engfuncs.pfnPEntityOfEntIndex(i);
        if (!ed || ed->free || ed == bot)
            continue;
        if (!(ed->v.flags & FL_CLIENT) && !(ed->v.flags & FL_FAKECLIENT))
            continue;
        if (ed->v.deadflag != DEAD_NO)
            continue;               /* dead/respawning players aren't a threat */

        rb_entity_raw_t *e = &out[n];
        memset(e, 0, sizeof(*e));
        e->valid = 1;
        e->is_teammate = (ed->v.team == bot->v.team);
        e->visible = rb_is_visible(bot, ed);
        e->pos[0] = ed->v.origin.x; e->pos[1] = ed->v.origin.y; e->pos[2] = ed->v.origin.z;
        e->vel[0] = ed->v.velocity.x; e->vel[1] = ed->v.velocity.y; e->vel[2] = ed->v.velocity.z;
        e->health_frac = (ed->v.max_health > 0.0f)
            ? rb_clamp(ed->v.health / ed->v.max_health, 0.0f, 1.0f) : 0.0f;
        n++;
    }
    return n;
}

/* Field-by-field provenance (so a reviewer never has to guess which of
 * these are real and which are placeholders):
 *
 *   pos/vel/angles/health/max_health/armor/on_ground/crouching/deadflag
 *       -- entvars_t (progdefs.h), generic to every GoldSrc mod, always real.
 *   in_water, maxspeed, current weapon id, reloading
 *       -- clientdata_t / weapon_data_t, captured from the POST hooks below;
 *          zero/"unknown" if the cache is empty or stale (e.g. this bot has
 *          never had pfnUpdateClientData called for it yet this life).
 *   ammo_clip / ammo_clip_max, ammo_reserve / ammo_reserve_max, weapon_id_max
 *       -- NOT available generically (see file header); left at 0/0 so
 *          rb_norm01() reads them as 0, honestly, not NaN.
 *   took_damage, killed_someone, died
 *       -- derived from this-frame-vs-last-frame deltas in health/frags/
 *          deadflag, which ARE generic and real; damage_dir stays zero
 *          because "who hit me from where" needs a hook this plugin does
 *          not install.
 *   round_time_frac, score_diff_norm, objective
 *       -- not available generically from CS's gamerules; left at 0.
 *   teammates_alive_frac, enemies_alive_frac
 *       -- computed here from a live scan of connected players by team,
 *          real and cheap (<= maxClients, done once per bot per frame). */
static void rb_build_raw_obs(rb_bot_t *bot_reg, rb_raw_obs_t *raw)
{
    edict_t *bot = bot_reg->ed;
    memset(raw, 0, sizeof(*raw));

    raw->far_plane = RB_FAR_PLANE;

    raw->pos[0] = bot->v.origin.x; raw->pos[1] = bot->v.origin.y; raw->pos[2] = bot->v.origin.z;
    raw->vel[0] = bot->v.velocity.x; raw->vel[1] = bot->v.velocity.y; raw->vel[2] = bot->v.velocity.z;
    raw->angles[0] = bot->v.v_angle.x; raw->angles[1] = bot->v.v_angle.y; raw->angles[2] = bot->v.v_angle.z;

    raw->health = bot->v.health;
    raw->max_health = (bot->v.max_health > 0.0f) ? bot->v.max_health : 100.0f;
    raw->armor = bot->v.armorvalue;
    raw->max_armor = RB_DEFAULT_MAXARMOR;

    raw->on_ground = (bot->v.flags & FL_ONGROUND) ? 1 : 0;
    raw->crouching = (bot->v.flags & FL_DUCKING) ? 1 : 0;
    raw->alive = (bot->v.deadflag == DEAD_NO) ? 1 : 0;

    /* waterlevel and maxspeed are, somewhat surprisingly, generic entvars_t
     * fields (progdefs.h) that the engine's own physics keeps current for
     * EVERY edict, fake clients included -- confirmed against
     * dlls/client.cpp's own UpdateClientData (`cd->waterlevel = pev->waterlevel`,
     * `cd->maxspeed = pev->maxspeed`), which just forwards these same
     * values. Reading them straight off pev is strictly better than the
     * clientdata_t cache below: it doesn't depend on the engine having
     * bothered to call pfnUpdateClientData for a bot with nobody to network
     * a snapshot to, which is a real and unverified risk (see the weapon
     * cache comment right below). */
    raw->in_water = (bot->v.waterlevel > 1) ? 1 : 0;
    raw->max_speed = (bot->v.maxspeed > 1.0f) ? bot->v.maxspeed : RB_DEFAULT_MAXSPEED;

    /* Current weapon id and reload state are NOT in entvars_t (they live in
     * the mod's private CBasePlayer/CBasePlayerWeapon objects) -- these two
     * fields are the one place this adapter genuinely depends on the
     * pfnUpdateClientData / pfnGetWeaponData POST hooks below having fired
     * for this bot. UNVERIFIED RISK: those two engine callbacks exist to
     * fill network snapshots for real clients; whether the engine bothers
     * calling them for a fakeclient with no connection to send a snapshot
     * to is not something this file can determine without a live server.
     * If it turns out the engine skips them for bots, this degrades exactly
     * the way an unknown value is supposed to: weapon_id_max stays 0, so
     * rb_norm01() reads weapon_id_norm as 0, not NaN, and `reloading` stays
     * 0 -- see README.md "Zero-filled fields" for the flag to go verify. */
    {
        int idx = g_engfuncs.pfnIndexOfEdict(bot);
        if (idx > 0 && idx < RB_MAX_TRACKED_EDICTS) {
            rb_cdata_cache_t *cc = &g_cdata_cache[idx];
            if (cc->valid && (g_frame_counter - cc->frame_stamp) <= RB_CACHE_STALE_FRAMES) {
                raw->weapon_id = cc->cd.m_iId;
                raw->weapon_id_max = 32;    /* MAX_WEAPONS, dlls/weapons.h */
            }
            rb_wdata_cache_t *wc = &g_wdata_cache[idx];
            if (wc->valid && (g_frame_counter - wc->frame_stamp) <= RB_CACHE_STALE_FRAMES &&
                raw->weapon_id > 0 && raw->weapon_id < 32) {
                raw->reloading = wc->data[raw->weapon_id].m_fInReload ? 1 : 0;
                /* Clip COUNT is real (m_iClip, confirmed in dlls/client.cpp's
                 * GetWeaponData); clip SIZE (the denominator a fraction
                 * needs) is not exposed anywhere generic -- see the file
                 * header. Left at 0 deliberately: reporting a raw count as
                 * if it were already normalised would be worse than
                 * reporting nothing. */
            }
        }
    }

    rb_cast_rays(bot, raw->angles[1], raw->ray_h, &raw->ray_up, &raw->ray_down);
    raw->n_entities = rb_gather_entities(bot, raw->entities, RB_MAX_ENTITIES_RAW);

    {
        float dmg = bot_reg->prev_health - bot->v.health;
        raw->took_damage = (dmg > 0.0f) ? dmg : 0.0f;
        /* damage_dir_world is left zero -- see file header. */
        raw->killed_someone = (bot->v.frags > bot_reg->prev_frags) ? 1 : 0;
        raw->died = (bot_reg->prev_deadflag == DEAD_NO && bot->v.deadflag != DEAD_NO) ? 1 : 0;
        bot_reg->prev_health = bot->v.health;
        bot_reg->prev_frags = (int)bot->v.frags;
        bot_reg->prev_deadflag = bot->v.deadflag;
    }

    {
        int team_total = 0, team_alive = 0, enemy_total = 0, enemy_alive = 0;
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t *ed = g_engfuncs.pfnPEntityOfEntIndex(i);
            if (!ed || ed->free) continue;
            if (!(ed->v.flags & FL_CLIENT) && !(ed->v.flags & FL_FAKECLIENT)) continue;
            int alive = (ed->v.deadflag == DEAD_NO) ? 1 : 0;
            if (ed->v.team == bot->v.team) { team_total++; team_alive += alive; }
            else                            { enemy_total++; enemy_alive += alive; }
        }
        raw->teammates_alive_frac = (team_total > 0) ? (float)team_alive / (float)team_total : 0.0f;
        raw->enemies_alive_frac = (enemy_total > 0) ? (float)enemy_alive / (float)enemy_total : 0.0f;
    }
    /* round_time_frac, score_diff_norm, objective: left zero, see file header. */
}

/* ------------------------------------------------------------------ */
/* action application                                                   */
/* ------------------------------------------------------------------ */

static unsigned short rb_buttons_to_in(uint16_t gb_buttons)
{
    unsigned short b = 0;
    if (gb_buttons & GB_BTN_ATTACK)  b |= IN_ATTACK;
    if (gb_buttons & GB_BTN_ATTACK2) b |= IN_ATTACK2;
    if (gb_buttons & GB_BTN_JUMP)    b |= IN_JUMP;
    if (gb_buttons & GB_BTN_CROUCH)  b |= IN_DUCK;
    if (gb_buttons & GB_BTN_RELOAD)  b |= IN_RELOAD;
    if (gb_buttons & GB_BTN_USE)     b |= IN_USE;
    /* GB_BTN_WALK -> IN_RUN and GB_BTN_ZOOM -> IN_ALT1 are best-effort
     * analogues, not verified against a live CS 1.6 client build -- flag
     * this if a real test shows the mapping is wrong. */
    if (gb_buttons & GB_BTN_WALK)    b |= IN_RUN;
    if (gb_buttons & GB_BTN_ZOOM)    b |= IN_ALT1;
    return b;
}

static void rb_apply_action(rb_bot_t *bot_reg, const gb_action_t *act)
{
    edict_t *bot = bot_reg->ed;
    gb_action_t a = *act;
    gb_clamp(&a);        /* never trust the policy -- see gb_client.h */

    float new_pitch = bot->v.v_angle.x + a.pitch_delta;
    if (new_pitch > 89.0f) new_pitch = 89.0f;
    if (new_pitch < -89.0f) new_pitch = -89.0f;
    float new_yaw = bot->v.v_angle.y + a.yaw_delta;
    while (new_yaw > 180.0f) new_yaw -= 360.0f;
    while (new_yaw < -180.0f) new_yaw += 360.0f;

    float viewangles[3] = { new_pitch, new_yaw, 0.0f };
    bot->v.v_angle.x = new_pitch;
    bot->v.v_angle.y = new_yaw;

    /* pev->maxspeed directly, same reasoning as rb_build_raw_obs() above --
     * real for every edict, no dependency on the network-callback caches. */
    float ms = (bot->v.maxspeed > 1.0f) ? bot->v.maxspeed : RB_DEFAULT_MAXSPEED;

    unsigned short buttons = rb_buttons_to_in(a.buttons);
    int msec = (int)(gpGlobals->frametime * 1000.0f);
    if (msec < 1) msec = 1;
    if (msec > 255) msec = 255;

    g_engfuncs.pfnRunPlayerMove(bot, viewangles, a.forward * ms, a.side * ms,
                                 0.0f, buttons, 0, (byte)msec);
}

static void rb_apply_fallback_hold_still(rb_bot_t *bot_reg)
{
    edict_t *bot = bot_reg->ed;
    float viewangles[3] = { bot->v.v_angle.x, bot->v.v_angle.y, 0.0f };
    int msec = (int)(gpGlobals->frametime * 1000.0f);
    if (msec < 1) msec = 1;
    if (msec > 255) msec = 255;
    /* No policy this frame: stand still, no buttons, keep looking where it
     * already was. See the file header for why this -- not a copied engine
     * AI -- is the honest fallback here. */
    g_engfuncs.pfnRunPlayerMove(bot, viewangles, 0.0f, 0.0f, 0.0f, 0, 0, (byte)msec);
}

/* ------------------------------------------------------------------ */
/* dllapi hooks                                                         */
/* ------------------------------------------------------------------ */

static void RB_ServerActivate_Post(edict_t *pEdictList, int edictCount, int clientMax)
{
    memset(g_wdata_cache, 0, sizeof(g_wdata_cache));
    memset(g_cdata_cache, 0, sizeof(g_cdata_cache));
    memset(g_bots, 0, sizeof(g_bots));
    g_frame_counter = 0;
    RETURN_META(MRES_IGNORED);
}

static void RB_ServerDeactivate_Post(void)
{
    if (g_gb_inited) {
        gb_client_close(&g_gb);
        g_gb_inited = 0;
    }
    memset(g_bots, 0, sizeof(g_bots));
    RETURN_META(MRES_IGNORED);
}

static void RB_ClientPutInServer_Post(edict_t *pEntity)
{
    if (pEntity->v.flags & FL_FAKECLIENT) {
        rb_bot_t *slot = rb_find_bot(pEntity);
        if (!slot)
            slot = rb_free_bot_slot();
        if (slot) {
            memset(slot, 0, sizeof(*slot));
            slot->in_use = 1;
            slot->ed = pEntity;
            slot->gb_id = g_next_gb_id++;
            slot->prev_health = pEntity->v.health;
            slot->prev_deadflag = pEntity->v.deadflag;
            /* CS 1.6 auto-join convention -- see file header. */
            g_engfuncs.pfnClientCommand(pEntity, (char *)"jointeam %d\n", RB_CS_AUTO_JOIN);
            g_engfuncs.pfnClientCommand(pEntity, (char *)"joinclass %d\n", RB_CS_AUTO_JOIN);
        } else {
            UTIL_LogPrintf("retrobot: no free bot slot for fake client, ignoring it\n");
        }
    }
    RETURN_META(MRES_IGNORED);
}

static void RB_ClientDisconnect_Post(edict_t *pEntity)
{
    rb_bot_t *slot = rb_find_bot(pEntity);
    if (slot)
        memset(slot, 0, sizeof(*slot));
    RETURN_META(MRES_IGNORED);
}

static int RB_GetWeaponData_Post(edict_t *player, weapon_data_t *info)
{
    int idx = g_engfuncs.pfnIndexOfEdict(player);
    if (idx > 0 && idx < RB_MAX_TRACKED_EDICTS && info) {
        rb_wdata_cache_t *wc = &g_wdata_cache[idx];
        memcpy(wc->data, info, sizeof(wc->data));
        wc->valid = 1;
        wc->frame_stamp = g_frame_counter;
    }
    RETURN_META_VALUE(MRES_IGNORED, 0);
}

static void RB_UpdateClientData_Post(const edict_t *ent, int sendweapons, clientdata_t *cd)
{
    int idx = g_engfuncs.pfnIndexOfEdict((edict_t *)ent);
    if (idx > 0 && idx < RB_MAX_TRACKED_EDICTS && cd) {
        rb_cdata_cache_t *cc = &g_cdata_cache[idx];
        cc->cd = *cd;
        cc->valid = 1;
        cc->frame_stamp = g_frame_counter;
    }
    RETURN_META(MRES_IGNORED);
}

/* The main loop: one gb_client round trip per frame for every registered
 * bot on this server. Bounded and non-blocking by construction -- gb_client
 * itself never waits past its timeout_us (see gb_client.h), and everything
 * else here is O(bots * (rays + players)), no unbounded loops, no
 * allocation. */
static void RB_StartFrame_Post(void)
{
    g_frame_counter++;

    if (!g_gb_inited) {
        gb_client_init(&g_gb, NULL);   /* NULL -> GAMEBOTS_SOCKET env or the
                                         * default /run/user/1000/gamebots/policy.sock */
        g_gb_inited = 1;
    }

    int n_active = 0;
    rb_bot_t *active[RB_MAX_BOTS];
    float obsbuf[GB_OBS_DIM];

    gb_begin(&g_gb, (uint32_t)g_frame_counter);

    for (int i = 0; i < RB_MAX_BOTS; i++) {
        rb_bot_t *b = &g_bots[i];
        if (!b->in_use || !b->ed || b->ed->free)
            continue;
        if (b->ed->v.deadflag != DEAD_NO) {
            /* Dead/respawning bots still hold their slot but skip the
             * policy round trip -- pfnRunPlayerMove on a dead player is a
             * no-op in the engine's own player-move code anyway, and this
             * keeps a round with lots of dead bots cheap. */
            continue;
        }
        rb_raw_obs_t raw;
        rb_build_raw_obs(b, &raw);
        rb_build_observation(&raw, obsbuf);
        if (gb_add(&g_gb, b->gb_id, obsbuf) != 0)
            break;                   /* batch full (GB_MAX_BOTS=256); should
                                       * never happen at RB_MAX_BOTS<=32 */
        active[n_active++] = b;
    }

    gb_result_t r = gb_exchange(&g_gb);

    for (int i = 0; i < n_active; i++) {
        rb_bot_t *b = active[i];
        const gb_action_t *act = (r == GB_OK) ? gb_action(&g_gb, (uint16_t)i) : NULL;
        if (act)
            rb_apply_action(b, act);
        else
            rb_apply_fallback_hold_still(b);
    }

    RETURN_META(MRES_IGNORED);
}

/* ------------------------------------------------------------------ */
/* server command: retrobot_addbot [count]                              */
/* ------------------------------------------------------------------ */

static void RB_Cmd_AddBot(void)
{
    int count = 1;
    if (g_engfuncs.pfnCmd_Argc() >= 2) {
        int v = atoi(g_engfuncs.pfnCmd_Argv(1));
        if (v > 0)
            count = v;
    }
    for (int i = 0; i < count; i++) {
        char name[32];
        snprintf(name, sizeof(name), "RetroBot_%d", g_next_gb_id);
        edict_t *ed = g_engfuncs.pfnCreateFakeClient(name);
        if (!ed) {
            UTIL_LogPrintf("retrobot: pfnCreateFakeClient failed "
                           "(server full or engine refused)\n");
            break;
        }
        /* Registration happens in RB_ClientPutInServer_Post once the
         * engine finishes spawning it -- do not touch pEntity->v here. */
    }
}

/* ------------------------------------------------------------------ */
/* metamod plugin boilerplate                                          */
/* ------------------------------------------------------------------ */

/* One line per DLL_FUNCTIONS field, in the exact order eiface.h declares
 * them (verified against build/hlsdk/engine/eiface.h field-by-field, and
 * cross-checked against metamod-hl1/stub_plugin/dllapi.cpp's own listing) --
 * spelled out individually rather than grouped, because a miscounted run of
 * NULLs here silently binds a hook to the WRONG engine callback, and that
 * is exactly the kind of error a compiler cannot catch for you. */
static DLL_FUNCTIONS gFunctionTable_Post = {
    NULL,                           /*  1 pfnGameInit */
    NULL,                           /*  2 pfnSpawn */
    NULL,                           /*  3 pfnThink */
    NULL,                           /*  4 pfnUse */
    NULL,                           /*  5 pfnTouch */
    NULL,                           /*  6 pfnBlocked */
    NULL,                           /*  7 pfnKeyValue */
    NULL,                           /*  8 pfnSave */
    NULL,                           /*  9 pfnRestore */
    NULL,                           /* 10 pfnSetAbsBox */
    NULL,                           /* 11 pfnSaveWriteFields */
    NULL,                           /* 12 pfnSaveReadFields */
    NULL,                           /* 13 pfnSaveGlobalState */
    NULL,                           /* 14 pfnRestoreGlobalState */
    NULL,                           /* 15 pfnResetGlobalState */
    NULL,                           /* 16 pfnClientConnect */
    RB_ClientDisconnect_Post,       /* 17 pfnClientDisconnect */
    NULL,                           /* 18 pfnClientKill */
    RB_ClientPutInServer_Post,      /* 19 pfnClientPutInServer */
    NULL,                           /* 20 pfnClientCommand */
    NULL,                           /* 21 pfnClientUserInfoChanged */
    RB_ServerActivate_Post,         /* 22 pfnServerActivate */
    RB_ServerDeactivate_Post,       /* 23 pfnServerDeactivate */
    NULL,                           /* 24 pfnPlayerPreThink */
    NULL,                           /* 25 pfnPlayerPostThink */
    RB_StartFrame_Post,             /* 26 pfnStartFrame */
    NULL,                           /* 27 pfnParmsNewLevel */
    NULL,                           /* 28 pfnParmsChangeLevel */
    NULL,                           /* 29 pfnGetGameDescription */
    NULL,                           /* 30 pfnPlayerCustomization */
    NULL,                           /* 31 pfnSpectatorConnect */
    NULL,                           /* 32 pfnSpectatorDisconnect */
    NULL,                           /* 33 pfnSpectatorThink */
    NULL,                           /* 34 pfnSys_Error */
    NULL,                           /* 35 pfnPM_Move */
    NULL,                           /* 36 pfnPM_Init */
    NULL,                           /* 37 pfnPM_FindTextureType */
    NULL,                           /* 38 pfnSetupVisibility */
    RB_UpdateClientData_Post,       /* 39 pfnUpdateClientData */
    NULL,                           /* 40 pfnAddToFullPack */
    NULL,                           /* 41 pfnCreateBaseline */
    NULL,                           /* 42 pfnRegisterEncoders */
    RB_GetWeaponData_Post,          /* 43 pfnGetWeaponData */
    NULL,                           /* 44 pfnCmdStart */
    NULL,                           /* 45 pfnCmdEnd */
    NULL,                           /* 46 pfnConnectionlessPacket */
    NULL,                           /* 47 pfnGetHullBounds */
    NULL,                           /* 48 pfnCreateInstancedBaselines */
    NULL,                           /* 49 pfnInconsistentFile */
    NULL,                           /* 50 pfnAllowLagCompensation */
};

C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion)
{
    if (!pFunctionTable) {
        UTIL_LogPrintf("GetEntityAPI2_Post called with null pFunctionTable\n");
        return FALSE;
    }
    if (*interfaceVersion != INTERFACE_VERSION) {
        UTIL_LogPrintf("GetEntityAPI2_Post version mismatch; requested=%d ours=%d\n",
                        *interfaceVersion, INTERFACE_VERSION);
        *interfaceVersion = INTERFACE_VERSION;
        return FALSE;
    }
    memcpy(pFunctionTable, &gFunctionTable_Post, sizeof(DLL_FUNCTIONS));
    return TRUE;
}

static META_FUNCTIONS gMetaFunctionTable = {
    NULL,                   /* pfnGetEntityAPI */
    NULL,                   /* pfnGetEntityAPI_Post */
    NULL,                   /* pfnGetEntityAPI2 -- no pre-hooks needed */
    GetEntityAPI2_Post,      /* pfnGetEntityAPI2_Post */
    NULL, NULL,             /* pfnGetNewDLLFunctions{,_Post} */
    NULL, NULL,             /* pfnGetEngineFunctions{,_Post} -- we use the
                              * real g_engfuncs directly, no need to hook it */
};

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION,
    "RetroBot GoldSrc Adapter",
    "0.1",
    __DATE__,
    "retro-agent gamebots",
    "https://github.com/",     /* no public repo yet */
    "RETROBOT",
    PT_STARTUP,       /* only loadable at server startup: bots created after
                        * ServerActivate can't be trivially reconciled with
                        * mid-map plugin (re)load */
    PT_ANYPAUSE,
};

meta_globals_t *gpMetaGlobals;
gamedll_funcs_t *gpGamedllFuncs;
mutil_funcs_t *gpMetaUtilFuncs;

C_DLLEXPORT int Meta_Query(const char * /*ifvers*/, plugin_info_t **pPlugInfo,
                            mutil_funcs_t *pMetaUtilFuncs)
{
    *pPlugInfo = &Plugin_info;
    gpMetaUtilFuncs = pMetaUtilFuncs;
    return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME /*now*/, META_FUNCTIONS *pFunctionTable,
                             meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs)
{
    if (!pMGlobals) {
        LOG_ERROR(PLID, "Meta_Attach called with null pMGlobals");
        return FALSE;
    }
    gpMetaGlobals = pMGlobals;
    if (!pFunctionTable) {
        LOG_ERROR(PLID, "Meta_Attach called with null pFunctionTable");
        return FALSE;
    }
    memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));
    gpGamedllFuncs = pGamedllFuncs;

    g_engfuncs.pfnAddServerCommand((char *)"retrobot_addbot", RB_Cmd_AddBot);
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME /*now*/, PL_UNLOAD_REASON /*reason*/)
{
    if (g_gb_inited) {
        gb_client_close(&g_gb);
        g_gb_inited = 0;
    }
    return TRUE;
}

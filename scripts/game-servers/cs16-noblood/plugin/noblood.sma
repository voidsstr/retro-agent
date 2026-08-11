/*
 * noblood.sma - vanilla Counter-Strike 1.6, minus the blood.
 *
 * Runs on the SERVER only. Players connect with a completely stock, unmodified
 * CS 1.6 client (including the non-Steam "BCS 1.6 Romania" build the retro
 * fleet runs) and simply never see blood. Nothing is downloaded to the client
 * and no client cvar is touched.
 *
 * HOW IT WORKS
 *   Blood in GoldSrc is not drawn by the server - it is drawn by the client in
 *   response to a temp entity the server broadcasts. Every temp entity arrives
 *   as engine message SVC_TEMPENTITY whose first byte is the TE_* type. Hook
 *   that message, and drop the three blood types before they leave the server.
 *   The client is never told to draw blood, so there is nothing to render and
 *   nothing to decal onto the wall.
 *
 *   This is deliberately the narrowest possible cut: bullet holes, sparks,
 *   smoke, ricochets, glass and explosions all still use their own TE_* types
 *   and are untouched, so the game reads exactly like vanilla otherwise.
 *
 * WHY NOT JUST SET THE CLIENT CVARS
 *   violence_hblood / violence_ablood / violence_hgibs / violence_agibs are
 *   CLIENT cvars. A server cannot set them. A plugin could stuffcmd them, but
 *   that silently rewrites a player's own settings and persists after they
 *   leave our server - rude, and trivially undone. Dropping the temp entity is
 *   authoritative and leaves the client's config alone.
 *
 * VERIFYING IT
 *   The server console command `noblood_stats` prints how many blood temp
 *   entities have been dropped. Shoot someone, run it, and the counter moves -
 *   that is the proof the hook is live, without needing to eyeball the screen.
 *
 * Requires: Metamod + AMX Mod X 1.8.2+ (see ../README.md).
 */

#include <amxmodx>

/* Engine message: every temp entity the server spawns is one of these.
 * (SVC_TEMPENTITY is 23 in the GoldSrc protocol.) */
#define SVC_TEMPENTITY 23

/* The blood temp entities, from GoldSrc's const.h. These three are the whole
 * of "blood" in CS 1.6 - there is no fourth. */
#define TE_BLOODSTREAM 101   /* directional blood spurt */
#define TE_BLOOD       103   /* HL-style blood stream */
#define TE_BLOODSPRITE 115   /* CS's hit puff + the decal it leaves behind */

new g_blocked           /* dropped this map */
new g_blocked_total     /* dropped since the server started */

public plugin_init()
{
    register_plugin("CS 1.6 No Blood", "1.0.0", "retro-agent")

    register_message(SVC_TEMPENTITY, "on_temp_entity")

    /* Lets anyone querying the server confirm the mod is actually loaded. */
    register_cvar("noblood_version", "1.0.0", FCVAR_SERVER | FCVAR_SPONLY)
    register_srvcmd("noblood_stats", "cmd_noblood_stats")
}

/* Called for every temp entity. Cheap: one integer read and a compare. */
public on_temp_entity()
{
    switch (get_msg_arg_int(1))
    {
        case TE_BLOODSTREAM, TE_BLOOD, TE_BLOODSPRITE:
        {
            g_blocked++
            g_blocked_total++
            return PLUGIN_HANDLED   /* dropped - never reaches any client */
        }
    }

    return PLUGIN_CONTINUE
}

public cmd_noblood_stats()
{
    server_print("[noblood] v1.0.0 active - dropped %d blood effects this map, %d since server start",
                 g_blocked, g_blocked_total)
    return PLUGIN_HANDLED
}

/* plugin_cfg runs after server.cfg on each map change. */
public plugin_cfg()
{
    g_blocked = 0
}

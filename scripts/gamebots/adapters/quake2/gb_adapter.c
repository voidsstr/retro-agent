/* gb_adapter — the Yamagi Quake II engine adapter.
 *
 * Compiled INTO the game module (baseq2/game.so). Unlike Quake III's qagame,
 * Yamagi's baseq2 ships NO bot AI at all: no botlib, no SVF_BOT, no notion of
 * a "fake client" anywhere in the source tree. So this adapter has to do two
 * jobs the Quake III one did not:
 *
 *   1. CREATE the bots. `gb_bots N` makes the adapter occupy N otherwise-free
 *      client slots with fake clients, using exactly the same ClientConnect()
 *      / ClientBegin() the engine calls for a real network connection --
 *      there is no separate "spawn a bot" API in Quake II, so we drive the
 *      real one directly. This is the technique every Quake II bot mod back
 *      to ACE/Eraser used, purely from game-DLL code, no engine changes.
 *   2. DRIVE them. A real client's usercmd_t is produced by the network
 *      layer and consumed by the engine calling ge->ClientThink() -- neither
 *      side of that exists for a fake client. So once a frame, for every bot,
 *      this adapter builds a usercmd_t itself (from the policy if one
 *      answered, otherwise from a tiny built-in wander-and-shoot fallback)
 *      and calls ClientThink() directly. "Apply your action after the
 *      engine's own AI" becomes "apply your action after OUR OWN fallback AI
 *      has filled it", because Quake II has no engine AI to defer to -- see
 *      the README for why that substitution is honest rather than a shortcut.
 *
 * ONE hook in g_main.c (g_main.patch, applied by build.sh): GB_RunFrame() as
 * the first statement of G_RunFrame(), mirroring where a real client's
 * ClientThink() would already have run (the engine calls that from the
 * network read path, before G_RunFrame(), not from within it).
 *
 * OFF BY DEFAULT, in two independent stages: `gb_bots 0` (default) means the
 * module never touches a client slot -- a server with this module installed
 * and gb_bots at 0 behaves exactly like stock baseq2. `gb_enable 0` (default)
 * means any bots that ARE spawned run the built-in fallback, not the policy.
 * Both have to be turned on for the neural policy to touch anything.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "header/local.h"
#include "gb_adapter.h"
#include "gb_client.h"

/* Quake II units (see pmove.c: pm_maxspeed=300, jump/duck thresholds are
 * +-10 on cmd.upmove regardless of magnitude). These are normalisation scale
 * factors, not new physics -- what matters is the policy seeing the same
 * scale on every map and engine. */
#define GB_FAR_PLANE      2048.0f
#define GB_MAX_SPEED       300.0f  /* pm_maxspeed, common/pmove.c */
#define GB_MAX_HEALTH      100.0f
#define GB_MAX_ARMOR       200.0f  /* Body Armor capacity, the biggest of the three */
#define GB_MAX_AMMO        100.0f  /* flat normaliser -- Q2 ammo pools vary by type and
                                     * there is no clip/reserve split to normalise against */
#define GB_CMD_SCALE       400.0f  /* forwardmove/sidemove magnitude; pmove clamps the
                                     * resultant wishspeed to GB_MAX_SPEED itself */
#define GB_JUMP_UPMOVE      400
#define GB_CROUCH_UPMOVE   -400

#define GB_NUM_WEAPONS 10  /* Blaster..BFG10K, vanilla baseq2 only (no ctf/rogue/xatrix) */

static gb_client_t gb;
static int         gb_ready;
static cvar_t      *gb_enable;
static cvar_t      *gb_bots;
static cvar_t      *gb_debug;
static int         gb_msec = 100;             /* usercmd_t.msec, derived from sv_fps */

static gb_action_t gb_action_of[MAX_CLIENTS + 1];
static int         gb_have_action[MAX_CLIENTS + 1];
static qboolean    gb_is_bot[MAX_CLIENTS + 1]; /* indexed by edict number, 1..maxclients */
static int         gb_bot_serial;

/* Per-client history for the observation's "what just happened" group -- the
 * engine hands us no deltas, same situation the Quake III adapter is in. */
static int  gb_prev_health[MAX_CLIENTS + 1];
static int  gb_prev_score[MAX_CLIENTS + 1];
static int  gb_prev_alive[MAX_CLIENTS + 1];

static gitem_t *gb_weapons[GB_NUM_WEAPONS + 1];  /* [1..GB_NUM_WEAPONS] */
static qboolean gb_weapons_ready;

/* Reported on change, not every frame -- at 10 logic fps that is still 36000
 * lines an hour if printed unconditionally. */
static int gb_reported_state = -1;

static float gb_clampf(float v, float lo, float hi)
{
	if (v != v) return 0.0f;             /* NaN */
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

/* Quake II's shared.h has no AngleNormalize180 (that is a Quake III-ism). */
static float gb_norm180(float a)
{
	a = fmodf(a, 360.0f);
	if (a < -180.0f) a += 360.0f;
	if (a > 180.0f)  a -= 360.0f;
	return a;
}

static void GB_ResolveWeapons(void)
{
	static const char *names[GB_NUM_WEAPONS + 1] = {
		NULL, "Blaster", "Shotgun", "Super Shotgun", "Machinegun", "Chaingun",
		"Grenade Launcher", "Rocket Launcher", "HyperBlaster", "Railgun", "BFG10K"
	};
	int i;

	for (i = 1; i <= GB_NUM_WEAPONS; i++)
		gb_weapons[i] = FindItem(names[i]);
	gb_weapons_ready = true;
}

static int GB_WeaponIndex(gitem_t *item)
{
	int i;

	if (!item)
		return 0;
	if (!gb_weapons_ready)
		GB_ResolveWeapons();
	for (i = 1; i <= GB_NUM_WEAPONS; i++)
		if (gb_weapons[i] == item)
			return i;
	return 0;
}

void GB_Init(void)
{
	cvar_t *fps;

	gb_client_init(&gb, NULL);
	gb_enable = gi.cvar("gb_enable", "0", 0);
	gb_bots   = gi.cvar("gb_bots", "0", 0);
	gb_debug  = gi.cvar("gb_debug", "0", 0);

	fps = gi.cvar("sv_fps", "10", 0);
	gb_msec = (int)gb_clampf(1000.0f / ((fps && fps->value > 0) ? fps->value : 10.0f),
	                        1.0f, 255.0f);

	memset(gb_is_bot, 0, sizeof(gb_is_bot));
	memset(gb_have_action, 0, sizeof(gb_have_action));
	memset(gb_prev_health, 0, sizeof(gb_prev_health));
	memset(gb_prev_score, 0, sizeof(gb_prev_score));
	memset(gb_prev_alive, 0, sizeof(gb_prev_alive));
	gb_weapons_ready = false;
	gb_bot_serial = 0;
	gb_ready = 1;

	gi.dprintf("gamebots: adapter loaded (schema 0x%08x, obs %d floats), socket %s -- "
	           "'gb_bots N' spawns fake-client bots, 'gb_enable 1' lets the policy "
	           "drive them (default: neither, module is inert)\n",
	           GB_SCHEMA_HASH, GB_OBS_DIM, gb.socket_path[0] ? gb.socket_path : "<none>");
}

void GB_Shutdown(void)
{
	if (gb_ready)
		gb_client_close(&gb);
	gb_ready = 0;
}

/* ------------------------------------------------------------------ obs */

/* Ego-centric, yaw-only body frame -- see the Quake III adapter for why
 * pitch is reported separately rather than folded into the frame. */
static void GB_ToLocal(const vec3_t fwd, const vec3_t right, const vec3_t up,
                       const vec3_t worldvec, float *out)
{
	/* NB: "world" is a macro in header/local.h ((&g_edicts[0])) -- do not
	 * reuse that name for a parameter here. */
	out[0] = DotProduct(worldvec, fwd);
	out[1] = DotProduct(worldvec, right);
	out[2] = DotProduct(worldvec, up);
}

static int GB_Visible(edict_t *self, vec3_t fromEye, vec3_t toEye, edict_t *target)
{
	trace_t tr = gi.trace(fromEye, NULL, NULL, toEye, self, MASK_SHOT);
	return (tr.fraction >= 1.0f || tr.ent == target);
}

typedef struct {
	int   entnum;
	float dist;
	int   visible;
} gb_cand_t;

static int GB_CandCompare(const void *a, const void *b)
{
	const gb_cand_t *x = (const gb_cand_t *)a;
	const gb_cand_t *y = (const gb_cand_t *)b;

	/* Visible first, then by distance -- baseq2 deathmatch has no teams, so
	 * there is no teammate tie-break here (unlike the Quake III adapter). */
	if (x->visible != y->visible) return y->visible - x->visible;
	if (x->dist < y->dist) return -1;
	if (x->dist > y->dist) return 1;
	return 0;
}

static void GB_BuildObs(int clientNum, float *obs)
{
	edict_t    *ent = &g_edicts[clientNum];
	gclient_t  *client = ent->client;
	vec3_t      fwd, right, up, eye, ang;
	gb_cand_t   cand[MAX_CLIENTS];
	int         ncand = 0;
	int         i, alive, enemies_alive = 0, bestOther = 0, armor_idx;
	float       rel[3];

	memset(obs, 0, sizeof(float) * GB_OBS_DIM);

	alive = (ent->health > 0 && client->ps.pmove.pm_type != PM_DEAD);

	VectorSet(ang, 0, client->ps.viewangles[YAW], 0);
	AngleVectors(ang, fwd, right, up);
	VectorCopy(ent->s.origin, eye);
	eye[2] += ent->viewheight;

	/* --- self --- */
	obs[GB_OBS_HEALTH_FRAC] = gb_clampf((float)ent->health / GB_MAX_HEALTH, 0, 1);

	armor_idx = ArmorIndex(ent);
	obs[GB_OBS_ARMOR_FRAC] = armor_idx ?
		gb_clampf((float)client->pers.inventory[armor_idx] / GB_MAX_ARMOR, 0, 1) : 0.0f;

	if (client->pers.weapon && client->ammo_index)
		obs[GB_OBS_AMMO_FRAC] = gb_clampf(
			(float)client->pers.inventory[client->ammo_index] / GB_MAX_AMMO, 0, 1);
	/* Quake II has one ammo pool per weapon and no reload -- ammo_reserve_frac
	 * and reloading stay zero, exactly the reasoning the Quake III adapter
	 * gives for the same missing mechanic. */
	obs[GB_OBS_WEAPON_ID_NORM] =
		gb_clampf((float)GB_WeaponIndex(client->pers.weapon) / GB_NUM_WEAPONS, 0, 1);

	GB_ToLocal(fwd, right, up, ent->velocity, rel);
	obs[GB_OBS_VEL_LOCAL + 0] = gb_clampf(rel[0] / GB_MAX_SPEED, -2, 2);
	obs[GB_OBS_VEL_LOCAL + 1] = gb_clampf(rel[1] / GB_MAX_SPEED, -2, 2);
	obs[GB_OBS_VEL_LOCAL + 2] = gb_clampf(rel[2] / GB_MAX_SPEED, -2, 2);
	obs[GB_OBS_SPEED_FRAC] = gb_clampf(VectorLength(ent->velocity) / GB_MAX_SPEED, 0, 2);
	obs[GB_OBS_PITCH_NORM] = gb_clampf(gb_norm180(client->ps.viewangles[PITCH]) / 90.0f, -1, 1);
	obs[GB_OBS_ON_GROUND]  = ent->groundentity ? 1.0f : 0.0f;
	obs[GB_OBS_CROUCHING]  = (client->ps.pmove.pm_flags & PMF_DUCKED) ? 1.0f : 0.0f;
	obs[GB_OBS_IN_WATER]   = (ent->waterlevel > 1) ? 1.0f : 0.0f;
	obs[GB_OBS_ALIVE]      = alive ? 1.0f : 0.0f;

	/* --- local geometry: 16 rays around us, plus up and down --- */
	for (i = 0; i < GB_NUM_RAYS_H; i++) {
		vec3_t dir, end, a2;
		trace_t tr;
		VectorSet(a2, 0, client->ps.viewangles[YAW] + (360.0f * i) / GB_NUM_RAYS_H, 0);
		AngleVectors(a2, dir, NULL, NULL);
		VectorMA(eye, GB_FAR_PLANE, dir, end);
		tr = gi.trace(eye, NULL, NULL, end, ent, MASK_PLAYERSOLID);
		obs[GB_OBS_RAY_H + i] = gb_clampf(tr.fraction, 0, 1);
	}
	{
		vec3_t end;
		trace_t tr;
		VectorCopy(eye, end); end[2] += GB_FAR_PLANE;
		tr = gi.trace(eye, NULL, NULL, end, ent, MASK_PLAYERSOLID);
		obs[GB_OBS_RAY_UP] = gb_clampf(tr.fraction, 0, 1);
		VectorCopy(eye, end); end[2] -= GB_FAR_PLANE;
		tr = gi.trace(eye, NULL, NULL, end, ent, MASK_PLAYERSOLID);
		obs[GB_OBS_RAY_DOWN] = gb_clampf(tr.fraction, 0, 1);
	}

	/* --- other players ---
	 * Vanilla baseq2 deathmatch has no team mode at all (that needs the ctf
	 * mod, which this server does not run), so every other connected player
	 * is scored as an enemy and is_teammate/teammates_alive_frac are always
	 * zero -- an honest zero-fill, not a bug. */
	for (i = 1; i <= game.maxclients; i++) {
		edict_t *other = &g_edicts[i];
		vec3_t   otherEye, delta;
		float    dist;

		if (i == clientNum || !other->inuse || !other->client)
			continue;
		if (!other->client->pers.connected)
			continue;

		VectorCopy(other->s.origin, otherEye);
		otherEye[2] += other->viewheight;
		VectorSubtract(otherEye, eye, delta);
		dist = VectorLength(delta);

		cand[ncand].entnum  = i;
		cand[ncand].dist    = dist;
		cand[ncand].visible = GB_Visible(ent, eye, otherEye, other);
		ncand++;

		if (other->health > 0 && other->client->ps.pmove.pm_type != PM_DEAD)
			enemies_alive++;
		if (other->client->resp.score > bestOther)
			bestOther = other->client->resp.score;
	}

	qsort(cand, ncand, sizeof(cand[0]), GB_CandCompare);

	for (i = 0; i < GB_MAX_ENTITIES && i < ncand; i++) {
		edict_t *other = &g_edicts[cand[i].entnum];
		vec3_t   otherEye, delta, dir;
		int      base = GB_OBS_E0_PRESENT + i * GB_ENT_SLOT_STRIDE;

		VectorCopy(other->s.origin, otherEye);
		otherEye[2] += other->viewheight;
		VectorSubtract(otherEye, eye, delta);
		if (cand[i].dist > 0.001f)
			VectorScale(delta, 1.0f / cand[i].dist, dir);
		else
			VectorClear(dir);

		obs[base + GB_ENT_PRESENT]  = 1.0f;
		obs[base + GB_ENT_TEAMMATE] = 0.0f;  /* no teams in vanilla baseq2 DM */
		GB_ToLocal(fwd, right, up, dir, rel);
		obs[base + GB_ENT_DIR + 0] = gb_clampf(rel[0], -1, 1);
		obs[base + GB_ENT_DIR + 1] = gb_clampf(rel[1], -1, 1);
		obs[base + GB_ENT_DIR + 2] = gb_clampf(rel[2], -1, 1);
		obs[base + GB_ENT_DIST]    = gb_clampf(cand[i].dist / GB_FAR_PLANE, 0, 1);
		GB_ToLocal(fwd, right, up, other->velocity, rel);
		obs[base + GB_ENT_RELVEL + 0] = gb_clampf(rel[0] / GB_MAX_SPEED, -2, 2);
		obs[base + GB_ENT_RELVEL + 1] = gb_clampf(rel[1] / GB_MAX_SPEED, -2, 2);
		obs[base + GB_ENT_HEALTH]  = gb_clampf((float)other->health / GB_MAX_HEALTH, 0, 1);
		obs[base + GB_ENT_VISIBLE] = cand[i].visible ? 1.0f : 0.0f;
	}

	/* --- what just happened ---
	 * Quake II accumulates per-frame damage in client->damage_blood and the
	 * point of impact in client->damage_from for the screen-flash effect,
	 * but p_view.c's ClientEndServerFrame() clears both at the tail of THIS
	 * G_RunFrame -- one statement before our hook runs again at the head of
	 * the next one. Reading them earlier would mean a second patch site
	 * inside combat code, which is out of scope for a "hook the usercmd"
	 * patch, so took_damage comes from the same health-delta trick Quake III
	 * uses (no engine change needed) and damage_dir stays zero: Quake II
	 * also has no persistent last-attacker field for PLAYER targets (only
	 * monsters get one, via ent->enemy in g_combat.c's M_ReactToDamage) so
	 * there is nothing honest to fill it with at this hook point. */
	{
		int dh = gb_prev_health[clientNum] - ent->health;
		if (dh > 0)
			obs[GB_OBS_TOOK_DAMAGE] = gb_clampf((float)dh / GB_MAX_HEALTH, 0, 1);
		if (client->resp.score > gb_prev_score[clientNum])
			obs[GB_OBS_KILLED_SOMEONE] = 1.0f;
		if (gb_prev_alive[clientNum] && !alive)
			obs[GB_OBS_DIED] = 1.0f;
	}

	/* --- match context --- */
	if (timelimit->value > 0)
		obs[GB_OBS_ROUND_TIME_FRAC] = gb_clampf(level.time / (timelimit->value * 60.0f), 0, 1);
	if (fraglimit->value > 0)
		obs[GB_OBS_SCORE_DIFF_NORM] = gb_clampf(
			(float)(client->resp.score - bestOther) / fraglimit->value, -1, 1);
	if (game.maxclients > 1) {
		obs[GB_OBS_TEAMMATES_ALIVE_FRAC] = 0.0f;  /* no teams, see above */
		obs[GB_OBS_ENEMIES_ALIVE_FRAC] =
			gb_clampf((float)enemies_alive / game.maxclients, 0, 1);
	}
	/* objective[] stays zero: vanilla baseq2 has no CTF/bomb mode.
	 * intent[] stays zero: the policy server injects the planner's vector. */

	gb_prev_health[clientNum] = ent->health;
	gb_prev_score[clientNum]  = client->resp.score;
	gb_prev_alive[clientNum]  = alive;
}

/* -------------------------------------------------------------- commands */

/* Quake II's usercmd_t angles are delta-encoded exactly like Quake III's:
 * the ENGINE (inside gi.Pmove) computes the final view angle as
 * SHORT2ANGLE(cmd.angles[i] + ps.pmove.delta_angles[i]), so we have to
 * subtract the current delta before sending an absolute target. */
static void GB_EncodeAngles(gclient_t *client, float pitch, float yaw, usercmd_t *ucmd)
{
	ucmd->angles[PITCH] = (short)(ANGLE2SHORT(pitch) - client->ps.pmove.delta_angles[PITCH]);
	ucmd->angles[YAW]   = (short)(ANGLE2SHORT(yaw)   - client->ps.pmove.delta_angles[YAW]);
	ucmd->angles[ROLL]  = 0;
}

/* Respawning is boilerplate, not something a policy should have to learn:
 * nothing else drives a fake client's input, so without this a dead bot
 * (fallback OR an unanswering policy) would sit at the death screen forever
 * instead of pressing attack to come back, the way a human eventually would. */
static qboolean GB_WantsRespawn(edict_t *ent)
{
	return ent->deadflag && level.time > ent->client->respawn_time;
}

/* No policy has ever answered for this bot yet (or the policy server is
 * unreachable): walk forward in a slow circle. This is not a real bot AI --
 * Quake II ships none -- it exists only so a disabled or unreachable policy
 * does not leave the bot standing dead still, and so the on/off control
 * experiment in the README has something to show for "off". */
#define GB_FALLBACK_TURN_DEG  3.0f

static void GB_FallbackCmd(edict_t *ent, usercmd_t *ucmd)
{
	gclient_t *client = ent->client;
	float yaw = client->ps.viewangles[YAW] + GB_FALLBACK_TURN_DEG;

	memset(ucmd, 0, sizeof(*ucmd));
	ucmd->msec = (byte)gb_msec;
	GB_EncodeAngles(client, client->ps.viewangles[PITCH], yaw, ucmd);
	ucmd->forwardmove = (short)GB_CMD_SCALE;

	if (GB_WantsRespawn(ent))
		ucmd->buttons |= BUTTON_ATTACK;
}

static void GB_CmdFromAction(edict_t *ent, const gb_action_t *a, usercmd_t *ucmd)
{
	gclient_t *client = ent->client;
	float pitch = gb_clampf(gb_norm180(client->ps.viewangles[PITCH]) + a->pitch_delta, -89.0f, 89.0f);
	float yaw   = client->ps.viewangles[YAW] + a->yaw_delta;

	memset(ucmd, 0, sizeof(*ucmd));
	ucmd->msec = (byte)gb_msec;
	GB_EncodeAngles(client, pitch, yaw, ucmd);

	ucmd->forwardmove = (short)gb_clampf(a->forward * GB_CMD_SCALE, -GB_CMD_SCALE, GB_CMD_SCALE);
	ucmd->sidemove    = (short)gb_clampf(a->side    * GB_CMD_SCALE, -GB_CMD_SCALE, GB_CMD_SCALE);
	if (a->buttons & GB_BTN_JUMP)   ucmd->upmove = GB_JUMP_UPMOVE;
	if (a->buttons & GB_BTN_CROUCH) ucmd->upmove = GB_CROUCH_UPMOVE;

	/* Only ATTACK, JUMP and CROUCH do anything in vanilla baseq2. ATTACK2,
	 * RELOAD, WALK and ZOOM have no usercmd_t representation at all in
	 * Quake II (no secondary fire, no reload, no walk-toggle bit, no zoom in
	 * the base weapon set) and BUTTON_USE, while it exists on the wire, is
	 * never read anywhere in the baseq2 source -- it is reserved for mods.
	 * We still forward it in case a future mod reads it; today it is a
	 * documented no-op, not a missing feature. */
	if (a->buttons & GB_BTN_ATTACK) ucmd->buttons |= BUTTON_ATTACK;
	if (a->buttons & GB_BTN_USE)    ucmd->buttons |= BUTTON_USE;

	if (a->weapon > 0) {
		if (!gb_weapons_ready)
			GB_ResolveWeapons();
		if (a->weapon <= GB_NUM_WEAPONS && gb_weapons[a->weapon])
			client->newweapon = gb_weapons[a->weapon];
	}

	if (GB_WantsRespawn(ent))
		ucmd->buttons |= BUTTON_ATTACK;
}

/* --------------------------------------------------------------- bots */

static edict_t *GB_FindFreeSlot(void)
{
	int i;

	for (i = 1; i <= game.maxclients; i++)
		if (!g_edicts[i].inuse)
			return &g_edicts[i];
	return NULL;
}

static qboolean GB_SpawnBot(void)
{
	edict_t *bot = GB_FindFreeSlot();
	char     userinfo[MAX_INFO_STRING];
	char     name[32];
	int      slot;

	if (!bot)
		return false;
	slot = (int)(bot - g_edicts);

	Com_sprintf(name, sizeof(name), "gb_bot_%d", ++gb_bot_serial);
	memset(userinfo, 0, sizeof(userinfo));
	Info_SetValueForKey(userinfo, "name", name);
	Info_SetValueForKey(userinfo, "skin", "male/grunt");
	Info_SetValueForKey(userinfo, "hand", "2");
	Info_SetValueForKey(userinfo, "fov", "90");

	/* Exactly the same two calls the engine makes for a real connecting
	 * client (SVC_DirectConnect -> ClientConnect, then the client's first
	 * "begin" -> ClientBegin). Nothing here is bot-specific engine API --
	 * there isn't one -- this IS the network path, just invoked directly. */
	if (!ClientConnect(bot, userinfo)) {
		gi.dprintf("gamebots: ClientConnect refused the bot in slot %d\n", slot);
		return false;
	}
	ClientBegin(bot);

	gb_is_bot[slot]      = true;
	gb_have_action[slot] = 0;
	gb_prev_health[slot] = bot->health;
	gb_prev_score[slot]  = bot->client->resp.score;
	gb_prev_alive[slot]  = (bot->health > 0);

	gi.bprintf(PRINT_HIGH, "gamebots: spawned bot %s in slot %d\n", name, slot);
	return true;
}

static void GB_DespawnOneBot(void)
{
	int i;

	for (i = game.maxclients; i >= 1; i--) {
		if (gb_is_bot[i] && g_edicts[i].inuse) {
			ClientDisconnect(&g_edicts[i]);
			gb_is_bot[i]      = false;
			gb_have_action[i] = 0;
			return;
		}
	}
}

static void GB_SyncBotCount(void)
{
	int want = (int)gb_bots->value;
	int have = 0, i;

	if (want < 0) want = 0;
	if (want > game.maxclients) want = game.maxclients;

	for (i = 1; i <= game.maxclients; i++) {
		if (gb_is_bot[i] && g_edicts[i].inuse)
			have++;
		else if (gb_is_bot[i])
			gb_is_bot[i] = false;  /* left some other way, e.g. kicked/banned */
	}

	while (have < want) {
		if (!GB_SpawnBot()) {
			if ((level.framenum % 100) == 0)
				gi.dprintf("gamebots: wanted %d bot(s), only fielded %d "
				           "(maxclients %d -- raise it for more bots)\n",
				           want, have, game.maxclients);
			break;
		}
		have++;
	}
	while (have > want) {
		GB_DespawnOneBot();
		have--;
	}
}

/* ---------------------------------------------------------------- frame */

void GB_RunFrame(void)
{
	int         i, n = 0;
	gb_result_t r = GB_FALLBACK;
	int         bot_list[MAX_CLIENTS];
	int         nbots = 0;

	if (!gb_ready)
		return;

	GB_SyncBotCount();

	for (i = 1; i <= game.maxclients; i++)
		if (gb_is_bot[i] && g_edicts[i].inuse)
			bot_list[nbots++] = i;

	if (nbots == 0)
		return;

	memset(gb_have_action, 0, sizeof(gb_have_action));

	if (gb_enable->value != 0) {
		gb_begin(&gb, (unsigned)level.framenum);
		for (i = 0; i < nbots; i++) {
			float ob[GB_OBS_DIM];
			GB_BuildObs(bot_list[i], ob);
			if (gb_add(&gb, (unsigned short)bot_list[i], ob) != 0)
				break;                  /* batch full */
			n++;
		}
		if (n > 0)
			r = gb_exchange(&gb);

		if (r != GB_OK) {
			if (gb_reported_state != 0) {
				gb_reported_state = 0;
				gi.dprintf("gamebots: policy server unavailable (%s) -- bots are "
				           "on the fallback wander AI\n", gb.last_error);
			}
		} else {
			if (gb_reported_state != 1) {
				gb_reported_state = 1;
				gi.dprintf("gamebots: policy server answering, driving %d bot(s)\n", n);
			}
			for (i = 0; i < n; i++) {
				const gb_action_t *a = gb_action(&gb, (unsigned short)i);
				if (!a || a->bot_id < 1 || a->bot_id > (unsigned)game.maxclients)
					continue;
				gb_action_of[a->bot_id] = *a;
				gb_clamp(&gb_action_of[a->bot_id]);
				gb_have_action[a->bot_id] = 1;
			}
		}
	} else {
		gb_reported_state = -1;  /* next enable should announce fresh state */
	}

	for (i = 0; i < nbots; i++) {
		edict_t  *ent = &g_edicts[bot_list[i]];
		usercmd_t ucmd;

		if (gb_have_action[bot_list[i]])
			GB_CmdFromAction(ent, &gb_action_of[bot_list[i]], &ucmd);
		else
			GB_FallbackCmd(ent, &ucmd);

		if (gb_debug->value != 0 && (level.framenum % 20) == 0)
			gi.dprintf("gamebots: bot %d fwd%+d side%+d up%+d btn0x%02x (%s)\n",
			           bot_list[i], ucmd.forwardmove, ucmd.sidemove, ucmd.upmove,
			           ucmd.buttons,
			           gb_have_action[bot_list[i]] ? "policy" : "fallback");

		/* This IS the hook: nothing else will ever call ClientThink for a
		 * fake client, so the "consume the usercmd" half of the contract
		 * happens right here, in the same call that "produced" it. */
		ClientThink(ent, &ucmd);
	}
}

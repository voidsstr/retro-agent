/* gb_adapter — the Quake III engine adapter.
 *
 * Compiled INTO the game module (qagame.so). Once per server frame it packs
 * every bot's observation into the shared schema, asks the policy server what
 * they should do, and rewrites their usercmds. If the policy server is not
 * there, is slow, or answers badly, it does nothing at all and the bots run
 * ioquake3's own botlib exactly as before.
 *
 * Two hooks in ai_main.c's per-frame loop, and nothing else:
 *
 *     GB_FrameBegin(time);                       // before the loop
 *     ...
 *         BotUpdateInput(botstates[i], ...);     // botlib fills lastucmd
 *         GB_ApplyAction(i, ..., &lastucmd);     // we overwrite it, or not
 *         trap_BotUserCommand(...);
 *
 * That ordering is deliberate: botlib still runs and still produces a complete
 * usercmd, so "our policy declined to answer" degrades to the stock bot rather
 * than to a bot standing still. The fallback is not a code path we maintain --
 * it is the code that was already there.
 *
 * OFF BY DEFAULT. `gb_enable 0` means installing this module changes nothing;
 * a server has to opt in. Dropping a new brain into a game people play on
 * should require someone to say so.
 */

#include "g_local.h"
#include "gb_client.h"

/* Q3 units. These are scale factors for normalisation, not physics: what
 * matters is that the policy sees the same scale on every map and engine, so a
 * value trained against Quake means something in GoldSrc. */
#define GB_FAR_PLANE     2000.0f   /* raycast length and distance normaliser */
#define GB_MAX_SPEED      320.0f   /* g_speed default */
#define GB_MAX_HEALTH     100.0f
#define GB_MAX_ARMOR      100.0f
#define GB_MAX_AMMO       100.0f
#define GB_EYE_HEIGHT      26.0f   /* DEFAULT_VIEWHEIGHT */

static gb_client_t gb;
static int         gb_ready;                    /* init done */
static vmCvar_t    gb_enable;
static vmCvar_t    gb_debug;

static gb_action_t gb_action_of[MAX_CLIENTS];
static int         gb_have_action[MAX_CLIENTS];

/* Per-client history, for the observation's "what just happened" group. The
 * engine does not hand us deltas, so we keep the previous frame ourselves. */
static int   gb_prev_health[MAX_CLIENTS];
static int   gb_prev_score[MAX_CLIENTS];
static int   gb_prev_alive[MAX_CLIENTS];

/* Reported once rather than every frame: a per-frame log line at sv_fps 20 is
 * 72,000 lines an hour, which buries the one that mattered. */
static int   gb_reported_state = -1;

static float gb_clampf(float v, float lo, float hi)
{
	if (v != v) return 0.0f;            /* NaN */
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

void GB_Init(void)
{
	gb_client_init(&gb, NULL);
	trap_Cvar_Register(&gb_enable, "gb_enable", "0", CVAR_ARCHIVE);
	trap_Cvar_Register(&gb_debug, "gb_debug", "0", CVAR_ARCHIVE);
	memset(gb_prev_health, 0, sizeof(gb_prev_health));
	memset(gb_prev_score, 0, sizeof(gb_prev_score));
	memset(gb_prev_alive, 0, sizeof(gb_prev_alive));
	gb_ready = 1;
	G_Printf("gamebots: adapter loaded (schema 0x%08x, obs %d floats), "
	         "socket %s — set gb_enable 1 to use it\n",
	         GB_SCHEMA_HASH, GB_OBS_DIM,
	         gb.socket_path[0] ? gb.socket_path : "<none>");
}

void GB_Shutdown(void)
{
	if (gb_ready)
		gb_client_close(&gb);
	gb_ready = 0;
}

/* ------------------------------------------------------------------ obs */

/* Everything the policy sees is ego-centric: positions relative to the bot and
 * expressed in ITS frame, distances normalised. A policy that only ever sees
 * "enemy 0.3 ahead and 0.1 right" transfers between maps and engines; one that
 * sees world coordinates memorises q3dm7. */
static void GB_ToLocal(const vec3_t fwd, const vec3_t right, const vec3_t up,
                       const vec3_t world, float *out)
{
	out[0] = DotProduct(world, fwd);
	out[1] = DotProduct(world, right);
	out[2] = DotProduct(world, up);
}

static int GB_Visible(int fromEnt, const vec3_t fromEye, const vec3_t toEye,
                      int toEnt)
{
	trace_t tr;
	trap_Trace(&tr, fromEye, NULL, NULL, toEye, fromEnt, MASK_SHOT);
	return (tr.fraction >= 1.0f || tr.entityNum == toEnt);
}

typedef struct {
	int   entnum;
	float dist;
	int   visible;
	int   teammate;
} gb_cand_t;

static int GB_CandCompare(const void *a, const void *b)
{
	const gb_cand_t *x = (const gb_cand_t *)a;
	const gb_cand_t *y = (const gb_cand_t *)b;
	/* Visible enemies first, then by distance. The slot order IS information:
	 * a stable, meaningful ordering lets a small net learn "slot 0 is the
	 * thing about to kill me" instead of spending capacity on permutation
	 * invariance it does not need. */
	if (x->visible != y->visible) return y->visible - x->visible;
	if (x->teammate != y->teammate) return x->teammate - y->teammate;
	if (x->dist < y->dist) return -1;
	if (x->dist > y->dist) return 1;
	return 0;
}

static void GB_BuildObs(int clientNum, float *obs)
{
	gentity_t   *ent = &g_entities[clientNum];
	playerState_t *ps = &ent->client->ps;
	vec3_t       fwd, right, up, eye, ang;
	gb_cand_t    cand[MAX_CLIENTS];
	int          ncand = 0;
	int          i, alive, teammates_alive = 0, enemies_alive = 0;
	int          bestOther = 0;
	float        rel[3];

	memset(obs, 0, sizeof(float) * GB_OBS_DIM);

	alive = (ps->pm_type != PM_DEAD && ent->health > 0);

	/* Yaw only for the body frame: pitch is reported separately, and folding
	 * it in here would make "an enemy above me" indistinguishable from "an
	 * enemy ahead while I happen to be looking up". */
	VectorSet(ang, 0, ps->viewangles[YAW], 0);
	AngleVectors(ang, fwd, right, up);
	VectorCopy(ps->origin, eye);
	eye[2] += GB_EYE_HEIGHT;

	/* --- self --- */
	obs[GB_OBS_HEALTH_FRAC]  = gb_clampf(ent->health / GB_MAX_HEALTH, 0, 1);
	obs[GB_OBS_ARMOR_FRAC]   = gb_clampf(ps->stats[STAT_ARMOR] / GB_MAX_ARMOR, 0, 1);
	if (ps->weapon > 0 && ps->weapon < MAX_WEAPONS)
		obs[GB_OBS_AMMO_FRAC] = gb_clampf(ps->ammo[ps->weapon] / GB_MAX_AMMO, 0, 1);
	/* Q3 has no reserve pool and no reload; those stay zero and the README
	 * says so rather than inventing a number the policy would learn from. */
	obs[GB_OBS_WEAPON_ID_NORM] = gb_clampf((float)ps->weapon / WP_NUM_WEAPONS, 0, 1);

	GB_ToLocal(fwd, right, up, ps->velocity, rel);
	obs[GB_OBS_VEL_LOCAL + 0] = gb_clampf(rel[0] / GB_MAX_SPEED, -2, 2);
	obs[GB_OBS_VEL_LOCAL + 1] = gb_clampf(rel[1] / GB_MAX_SPEED, -2, 2);
	obs[GB_OBS_VEL_LOCAL + 2] = gb_clampf(rel[2] / GB_MAX_SPEED, -2, 2);
	obs[GB_OBS_SPEED_FRAC] = gb_clampf(VectorLength(ps->velocity) / GB_MAX_SPEED, 0, 2);
	obs[GB_OBS_PITCH_NORM] = gb_clampf(AngleNormalize180(ps->viewangles[PITCH]) / 90.0f, -1, 1);
	obs[GB_OBS_ON_GROUND]  = (ps->groundEntityNum != ENTITYNUM_NONE) ? 1.0f : 0.0f;
	obs[GB_OBS_CROUCHING]  = (ps->pm_flags & PMF_DUCKED) ? 1.0f : 0.0f;
	obs[GB_OBS_IN_WATER]   = (ent->waterlevel > 1) ? 1.0f : 0.0f;
	obs[GB_OBS_ALIVE]      = alive ? 1.0f : 0.0f;

	/* --- local geometry: 16 rays around us, plus up and down --- */
	for (i = 0; i < GB_NUM_RAYS_H; i++) {
		vec3_t dir, end, a2;
		trace_t tr;
		VectorSet(a2, 0, ps->viewangles[YAW] + (360.0f * i) / GB_NUM_RAYS_H, 0);
		AngleVectors(a2, dir, NULL, NULL);
		VectorMA(eye, GB_FAR_PLANE, dir, end);
		trap_Trace(&tr, eye, NULL, NULL, end, clientNum, MASK_PLAYERSOLID);
		obs[GB_OBS_RAY_H + i] = gb_clampf(tr.fraction, 0, 1);
	}
	{
		vec3_t end;
		trace_t tr;
		VectorCopy(eye, end); end[2] += GB_FAR_PLANE;
		trap_Trace(&tr, eye, NULL, NULL, end, clientNum, MASK_PLAYERSOLID);
		obs[GB_OBS_RAY_UP] = gb_clampf(tr.fraction, 0, 1);
		VectorCopy(eye, end); end[2] -= GB_FAR_PLANE;
		trap_Trace(&tr, eye, NULL, NULL, end, clientNum, MASK_PLAYERSOLID);
		obs[GB_OBS_RAY_DOWN] = gb_clampf(tr.fraction, 0, 1);
	}

	/* --- other players --- */
	for (i = 0; i < level.maxclients; i++) {
		gentity_t *other = &g_entities[i];
		vec3_t     otherEye, delta;
		float      dist;

		if (i == clientNum || !other->inuse || !other->client)
			continue;
		if (other->client->pers.connected != CON_CONNECTED)
			continue;
		if (other->client->sess.sessionTeam == TEAM_SPECTATOR)
			continue;

		VectorCopy(other->client->ps.origin, otherEye);
		otherEye[2] += GB_EYE_HEIGHT;
		VectorSubtract(otherEye, eye, delta);
		dist = VectorLength(delta);

		cand[ncand].entnum   = i;
		cand[ncand].dist     = dist;
		cand[ncand].visible  = GB_Visible(clientNum, eye, otherEye, i);
		cand[ncand].teammate = (g_gametype.integer >= GT_TEAM &&
		                        other->client->sess.sessionTeam ==
		                        ent->client->sess.sessionTeam);
		ncand++;

		if (other->health > 0 && other->client->ps.pm_type != PM_DEAD) {
			if (cand[ncand - 1].teammate) teammates_alive++;
			else                          enemies_alive++;
		}
		if (other->client->ps.persistant[PERS_SCORE] > bestOther)
			bestOther = other->client->ps.persistant[PERS_SCORE];
	}

	qsort(cand, ncand, sizeof(cand[0]), GB_CandCompare);

	for (i = 0; i < GB_MAX_ENTITIES && i < ncand; i++) {
		gentity_t *other = &g_entities[cand[i].entnum];
		vec3_t     otherEye, delta, dir;
		int        base = GB_OBS_E0_PRESENT + i * GB_ENT_SLOT_STRIDE;

		VectorCopy(other->client->ps.origin, otherEye);
		otherEye[2] += GB_EYE_HEIGHT;
		VectorSubtract(otherEye, eye, delta);
		if (cand[i].dist > 0.001f)
			VectorScale(delta, 1.0f / cand[i].dist, dir);
		else
			VectorClear(dir);

		obs[base + GB_ENT_PRESENT]  = 1.0f;
		obs[base + GB_ENT_TEAMMATE] = cand[i].teammate ? 1.0f : 0.0f;
		GB_ToLocal(fwd, right, up, dir, rel);
		obs[base + GB_ENT_DIR + 0] = gb_clampf(rel[0], -1, 1);
		obs[base + GB_ENT_DIR + 1] = gb_clampf(rel[1], -1, 1);
		obs[base + GB_ENT_DIR + 2] = gb_clampf(rel[2], -1, 1);
		obs[base + GB_ENT_DIST]    = gb_clampf(cand[i].dist / GB_FAR_PLANE, 0, 1);
		GB_ToLocal(fwd, right, up, other->client->ps.velocity, rel);
		obs[base + GB_ENT_RELVEL + 0] = gb_clampf(rel[0] / GB_MAX_SPEED, -2, 2);
		obs[base + GB_ENT_RELVEL + 1] = gb_clampf(rel[1] / GB_MAX_SPEED, -2, 2);
		obs[base + GB_ENT_HEALTH]  = gb_clampf(other->health / GB_MAX_HEALTH, 0, 1);
		obs[base + GB_ENT_VISIBLE] = cand[i].visible ? 1.0f : 0.0f;
	}

	/* --- what just happened --- */
	{
		int dh = gb_prev_health[clientNum] - ent->health;
		if (dh > 0)
			obs[GB_OBS_TOOK_DAMAGE] = gb_clampf(dh / GB_MAX_HEALTH, 0, 1);
		if (dh > 0 && ent->client->lasthurt_client >= 0 &&
		    ent->client->lasthurt_client < level.maxclients &&
		    ent->client->lasthurt_client != clientNum) {
			gentity_t *src = &g_entities[ent->client->lasthurt_client];
			if (src->inuse && src->client) {
				vec3_t d;
				VectorSubtract(src->client->ps.origin, ps->origin, d);
				VectorNormalize(d);
				GB_ToLocal(fwd, right, up, d, rel);
				obs[GB_OBS_DAMAGE_DIR + 0] = gb_clampf(rel[0], -1, 1);
				obs[GB_OBS_DAMAGE_DIR + 1] = gb_clampf(rel[1], -1, 1);
			}
		}
		if (ps->persistant[PERS_SCORE] > gb_prev_score[clientNum])
			obs[GB_OBS_KILLED_SOMEONE] = 1.0f;
		if (gb_prev_alive[clientNum] && !alive)
			obs[GB_OBS_DIED] = 1.0f;
	}

	/* --- match context --- */
	if (g_timelimit.integer > 0) {
		obs[GB_OBS_ROUND_TIME_FRAC] =
			gb_clampf((float)(level.time - level.startTime) /
			          (g_timelimit.integer * 60000.0f), 0, 1);
	}
	if (g_fraglimit.integer > 0) {
		obs[GB_OBS_SCORE_DIFF_NORM] =
			gb_clampf((float)(ps->persistant[PERS_SCORE] - bestOther) /
			          g_fraglimit.integer, -1, 1);
	}
	if (level.maxclients > 1) {
		obs[GB_OBS_TEAMMATES_ALIVE_FRAC] =
			gb_clampf((float)teammates_alive / level.maxclients, 0, 1);
		obs[GB_OBS_ENEMIES_ALIVE_FRAC] =
			gb_clampf((float)enemies_alive / level.maxclients, 0, 1);
	}

	/* intent[] stays zero: the policy server injects the planner's vector. */

	gb_prev_health[clientNum] = ent->health;
	gb_prev_score[clientNum]  = ps->persistant[PERS_SCORE];
	gb_prev_alive[clientNum]  = alive;
}

/* ---------------------------------------------------------------- frame */

void GB_FrameBegin(int time)
{
	int i, n = 0;
	float obs[GB_OBS_DIM];
	gb_result_t r;

	if (!gb_ready)
		return;
	trap_Cvar_Update(&gb_enable);
	trap_Cvar_Update(&gb_debug);

	memset(gb_have_action, 0, sizeof(gb_have_action));
	if (!gb_enable.integer)
		return;

	gb_begin(&gb, (unsigned)time);
	for (i = 0; i < level.maxclients; i++) {
		gentity_t *ent = &g_entities[i];
		if (!ent->inuse || !ent->client)
			continue;
		if (!(ent->r.svFlags & SVF_BOT))
			continue;                       /* only bots; humans are humans */
		if (ent->client->pers.connected != CON_CONNECTED)
			continue;
		GB_BuildObs(i, obs);
		if (gb_add(&gb, (unsigned short)i, obs) != 0)
			break;                          /* batch full */
		n++;
	}
	if (n == 0)
		return;

	r = gb_exchange(&gb);
	if (r != GB_OK) {
		/* Not an error worth a line per frame — the policy server being down
		 * is a normal state, and botlib is already covering for it. Report
		 * only when the answer CHANGES. */
		if (gb_reported_state != 0) {
			gb_reported_state = 0;
			G_Printf("gamebots: policy server unavailable (%s) — bots are on "
			         "botlib\n", gb.last_error);
		}
		return;
	}
	if (gb_reported_state != 1) {
		gb_reported_state = 1;
		G_Printf("gamebots: policy server answering, driving %d bot(s)\n", n);
	}

	for (i = 0; i < n; i++) {
		const gb_action_t *a = gb_action(&gb, (unsigned short)i);
		if (!a || a->bot_id >= MAX_CLIENTS)
			continue;
		gb_action_of[a->bot_id] = *a;
		gb_clamp(&gb_action_of[a->bot_id]);
		gb_have_action[a->bot_id] = 1;
	}
}

void GB_ApplyAction(int clientNum, const vec3_t viewangles,
                    const int *deltaAngles, usercmd_t *ucmd)
{
	const gb_action_t *a;
	float pitch, yaw;

	if (!gb_ready || !gb_enable.integer)
		return;
	if (clientNum < 0 || clientNum >= MAX_CLIENTS || !gb_have_action[clientNum])
		return;                             /* botlib's usercmd stands */

	a = &gb_action_of[clientNum];

	/* View is a DELTA on the bot's current angles. Absolute aim from a policy
	 * would let it teleport its crosshair, which is both unfair and looks
	 * nothing like a player. */
	pitch = AngleNormalize180(viewangles[PITCH]) + a->pitch_delta;
	yaw   = viewangles[YAW] + a->yaw_delta;
	pitch = gb_clampf(pitch, -89.0f, 89.0f);

	ucmd->angles[PITCH] = ANGLE2SHORT(pitch) - deltaAngles[PITCH];
	ucmd->angles[YAW]   = ANGLE2SHORT(yaw)   - deltaAngles[YAW];
	ucmd->angles[ROLL]  = 0;

	ucmd->forwardmove = (signed char)gb_clampf(a->forward * 127.0f, -127, 127);
	ucmd->rightmove   = (signed char)gb_clampf(a->side * 127.0f, -127, 127);
	ucmd->upmove      = 0;
	if (a->buttons & GB_BTN_JUMP)   ucmd->upmove =  127;
	if (a->buttons & GB_BTN_CROUCH) ucmd->upmove = -127;

	ucmd->buttons = 0;
	if (a->buttons & GB_BTN_ATTACK) ucmd->buttons |= BUTTON_ATTACK;
	if (a->buttons & GB_BTN_USE)    ucmd->buttons |= BUTTON_USE_HOLDABLE;
	if (a->buttons & GB_BTN_WALK)   ucmd->buttons |= BUTTON_WALKING;

	/* Weapon 0 means "no change" in the schema, so an untrained policy that
	 * always emits 0 leaves the bot holding whatever botlib chose. */
	if (a->weapon > 0 && a->weapon < WP_NUM_WEAPONS)
		ucmd->weapon = a->weapon;

	if (gb_debug.integer && (level.time % 2000) < 50) {
		G_Printf("gamebots: bot %d pitch%+.1f yaw%+.1f fwd%+.2f side%+.2f "
		         "btn0x%02x\n", clientNum, a->pitch_delta, a->yaw_delta,
		         a->forward, a->side, a->buttons);
	}
}

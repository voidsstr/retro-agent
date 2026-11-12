/*
 * aigate.h - the decision half of the Fleet AI engine gate (agent 1.99.0),
 * Win32-free so tests/native/test_aigate.c compiles the exact code the agent
 * runs.
 *
 * WHY THIS EXISTS AT ALL. The AI engine (retro-infer.exe) has been opt-in
 * since 1.17.0: HKLM\Software\RetroAgent\AIEngine absent or 0 meant the
 * startup thread would not probe it and infer_ensure() would not spawn it.
 * That is a DEFAULT, and a default is not an off switch - anything that could
 * speak the protocol could issue AI_ENABLE, which set the flag, pulled
 * retro-infer.exe from the share and spawned it, permanently, across reboots.
 * On a single-core vintage box that engine steals cycles from games and skews
 * every benchmark, and nothing on the box said why.
 *
 * User directive 2026-11-08: the fleet AI is off, its files are removed, and
 * the agent must not turn it back on. So the gate is now two values, not one:
 *
 *   AIEngineAllowed  (DWORD)  may this box run the AI engine AT ALL?
 *                             ABSENT or 0 => NO. This is the shipped default,
 *                             so a freshly imaged or freshly updated box is
 *                             locked without anyone having to set anything.
 *   AIEngine         (DWORD)  the old opt-in flag: is it switched on now?
 *                             Only consulted when AIEngineAllowed permits.
 *
 * Three consequences, all deliberate:
 *   - AI_ENABLE REFUSES while the lock is in force. It does not set the flag
 *     and return OK, because a command that reports success and changes
 *     nothing is this project's signature failure shape.
 *   - The engine is never staged from the share and never spawned.
 *   - Every agent start PURGES: a stray AIEngine=1 left over from an earlier
 *     enable is cleared and a staged retro-infer.exe is deleted. "Off by
 *     default" that leaves yesterday's enable in place is not off, and the
 *     binary is what makes re-enabling instant.
 *
 * To deliberately allow AI on one box (the only route - there is no command
 * for it, on purpose):
 *     REGWRITE HKLM Software\RetroAgent AIEngineAllowed REG_DWORD 1
 *     AI_ENABLE
 * A persisted "allow" is exactly the automatic behaviour the directive
 * forbids, so no code path ever WRITES AIEngineAllowed - only reads it. The
 * same reasoning as drvsafe.h's deliberate absence of an ALLOW3DFX registry
 * switch.
 *
 * NOT guarded by hostpolicy, and that is the point: the purge must run on a
 * modern Windows box too. .139 (the Win10/11 copier host) and .145 were the
 * two machines actually carrying a staged retro-infer.exe on 2026-11-08, and
 * a hostpolicy guard would have skipped the one box with the most disk and
 * the fewest reasons to host an inference engine. Deleting a file the agent
 * itself staged into its OWN directory is agent self-maintenance, like
 * auto-update - it does not reconfigure the host as a fleet box.
 * tests/python/test_aigate_agent.py pins that choice in both directions.
 */
#ifndef RETRO_AIGATE_H
#define RETRO_AIGATE_H

/* A registry DWORD the agent may or may not have found. `present` is 0 when
 * the value is absent OR was not a REG_DWORD - a value of the wrong type is
 * not a permission. */
typedef struct {
    int   present;
    unsigned long value;
} ai_regflag_t;

/* Reasons, for the log line and the AI_ENABLE error text. Stable strings:
 * the tests match on them, and an operator greps for them. */
#define AI_WHY_LOCKED_ABSENT "AI is off on this box: AIEngineAllowed is not set"
#define AI_WHY_LOCKED_ZERO   "AI is off on this box: AIEngineAllowed is 0"
#define AI_WHY_NOT_ENABLED   "AI is allowed on this box but not enabled (AI_ENABLE)"
#define AI_WHY_RUNNABLE      "AI is allowed and enabled"

/* May this box run the engine at all? The lock. */
static int ai_gate_allowed(ai_regflag_t allowed)
{
    return allowed.present && allowed.value != 0;
}

/* May AI_ENABLE do anything? Identical to the lock - stated separately
 * because they are different questions and a later change could reasonably
 * move one without the other. */
static int ai_gate_may_enable(ai_regflag_t allowed)
{
    return ai_gate_allowed(allowed);
}

/* Should the engine be staged / probed / spawned right now? Requires BOTH:
 * the box is allowed, and the opt-in flag is on. */
static int ai_gate_may_run(ai_regflag_t allowed, ai_regflag_t enabled)
{
    if (!ai_gate_allowed(allowed))
        return 0;
    return enabled.present && enabled.value != 0;
}

/* Should this start clear a stray AIEngine flag and delete a staged engine
 * binary? Whenever the lock is in force. Note this is true even when
 * AIEngine is already absent - the BINARY still wants deleting, and asking
 * "is there anything to undo" is the caller's job (it reports what it
 * actually removed, not what it intended to). */
static int ai_gate_should_purge(ai_regflag_t allowed)
{
    return !ai_gate_allowed(allowed);
}

/* One-line explanation of the current state, for the log and the console. */
static const char *ai_gate_why(ai_regflag_t allowed, ai_regflag_t enabled)
{
    if (!allowed.present)
        return AI_WHY_LOCKED_ABSENT;
    if (allowed.value == 0)
        return AI_WHY_LOCKED_ZERO;
    if (!(enabled.present && enabled.value != 0))
        return AI_WHY_NOT_ENABLED;
    return AI_WHY_RUNNABLE;
}

#endif /* RETRO_AIGATE_H */

/* gb_adapter — hooks the Quake III game module calls. See gb_adapter.c.
 *
 * Three call sites in ai_main.c and nothing else; the patch that adds them is
 * ai_main.patch in this directory, applied by build.sh.
 */
#ifndef GB_ADAPTER_H
#define GB_ADAPTER_H

void GB_Init(void);
void GB_Shutdown(void);

/* Once per server frame, BEFORE the usercmd loop: packs every bot's
 * observation and exchanges with the policy server. */
void GB_FrameBegin(int time);

/* Per bot, AFTER BotUpdateInput has filled ucmd: overwrites it if the policy
 * answered for this bot, otherwise leaves botlib's command untouched. */
void GB_ApplyAction(int clientNum, const vec3_t viewangles,
                    const int *deltaAngles, usercmd_t *ucmd);

#endif /* GB_ADAPTER_H */

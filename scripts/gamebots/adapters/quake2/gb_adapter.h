/* gb_adapter — hooks the Yamagi Quake II game module calls. See gb_adapter.c.
 *
 * ONE call site in g_main.c (the patch is g_main.patch, applied by build.sh):
 * the first statement of G_RunFrame(). Everything else -- spawning the fake
 * clients, driving them, tearing them down -- happens inside GB_RunFrame()
 * and nothing upstream needs to know about it.
 *
 * Unlike the Quake III adapter, Quake II's baseq2 game module ships NO bot
 * AI at all (no botlib, no fake-client support, no SVF_BOT). GB_Init()/
 * GB_Shutdown() are called once each from GetGameAPI()/ShutdownGame(), also
 * in g_main.c.
 */
#ifndef GB_ADAPTER_H
#define GB_ADAPTER_H

void GB_Init(void);
void GB_Shutdown(void);

/* Once per server frame, as the FIRST statement of G_RunFrame() -- before
 * the entity loop, before AI_SetSightClient(), before anything that reads
 * this frame's positions. This mirrors where a real client's usercmd would
 * already have been consumed by SV_ClientThink() (network path, engine-
 * side) by the time G_RunFrame() starts; our fake clients have no network
 * path, so we produce and consume their usercmd_t here instead. */
void GB_RunFrame(void);

#endif /* GB_ADAPTER_H */

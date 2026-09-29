#ifndef RETRO_FXPANEL_SRC_H
#define RETRO_FXPANEL_SRC_H
/* Copy the 3dfx Control Panel (3dfxctl.exe + 3dfxlogo.ico) from the share to
 * C:\RETRO_AGENT when this box has a card it serves and its copy is missing or
 * stale. See src/fxpanel.c and agent/shared/fxpanel.h. */
void fxpanel_ensure(void);
#endif

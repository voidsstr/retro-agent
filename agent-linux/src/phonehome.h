#ifndef PHONEHOME_H
#define PHONEHOME_H

/*
 * Phone-home registration thread.
 * Periodically POSTs system info to the dashboard's /api/fleet/register
 * endpoint so WAN agents can be discovered without LAN broadcasts.
 */

/* Phone-home thread entry point (pass NULL as arg). */
void *phonehome_thread(void *param);

#endif /* PHONEHOME_H */

/* Tiny platform shim so the engine also builds natively on the Linux dev box
 * (`make host`) for fast parity iteration. Fleet builds are always MinGW. */
#ifndef RETRO_INFER_PORT_H
#define RETRO_INFER_PORT_H

double ri_now(void);                 /* monotonic seconds */
unsigned long ri_ram_mb(int avail);  /* avail!=0: available, else total */

#endif

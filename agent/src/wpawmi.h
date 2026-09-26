/*
 * wpawmi.h - read Windows product activation state through WMI (read-only).
 *
 * Win32_WindowsProductActivation's ActivationRequired and RemainingGracePeriod
 * are what Windows itself decides logon with. The registry values LICSTATUS
 * reads cannot answer "how many days are left", and on the Dell Dimension 4600
 * (2026-09-26) the Winlogon flag read "not present" while WMI said grace 0.
 *
 * ole32/oleaut32 are LoadLibrary'd: nothing here may become a static import
 * (see ntdyn.h), and on Win9x there is no WMI and no activation to read.
 */
#ifndef RETRO_WPAWMI_H
#define RETRO_WPAWMI_H

#include <windows.h>

/* 1 = WMI answered and *required / *grace_days are set (grace_days is
 * 2147483647 once activated). 0 = could not ask; why says what failed.
 * Initialises COM on the calling thread and uninitialises it again. */
int wpa_query(DWORD *required, DWORD *grace_days, char *why, size_t why_cch);

/* NT 5.1 (XP) or 5.2 (Server 2003 / XP x64): the versions whose activation
 * blocks LOGON when it lapses. */
int wpa_logon_os(void);

#endif

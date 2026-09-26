/*
 * clockfix.c - a box whose clock is YEARS wrong sets it from the NAS
 *
 * Found on .243 (Win98 SE, 2026-09-24): a dead CMOS battery, so the box came
 * up in January 1980 at every power-on - and the fleet is powered on demand,
 * so that is every session. Everything it wrote carried 1980: agent.log, the
 * hardware record it publishes (so docs/fleet-inventory.md reported the box
 * `stale` forever), gamesync.done, the files on the share.
 *
 * The NAS every box already depends on (\\192.168.1.122\files) answers HTTP on
 * port 80 with a Date: header, so at startup, and only when the clock reads a
 * year before CLOCKFIX_MIN_YEAR, this asks it and sets the system clock (UTC,
 * so a box's time-zone setting is irrelevant). A clock that is merely minutes
 * or hours off is LEFT ALONE: that is Windows' own business (w32time on XP),
 * and hours off can be a time-zone configuration this has no way to judge.
 *
 * The host is taken from the UpdatePath registry value's UNC server when set,
 * so a relocated share moves this with it. Retries for ~2 minutes because the
 * thread starts at logon, possibly before the network is up.
 *
 * Registry (HKLM\Software\RetroAgent):
 *   ClockFix    REG_DWORD  0 = never touch the clock
 *   ClockFixed  REG_SZ     written by the agent: what it changed, from what,
 *                          to what, from where - the post-condition, not "OK"
 *
 * Never on a modern Windows host (host policy): that clock is its owner's.
 *
 * NEVER past an unactivated XP box's activation grace (2026-09-26). A box
 * whose CMOS clock is wrong DURING SETUP records its install - and starts its
 * 30-day grace - in that wrong year. Correcting the clock afterwards spends
 * the whole grace at once: a freshly imaged Dell went from "installed 2004" to
 * "now 2026", WMI read RemainingGracePeriod=0, and its next reboot would have
 * stopped at the logon-blocking activation screen. clk_activation_allows()
 * asks Windows (shared/clockguard.h has the rule) and leaves the clock wrong,
 * loudly, rather than lock the box.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "hostpolicy.h"
#include "wpawmi.h"
#include "../shared/httpdate.h"
#include "../shared/clockguard.h"
#include <string.h>
#include <stdio.h>

#define LOG_CLK            "CLOCKFIX"
#define CLOCKFIX_MIN_YEAR  2024
#define CLOCKFIX_TRIES     12
#define CLOCKFIX_GAP_MS    10000
#define CLOCKFIX_DEFAULT   "192.168.1.122"

/* \\server\share\... from UpdatePath -> "server"; else the default NAS. */
static void clk_host(char *out, size_t cch)
{
    char up[512];
    DWORD type = 0, sz = sizeof(up);
    HKEY h;
    safe_strncpy(out, CLOCKFIX_DEFAULT, cch);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(h, "UpdatePath", NULL, &type, (LPBYTE)up, &sz) == ERROR_SUCCESS
            && type == REG_SZ && up[0] == '\\' && up[1] == '\\') {
        char *end;
        up[sizeof(up) - 1] = 0;
        end = strchr(up + 2, '\\');
        if (end) *end = 0;
        if (up[2]) safe_strncpy(out, up + 2, cch);
    }
    RegCloseKey(h);
}

static DWORD clk_enabled(void)
{
    DWORD v = 1, sz = sizeof(v), type = 0;
    HKEY h;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, "ClockFix", NULL, &type, (LPBYTE)&v, &sz) != ERROR_SUCCESS
                || type != REG_DWORD)
            v = 1;
        RegCloseKey(h);
    }
    return v;
}

/* HEAD / and read the response headers. 1 = got a parseable Date. */
static int clk_fetch(const char *host, hd_time_t *t)
{
    char buf[2048];
    int got = 0, ok = 0;
    struct sockaddr_in sa;
    unsigned long ip = inet_addr(host);
    SOCKET s;
    static const char req[] = "HEAD / HTTP/1.0\r\nUser-Agent: retro-agent-clockfix\r\n\r\n";

    if (ip == INADDR_NONE) {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) return 0;
        memcpy(&ip, he->h_addr_list[0], 4);
    }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    sa.sin_addr.s_addr = ip;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0
            && send(s, req, (int)sizeof(req) - 1, 0) == (int)sizeof(req) - 1) {
        for (;;) {
            fd_set rf;
            struct timeval tv;
            int n;
            FD_ZERO(&rf);
            FD_SET(s, &rf);
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            if (select(0, &rf, NULL, NULL, &tv) <= 0) break;
            n = recv(s, buf + got, (int)sizeof(buf) - 1 - got, 0);
            if (n <= 0) break;
            got += n;
            buf[got] = 0;
            if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n") || got >= (int)sizeof(buf) - 1)
                break;
        }
        buf[got] = 0;
        ok = got > 0 && hd_find_date(buf, t);
    }
    /* graceful: the NAS is not a Win98 box, but there is no reason to RST it */
    shutdown(s, 1);
    closesocket(s);
    return ok;
}

static void clk_note(const char *note)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0,
                        KEY_WRITE, NULL, &h, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(h, "ClockFixed", 0, REG_SZ, (const BYTE *)note, (DWORD)strlen(note) + 1);
        RegCloseKey(h);
    }
}

/* Whole days from *from to *to (negative = backwards). */
static long long clk_days_between(const SYSTEMTIME *from, const SYSTEMTIME *to)
{
    FILETIME a, b;
    ULARGE_INTEGER ua, ub;
    if (!SystemTimeToFileTime(from, &a) || !SystemTimeToFileTime(to, &b))
        return 0;
    ua.LowPart = a.dwLowDateTime; ua.HighPart = a.dwHighDateTime;
    ub.LowPart = b.dwLowDateTime; ub.HighPart = b.dwHighDateTime;
    return ((long long)ub.QuadPart - (long long)ua.QuadPart) / 864000000000LL;
}

/* Would moving the clock cost this box its logon? See shared/clockguard.h for
 * the 2026-09-26 Dell that this exists for. 1 = go ahead. A refusal is logged
 * loudly and written to ClockFixed, because a clock left in 2004 on purpose
 * must never look like a clock nobody tried to fix. Once the box is activated
 * the next agent start corrects it. */
static int clk_activation_allows(const SYSTEMTIME *now, const SYSTEMTIME *target,
                                 const char *host)
{
    DWORD required = 0, grace = 0;
    char why[160], note[256];
    int known, verdict;
    long long jump = clk_days_between(now, target);

    if (!wpa_logon_os())
        return 1;
    known = wpa_query(&required, &grace, why, sizeof(why));
    verdict = clockguard_decide(1, known, required, grace, jump);
    if (verdict == CLOCKGUARD_ALLOW) {
        log_msg(LOG_CLK, "activation: %s (grace %lu day(s)) - moving the clock %lld day(s) is safe",
                required ? "NOT activated" : "activated", (unsigned long)grace, jump);
        return 1;
    }
    if (known)
        _snprintf(note, sizeof(note) - 1,
                  "REFUSED %04u-%02u-%02u -> %04u-%02u-%02u via http://%s/: Windows is not "
                  "activated (grace %lu day(s)) and the move is %lld day(s) - activate, then "
                  "restart the agent",
                  now->wYear, now->wMonth, now->wDay, target->wYear, target->wMonth, target->wDay,
                  host, (unsigned long)grace, jump);
    else
        _snprintf(note, sizeof(note) - 1,
                  "REFUSED %04u-%02u-%02u -> %04u-%02u-%02u via http://%s/: %s: %s",
                  now->wYear, now->wMonth, now->wDay, target->wYear, target->wMonth, target->wDay,
                  host, clockguard_reason(verdict), why);
    note[sizeof(note) - 1] = 0;
    log_msg(LOG_CLK, "==================================================");
    log_msg(LOG_CLK, "clock NOT set - %s", clockguard_reason(verdict));
    log_msg(LOG_CLK, "%s", note);
    log_msg(LOG_CLK, "moving an unactivated XP clock past its grace makes the NEXT reboot stop");
    log_msg(LOG_CLK, "at the activation lockout, where the agent never starts");
    log_msg(LOG_CLK, "==================================================");
    clk_note(note);
    return 0;
}

/* NT needs SE_SYSTEMTIME_NAME enabled in the token; Win9x has no tokens. */
static void clk_enable_privilege(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;
    if (GetVersion() & 0x80000000) return;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueA(NULL, "SeSystemtimePrivilege", &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(tok);
}

DWORD WINAPI clockfix_thread(LPVOID param)
{
    SYSTEMTIME now, set;
    char host[128], note[256];
    hd_time_t t;
    int i;
    (void)param;

    GetSystemTime(&now);
    if (now.wYear >= CLOCKFIX_MIN_YEAR) return 0;          /* the normal case */
    if (host_policy_skip("clockfix")) return 0;
    if (!clk_enabled()) { log_msg(LOG_CLK, "clock reads %u - ClockFix=0, left alone", now.wYear); return 0; }

    clk_host(host, sizeof(host));
    log_msg(LOG_CLK, "clock reads %04u-%02u-%02u (dead CMOS battery?) - asking http://%s/",
            now.wYear, now.wMonth, now.wDay, host);
    for (i = 0; i < CLOCKFIX_TRIES; i++) {
        if (clk_fetch(host, &t)) break;
        Sleep(CLOCKFIX_GAP_MS);
    }
    if (i == CLOCKFIX_TRIES) {
        log_msg(LOG_CLK, "no Date from http://%s/ after %d tries - clock NOT set", host, CLOCKFIX_TRIES);
        return 0;
    }
    if (t.year < CLOCKFIX_MIN_YEAR) {
        log_msg(LOG_CLK, "server says %04d - not plausible either, clock NOT set", t.year);
        return 0;
    }
    memset(&set, 0, sizeof(set));
    set.wYear = (WORD)t.year;   set.wMonth = (WORD)t.month;   set.wDay = (WORD)t.day;
    set.wHour = (WORD)t.hour;   set.wMinute = (WORD)t.minute; set.wSecond = (WORD)t.second;
    GetSystemTime(&now);
    if (!clk_activation_allows(&now, &set, host))
        return 0;
    clk_enable_privilege();
    if (!SetSystemTime(&set)) {
        log_msg(LOG_CLK, "SetSystemTime failed: %lu - clock NOT set", GetLastError());
        return 0;
    }
    _snprintf(note, sizeof(note) - 1,
              "from %04u-%02u-%02u %02u:%02u:%02u to %04d-%02d-%02d %02d:%02d:%02d UTC via http://%s/",
              now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
              t.year, t.month, t.day, t.hour, t.minute, t.second, host);
    note[sizeof(note) - 1] = 0;
    log_msg(LOG_CLK, "clock set %s", note);
    clk_note(note);
    return 0;
}

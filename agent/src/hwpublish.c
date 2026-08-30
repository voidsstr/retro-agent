/*
 * hwpublish.c - every box publishes its own hardware record, on every startup.
 *
 * THE PROBLEM THIS EXISTS TO KILL. The fleet's machine documentation was
 * hand-maintained, and it was wrong about most of the fleet.  Twice a box's
 * graphics card was swapped without the docs noticing: .124's Voodoo 3 came
 * out in August and the stale claim survived for weeks, and .133's Voodoo5
 * 6000 is physically gone while three separate documents still named the box
 * by it.  Three machines carried a DX9-or-better GPU that nothing mentioned,
 * which would have wrongly refused 2004-era titles on five of eight boxes.
 *
 * A document that has to be updated by hand after a screwdriver goes into a
 * case will be wrong, and nothing about writing a better document changes
 * that.  So the machine reports itself.  On every startup the agent writes its
 * own HWPROFILE JSON to
 *
 *   \\192.168.1.122\files\Utility\Retro Automation\fleet-inventory\<host>.json
 *
 * and scripts/fleet/inventory.py renders those into docs/fleet-inventory.md.
 * Pull a card out and the next boot says so.
 *
 * Modelled on retrowall.c, which re-applies the fleet look on EVERY startup
 * rather than once at onboarding, precisely so a box keeps it across reboots.
 * Same reasoning here: once-at-onboarding is how documentation goes stale.
 *
 * A COSMETIC FEATURE MUST NEVER COST A BOX ITS AGENT.  This project has
 * already killed an agent this way: dosstage copying an 11 MB tile payload at
 * startup took the 31 MB Pentium-1 Deskpro off the network ~45s after every
 * boot, and it looked like a startup crash for hours.  So, exactly as
 * dosstage.c now does:
 *
 *   - background thread, below-normal priority, started after a delay
 *   - the work is HARD CAPPED: one profile build and one file write of at most
 *     HWPUB_MAX_BYTES, never a directory walk or a bulk copy
 *   - a free-RAM floor below which we do not run at all, and say so
 *   - a BOUNDED retry schedule (hwpub_retry_delay_sec) - the share is often
 *     unreachable at startup, and an unbounded retry against an absent server
 *     is the shape of thing that eats a single-core box forever
 *   - an unreachable share is a clean no-op, logged once, not an error
 *
 * CONCURRENCY. Eight boxes publish at once and SMB gives us no lock worth
 * trusting, so each host writes its OWN file and nothing else - never a shared
 * file two agents append to.  The name comes from hwpub_safe_name(), because a
 * NetBIOS name pasted into a path is not a filename (see agent/shared/hwpub.h).
 *
 * WRITTEN DIRECTLY TO THE SHARE, AND THAT IS DELIBERATE.  The obvious
 * implementation - write a local temp file, CopyFileA it across - was tried
 * first and is WRONG, for a reason that took a measurement to see:
 * **CopyFile propagates the SOURCE file's timestamp**, so the record landed on
 * the share stamped with the retro box's own clock.  Measured on .124, whose
 * clock is two hours fast: an `echo >` straight to the share produced mtime
 * 12:55:58 (the file server's clock, correct) while a `copy` of an identical
 * local file produced 14:56:31 (the box's).
 *
 * That matters because the host-side renderer judges a record's age by its
 * mtime PRECISELY so that a retro machine's wrong clock cannot make a fresh
 * record look ancient - and CopyFile was quietly handing it the very clock it
 * was trying not to trust.  A single CreateFile + WriteFile of ~2 KB lets the
 * server stamp the time, which is the whole point.
 *
 * It is written straight to its final name rather than to a temp name renamed
 * into place.  A reader can therefore catch a partial file - but that renders
 * as `unreadable`, which is honest and self-heals on the next publish, whereas
 * a delete-then-rename would briefly show NO file at all and render as
 * `never seen`: a box that has never reported. The misleading failure is the
 * one worth designing out.
 *
 * Registry (HKLM\Software\RetroAgent):
 *   HwPublish     REG_DWORD  0 = disabled; absent/1 = enabled (default)
 *   HwPublishDir  REG_SZ     override the destination directory
 *   HwPublished   REG_SZ     timestamp of the last successful publish
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "../shared/hwpub.h"
#include <string.h>
#include <stdio.h>

#define LOG_HWPUB "HWPUBLISH"

#define HWPUB_KEY      "Software\\RetroAgent"
#define HWPUB_ENABLE   "HwPublish"
#define HWPUB_DIRVAL   "HwPublishDir"
#define HWPUB_MARKER   "HwPublished"

/* Re-publish this often while the agent runs.  A box left on for days would
 * otherwise carry a startup timestamp forever, and the renderer would have to
 * call a perfectly healthy machine stale.  One small file copy every six hours
 * is free even on the Pentium 1. */
#define HWPUB_REFRESH_SEC (6 * 60 * 60)

/* Below this much free RAM we do not publish at all.  The record itself is
 * tiny, but building it touches the registry and the display API, and the
 * Deskpro lesson is that on a box this tight the honest thing is to skip the
 * cosmetic work and say why. */
#define HWPUB_MIN_FREE_MB 4

static int g_hwpub_running = 0;

/* ------------------------------------------------------------------ */

static int hwpub_enabled(void)
{
    HKEY  h;
    DWORD ty = REG_DWORD, val = 1, n = sizeof(val);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, HWPUB_KEY, 0, KEY_QUERY_VALUE, &h)
            != ERROR_SUCCESS)
        return 1;                                  /* default on */
    if (RegQueryValueExA(h, HWPUB_ENABLE, NULL, &ty, (BYTE *)&val, &n)
            != ERROR_SUCCESS)
        val = 1;
    RegCloseKey(h);
    return val != 0;
}

static void hwpub_config_dir(char *out, int outsz)
{
    HKEY  h;
    safe_strncpy(out, HWPUB_DIR_DEFAULT, outsz);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, HWPUB_KEY, 0, KEY_QUERY_VALUE, &h)
            == ERROR_SUCCESS) {
        DWORD ty = REG_SZ, n = (DWORD)outsz;
        char  buf[512];
        n = sizeof(buf);
        if (RegQueryValueExA(h, HWPUB_DIRVAL, NULL, &ty, (BYTE *)buf, &n)
                == ERROR_SUCCESS && buf[0]) {
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            buf[n] = 0;
            safe_strncpy(out, buf, outsz);
        }
        RegCloseKey(h);
    }
}

static void hwpub_stamp_marker(const char *what)
{
    HKEY  h;
    DWORD disp;
    SYSTEMTIME st;
    char  stamp[64];

    GetLocalTime(&st);
    _snprintf(stamp, sizeof(stamp) - 1, "%04d-%02d-%02d %02d:%02d:%02d %s",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              what);
    stamp[sizeof(stamp) - 1] = 0;

    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, HWPUB_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &h, &disp) == ERROR_SUCCESS) {
        RegSetValueExA(h, HWPUB_MARKER, 0, REG_SZ, (const BYTE *)stamp,
                       (DWORD)strlen(stamp) + 1);
        RegCloseKey(h);
    }
}

static int hwpub_free_mb(void)
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    return (int)(ms.dwAvailPhys / (1024 * 1024));
}

/*
 * Find a directory we can actually write the record into.
 *
 * On Windows 9x a bare UNC path is NOT usable without an authenticated
 * session, so CreateFile against \\server\share\... fails while the very same
 * share is sitting on a mapped drive letter (hardware-confirmed on the
 * Deskpro, which staged 0 files while reporting success).  dosstage.c hit
 * exactly this; the fleet always maps the share, so fall back to the letter.
 *
 * Returns 1 with `out` set to a usable directory, 0 when the share is not
 * reachable at all - which is a clean no-op, not an error.
 */
static int hwpub_resolve_dir(const char *configured, char *out, int outsz)
{
    char parent[512], probe[512];
    const char *sub;
    char letter;
    int  i;

    /* (1) exactly as configured, creating the leaf if the parent is there. */
    safe_strncpy(out, configured, outsz);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
        return 1;
    safe_strncpy(parent, configured, sizeof(parent));
    for (i = (int)strlen(parent) - 1; i > 0; i--) {
        if (parent[i] == '\\') { parent[i] = 0; break; }
    }
    if (GetFileAttributesA(parent) != INVALID_FILE_ATTRIBUTES) {
        if (CreateDirectoryA(out, NULL) ||
            GetLastError() == ERROR_ALREADY_EXISTS)
            return 1;
    }

    /* Not a UNC path? Then there is no drive-letter fallback to try. */
    if (!(configured[0] == '\\' && configured[1] == '\\'))
        return 0;

    /* Skip past \\server\share to the subpath. */
    sub = configured + 2;
    while (*sub && *sub != '\\') sub++;           /* end of server */
    if (*sub) sub++;
    while (*sub && *sub != '\\') sub++;           /* end of share name */
    if (*sub) sub++;
    if (!*sub) return 0;

    for (letter = 'Z'; letter >= 'D'; letter--) {
        char root[8];
        _snprintf(root, sizeof(root), "%c:\\", letter);
        if (GetDriveTypeA(root) != DRIVE_REMOTE)
            continue;
        _snprintf(probe, sizeof(probe) - 1, "%c:\\%s", letter, sub);
        probe[sizeof(probe) - 1] = 0;
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
            safe_strncpy(out, probe, outsz);
            return 1;
        }
        /* Leaf missing but parent present: make it. */
        safe_strncpy(parent, probe, sizeof(parent));
        for (i = (int)strlen(parent) - 1; i > 0; i--) {
            if (parent[i] == '\\') { parent[i] = 0; break; }
        }
        if (GetFileAttributesA(parent) != INVALID_FILE_ATTRIBUTES) {
            if (CreateDirectoryA(probe, NULL) ||
                GetLastError() == ERROR_ALREADY_EXISTS) {
                safe_strncpy(out, probe, outsz);
                return 1;
            }
        }
    }
    return 0;
}

/*
 * Build the record and put it on the share.
 *
 * `dest` receives the path actually written (for the log and for HWPUBLISH's
 * reply), `err` a short reason on failure.  Returns 1 on success.
 *
 * VERIFY THE POST-CONDITION, NOT THE RETURN VALUE.  Every serious defect in
 * this project reported success: GAMESYNC said state=done while skipping every
 * same-size file, rd /s /q returned cleanly after hitting access-denied.  So
 * this re-reads the destination's size after the copy and only calls it a
 * success when the bytes are actually there.
 */
static int hwpub_publish_once(char *dest, int destsz, char *err, int errsz)
{
    char  dir[512], resolved[512], name[160], host[128];
    DWORD hlen = sizeof(host), written = 0;
    char *json = NULL;
    DWORD len;
    HANDLE h;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    int   ok = 0;

    dest[0] = 0;
    err[0]  = 0;

    if (!GetComputerNameA(host, &hlen))
        safe_strncpy(host, "unknown", sizeof(host));
    if (!hwpub_safe_name(host, name, sizeof(name)))
        log_msg(LOG_HWPUB, "computer name \"%s\" is not a filename; "
                           "filing under \"%s\"", host, name);

    hwpub_config_dir(dir, sizeof(dir));
    if (!hwpub_resolve_dir(dir, resolved, sizeof(resolved))) {
        safe_strncpy(err, "share not reachable", errsz);
        return 0;
    }
    _snprintf(dest, destsz - 1, "%s\\%s.json", resolved, name);
    dest[destsz - 1] = 0;

    json = hwprofile_json();
    if (!json) {
        safe_strncpy(err, "profile build failed", errsz);
        return 0;
    }
    len = (DWORD)strlen(json);
    if (len == 0 || len > HWPUB_MAX_BYTES) {
        _snprintf(err, errsz - 1, "record is %lu bytes (cap %d)",
                  (unsigned long)len, HWPUB_MAX_BYTES);
        err[errsz - 1] = 0;
        HeapFree(GetProcessHeap(), 0, json);
        return 0;
    }

    /* Straight to the share, in ONE write, so the FILE SERVER stamps the
     * timestamp - see the header. A local temp plus CopyFileA would carry this
     * box's clock across and defeat the renderer's staleness test. */
    h = CreateFileA(dest, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        _snprintf(err, errsz - 1, "cannot write %s (%lu)", dest,
                  (unsigned long)GetLastError());
        err[errsz - 1] = 0;
        HeapFree(GetProcessHeap(), 0, json);
        return 0;
    }
    if (!WriteFile(h, json, len, &written, NULL))
        written = 0;
    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, json);

    if (written != len) {
        _snprintf(err, errsz - 1, "short write %lu/%lu to %s",
                  (unsigned long)written, (unsigned long)len, dest);
        err[errsz - 1] = 0;
        return 0;
    }

    /* The post-condition, not the return value. WriteFile can report every
     * byte written and still leave a short file on a share that ran out of
     * room or dropped the session, so ask the share what is actually there. */
    if (GetFileAttributesExA(dest, GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow == len && fad.nFileSizeHigh == 0) {
        ok = 1;
    } else {
        _snprintf(err, errsz - 1,
                  "write reported success but %s is not %lu bytes", dest,
                  (unsigned long)len);
        err[errsz - 1] = 0;
    }

    return ok;
}

/* ------------------------------------------------------------------ */

DWORD WINAPI hwpublish_thread(LPVOID param)
{
    char dest[600], err[256];
    int  attempt, freemb;

    (void)param;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    if (!hwpub_enabled()) {
        log_msg(LOG_HWPUB, "disabled by HKLM\\%s\\%s", HWPUB_KEY, HWPUB_ENABLE);
        return 0;
    }

    for (attempt = 0; attempt < HWPUB_MAX_ATTEMPTS && g_running; attempt++) {
        int wait = hwpub_retry_delay_sec(attempt), i;
        for (i = 0; i < wait && g_running; i++)
            Sleep(1000);
        if (!g_running)
            return 0;

        freemb = hwpub_free_mb();
        if (freemb < HWPUB_MIN_FREE_MB) {
            /* Say why. A feature that silently does nothing on the one box
             * where it matters is how the Deskpro cost a day. */
            log_msg(LOG_HWPUB, "skipped: %d MB free, floor is %d MB",
                    freemb, HWPUB_MIN_FREE_MB);
            return 0;
        }

        if (hwpub_publish_once(dest, sizeof(dest), err, sizeof(err))) {
            log_msg(LOG_HWPUB, "published to %s", dest);
            hwpub_stamp_marker("ok");
            break;
        }
        log_msg(LOG_HWPUB, "attempt %d/%d: %s", attempt + 1,
                HWPUB_MAX_ATTEMPTS, err);
    }

    if (attempt >= HWPUB_MAX_ATTEMPTS)
        log_msg(LOG_HWPUB, "giving up for now; will retry on the %d-hour "
                           "refresh", HWPUB_REFRESH_SEC / 3600);

    /* Slow refresh, so a box that has been up for days does not carry a
     * startup timestamp forever and get called stale for being healthy. */
    while (g_running) {
        int i;
        for (i = 0; i < HWPUB_REFRESH_SEC && g_running; i++)
            Sleep(1000);
        if (!g_running)
            break;
        if (hwpub_free_mb() < HWPUB_MIN_FREE_MB)
            continue;
        if (hwpub_publish_once(dest, sizeof(dest), err, sizeof(err)))
            hwpub_stamp_marker("ok");
    }
    return 0;
}

/* HWPUBLISH - publish now, synchronously, and say exactly what happened.
 *
 * Exists so a publish can be VERIFIED rather than inferred from a log line:
 * the reply carries the path written and the byte count, or the reason it
 * did not happen. */
void handle_hwpublish(SOCKET sock, const char *args)
{
    char dest[600], err[256], reply[900];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    int force = (args && (str_starts_with(args, "force") ||
                          str_starts_with(args, "FORCE")));

    if (!force && !hwpub_enabled()) {
        send_error_response(sock,
            "hardware publish disabled (HKLM\\Software\\RetroAgent\\HwPublish"
            "=0); use HWPUBLISH force to override");
        return;
    }
    if (g_hwpub_running) {
        send_error_response(sock, "a publish is already in progress");
        return;
    }
    g_hwpub_running = 1;

    if (hwpub_publish_once(dest, sizeof(dest), err, sizeof(err))) {
        DWORD sz = 0;
        if (GetFileAttributesExA(dest, GetFileExInfoStandard, &fad))
            sz = fad.nFileSizeLow;
        hwpub_stamp_marker("ok");
        _snprintf(reply, sizeof(reply) - 1,
                  "{\"published\":true,\"path\":\"%s\",\"bytes\":%lu}",
                  dest, (unsigned long)sz);
        reply[sizeof(reply) - 1] = 0;
        log_msg(LOG_HWPUB, "HWPUBLISH -> %s (%lu bytes)", dest,
                (unsigned long)sz);
        g_hwpub_running = 0;
        send_text_response(sock, reply);
        return;
    }

    g_hwpub_running = 0;
    log_msg(LOG_HWPUB, "HWPUBLISH failed: %s", err);
    send_error_response(sock, err);
}

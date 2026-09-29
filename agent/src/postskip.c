/*
 * postskip.c - keep a Compaq Deskpro 2000's POST from waiting for F1 (1.86.0;
 * 1.86.1 also clears a stale 0Eh bit 2, POST 163 - see agent/shared/postskip.h)
 *
 * .243 (Compaq Deskpro 2000, 586C BIOS 04/25/97, Win98 SE) reports
 * "301-Keyboard Error" at every POST and, unless CMOS 2Dh bit 3 is set, waits
 * for F1 - so no reboot comes back unattended. Its CMOS battery is soldered
 * and dead, so a power loss can take the setting away again. At every agent
 * start this re-asserts it; while the power then stays on, every reboot is
 * unattended. Decision logic and the reasoning: agent/shared/postskip.h.
 *
 * Guards, in order - nothing touches a port until all of them pass:
 *   - Win9x only (ring-3 port I/O faults on NT);
 *   - the F000 ROM is readable and is exactly the Compaq Deskpro 2000
 *     04/25/97 ROM ("COMPAQ" at FFEA, the date at FFF5, the model string);
 *   - the bank looks populated (640 KB base memory at 15h/16h) and the
 *     standard CMOS checksum (sum 10h..2Dh at 2Eh/2Fh) is valid - a CMOS we
 *     cannot validate is never written;
 * then ONE compare-and-swap of 2Dh/2Eh/2Fh, index/data pairs paced with the
 * ROM's own delay (in al,84h). Every read of the bank is taken twice and must
 * agree (0Ah's UIP bit masked). Success = 2Dh-2Fh as planned, a valid
 * checksum, and none of this run's bytes in any other register. Anything else
 * is put back to the snapshot - 2Dh-2Fh, and our own stray bytes out of
 * wherever they landed (a register is "ours" only if it changed and now holds
 * a byte this run wrote) - and reported with the registers it could not
 * account for. It is never retried within a run: two reviews showed a retry
 * re-reading a lost restore byte as the new truth and reporting success over
 * a damaged 0Bh. The next agent start, or POSTSKIP apply, tries again. The
 * startup pass waits for clockfix (the other RTC writer) and skips loudly
 * rather than run beside it. Loop: agent/shared/postskip.h, tested against a
 * simulated RTC. Hand-tool twin: scripts/fleet/win9x/cmosw9x.c.
 *
 * Residual risk, stated: a byte of ours that loses its index to a clock
 * register (00h-09h) cannot be told from the ticking clock, and a legitimate
 * change that happens to equal one of our bytes would be "undone".
 *
 * 1.92.0 - CMOS 1Bh, the secondary IDE master type, on request. .243's 80 GB
 * disk runs natively under Windows' ESDI_506 only while POST leaves it alone
 * (1Bh = 00); a power loss resets 1Bh to auto (44h), POST types the drive,
 * the ROM's translation overflows to 256 heads and Windows drops the channel.
 * With CmosIde2Type set, the same one-write pass puts 1Bh back, and when it had
 * to - and the disk is not running natively - the agent reboots ONCE through
 * the shell (never twice within 20 minutes), so the next POST leaves the disk
 * to Windows. Nothing here runs without CmosIde2Type.
 *
 * Registry (HKLM\Software\RetroAgent):
 *   PostSkip        REG_DWORD  0 = do not run at startup (POSTSKIP still reports)
 *   PostSkipBoot    REG_SZ     what the startup pass found and did
 *   CmosIde2Type    REG_DWORD  what CMOS 1Bh must hold (0 on .243); absent = never touched
 *   CmosIde2Reboot  REG_DWORD  0 = restore 1Bh but never reboot for it
 *   CmosIde2Last    REG_DWORD  when the agent last rebooted for it (time())
 * Command: POSTSKIP [apply] - the live state as JSON (and set it now).
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "../shared/postskip.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define LOG_PS "POSTSKIP"
#define PS_IDE2_REBOOT_GAP_S 1200L  /* never reboot for 1Bh twice within 20 min */

typedef struct {
    int applicable;
    int ide2_want;                  /* -1 = CmosIde2Type absent: 1Bh never touched */
    ps_outcome_t o;
} ps_result_t;

static DWORD ps_reg_dword(const char *name, DWORD dflt, int *present)
{
    HKEY h;
    DWORD v = dflt, sz = sizeof(v), type = 0;
    if (present) *present = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) != ERROR_SUCCESS)
        return dflt;
    if (RegQueryValueExA(h, name, NULL, &type, (LPBYTE)&v, &sz) != ERROR_SUCCESS || type != REG_DWORD)
        v = dflt;
    else if (present)
        *present = 1;
    RegCloseKey(h);
    return v;
}

static int ps_ide2_want(void)
{
    int present;
    DWORD v = ps_reg_dword("CmosIde2Type", 0, &present);
    return present && v <= 0xFF ? (int)v : -1;
}

/* Is a disk on the secondary channel running under Windows right now - an ESDI
 * devnode on &CHILD0001& with no problem? (What a BIOS-typed 80 GB disk takes
 * away: ESDI_506's BIOS verify read fails and the channel is torn down.) */
static int ps_ide2_disk_native(void)
{
    HKEY root, k;
    char name[64], hw[256];
    DWORD i, n, t, sz, problem;
    int ok = 0;
    if (RegOpenKeyExA(HKEY_DYN_DATA, "Config Manager\\Enum", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return 0;
    for (i = 0; !ok; i++) {
        n = sizeof(name);
        if (RegEnumKeyExA(root, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExA(root, name, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        memset(hw, 0, sizeof(hw));
        sz = sizeof(hw) - 1;
        if (RegQueryValueExA(k, "HardWareKey", NULL, &t, (BYTE *)hw, &sz) == ERROR_SUCCESS
                && _strnicmp(hw, "ESDI\\", 5) == 0 && strstr(hw, "&CHILD0001&")) {
            problem = 0xFFFFFFFFUL;
            sz = sizeof(problem);
            RegQueryValueExA(k, "Problem", NULL, &t, (BYTE *)&problem, &sz);
            ok = problem == 0;
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    return ok;
}

#define PS_CLOCK_WAIT_MS 1200000    /* clockfix worst case: 12 x (connect timeout + 10 s gap) */
#define PS_BUSY_WAIT_MS  60000      /* a POSTSKIP command holds the CMOS for seconds at most */

static volatile LONG g_ps_busy = 0;

static int ps_is_win9x(void)
{
    OSVERSIONINFOA o;
    o.dwOSVersionInfoSize = sizeof(o);
    if (!GetVersionExA(&o)) return 0;
    return o.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS;
}

/* Ring-3 port I/O: allowed on Win9x, a fault on NT - ps_run() never reaches
 * these unless ps_is_win9x() said so. */
static unsigned char ps_io_inb(void *ctx, unsigned short p)
{
    unsigned char v;
    (void)ctx;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static void ps_io_outb(void *ctx, unsigned short p, unsigned char v)
{
    (void)ctx;
    __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p));
}
static void ps_io_sleep(void *ctx, unsigned ms) { (void)ctx; Sleep(ms); }

/* Win9x on the Deskpro 2000 04/25/97 ROM? Touches no port. `why` explains a 0. */
static int ps_applicable(const char **why)
{
    if (!ps_is_win9x()) { *why = "not Windows 9x - not applicable"; return 0; }
    if (IsBadReadPtr((const void *)0xF0000, 0x10000)) { *why = "cannot read the BIOS ROM at F0000 - not applicable"; return 0; }
    if (!ps_rom_matches((const unsigned char *)0xF0000, 0x10000UL)) {
        *why = "not the Compaq Deskpro 2000 04/25/97 ROM - not applicable";
        return 0;
    }
    *why = "";
    return 1;
}

static void ps_run(ps_result_t *r, int apply)
{
    static const ps_io_t io = { ps_io_inb, ps_io_outb, ps_io_sleep, NULL };
    const char *why;
    memset(r, 0, sizeof(*r));
    r->o.before_2d = r->o.now_2d = r->o.before_0e = r->o.now_0e = r->o.before_1b = r->o.now_1b = -1;
    r->o.failed = 1;
    r->ide2_want = -1;
    if (!ps_applicable(&why)) { r->o.state = why; return; }
    r->applicable = 1;
    r->ide2_want = ps_ide2_want();
    ps_cmos_run2(&io, apply, r->ide2_want, &r->o);
}

static void ps_summary(const ps_result_t *r, char *out, int cch)
{
    const ps_outcome_t *o = &r->o;
    _snprintf(out, cch - 1, "%s; 2Dh %s%02X -> %s%02X; 0Eh %s%02X -> %s%02X; checksum %s%s%s%s",
              o->state, o->before_2d < 0 ? "?" : "", o->before_2d < 0 ? 0 : o->before_2d,
              o->now_2d < 0 ? "?" : "", o->now_2d < 0 ? 0 : o->now_2d,
              o->before_0e < 0 ? "?" : "", o->before_0e < 0 ? 0 : o->before_0e,
              o->now_0e < 0 ? "?" : "", o->now_0e < 0 ? 0 : o->now_0e,
              !r->applicable || o->now_2d < 0 ? "n/a" : o->cs_now ? "valid" : "INVALID",
              o->strays_undone ? "; stray bytes undone" : "",
              o->changed[0] ? "; changed: " : "", o->changed);
    out[cch - 1] = 0;
    if (r->ide2_want >= 0) {
        size_t len = strlen(out);
        _snprintf(out + len, cch - 1 - len, "; 1Bh %s%02X -> %s%02X (want %02X)",
                  o->before_1b < 0 ? "?" : "", o->before_1b < 0 ? 0 : o->before_1b,
                  o->now_1b < 0 ? "?" : "", o->now_1b < 0 ? 0 : o->now_1b, r->ide2_want);
        out[cch - 1] = 0;
    }
}

/* Did this run put 1Bh back to what CmosIde2Type asks for - verified? */
static int ps_ide2_fixed(const ps_result_t *r)
{
    return r->applicable && r->ide2_want >= 0 && !r->o.failed && r->o.before_1b >= 0
        && r->o.before_1b != r->ide2_want && r->o.now_1b == r->ide2_want;
}

static void ps_store_boot(const char *s)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0,
                        KEY_WRITE, NULL, &h, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(h, "PostSkipBoot", 0, REG_SZ, (const BYTE *)s, (DWORD)strlen(s) + 1);
    RegCloseKey(h);
}

static void ps_load_boot(char *out, DWORD cch)
{
    HKEY h;
    DWORD type = 0, sz = cch;
    out[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(h, "PostSkipBoot", NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS || type != REG_SZ)
        out[0] = 0;
    out[cch - 1] = 0;
    RegCloseKey(h);
}

static int ps_boot_recorded(void)
{
    HKEY h;
    DWORD type = 0;
    int have = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) != ERROR_SUCCESS)
        return 0;
    have = RegQueryValueExA(h, "PostSkipBoot", NULL, &type, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(h);
    return have;
}

DWORD WINAPI postskip_thread(LPVOID param)
{
    char summary[400];
    ps_result_t r;
    const char *why;
    DWORD enabled = 1, sz = sizeof(enabled), type = 0, waited = 0;
    HKEY h;
    (void)param;
    if (!ps_is_win9x()) return 0;               /* nothing to do, nothing to log */
    if (!ps_applicable(&why)) {
        /* Replace an earlier record (a ROMPaq'd box must not keep reporting its
         * old result); never create one on a box this does not apply to. */
        if (ps_boot_recorded()) ps_store_boot(why);
        return 0;
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, "PostSkip", NULL, &type, (LPBYTE)&enabled, &sz) != ERROR_SUCCESS
                || type != REG_DWORD)
            enabled = 1;
        RegCloseKey(h);
    }
    if (!enabled) {
        log_msg(LOG_PS, "disabled by PostSkip=0");
        ps_store_boot("disabled by PostSkip=0");
        return 0;
    }
    /* Say at once that THIS boot's pass is pending: until it finishes, the
     * previous boot's "already set" must not be read as this boot's answer. */
    ps_store_boot("PENDING: this start's pass has not finished yet (waiting for clockfix)");
    /* clockfix sets the RTC through the same index/data ports: never race it.
     * Its pass is bounded; if it still has not finished, skip this start
     * loudly rather than write beside it. */
    while (!clockfix_finished() && waited < PS_CLOCK_WAIT_MS) { Sleep(1000); waited += 1000; }
    if (!clockfix_finished()) {
        log_msg(LOG_PS, "SKIPPED: clockfix still running after %lu s - CMOS NOT checked this start",
                (unsigned long)(waited / 1000));
        ps_store_boot("SKIPPED at startup: clockfix still running - run POSTSKIP apply once it has finished");
        return 0;
    }
    /* A POSTSKIP command holds the CMOS for seconds; wait for it, never give up
     * the boot's pass to a report-only query. */
    for (waited = 0; InterlockedExchange((LONG *)&g_ps_busy, 1); waited += 250) {
        if (waited >= PS_BUSY_WAIT_MS) {
            log_msg(LOG_PS, "SKIPPED: a POSTSKIP command held the CMOS for %lu s", (unsigned long)(waited / 1000));
            ps_store_boot("SKIPPED at startup: a POSTSKIP command held the CMOS too long");
            return 0;
        }
        Sleep(250);
    }
    ps_run(&r, 1);
    InterlockedExchange((LONG *)&g_ps_busy, 0);
    ps_summary(&r, summary, sizeof(summary));
    if (ps_ide2_fixed(&r)) {
        /* The POST that just ran typed the secondary disk. */
        int native = ps_ide2_disk_native(), want_reboot = ps_reg_dword("CmosIde2Reboot", 1, NULL) != 0;
        DWORD last = ps_reg_dword("CmosIde2Last", 0, NULL), now = (DWORD)time(NULL);
        long since = last ? (long)(now - last) : -1;
        size_t len = strlen(summary);
        if (want_reboot && ps_ide2_should_reboot(1, native, since, PS_IDE2_REBOOT_GAP_S)) {
            HKEY h;
            _snprintf(summary + len, sizeof(summary) - 1 - len,
                      "; secondary disk NOT running under Windows - REBOOTING once so POST leaves it alone");
            summary[sizeof(summary) - 1] = 0;
            log_msg(LOG_PS, "%s", summary);
            ps_store_boot(summary);
            if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0, KEY_WRITE, NULL, &h, NULL)
                    == ERROR_SUCCESS) {
                RegSetValueExA(h, "CmosIde2Last", 0, REG_DWORD, (const BYTE *)&now, sizeof(now));
                RegFlushKey(h);                 /* Win9x writes the registry lazily */
                RegCloseKey(h);
            }
            log_flush();
            agent_self_reboot_9x("POSTSKIP 1Bh");
            return 0;
        }
        _snprintf(summary + len, sizeof(summary) - 1 - len, "; %s",
                  native ? "the disk is running under Windows anyway - no reboot"
                  : !want_reboot ? "NOT rebooting (CmosIde2Reboot=0) - D:/E: return at the next reboot"
                  : "NOT rebooting: the agent already rebooted for 1Bh less than 20 minutes ago - does the CMOS keep 1Bh?");
        summary[sizeof(summary) - 1] = 0;
    }
    log_msg(LOG_PS, "%s", summary);
    ps_store_boot(summary);
    return 0;
}

void handle_postskip(SOCKET sock, const char *args)
{
    char last_boot[400];
    ps_result_t r;
    json_t j;
    char *out;
    const char *why;
    int apply = args && _stricmp(args, "apply") == 0;

    memset(&r, 0, sizeof(r));
    r.o.before_2d = r.o.now_2d = r.o.before_0e = r.o.now_0e = r.o.before_1b = r.o.now_1b = -1;
    r.o.failed = 1;
    r.ide2_want = -1;
    if (!ps_applicable(&why)) {
        r.o.state = why;                        /* applicable:false - no port touched */
    } else if (!clockfix_finished()) {
        /* Not even a read: clockfix writes the RTC through the same index
         * port, and a read's index write can retarget its data byte. */
        r.applicable = 1;
        r.o.state = "NOT READ: clockfix is still setting the clock through the same RTC ports - retry once it has finished";
    } else if (InterlockedExchange((LONG *)&g_ps_busy, 1)) {
        send_error_response(sock, "POSTSKIP already running");
        return;
    } else {
        ps_run(&r, apply);
        InterlockedExchange((LONG *)&g_ps_busy, 0);
    }

    json_init(&j);
    json_object_start(&j);
    json_kv_bool(&j, "applicable", r.applicable);
    json_kv_str(&j, "state", r.o.state ? r.o.state : "");
    json_kv_int(&j, "cmos_2d_before", r.o.before_2d);
    json_kv_int(&j, "cmos_2d_now", r.o.now_2d);
    json_kv_bool(&j, "skip_f1", r.o.now_2d >= 0 && (r.o.now_2d & PS_SKIP_F1) != 0);
    json_kv_bool(&j, "checksum_valid", r.o.now_2d >= 0 && r.o.cs_now);
    json_kv_int(&j, "cmos_0e_now", r.o.now_0e);
    json_kv_bool(&j, "time_invalid_flag", r.o.now_0e >= 0 && (r.o.now_0e & PS_DIAG_TIME_BAD) != 0);
    json_kv_int(&j, "cmos_1b_before", r.o.before_1b);
    json_kv_int(&j, "cmos_1b_now", r.o.now_1b);
    json_kv_int(&j, "cmos_1b_want", r.ide2_want);
    json_kv_int(&j, "write_attempts", r.o.attempts);
    json_kv_int(&j, "strays_undone", r.o.strays_undone);
    json_kv_str(&j, "registers_changed", r.o.changed);
    json_kv_bool(&j, "ok", r.applicable && !r.o.failed);
    ps_load_boot(last_boot, sizeof(last_boot));
    json_kv_str(&j, "last_boot", last_boot);
    json_object_end(&j);
    out = json_finish(&j);
    if (!out) { send_error_response(sock, "POSTSKIP: out of memory"); return; }
    send_text_response(sock, out);
    HeapFree(GetProcessHeap(), 0, out);
}

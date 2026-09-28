/* glide2probe.c - the Glide 2 API, step by step, through whatever glide2x.dll
 * the loader finds (on a V5 box: AmigaMerlin's "Glide2 to Glide3 Translator",
 * which forwards to glide3x.dll). Each step's result is logged and flushed to
 * the disk BEFORE the next call, so the step that fails - or the last one
 * reached before a fault or a hang - is on disk.
 *
 *   glide2probe.exe [--dll glide2x.dll] [--log path] [--res 7] [--ref 0]
 *                   [--bufs 2] [--aux 1] [--cformat 0] [--frames 60]
 *                   [--reopen RES:REF:BUFS[:AUX]]... [--killfocus natural|same|thread]
 *                   [--hold S] [--timeout S] [--hard-timeout S] [--pace MS]
 *                   [--no-plugin] [--trace N] [--alt-tab-fix 0] [--no-minimize]
 *                   [--ue1]
 *
 * res/ref are Glide 2 enums (GR_RESOLUTION_640x480 = 7, 800x600 = 8, 1024x768
 * = 12, 1280x1024 = 13, 1600x1200 = 14; GR_REFRESH_60Hz = 0). With no option it
 * is the old probe: open, clear+swap 60 frames, close, grGlideShutdown.
 *
 * THE UNREAL GOLD RE-OPEN (2026-09-28, .124 V5 6000, our h5 glide3x under the
 * translator). Unreal Gold's GlideDrv opened Glide, then shut it down and
 * hung in its SECOND grSstWinOpen. From the game's own binaries (GlideDrv.dll
 * and WinDrv.dll of the staged tree, disassembled): UE1 calls grGlideInit ONCE
 * per process (a static counter), and every later render-device Init is
 * grGlideGetVersion, grSstQueryBoards, grErrorSetCallback,
 * grSstQueryHardware, grSstSelect(0), grSstWinOpen; its Exit is grSstWinClose
 * alone (no grGlideShutdown). The Exit/Init pair is WinDrv's WM_KILLFOCUS
 * handler: in fullscreen it calls EndFullscreen, which runs
 * TryRenderDevice(WindowedRenderDevice, windowed size, Fullscreen=0) - and the
 * fleet launcher had written GlideDrv into WindowedRenderDevice, so the re-open
 * is a WINDOWED GlideDrv Init: grSstWinOpen at the windowed size (800x600 ->
 * GR_RESOLUTION_800x600, 2 color buffers) made from INSIDE WM_KILLFOCUS, after
 * which WinDrv minimizes the window (SW_SHOWMINNOACTIVE).
 * NB Unreal Gold's log line "grSstOpen Res=%i Ref=%i" has the two numbers
 * SWAPPED (UT's GlideDrv prints them in order): "Res=0 Ref=13" is
 * GR_RESOLUTION_1280x1024 at GR_REFRESH_60Hz, not 320x200.
 *
 * --ue1 replays that: open 13:0 with 3+1 buffers, colour format ABGR (UE1's),
 * then the re-open 8:0:2:1 inside the window's WM_KILLFOCUS (--killfocus
 * natural = only if a focus loss happens by itself within --hold seconds,
 * which is the question "does our open lose the window the focus"; same /
 * thread = cause one with a second window on this thread / on another thread,
 * as another application would). Without --killfocus a --reopen is made in
 * sequence, UE1's calls between the close and the open.
 *
 * Every focus, activation, size, display-change and palette message the
 * window receives is logged with its tick, and so is the foreground window at
 * each step: whether the window HAS the foreground when Glide opens is half
 * the question.
 *
 * WATCHDOG. A step that runs past --timeout seconds is reported - "HUNG" and
 * the main thread's EIP and every stack word that points into a module's
 * code, as module+offset and nearest export - again every --timeout seconds,
 * so a moving EIP (a spin) reads differently from a still one (a block). It
 * does not kill unless --hard-timeout is given: then, after that many
 * seconds, it stamps the pace gate AHEAD (XP reverts the mode as the process
 * dies) and ends the process. Otherwise the host ends it, then runs `vcrctl
 * pace-mark` (the kill is a switch nothing else records).
 *
 * PACED. Every grSstWinOpen / grSstWinClose / grGlideShutdown with a context
 * open goes through vcr-kmd/tools/vcr_pace.h (at least 3 s between any two
 * mode switches on the box, across processes); a refused switch ends the run
 * without switching. A focus loss while a context is open is a switch
 * DirectDraw makes by itself (its window hook gives the desktop back): it is
 * stamped when it is seen.
 *
 * Environment for our glide3x, set before the DLL loads: --no-plugin
 * (FX_GLIDE_NO_PLUGIN=1: no 3dfxspl3.dll), --trace N (FX_GLIDE_TRACE=N,
 * FX_GLIDE_TRACE_FILE=<log>.trace), --alt-tab-fix 0 (FX_GLIDE_ALT_TAB_FIX=0:
 * no XP alt-tab hook, so a focus loss does not mark the context lost).
 *
 * Clean-room: only the public Glide 2 API names and signatures (sst1/cvg/h3
 * glide.h in the GPL release). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>
#include "../vcr-kmd/tools/vcr_pace.h"

typedef int FxBool;
typedef unsigned long FxU32;
typedef void (__stdcall *pv0)(void);
typedef FxBool (__stdcall *pqh)(void *);
typedef void (__stdcall *psel)(int);
typedef FxBool (__stdcall *pwo)(FxU32, int, int, int, int, int, int);
typedef void (__stdcall *pclr)(FxU32, unsigned char, unsigned short);
typedef void (__stdcall *pswp)(int);
typedef void (__stdcall *pver)(char *);
typedef void (*G2ErrCb)(const char *, FxBool);      /* Glide 2's callback: cdecl */
typedef void (__stdcall *perr)(G2ErrCb);

/* ---- the flushed log ------------------------------------------------------------ */
static FILE *L;
static CRITICAL_SECTION g_logcs;

static void say(const char *fmt, ...)
{
    va_list ap;
    FILE *f;
    EnterCriticalSection(&g_logcs);
    f = L ? L : stdout;
    fprintf(f, "%lu ", (unsigned long)GetTickCount());
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    if (L)      /* fflush reaches the OS cache only; a frozen PC never writes it back */
        FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(L)));
    LeaveCriticalSection(&g_logcs);
}

/* ---- options ----------------------------------------------------------------------- */
#define MAX_OPENS 5
typedef struct { int res, ref, bufs, aux; } OpenSpec;
static struct {
    const char *dll, *log, *killfocus;
    OpenSpec open[MAX_OPENS];
    int nopen, cformat, frames, hold_s, timeout_s, hard_s, trace, no_plugin, alt_tab_fix,
        minimize, ue1;
    DWORD pace_ms;
} O = { "glide2x.dll", NULL, NULL, {{7, 0, 2, 1}}, 1, 0, 60, 8, 30, 0, 0, 0, -1, 1, 0, 0 };

static int parse_open(const char *s, OpenSpec *o)
{
    int n;
    o->aux = 1;
    n = sscanf(s, "%d:%d:%d:%d", &o->res, &o->ref, &o->bufs, &o->aux);
    return n >= 3 && o->res >= 0 && o->res <= 0x17 && o->ref >= 0 && o->ref <= 8 &&
           o->bufs >= 1 && o->bufs <= 3 && o->aux >= 0 && o->aux <= 1;
}

/* ---- Glide 2 entry points ----------------------------------------------------------- */
static pv0 g_init, g_shut, g_close;
static pqh g_query, g_boards;
static psel g_sel;
static pwo g_open;
static pclr g_clr;
static pswp g_swp;
static pver g_ver;
static perr g_errcb;

static FARPROC sym(HMODULE h, const char *dec, const char *plain)
{
    FARPROC p = GetProcAddress(h, dec);
    if (!p)
        p = GetProcAddress(h, plain);
    say("  %s -> %p", dec, (void *)p);
    return p;
}

static void glide_error(const char *s, FxBool fatal)
{
    say("GLIDE ERROR CALLBACK (fatal %d): %s", fatal, s ? s : "(null)");
}

static HWND g_wnd_for_log;              /* = g_wnd, for foreground_line() */
static DWORD g_main_tid;

/* ---- the step the main thread is in, for the watchdog ---------------------------------- */
#define MAX_DEPTH 8
static volatile LONG g_depth;
static const char *volatile g_step[MAX_DEPTH];
static volatile DWORD g_step_t0[MAX_DEPTH];
static volatile LONG g_hung;            /* a step ran past --timeout at least once */
static const char *volatile g_hung_step;
static HANDLE g_main_thread;

static void step_in(const char *name)
{
    LONG d = g_depth;
    say("step: %s", name);
    if (d < MAX_DEPTH) {
        g_step[d] = name;
        g_step_t0[d] = GetTickCount();
    }
    InterlockedIncrement(&g_depth);
}

static void step_out(void)
{
    if (g_depth > 0)
        InterlockedDecrement(&g_depth);
}

/* ---- stack words that point into code ------------------------------------------------- */
/* No loader lock anywhere on this path: the main thread may be hung holding it
 * (inside LoadLibrary, a DLL's init, GetProcAddress), and GetModuleFileName
 * would then block the watchdog too. The module's name comes from its own
 * export directory, read straight out of the mapped image. */
static int readable(const void *p, DWORD n)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD a = (DWORD)(ULONG_PTR)p;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    return a + n <= (DWORD)(ULONG_PTR)mbi.BaseAddress + (DWORD)mbi.RegionSize;
}

static void describe(DWORD a, char *out, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const BYTE *img;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_DATA_DIRECTORY *dd;
    const char *mname = NULL, *bname = NULL;
    DWORD rva, best = 0;

    out[0] = 0;
    if (!VirtualQuery((LPCVOID)(ULONG_PTR)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        mbi.Type != MEM_IMAGE)
        return;
    if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return;
    img = (const BYTE *)mbi.AllocationBase;
    dos = (const IMAGE_DOS_HEADER *)img;
    if (!readable(dos, sizeof *dos) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (const IMAGE_NT_HEADERS *)(img + dos->e_lfanew);
    if (!readable(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    rva = a - (DWORD)(ULONG_PTR)img;
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dd->VirtualAddress && dd->Size &&
        readable(img + dd->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) {
        /* the nearest export at or below the address: enough to tell
         * user32!SetForegroundWindow from ddraw's internals on the host */
        const IMAGE_EXPORT_DIRECTORY *ed =
            (const IMAGE_EXPORT_DIRECTORY *)(img + dd->VirtualAddress);
        const DWORD *fn = (const DWORD *)(img + ed->AddressOfFunctions);
        const DWORD *nm = (const DWORD *)(img + ed->AddressOfNames);
        const WORD *ord = (const WORD *)(img + ed->AddressOfNameOrdinals);
        DWORD i;
        mname = (const char *)(img + ed->Name);
        for (i = 0; i < ed->NumberOfNames; i++) {
            DWORD f = fn[ord[i]];
            if (f <= rva && f > best) {
                best = f;
                bname = (const char *)(img + nm[i]);
            }
        }
    }
    if (!mname)
        _snprintf(out, n, "image@%08lx+0x%lx", (unsigned long)(ULONG_PTR)img,
                  (unsigned long)rva);
    else if (bname)
        _snprintf(out, n, "%s+0x%lx (%s+0x%lx)", mname, (unsigned long)rva, bname,
                  (unsigned long)(rva - best));
    else
        _snprintf(out, n, "%s+0x%lx", mname, (unsigned long)rva);
    out[n - 1] = 0;
}

/* InternalGetWindowText sends no message: GetWindowText to a window of a hung
 * thread (ours, when the main thread is the one stuck) never returns */
typedef int (WINAPI *pigwt)(HWND, LPWSTR, int);

static void foreground_line(const char *why)
{
    static pigwt igwt;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0, tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
    char cls[64] = "", title[96] = "";
    WCHAR wt[96];
    if (!igwt)
        igwt = (pigwt)GetProcAddress(GetModuleHandleA("user32.dll"), "InternalGetWindowText");
    if (fg) {
        GetClassNameA(fg, cls, sizeof cls);
        if (igwt && igwt(fg, wt, 96) > 0)
            WideCharToMultiByte(CP_ACP, 0, wt, -1, title, sizeof title, NULL, NULL);
    }
    say("  foreground (%s): hwnd %p pid %lu tid %lu%s class \"%s\" title \"%s\"; ours %p, "
        "thread %s", why, (void *)fg, (unsigned long)pid, (unsigned long)tid,
        pid == GetCurrentProcessId() ? " (THIS process)" : "", cls, title,
        (void *)g_wnd_for_log, !fg ? "-" : tid == g_main_tid ? "main" : "other");
}

/* Suspended, the main thread's context and stack are only COPIED - no lock
 * of any kind is taken until it runs again - then described. */
#define STACK_COPY 16384
static DWORD g_stack[STACK_COPY / 4];

static void dump_main_thread(void)
{
    CONTEXT c;
    MEMORY_BASIC_INFORMATION mbi;
    char what[160];
    DWORD esp, end, i, nwords = 0;
    SIZE_T got = 0;
    int hits = 0;

    if (SuspendThread(g_main_thread) == (DWORD)-1) {
        say("  (SuspendThread failed: %lu)", GetLastError());
        return;
    }
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(g_main_thread, &c)) {
        esp = c.Esp;
        end = esp + STACK_COPY;
        if (VirtualQuery((LPCVOID)(ULONG_PTR)esp, &mbi, sizeof mbi) &&
            (DWORD)(ULONG_PTR)mbi.BaseAddress + (DWORD)mbi.RegionSize < end)
            end = (DWORD)(ULONG_PTR)mbi.BaseAddress + (DWORD)mbi.RegionSize;
        if (end > esp && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)esp, g_stack,
                                           end - esp, &got))
            nwords = (DWORD)got / 4;
    } else {
        c.Eip = 0;
    }
    ResumeThread(g_main_thread);
    if (!c.Eip) {
        say("  (GetThreadContext failed)");
        return;
    }
    say("  main thread EIP %08lx ESP %08lx EBP %08lx (raw; stack words copied: %lu)",
        (unsigned long)c.Eip, (unsigned long)c.Esp, (unsigned long)c.Ebp,
        (unsigned long)nwords);
    describe(c.Eip, what, sizeof what);
    say("  EIP = %s", what[0] ? what : "(not in a module)");
    for (i = 0; i < nwords && hits < 40; i++) {
        describe(g_stack[i], what, sizeof what);
        if (what[0]) {
            say("    [esp+%04lx] %08lx %s", (unsigned long)(i * 4), (unsigned long)g_stack[i],
                what);
            hits++;
        }
    }
}

static DWORD WINAPI watchdog(LPVOID arg)
{
    DWORD last_dump = 0, t0 = GetTickCount();
    (void)arg;
    for (;;) {
        LONG d;
        DWORD now;
        Sleep(1000);
        now = GetTickCount();
        d = g_depth;
        if (d > 0 && d <= MAX_DEPTH) {
            DWORD age = now - g_step_t0[d - 1];
            if (age >= (DWORD)O.timeout_s * 1000u &&
                (!last_dump || now - last_dump >= (DWORD)O.timeout_s * 1000u)) {
                g_hung = 1;
                g_hung_step = g_step[d - 1];
                say("HUNG: step \"%s\" (depth %ld) running %lu s", g_step[d - 1], d,
                    (unsigned long)(age / 1000));
                foreground_line("at the hang");
                dump_main_thread();
                last_dump = now;
            }
        }
        if (O.hard_s > 0 && g_hung && now - t0 >= (DWORD)O.hard_s * 1000u) {
            say("HARD TIMEOUT after %d s, a step hung (\"%s\") - the revert XP makes as the "
                "process dies is stamped ahead; ending the process", O.hard_s,
                g_hung_step ? g_hung_step : "?");
            say("RESULT {\"mode\":\"%s\",\"hung\":true,\"hung_step\":\"%s\",\"killed\":true}",
                O.ue1 ? "ue1" : O.nopen > 1 ? "reopen" : "open",
                g_hung_step ? g_hung_step : "?");
            /* the revert XP makes as the process dies, stamped AHEAD; then the
             * process ends at once: ExitProcess would run glide3x's
             * DLL_PROCESS_DETACH (grGlideShutdown, DirectDraw) beside a main
             * thread hung inside Glide or DirectDraw, and the exit hold would
             * wait on the switch lock that thread may hold */
            vcr_pace_mark_ahead(VCR_PACE_EXIT_LAG_MS);
            TerminateProcess(GetCurrentProcess(), 99);
        }
    }
    return 0;
}

/* ---- the window ---------------------------------------------------------------------- */
static HWND g_wnd, g_thief;
static int g_ctx;                       /* a Glide context is open */
static int g_killfocus_seen, g_activateapp_off, g_reopen_done, g_in_reopen;
static int g_want_reopen_in_killfocus;
static int g_next_open = 1;             /* index into O.open of the next open */
static int g_opens_ok, g_opens_failed, g_refused;

static int do_open(const OpenSpec *s, const char *why);
static void do_close(const char *why);
static void ue1_requery(void);
static void frames(int n);

static const char *msgname(UINT m)
{
    switch (m) {
    case WM_ACTIVATE: return "WM_ACTIVATE";
    case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP";
    case WM_SETFOCUS: return "WM_SETFOCUS";
    case WM_KILLFOCUS: return "WM_KILLFOCUS";
    case WM_SIZE: return "WM_SIZE";
    case WM_SHOWWINDOW: return "WM_SHOWWINDOW";
    case WM_DISPLAYCHANGE: return "WM_DISPLAYCHANGE";
    case WM_PALETTECHANGED: return "WM_PALETTECHANGED";
    case WM_QUERYNEWPALETTE: return "WM_QUERYNEWPALETTE";
    case WM_NCACTIVATE: return "WM_NCACTIVATE";
    case WM_STYLECHANGED: return "WM_STYLECHANGED";
    case WM_WINDOWPOSCHANGED: return "WM_WINDOWPOSCHANGED";
    }
    return NULL;
}

/* UE1 WinDrv's WM_KILLFOCUS in fullscreen: EndFullscreen (the render device's
 * Exit + a windowed Init), then minimize. Glide is closed and re-opened HERE,
 * inside the focus change, as the game did. */
static void reopen_in_killfocus(void)
{
    if (g_in_reopen || g_reopen_done || g_next_open >= O.nopen || !g_ctx)
        return;
    g_in_reopen = 1;
    step_in("WM_KILLFOCUS handler: UE1 EndFullscreen - Exit (close) + windowed Init (re-open)");
    foreground_line("in WM_KILLFOCUS");
    do_close("render device Exit, inside WM_KILLFOCUS");
    ue1_requery();
    if (do_open(&O.open[g_next_open], "windowed render device Init, inside WM_KILLFOCUS"))
        frames(O.frames < 10 ? O.frames : 10);
    g_next_open++;
    g_reopen_done = 1;
    if (O.minimize) {
        step_in("ShowWindow(SW_SHOWMINNOACTIVE), as WinDrv does after EndFullscreen");
        ShowWindow(g_wnd, SW_SHOWMINNOACTIVE);
        step_out();
    }
    step_out();
    g_in_reopen = 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    const char *n = h == g_wnd ? msgname(m) : NULL;
    if (n)
        say("msg %s wParam %08lx lParam %08lx%s", n, (unsigned long)w, (unsigned long)l,
            g_ctx ? " (context open)" : "");
    if (h == g_wnd && m == WM_ACTIVATEAPP && !w) {
        g_activateapp_off = 1;
        /* DirectDraw's hook gives the desktop back right now, unpaced: record it */
        if (g_ctx)
            vcr_pace_mark();
    }
    if (h == g_wnd && m == WM_KILLFOCUS) {
        g_killfocus_seen = 1;
        if (g_want_reopen_in_killfocus)
            reopen_in_killfocus();
    }
    if (m == WM_CLOSE)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ---- focus thieves ---------------------------------------------------------------------- */
/* created HIDDEN: showing a top-level window from the foreground thread
 * activates it, and the focus change would then happen outside the step that
 * is meant to cause it */
static HWND make_thief_window(const char *title)
{
    return CreateWindowExA(0, "glide2probe", title, WS_POPUP, 0, 0, 64, 64, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
}

static DWORD WINAPI thief_thread(LPVOID arg)
{
    HWND t;
    MSG msg;
    DWORD until;
    (void)arg;
    t = make_thief_window("glide2probe thief (thread)");
    say("thief thread: window %p, ShowWindow + SetForegroundWindow ...", (void *)t);
    ShowWindow(t, SW_SHOW);
    say("thief thread: SetForegroundWindow -> %d", SetForegroundWindow(t));
    until = GetTickCount() + 20000;
    while ((LONG)(GetTickCount() - until) < 0) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        Sleep(20);
    }
    DestroyWindow(t);
    return 0;
}

/* ---- Glide steps -------------------------------------------------------------------------- */
static void frames(int n)
{
    int i;
    char label[64];
    _snprintf(label, sizeof label, "%d clears + swaps", n);
    label[sizeof label - 1] = 0;
    step_in(label);
    for (i = 0; i < n && g_ctx; i++) {
        if (g_clr)
            g_clr(0x00400000u | ((unsigned)i * 4u), 0, 0xffff);
        if (g_swp)
            g_swp(1);
        pump();
    }
    step_out();
}

static int do_open(const OpenSpec *s, const char *why)
{
    char label[160];
    FxBool ok;
    _snprintf(label, sizeof label, "grSstWinOpen(res %d, ref %d, cformat %d, bufs %d, aux %d) "
              "- %s", s->res, s->ref, O.cformat, s->bufs, s->aux, why);
    label[sizeof label - 1] = 0;
    foreground_line("before the open");
    /* its own step: a wait for the pace floor or the switch lock is not Glide */
    step_in("pace gate: the floor since the last switch, before the open");
    if (!vcr_pace_before_switch()) {
        step_out();
        say("PACE: open NOT made - %s", g_vcr_pace_why ? g_vcr_pace_why : "refused");
        g_refused = 1;
        return 0;
    }
    step_out();
    step_in(label);
    ok = g_open((FxU32)(ULONG_PTR)g_wnd, s->res, s->ref, O.cformat, 0, s->bufs, s->aux);
    vcr_pace_after_switch();    /* recorded whatever the outcome: the monitor re-synced */
    step_out();
    say("grSstWinOpen -> %d", ok);
    foreground_line("after the open");
    if (ok) {
        g_ctx = 1;
        g_opens_ok++;
    } else {
        g_opens_failed++;
    }
    return ok;
}

static void do_close(const char *why)
{
    char label[128];
    if (!g_ctx || !g_close)
        return;
    _snprintf(label, sizeof label, "grSstWinClose - %s", why);
    label[sizeof label - 1] = 0;
    step_in("pace gate: the floor since the last switch, before the close");
    if (!vcr_pace_before_switch()) {
        step_out();
        /* the mode is left to the exit hold (vcr_pace.h): no other road */
        say("PACE: close NOT made - %s", g_vcr_pace_why ? g_vcr_pace_why : "refused");
        g_refused = 1;
        return;
    }
    step_out();
    step_in(label);
    g_close();
    vcr_pace_after_restore();
    step_out();
    g_ctx = 0;
    say("grSstWinClose returned");
}

/* what UE1's render-device Init does before its SetRes when Glide is already
 * initialised: no grGlideInit (a static counter), the rest as at first */
static void ue1_requery(void)
{
    unsigned char hw[4096];
    char ver[128];
    FxBool ok;
    step_in("UE1 re-Init: grGlideGetVersion, grSstQueryBoards, grErrorSetCallback, "
            "grSstQueryHardware, grSstSelect(0)");
    if (g_ver) {
        memset(ver, 0, sizeof ver);
        g_ver(ver);
        ver[sizeof ver - 1] = 0;
        say("  grGlideGetVersion: %s", ver);
    }
    if (g_boards) {
        memset(hw, 0, sizeof hw);
        say("  grSstQueryBoards -> %d, boards %lu", g_boards(hw), *(FxU32 *)hw);
    }
    if (g_errcb)
        g_errcb(glide_error);
    memset(hw, 0, sizeof hw);
    ok = g_query(hw);
    say("  grSstQueryHardware -> %d, num_sst %lu, type %lu, fbRam %lu", ok, *(FxU32 *)hw,
        *(FxU32 *)(hw + 4), *(FxU32 *)(hw + 8));
    g_sel(0);
    step_out();
}

static void setenv_glide(const char *kv)
{
    say("env: %s", kv);
    _putenv(kv);
}

static void usage(void)
{
    printf("glide2probe [--dll glide2x.dll] [--log path] [--res N] [--ref N] [--bufs N] "
           "[--aux N]\n  [--cformat N] [--frames N] [--reopen RES:REF:BUFS[:AUX]]... "
           "[--killfocus natural|same|thread]\n  [--hold S] [--timeout S] [--hard-timeout S] "
           "[--pace MS] [--no-plugin] [--trace N]\n  [--alt-tab-fix 0] [--no-minimize] "
           "[--ue1]\n");
}

int main(int argc, char **argv)
{
    HMODULE h;
    WNDCLASSA wc;
    unsigned char hw[4096];
    char ver[128], envbuf[MAX_PATH + 32];
    FxBool ok;
    int i, rc;

    InitializeCriticalSection(&g_logcs);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--dll") && v) { O.dll = v; i++; }
        else if (!strcmp(a, "--log") && v) { O.log = v; i++; }
        else if (!strcmp(a, "--res") && v) { O.open[0].res = atoi(v); i++; }
        else if (!strcmp(a, "--ref") && v) { O.open[0].ref = atoi(v); i++; }
        else if (!strcmp(a, "--bufs") && v) { O.open[0].bufs = atoi(v); i++; }
        else if (!strcmp(a, "--aux") && v) { O.open[0].aux = atoi(v); i++; }
        else if (!strcmp(a, "--cformat") && v) { O.cformat = atoi(v); i++; }
        else if (!strcmp(a, "--frames") && v) { O.frames = atoi(v); i++; }
        else if (!strcmp(a, "--hold") && v) { O.hold_s = atoi(v); i++; }
        else if (!strcmp(a, "--timeout") && v) { O.timeout_s = atoi(v); i++; }
        else if (!strcmp(a, "--hard-timeout") && v) { O.hard_s = atoi(v); i++; }
        else if (!strcmp(a, "--trace") && v) { O.trace = atoi(v); i++; }
        else if (!strcmp(a, "--alt-tab-fix") && v) { O.alt_tab_fix = atoi(v); i++; }
        else if (!strcmp(a, "--killfocus") && v) { O.killfocus = v; i++; }
        else if (!strcmp(a, "--no-plugin")) O.no_plugin = 1;
        else if (!strcmp(a, "--no-minimize")) O.minimize = 0;
        else if (!strcmp(a, "--pace") && v) {
            if (!vcr_pace_parse_ms(v, &O.pace_ms)) {
                printf("--pace: 0..%u ms\n", VCR_PACE_MAX_MS);
                return 2;
            }
            i++;
        } else if (!strcmp(a, "--reopen") && v) {
            if (O.nopen >= MAX_OPENS || !parse_open(v, &O.open[O.nopen])) {
                printf("--reopen RES:REF:BUFS[:AUX] (at most %d opens)\n", MAX_OPENS);
                return 2;
            }
            O.nopen++;
            i++;
        } else if (!strcmp(a, "--ue1")) {
            /* Unreal Gold at the launcher's 1280x960 with GlideDrv as the
             * windowed device too: 13:0 3+1 ABGR, then 8:0 2+1 in WM_KILLFOCUS */
            static const OpenSpec first = { 13, 0, 3, 1 }, second = { 8, 0, 2, 1 };
            O.ue1 = 1;
            O.open[0] = first;
            O.open[1] = second;
            O.nopen = 2;
            O.cformat = 1;
            if (!O.killfocus)
                O.killfocus = "natural";
        } else { usage(); return 2; }
    }
    if (O.killfocus && strcmp(O.killfocus, "natural") && strcmp(O.killfocus, "same") &&
        strcmp(O.killfocus, "thread")) {
        usage();
        return 2;
    }
    if (O.killfocus && O.nopen < 2) {
        printf("--killfocus needs a --reopen (or --ue1)\n");
        return 2;
    }
    if (O.timeout_s < 2) O.timeout_s = 2;
    if (O.hold_s < 0) O.hold_s = 0;
    if (O.log)
        L = fopen(O.log, "w");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (O.pace_ms)
        vcr_pace_set_min(O.pace_ms);
    say("glide2probe: dll %s, %d open(s), first res %d ref %d bufs %d aux %d cformat %d, "
        "killfocus %s, frames %d, timeout %d s, hard timeout %d s", O.dll, O.nopen,
        O.open[0].res, O.open[0].ref, O.open[0].bufs, O.open[0].aux, O.cformat,
        O.killfocus ? O.killfocus : "-", O.frames, O.timeout_s, O.hard_s);

    /* the main thread, for the watchdog's stack dumps */
    g_main_tid = GetCurrentThreadId();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, 0);
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));

    if (O.no_plugin)
        setenv_glide("FX_GLIDE_NO_PLUGIN=1");
    if (O.alt_tab_fix == 0)
        setenv_glide("FX_GLIDE_ALT_TAB_FIX=0");
    if (O.trace > 0) {
        _snprintf(envbuf, sizeof envbuf, "FX_GLIDE_TRACE=%d", O.trace);
        envbuf[sizeof envbuf - 1] = 0;
        setenv_glide(envbuf);
        if (O.log) {
            _snprintf(envbuf, sizeof envbuf, "FX_GLIDE_TRACE_FILE=%s.trace", O.log);
            envbuf[sizeof envbuf - 1] = 0;
            setenv_glide(envbuf);
        }
    }

    step_in("LoadLibrary");
    h = LoadLibraryA(O.dll);
    step_out();
    say("LoadLibrary -> %p (err %lu)", (void *)h, h ? 0 : GetLastError());
    if (!h)
        return 1;
    {
        char path[MAX_PATH];
        HMODULE g3 = GetModuleHandleA("glide3x.dll");
        GetModuleFileNameA(h, path, sizeof path);
        say("  loaded %s", path);
        if (g3) {
            GetModuleFileNameA(g3, path, sizeof path);
            say("  glide3x already loaded by it: %s", path);
        }
    }
    g_ver = (pver)sym(h, "_grGlideGetVersion@4", "grGlideGetVersion");
    g_boards = (pqh)sym(h, "_grSstQueryBoards@4", "grSstQueryBoards");
    g_init = (pv0)sym(h, "_grGlideInit@0", "grGlideInit");
    g_errcb = (perr)sym(h, "_grErrorSetCallback@4", "grErrorSetCallback");
    g_query = (pqh)sym(h, "_grSstQueryHardware@4", "grSstQueryHardware");
    g_sel = (psel)sym(h, "_grSstSelect@4", "grSstSelect");
    g_open = (pwo)sym(h, "_grSstWinOpen@28", "grSstWinOpen");
    g_close = (pv0)sym(h, "_grSstWinClose@0", "grSstWinClose");
    g_clr = (pclr)sym(h, "_grBufferClear@12", "grBufferClear");
    g_swp = (pswp)sym(h, "_grBufferSwap@4", "grBufferSwap");
    g_shut = (pv0)sym(h, "_grGlideShutdown@0", "grGlideShutdown");
    if (!g_init || !g_query || !g_sel || !g_open || !g_shut)
        return 2;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "glide2probe";
    RegisterClassA(&wc);
    /* UE1's viewport is a plain WS_POPUP (no TOPMOST); the old probe's window
     * was TOPMOST, kept for the plain open */
    g_wnd = CreateWindowExA(O.nopen > 1 ? 0 : WS_EX_TOPMOST, "glide2probe", "glide2probe",
                            WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, NULL, NULL, wc.hInstance,
                            NULL);
    g_wnd_for_log = g_wnd;
    say("window %p", (void *)g_wnd);
    ShowWindow(g_wnd, SW_SHOW);
    say("SetForegroundWindow -> %d", SetForegroundWindow(g_wnd));
    pump();
    foreground_line("window up");

    step_in("grGlideGetVersion / grSstQueryBoards / grGlideInit / grErrorSetCallback / "
            "grSstQueryHardware / grSstSelect(0) - UE1's first Init");
    if (g_ver) {
        memset(ver, 0, sizeof ver);
        g_ver(ver);
        ver[sizeof ver - 1] = 0;
        say("  grGlideGetVersion: %s", ver);
    }
    if (g_boards) {
        memset(hw, 0, sizeof hw);
        say("  grSstQueryBoards -> %d, boards %lu", g_boards(hw), *(FxU32 *)hw);
    }
    g_init();
    say("  grGlideInit returned");
    if (g_errcb)
        g_errcb(glide_error);
    memset(hw, 0, sizeof hw);
    ok = g_query(hw);
    say("  grSstQueryHardware -> %d, num_sst %lu, type %lu, fbRam %lu", ok, *(FxU32 *)hw,
        *(FxU32 *)(hw + 4), *(FxU32 *)(hw + 8));
    g_sel(0);
    say("  grSstSelect(0) returned");
    step_out();

    if (do_open(&O.open[0], "first open")) {
        frames(O.frames);
        if (O.killfocus) {
            DWORD until;
            g_want_reopen_in_killfocus = 1;
            if (!strcmp(O.killfocus, "same")) {
                say("focus thief on this thread: a second window, SetFocus to it");
                g_thief = make_thief_window("glide2probe thief (same thread)");
                step_in("ShowWindow + SetFocus(thief) - the WM_KILLFOCUS re-open runs inside it");
                ShowWindow(g_thief, SW_SHOW);
                SetFocus(g_thief);
                step_out();
            } else if (!strcmp(O.killfocus, "thread")) {
                say("focus thief on another thread (as another application would)");
                CloseHandle(CreateThread(NULL, 0, thief_thread, NULL, 0, NULL));
            } else {
                say("waiting up to %d s for a focus loss by itself (none is caused)", O.hold_s);
            }
            /* keep rendering and pumping, as the game does, until the re-open
             * ran or the hold ends */
            until = GetTickCount() + (DWORD)O.hold_s * 1000u + 2000u;
            step_in("render + pump, waiting for WM_KILLFOCUS");
            while (!g_reopen_done && (LONG)(GetTickCount() - until) < 0) {
                if (g_ctx && g_clr && g_swp) {
                    g_clr(0x00004000u, 0, 0xffff);
                    g_swp(1);
                }
                pump();
                Sleep(10);
            }
            step_out();
            say("focus: WM_KILLFOCUS %s, WM_ACTIVATEAPP(FALSE) %s, re-open in it %s",
                g_killfocus_seen ? "SEEN" : "not seen", g_activateapp_off ? "SEEN" : "not seen",
                g_reopen_done ? "MADE" : "not made");
            g_want_reopen_in_killfocus = 0;
            if (!g_reopen_done && !strcmp(O.killfocus, "natural"))
                say("no focus loss within %d s: at these settings the open did NOT take the "
                    "window's focus - the re-open follows in sequence", O.hold_s);
        }
        /* the remaining opens in sequence: UE1's Exit then Init each time */
        while (g_next_open < O.nopen && !g_refused) {
            Sleep((DWORD)O.hold_s * 1000u);
            pump();
            do_close("render device Exit");
            ue1_requery();
            if (do_open(&O.open[g_next_open], "re-open in sequence"))
                frames(O.frames);
            g_next_open++;
        }
        pump();
    }
    do_close("end of run");
    if (!g_refused) {
        /* with a context still open (no grSstWinClose export) the shutdown
         * closes it: a switch, paced like one */
        int held = g_ctx;
        if (held && !vcr_pace_before_switch()) {
            say("PACE: grGlideShutdown NOT made - %s", g_vcr_pace_why ? g_vcr_pace_why : "?");
            g_refused = 1;
        } else {
            step_in("grGlideShutdown");
            g_shut();
            if (held)
                vcr_pace_after_restore();
            step_out();
            g_ctx = 0;
            say("grGlideShutdown returned");
        }
    }
    if (g_thief)
        DestroyWindow(g_thief);
    DestroyWindow(g_wnd);
    rc = g_hung ? 9 : g_refused ? 10 : g_opens_failed ? 3 : 0;
    say("RESULT {\"mode\":\"%s\",\"opens_ok\":%d,\"opens_failed\":%d,\"killfocus\":\"%s\","
        "\"killfocus_seen\":%s,\"activateapp_off\":%s,\"reopen_in_killfocus\":%s,"
        "\"hung\":%s%s%s%s,\"pace_refused\":%s}",
        O.ue1 ? "ue1" : O.nopen > 1 ? "reopen" : "open", g_opens_ok, g_opens_failed,
        O.killfocus ? O.killfocus : "", g_killfocus_seen ? "true" : "false",
        g_activateapp_off ? "true" : "false", g_reopen_done ? "true" : "false",
        g_hung ? "true" : "false", g_hung ? ",\"hung_step\":\"" : "",
        g_hung && g_hung_step ? g_hung_step : "", g_hung ? "\"" : "",
        g_refused ? "true" : "false");
    return rc;
}

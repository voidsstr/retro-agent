/*
 * uiguard.h - the synthetic keystrokes UIKEY refuses (agent 1.91.0).
 * Win32-free: tests/native/test_uiguard.c compiles it as is.
 *
 * WHY (2026-09-28, .124): a sweep closing Aliens vs Predator sent WM_CLOSE,
 * then ALT+F4, then the id-engine console quit (TILDE, "quit", RETURN). The
 * game process was alive but its window was NOT in the foreground, so ALT+F4
 * went to the DESKTOP - which on Windows opens "Shut Down Windows" - and the
 * RETURN confirmed it. The agent logged console control event 5 (logoff) one
 * second later and the box restarted. With "Shut down" preselected it would
 * have powered off, and a fleet box that is off needs a person. Nothing that
 * sends keys can see where they land (keybd_event goes to whatever window has
 * the focus), so the check belongs where the keys are sent:
 *
 *   - ALT+F4 while the foreground is the shell itself (the desktop or the
 *     taskbar) is REFUSED: there it can only mean "shut Windows down" - and
 *     so is one with NO foreground window, whose destination nobody knows;
 *   - while the foreground is the shut-down / log-off dialog, every key is
 *     REFUSED except ESCAPE, which cancels it - the one key a tool may need to
 *     get out of it.
 *
 * Deliberately NOT refused: ALT+F4 to any other window (closing a game or a
 * dialog is what it is for), keys to the desktop that are not ALT+F4.
 */
#ifndef RETRO_UIGUARD_H
#define RETRO_UIGUARD_H

#define UIG_OK                  0
#define UIG_REFUSE_ALTF4_SHELL  1   /* ALT+F4 to the desktop/taskbar */
#define UIG_REFUSE_SHUTDOWN_DLG 2   /* a key into Shut Down / Log Off */

static __inline int uig__ieq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* The shell's own surfaces: the desktop (Progman; WorkerW behind a web
 * desktop) and the taskbar. ALT+F4 there opens "Shut Down Windows". */
static __inline int uig_is_shell_class(const char *cls)
{
    return uig__ieq(cls, "Progman") || uig__ieq(cls, "WorkerW") ||
           uig__ieq(cls, "Shell_TrayWnd");
}

/* XP's classic "Shut Down Windows" and "Log Off Windows" dialogs are ordinary
 * #32770 dialogs; the Welcome-screen style says "Turn off computer" / "Log Off
 * Windows". Matched by title, because the class is every dialog's class. */
static __inline int uig_is_shutdown_dialog(const char *cls, const char *title)
{
    if (!uig__ieq(cls, "#32770"))
        return 0;
    return uig__ieq(title, "Shut Down Windows") ||
           uig__ieq(title, "Turn off computer") ||
           uig__ieq(title, "Log Off Windows") ||
           uig__ieq(title, "Shut Down") || uig__ieq(title, "Log Off");
}

/* Is this UIKEY spec an ALT+F4? "ALT+F4", "alt+f4", "SHIFT+ALT+F4" - any combo
 * whose key is F4 with ALT among its modifiers. */
static __inline int uig_is_alt_f4(const char *spec)
{
    const char *p = spec, *part = spec;
    int alt = 0;

    if (!spec)
        return 0;
    for (;;) {
        if (*p == '+' || *p == 0) {
            int n = (int)(p - part);
            if (*p == 0)
                return alt && n == 2 && (part[0] == 'F' || part[0] == 'f') &&
                       part[1] == '4';
            if (n == 3 && (part[0] == 'A' || part[0] == 'a') &&
                (part[1] == 'L' || part[1] == 'l') &&
                (part[2] == 'T' || part[2] == 't'))
                alt = 1;
            part = p + 1;
        }
        p++;
    }
}

/* spec = the whole UIKEY argument (TEXT:... included); fg_class/fg_title =
 * the foreground window's class and title ("" when there is none). */
static __inline int uig_check(const char *spec, const char *fg_class, const char *fg_title)
{
    if (uig_is_shutdown_dialog(fg_class, fg_title) && !uig__ieq(spec, "ESCAPE") &&
        !uig__ieq(spec, "ESC"))
        return UIG_REFUSE_SHUTDOWN_DLG;
    /* no foreground window at all: nobody can say where it lands - refuse */
    if (uig_is_alt_f4(spec) && (!fg_class || !fg_class[0] || uig_is_shell_class(fg_class)))
        return UIG_REFUSE_ALTF4_SHELL;
    return UIG_OK;
}

static __inline const char *uig_reason(int r)
{
    switch (r) {
    case UIG_REFUSE_ALTF4_SHELL:
        return "refused: ALT+F4 with the desktop/taskbar (or no window) in the "
               "foreground opens Shut Down Windows - the target window is not focused";
    case UIG_REFUSE_SHUTDOWN_DLG:
        return "refused: the Shut Down/Log Off dialog is in the foreground - only "
               "ESCAPE (cancel) is sent to it";
    }
    return "ok";
}

#endif /* RETRO_UIGUARD_H */

/*
 * deskview.h - where the desktop's view state lives, whether it is the icon
 * view, and which shell command sets what on which Windows.
 *
 * Win32-free, so tests/native/test_desktop_bag_view.c compiles the code the
 * agent runs (agent/src/gamesync.c: gs_desktop_bag(), gs_desktop_view_fix(),
 * gs_bag_autoarrange(), gs_autoarrange_cmd()).
 *
 * FOUND 2026-09-28 on ADMIN-PC (192.168.1.195, Windows 7 6.1.7600): 8 of 117
 * desktop icons visible, in one row, while ICONARRANGE reported
 * "autoarrange":true,"icons":119 the whole time. The desktop was in LIST view:
 *
 *   HKCU\Software\Microsoft\Windows\Shell\BagMRU            NodeSlot = 4
 *   HKCU\Software\Microsoft\Windows\Shell\Bags\4\Desktop    Mode = 3 (FVM_LIST)
 *                                                           LogicalViewMode = 4 (FLVM_LIST)
 *                                                           IconSize = 16
 *
 * TWO DEFECTS, and the second one CAUSED the list view:
 *
 *  1. The agent wrote the desktop's persisted flags to a hardcoded
 *     Shell\Bags\1\Desktop. The desktop's bag is whichever slot the BagMRU
 *     ROOT's NodeSlot names - 1 on every XP box measured (.124, .110), 4 on
 *     .195 - so on .195 FFlags went into a key nothing reads, and ICONARRANGE
 *     reported that key's 0x221 as the desktop's state.
 *
 *  2. gs_autoarrange_cmd() posted WM_COMMAND 0x7051 on every NT box. 0x7051 is
 *     "&Auto Arrange" in XP SP3's shell32 - and "&List" in Windows 7's
 *     (shell32.dll.mui menu 215, read off .195). So on Win7, every boot on
 *     which LVS_AUTOARRANGE was clear, the agent put the desktop into List
 *     view, logged "shell toggle did not take", set the style bit by hand and
 *     reported success. Windows 7's Auto arrange is 0x7071 - which on XP is
 *     "&Help and Support Center". One constant is wrong somewhere by
 *     construction, exactly as 0x7031 was (see gamesync.c).
 *
 * Measured menu IDs (DefView "View" menu, resource 215/216):
 *
 *                          Win98 SE 4.10   XP SP3 5.1      Win7 6.1
 *   Auto arrange           0x7041          0x7051          0x7071
 *   Icons / Medium icons   (not read)      0x7029 Icons    0x704E Medium icons
 *   List                   -               0x702B          0x7051   <- the bug
 *   Help                   0x7051 Topics   0x7071 H&SC     -
 *
 * A Windows version with no measured row gets 0 = "post nothing", and the
 * caller falls back to setting the listview style directly. Guessing a command
 * ID is how this file came to exist.
 */
#ifndef RETRO_DESKVIEW_H
#define RETRO_DESKVIEW_H

#ifdef __GNUC__
#define DV_UNUSED __attribute__((unused))
#else
#define DV_UNUSED
#endif

#include <stddef.h>

/* The desktop is the namespace ROOT, so its bag slot is the NodeSlot value on
 * the BagMRU key itself (not on a numbered child). XP keeps local folders
 * under ShellNoRoam, but the desktop's own bag is under Shell there too:
 * measured on .124 and .110, Shell\BagMRU NodeSlot = 1 and ShellNoRoam\BagMRU
 * carries no NodeSlot at all, and Shell\Bags\1\Desktop holds the live Mode=1 /
 * FFlags=0x225 / ScrollPos* values. Win7 has no ShellNoRoam. */
#define DV_BAGMRU_KEY    "Software\\Microsoft\\Windows\\Shell\\BagMRU"
#define DV_BAGMRU_VALUE  "NodeSlot"
#define DV_BAGS_PREFIX   "Software\\Microsoft\\Windows\\Shell\\Bags\\"
#define DV_BAGS_SUFFIX   "\\Desktop"
#define DV_SLOT_DEFAULT  1u          /* no NodeSlot (Win98, fresh profile) */
#define DV_SLOT_MAX      1000000u    /* BagMRU Size is 400 on XP, 5000 on 7 */

/* FOLDERFLAGS - the shell's own values. */
#define DV_FWF_AUTOARRANGE    0x00000001ul
#define DV_FWF_SNAPTOGRID     0x00000004ul
#define DV_FFLAGS_DESKTOP_DEF 0x00000220ul   /* FWF_DESKTOP | FWF_NOCLIENTEDGE */

/* FOLDERVIEWMODE (bag "Mode") and FOLDERLOGICALVIEWMODE ("LogicalViewMode"). */
#define DV_FVM_ICON           1ul
#define DV_FVM_SMALLICON      2ul
#define DV_FVM_LIST           3ul
#define DV_FVM_DETAILS        4ul
#define DV_FLVM_DETAILS       1ul
#define DV_FLVM_TILES         2ul
#define DV_FLVM_ICONS         3ul
#define DV_FLVM_LIST          4ul
#define DV_FLVM_CONTENT       5ul
/* IconSize: Win7's desktop offers 32 (small), 48 (medium, the default) and 96
 * (large). List view writes 16. */
#define DV_ICONSIZE_MEDIUM    48ul
#define DV_ICONSIZE_MIN       32ul
#define DV_ICONSIZE_MAX       256ul

/* Listview: the classic style type bits, and comctl32 6's LVM_GETVIEW. */
#define DV_LVS_TYPEMASK       0x0003ul
#define DV_LVS_ICON           0x0000ul
#define DV_LVS_REPORT         0x0001ul
#define DV_LVS_SMALLICON      0x0002ul
#define DV_LVS_LIST           0x0003ul
#define DV_LV_VIEW_ICON       0
#define DV_LV_VIEW_DETAILS    1
#define DV_LV_VIEW_SMALLICON  2
#define DV_LV_VIEW_LIST       3
#define DV_LV_VIEW_TILE       4

/* Measured shell32 menu command IDs - the table above. */
#define DV_CMD_AUTOARRANGE_WIN98  0x7041u
#define DV_CMD_AUTOARRANGE_XP     0x7051u
#define DV_CMD_AUTOARRANGE_WIN7   0x7071u
#define DV_CMD_ICONS_XP           0x7029u
#define DV_CMD_MEDIUMICONS_WIN7   0x704Eu
#define DV_CMD_LIST_WIN7          0x7051u   /* what the agent posted, 1.84.1-1.85.x */

/* ---- which bag ---------------------------------------------------------- */

/* found = the NodeSlot value was read as a REG_DWORD. A zero or absurd value
 * is not a slot; fall back rather than write Bags\0 or Bags\4294967295. */
DV_UNUSED static unsigned long dv_bag_slot(int found, unsigned long nodeslot)
{
    if (found && nodeslot >= 1 && nodeslot <= DV_SLOT_MAX)
        return nodeslot;
    return DV_SLOT_DEFAULT;
}

/* "Software\...\Shell\Bags\<slot>\Desktop". Returns the length, or 0 if it
 * does not fit (out is then an empty string). */
DV_UNUSED static size_t dv_bag_path(char *out, size_t cap, unsigned long slot)
{
    char digits[12];
    size_t nd = 0, n = 0, i;
    const char *p;

    if (!out || !cap)
        return 0;
    do {
        digits[nd++] = (char)('0' + (slot % 10));
        slot /= 10;
    } while (slot && nd < sizeof(digits));
    for (p = DV_BAGS_PREFIX; *p; p++) {
        if (n + 1 >= cap) goto overflow;
        out[n++] = *p;
    }
    for (i = nd; i > 0; i--) {
        if (n + 1 >= cap) goto overflow;
        out[n++] = digits[i - 1];
    }
    for (p = DV_BAGS_SUFFIX; *p; p++) {
        if (n + 1 >= cap) goto overflow;
        out[n++] = *p;
    }
    out[n] = '\0';
    return n;
overflow:
    out[0] = '\0';
    return 0;
}

/* ---- FFlags ------------------------------------------------------------- */

/* Read-modify-write: only bit 0 may move. FFlags is NOT uniform (.143 0x220,
 * .171 0x224, .195 0x40200224), so a stamped constant changes other settings. */
DV_UNUSED static unsigned long dv_fflags_autoarrange(int have, unsigned long flags, int on)
{
    if (!have)
        flags = DV_FFLAGS_DESKTOP_DEF;
    return on ? (flags | DV_FWF_AUTOARRANGE) : (flags & ~DV_FWF_AUTOARRANGE);
}

/* ---- which command, on which Windows ------------------------------------ */

/* is_9x: GetVersion() high bit. 0 = no measured command: post nothing. */
DV_UNUSED static unsigned dv_autoarrange_cmd(int is_9x, unsigned major, unsigned minor)
{
    if (is_9x)
        return (major == 4 && minor == 10) ? DV_CMD_AUTOARRANGE_WIN98 : 0;
    if (major == 5 && minor == 1)
        return DV_CMD_AUTOARRANGE_XP;
    if (major == 6 && minor == 1)
        return DV_CMD_AUTOARRANGE_WIN7;
    return 0;
}

/* The shell's own "icon view" command - a radio item, i.e. a SET. */
DV_UNUSED static unsigned dv_iconview_cmd(int is_9x, unsigned major, unsigned minor)
{
    if (is_9x)
        return 0;
    if (major == 5 && minor == 1)
        return DV_CMD_ICONS_XP;
    if (major == 6 && minor == 1)
        return DV_CMD_MEDIUMICONS_WIN7;
    return 0;
}

/* comctl32 6 (LVM_GETVIEW / LVM_SETVIEW) - XP and later. */
DV_UNUSED static int dv_has_comctl6(int is_9x, unsigned major, unsigned minor)
{
    return !is_9x && (major > 5 || (major == 5 && minor >= 1));
}

/* ---- is the LIVE desktop in icon view? ---------------------------------- */

/* style = GWL_STYLE of the desktop's SysListView32; lvview = LVM_GETVIEW, or
 * a negative number when it was not asked (comctl32 < 6) or did not answer.
 * Measured on .195 in medium icons: style 0x56003B40, LVM_GETVIEW 0. */
DV_UNUSED static int dv_live_is_icon_view(unsigned long style, long lvview)
{
    if ((style & DV_LVS_TYPEMASK) != DV_LVS_ICON)
        return 0;
    if (lvview >= 0 && lvview != DV_LV_VIEW_ICON)
        return 0;
    return 1;
}

/* A name for the report. A non-icon LVM_GETVIEW answer names the view (Tiles
 * has no style bits); otherwise the style bits do. Consistent with
 * dv_live_is_icon_view(): "icon" exactly when that returns 1. */
DV_UNUSED static const char *dv_view_name(unsigned long style, long lvview)
{
    static const char *const by_style[4] = { "icon", "details", "smallicon", "list" };

    switch (lvview) {
    case DV_LV_VIEW_DETAILS:   return "details";
    case DV_LV_VIEW_SMALLICON: return "smallicon";
    case DV_LV_VIEW_LIST:      return "list";
    case DV_LV_VIEW_TILE:      return "tile";
    default:
        if (lvview > DV_LV_VIEW_TILE)
            return "unknown";
        return by_style[style & DV_LVS_TYPEMASK];
    }
}

/* ---- is the PERSISTED desktop view the icon view? ----------------------- */

typedef struct {
    int has_mode;  unsigned long mode;       /* "Mode"            */
    int has_lvm;   unsigned long lvm;        /* "LogicalViewMode" */
    int has_size;  unsigned long icon_size;  /* "IconSize"        */
} dv_bagview_t;

/* Absent values are not a fault: a bag with no Mode is the default, icons. */
DV_UNUSED static int dv_bag_view_is_bad(const dv_bagview_t *v)
{
    if (v->has_mode && v->mode != DV_FVM_ICON)
        return 1;
    if (v->has_lvm && v->lvm != DV_FLVM_ICONS)
        return 1;
    return 0;
}

/* The repaired values, or 0 (out = in) when the bag is already icon view - a
 * settled box writes nothing. LogicalViewMode and IconSize are Vista+ values:
 * written only there, so an XP bag never grows keys its shell does not read.
 * A legitimate desktop icon size is kept; anything smaller (List's 16) becomes
 * medium, which is what the person who fixed .195 set. */
DV_UNUSED static int dv_bag_view_repair(const dv_bagview_t *in, int vista_plus,
                                        dv_bagview_t *out)
{
    *out = *in;
    if (!dv_bag_view_is_bad(in))
        return 0;
    out->has_mode = 1;
    out->mode = DV_FVM_ICON;
    if (vista_plus || in->has_lvm) {
        out->has_lvm = 1;
        out->lvm = DV_FLVM_ICONS;
    }
    if (vista_plus || in->has_size) {
        if (!(in->has_size && in->icon_size >= DV_ICONSIZE_MIN &&
              in->icon_size <= DV_ICONSIZE_MAX)) {
            out->has_size = 1;
            out->icon_size = DV_ICONSIZE_MEDIUM;
        }
    }
    return 1;
}

#endif /* RETRO_DESKVIEW_H */

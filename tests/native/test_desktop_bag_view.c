/* test_desktop_bag_view.c - the desktop's shell bag is the slot BagMRU names,
 * the desktop must be in ICON view, and the shell command ids are per Windows
 * VERSION, not per platform family.
 *
 * FOUND 2026-09-28 on ADMIN-PC (192.168.1.195, Windows 7 6.1.7600): 8 of 117
 * desktop icons visible, one row, while ICONARRANGE answered
 * "autoarrange":true,"icons":119,"fflags":545. Measured on the box:
 *
 *   HKCU\...\Shell\BagMRU            NodeSlot = 4
 *   HKCU\...\Shell\Bags\4\Desktop    Mode = 3, LogicalViewMode = 4, IconSize = 16
 *                                    FFlags = 0x40200224  (auto-arrange OFF)
 *   HKCU\...\Shell\Bags\1\Desktop    FFlags = 0x221 only - the agent's own write
 *   desktop listview, medium icons:  style 0x56003B40, LVM_GETVIEW 0
 *   Win7 shell32.dll.mui menu 215:   0x7051 "&List", 0x7071 "&Auto arrange",
 *                                    0x704E "&Medium icons"
 *   XP SP3 shell32.dll (.110):       0x7051 "&Auto Arrange", 0x7029 "Ico&ns",
 *                                    0x702B "&List", 0x7071 "Help and Support"
 *   XP .124 / .110:                  Shell\BagMRU NodeSlot = 1, Bags\1\Desktop
 *                                    Mode = 1, FFlags = 0x225
 *
 * The person who fixed .195 by hand set Mode=1 LogicalViewMode=3 IconSize=48
 * and FFlags 0x40200224 -> 0x40200225 in Bags\4. Those are the values asserted.
 *
 * Every group asserts the FIXED behaviour and what the OLD code did.
 *
 * Mirrors: agent/shared/deskview.h, used by agent/src/gamesync.c
 *          gs_desktop_bag(), gs_bag_autoarrange(), gs_desktop_view_fix(),
 *          gs_autoarrange_cmd(), handle_iconarrange().
 */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/deskview.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } \
                           else printf("  [ ok ] %s\n", msg); } while (0)

/* ---- the old code, verbatim in effect --------------------------------- */
#define OLD_GS_DESKTOP_BAG "Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop"
static unsigned long old_bag_slot(int found, unsigned long nodeslot)
{
    (void)found; (void)nodeslot;
    return 1;                                   /* hardcoded Bags\1 */
}
static unsigned old_autoarrange_cmd(int is_9x, unsigned major, unsigned minor)
{
    (void)major; (void)minor;
    return is_9x ? 0x7041u : 0x7051u;           /* 1.84.1 - 1.89.x */
}
static int old_live_is_icon_view(unsigned long style, long lvview)
{
    (void)style; (void)lvview;
    return 1;                                   /* never looked */
}

static int bv_eq(const dv_bagview_t *a, const dv_bagview_t *b)
{
    return a->has_mode == b->has_mode && (!a->has_mode || a->mode == b->mode) &&
           a->has_lvm == b->has_lvm && (!a->has_lvm || a->lvm == b->lvm) &&
           a->has_size == b->has_size && (!a->has_size || a->icon_size == b->icon_size);
}

int main(void)
{
    char buf[96];
    dv_bagview_t in, out;

    printf("== which bag ==\n");
    CHECK(dv_bag_slot(1, 4) == 4, ".195 (Win7): BagMRU NodeSlot 4 -> Bags\\4");
    CHECK(old_bag_slot(1, 4) == 1 && old_bag_slot(1, 4) != dv_bag_slot(1, 4),
          "(old: Bags\\1 on .195 - a key nothing reads)");
    CHECK(dv_bag_slot(1, 1) == 1 && old_bag_slot(1, 1) == 1,
          "XP .124/.110: NodeSlot 1 -> Bags\\1, the same key as before (no XP regression)");
    CHECK(dv_bag_slot(0, 0) == DV_SLOT_DEFAULT && DV_SLOT_DEFAULT == 1,
          "Win98 .243 has no BagMRU -> fall back to slot 1, as every earlier agent did");
    CHECK(dv_bag_slot(1, 0) == 1, "NodeSlot 0 is not a slot -> fallback");
    CHECK(dv_bag_slot(1, 0xFFFFFFFFul) == 1, "an absurd NodeSlot -> fallback, never Bags\\4294967295");
    CHECK(dv_bag_slot(1, DV_SLOT_MAX) == DV_SLOT_MAX && dv_bag_slot(1, DV_SLOT_MAX + 1) == 1,
          "the bound is inclusive");
    CHECK(dv_bag_slot(0, 7) == 1, "a value that was not read as a REG_DWORD is ignored");

    CHECK(dv_bag_path(buf, sizeof(buf), 4) > 0 &&
          strcmp(buf, "Software\\Microsoft\\Windows\\Shell\\Bags\\4\\Desktop") == 0,
          "slot 4 -> Software\\Microsoft\\Windows\\Shell\\Bags\\4\\Desktop");
    CHECK(dv_bag_path(buf, sizeof(buf), 1) > 0 && strcmp(buf, OLD_GS_DESKTOP_BAG) == 0,
          "slot 1 -> byte-identical to the old hardcoded path");
    CHECK(dv_bag_path(buf, sizeof(buf), 1000000) > 0 &&
          strcmp(buf, "Software\\Microsoft\\Windows\\Shell\\Bags\\1000000\\Desktop") == 0,
          "multi-digit slots format in order");
    CHECK(strncmp(DV_BAGMRU_KEY, "Software\\Microsoft\\Windows\\Shell\\BagMRU", 64) == 0 &&
          strcmp(DV_BAGMRU_VALUE, "NodeSlot") == 0,
          "the slot comes from Shell\\BagMRU (not ShellNoRoam: XP's root has no NodeSlot there)");
    {
        char tiny[20];
        size_t n = dv_bag_path(tiny, sizeof(tiny), 4);
        CHECK(n == 0 && tiny[0] == '\0', "a buffer too small gives an empty string, not a truncated path");
    }

    printf("== FFlags: only bit 0 moves ==\n");
    CHECK(dv_fflags_autoarrange(1, 0x40200224ul, 1) == 0x40200225ul,
          ".195 Bags\\4: 0x40200224 -> 0x40200225 (what the person set by hand)");
    CHECK(dv_fflags_autoarrange(1, 0x220ul, 1) == 0x221ul, ".143: 0x220 -> 0x221");
    CHECK(dv_fflags_autoarrange(1, 0x224ul, 1) == 0x225ul, ".171: 0x224 -> 0x225 (snap-to-grid kept)");
    CHECK(dv_fflags_autoarrange(1, 0x225ul, 1) == 0x225ul, "XP .124/.110 at 0x225: settled, unchanged");
    CHECK(dv_fflags_autoarrange(0, 0, 1) == 0x221ul, "absent -> the shell default 0x220 plus bit 0");
    CHECK(dv_fflags_autoarrange(1, 0x40200225ul, 0) == 0x40200224ul, "bay mode clears only bit 0");
    CHECK(0x221ul != dv_fflags_autoarrange(1, 0x40200224ul, 1),
          "(a stamped 0x221 would have wiped .195's 0x40200004 bits)");

    printf("== the Auto Arrange command, per Windows version ==\n");
    CHECK(dv_autoarrange_cmd(0, 6, 1) == 0x7071u, "Win7 6.1: Auto arrange is 0x7071");
    CHECK(old_autoarrange_cmd(0, 6, 1) == DV_CMD_LIST_WIN7 && DV_CMD_LIST_WIN7 == 0x7051u,
          "(old: 0x7051 on Win7 - which is the LIST view command: the agent caused .195's List view)");
    CHECK(dv_autoarrange_cmd(0, 5, 1) == 0x7051u && old_autoarrange_cmd(0, 5, 1) == 0x7051u,
          "XP 5.1: still 0x7051 (unchanged)");
    CHECK(dv_autoarrange_cmd(0, 5, 1) != DV_CMD_AUTOARRANGE_WIN7,
          "and never Win7's 0x7071 on XP - that is Help and Support Center there");
    CHECK(dv_autoarrange_cmd(1, 4, 10) == 0x7041u && old_autoarrange_cmd(1, 4, 10) == 0x7041u,
          "Win98 4.10: still 0x7041 (unchanged)");
    CHECK(dv_autoarrange_cmd(0, 6, 0) == 0 && dv_autoarrange_cmd(0, 5, 0) == 0 &&
          dv_autoarrange_cmd(0, 5, 2) == 0 && dv_autoarrange_cmd(0, 6, 2) == 0,
          "Vista, 2000, 2003, 8+: no measured id -> 0 = post nothing, set the style bit");
    CHECK(dv_autoarrange_cmd(1, 4, 90) == 0 && dv_autoarrange_cmd(1, 4, 0) == 0,
          "ME and 95: not measured -> 0");
    CHECK(dv_iconview_cmd(0, 6, 1) == 0x704Eu && dv_iconview_cmd(0, 6, 1) != DV_CMD_LIST_WIN7,
          "Win7 icon view = 0x704E Medium icons");
    CHECK(dv_iconview_cmd(0, 5, 1) == 0x7029u, "XP icon view = 0x7029 Icons");
    CHECK(dv_iconview_cmd(1, 4, 10) == 0 && dv_iconview_cmd(0, 6, 0) == 0,
          "9x and unmeasured versions: no icon-view command");
    CHECK(dv_has_comctl6(0, 5, 1) && dv_has_comctl6(0, 6, 1) &&
          !dv_has_comctl6(0, 5, 0) && !dv_has_comctl6(1, 4, 10),
          "LVM_GETVIEW/LVM_SETVIEW only where comctl32 6 exists (XP+)");

    printf("== the LIVE view ==\n");
    CHECK(dv_live_is_icon_view(0x56003B40ul, 0),
          ".195 medium icons (style 0x56003B40, LVM_GETVIEW 0): icon view -> left alone");
    CHECK(!dv_live_is_icon_view((0x56003B40ul & ~3ul) | 3ul, 3),
          "List (style type 3, LVM_GETVIEW 3): NOT icon view -> repair");
    CHECK(old_live_is_icon_view((0x56003B40ul & ~3ul) | 3ul, 3),
          "(old: never looked, so List view passed as fine)");
    CHECK(!dv_live_is_icon_view(0x56003B40ul, DV_LV_VIEW_LIST),
          "comctl6 says List even though the style bits say icon -> repair");
    CHECK(!dv_live_is_icon_view(0x56003B40ul, DV_LV_VIEW_TILE),
          "Tiles has no style bits of its own -> caught by LVM_GETVIEW");
    CHECK(!dv_live_is_icon_view(0x56003B41ul, 0), "Details style bits -> repair");
    CHECK(dv_live_is_icon_view(0x56003B40ul, -1),
          "Win98 (no LVM_GETVIEW asked) with icon style bits -> fine");
    CHECK(!dv_live_is_icon_view(0x56003B43ul, -1), "Win98 with List style bits -> repair");
    CHECK(strcmp(dv_view_name(0x56003B40ul, 0), "icon") == 0 &&
          strcmp(dv_view_name(0x56003B43ul, 3), "list") == 0 &&
          strcmp(dv_view_name(0x56003B40ul, 4), "tile") == 0 &&
          strcmp(dv_view_name(0x56003B41ul, -1), "details") == 0 &&
          strcmp(dv_view_name(0x56003B40ul, 9), "unknown") == 0,
          "view names for the report");

    printf("== the PERSISTED view ==\n");
    memset(&in, 0, sizeof(in));
    in.has_mode = 1; in.mode = 3; in.has_lvm = 1; in.lvm = 4; in.has_size = 1; in.icon_size = 16;
    CHECK(dv_bag_view_is_bad(&in), ".195 before: Mode 3 / LogicalViewMode 4 / IconSize 16 is List");
    CHECK(dv_bag_view_repair(&in, 1, &out) == 1 &&
          out.has_mode && out.mode == 1 && out.has_lvm && out.lvm == 3 &&
          out.has_size && out.icon_size == 48,
          "-> Mode 1 / LogicalViewMode 3 / IconSize 48, exactly the hand fix");

    in.mode = 1; in.lvm = 3; in.icon_size = 48;
    CHECK(!dv_bag_view_is_bad(&in) && dv_bag_view_repair(&in, 1, &out) == 0 && bv_eq(&in, &out),
          ".195 after: settled -> nothing written");

    memset(&in, 0, sizeof(in));
    in.has_mode = 1; in.mode = 1;
    CHECK(!dv_bag_view_is_bad(&in) && dv_bag_view_repair(&in, 0, &out) == 0 && bv_eq(&in, &out),
          "XP .124/.110 Bags\\1\\Desktop Mode 1 (no LogicalViewMode/IconSize): nothing written");

    memset(&in, 0, sizeof(in));
    CHECK(!dv_bag_view_is_bad(&in) && dv_bag_view_repair(&in, 1, &out) == 0,
          "a bag with FFlags only (.195's stale Bags\\1) is the default view - nothing written");

    memset(&in, 0, sizeof(in));
    in.has_mode = 1; in.mode = 3;
    CHECK(dv_bag_view_repair(&in, 0, &out) == 1 && out.mode == 1 &&
          !out.has_lvm && !out.has_size,
          "XP in List: Mode -> 1 only; no Vista-only values added to an XP bag");

    memset(&in, 0, sizeof(in));
    in.has_mode = 1; in.mode = 4; in.has_lvm = 1; in.lvm = 1; in.has_size = 1; in.icon_size = 96;
    CHECK(dv_bag_view_repair(&in, 1, &out) == 1 && out.mode == 1 && out.lvm == 3 &&
          out.icon_size == 96,
          "Win7 in Details with IconSize 96: icon view, the large size kept");

    memset(&in, 0, sizeof(in));
    in.has_mode = 1; in.mode = 1; in.has_lvm = 1; in.lvm = 3; in.has_size = 1; in.icon_size = 16;
    CHECK(!dv_bag_view_is_bad(&in),
          "icon view at 16 px is a choice, not a fault - left alone");

    memset(&in, 0, sizeof(in));
    in.has_mode = 1; in.mode = 2; in.has_lvm = 1; in.lvm = 3; in.has_size = 1; in.icon_size = 16;
    CHECK(dv_bag_view_repair(&in, 1, &out) == 1 && out.mode == 1 && out.icon_size == 48,
          "FVM_SMALLICON is not icon view -> medium icons");

    memset(&in, 0, sizeof(in));
    in.has_mode = 1; in.mode = 1; in.has_lvm = 1; in.lvm = 4;
    CHECK(dv_bag_view_is_bad(&in),
          "Mode 1 but LogicalViewMode List: Win7 obeys the logical mode -> bad");

    printf("%d/%d passed\n", runs - fails, runs);
    return fails ? 1 : 0;
}

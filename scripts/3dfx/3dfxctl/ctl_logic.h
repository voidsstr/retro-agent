/*
 * ctl_logic.h - the 3dfx Control Panel's settings table and every decision it
 * makes, as pure C: no Win32, no CRT beyond <string.h>/<stdlib.h>. 3dfxctl.c
 * is the Win32 glue around it; tests/native/test_3dfxctl_logic.c compiles THIS
 * file, and tests/python/test_3dfxctl_settings.py parses the table below and
 * checks every name against the source that reads it.
 *
 * THE RULE THE TABLE KEEPS: a control whose value the stack never reads is not
 * in the table. A setting that "applies" and changes nothing is this project's
 * signature failure (a tool reporting success and being believed), so every
 * row names a value that OUR stack reads, in the place it reads it, and says
 * when that read happens:
 *
 *   CTL_ST_GLIDE  REG_SZ under HKLM\<getRegPath()>, the key our h5 Glide's
 *                 hwcGetenv() reads (minihwc.c getRegPath: Services\3dfxvs\
 *                 Device0\glide when Services\3dfxvs\Device0 exists, else
 *                 Services\banshee\Device0\glide). hwcGetenv looks at the
 *                 process environment first, then HKCU, then HKLM - so a copy in
 *                 either of those OVERRIDES what the panel writes, and the panel
 *                 says so. Read when a Glide or OpenGL program starts (gpci.c
 *                 _GlideInitEnvironment) or opens the board (hwcInitVideo).
 *   CTL_ST_ENV    REG_SZ in HKCU\Environment + WM_SETTINGCHANGE("Environment"):
 *                 for names our MesaFX ICD reads with plain getenv() - it never
 *                 looks at the registry - and for the two it DEFAULTS into the
 *                 process environment itself (fxapi.c: FX_GLIDE_SWAPINTERVAL=0,
 *                 FX_GLIDE_SWAPPENDINGCOUNT=2 when unset), which would shadow a
 *                 Glide-registry value in every OpenGL game. Explorer re-reads
 *                 the environment on the broadcast, so programs started from the
 *                 desktop or Start menu after Apply see it; a program started by
 *                 something that was already running (the retro agent's LAUNCH
 *                 or EXEC) keeps that parent's old environment.
 *   CTL_ST_DIAG   REG_DWORD under HKLM\SYSTEM\CurrentControlSet\Services\vcrmp\
 *                 Diag - ONLY the four switches the kernel reads without a
 *                 reboot (ctl_diag_name_ok). "Off" deletes the VALUE, never the
 *                 key: that key also holds the driver's flushed phase history
 *                 and the safety kill switches.
 *   CTL_ST_DISPLAY the desktop's refresh rate, changed by the panel through
 *                 tools/vcr_pace.h and only to a rate the driver enumerates.
 *   CTL_ST_V_*    the VINTAGE 3dfxvs driver's keys (Device0 / D3D / glide),
 *                 carried over from the first 3dfxctl for boxes on that lane.
 */
#ifndef CTL_LOGIC_H
#define CTL_LOGIC_H

#include <string.h>
#include <stdlib.h>

/* every decision is a static function; a program need not use them all */
#if defined(__GNUC__)
#define CTL_FN static __attribute__((unused))
#else
#define CTL_FN static
#endif

/* ---- enums -------------------------------------------------------------------- */

#define CTL_LANE_VCR        0x1     /* OUR stack: vcr-kmd + h5 Glide + MesaFX ICD */
#define CTL_LANE_VINTAGE    0x2     /* the vintage 3dfxvs driver (retro-3dfx / AmigaMerlin) */

enum { CTL_TAB_OVERVIEW, CTL_TAB_3D, CTL_TAB_AA, CTL_TAB_GL, CTL_TAB_DISPLAY, CTL_TAB_ADV,
       CTL_NTABS };

enum {
    CTL_ST_GLIDE = 1,       /* HKLM\<glide key> REG_SZ */
    CTL_ST_ENV,             /* HKCU\Environment REG_SZ */
    CTL_ST_DIAG,            /* HKLM\...\vcrmp\Diag REG_DWORD */
    CTL_ST_DISPLAY,         /* the desktop mode's refresh rate */
    CTL_ST_V_DEV0,          /* vintage: ...\3dfxvs\Device0 */
    CTL_ST_V_D3D,           /* vintage: ...\3dfxvs\Device0\D3D */
    CTL_ST_V_GLIDE          /* vintage: ...\3dfxvs\Device0\glide */
};

enum {
    CTL_K_CHOICE = 1,       /* combo over choices[] */
    CTL_K_CHECK,            /* checkbox: choices[0] = off, choices[1] = on */
    CTL_K_FLOAT,            /* slider fmin..fmax; the stack default is written as "absent" */
    CTL_K_GLIDE_REFRESH,    /* combo: Auto + the refresh rates the driver enumerates */
    CTL_K_DESK_REFRESH,     /* combo: the rates the driver enumerates for the desktop mode */
    CTL_K_AA,               /* combo: the SLI/AA configurations (ctl_aa_*) */
    CTL_K_V_INT,            /* vintage: a number in an edit box */
    CTL_K_V_GAMMA           /* vintage: a gamma number expanded to a REG_BINARY LUT */
};

enum {
    CTL_WHEN_LAUNCH = 1,    /* read when a game starts / opens the board: the next game */
    CTL_WHEN_EXPLORER,      /* environment: games started from the desktop/Start menu after Apply */
    CTL_WHEN_NOW,           /* applied by the panel now (a paced display change) */
    CTL_WHEN_REBOOT         /* vintage lane: boot, mode set or game start (its driver's rule) */
};

#define CTL_F_R_GLIDE       0x001   /* our h5 Glide reads it (hwcGetenv: env, HKCU, HKLM) */
#define CTL_F_R_ICD         0x002   /* our MesaFX ICD reads it (getenv, or Glide's hwcGetenv) */
#define CTL_F_R_KERNEL      0x004   /* vcr-kmd reads it (VcrDiagGet) */
#define CTL_F_EXPERIMENTAL  0x008   /* not proven on silicon: said on the control */
#define CTL_F_DWORD         0x010   /* vintage: REG_DWORD instead of REG_SZ */
#define CTL_F_NEEDS_PLUGIN  0x020   /* only meaningful when 3dfxspl3.dll is installed */
#define CTL_F_TRIPLE        0x040   /* name is "A|B|C": one control writes all three */
#define CTL_F_DESKTOP_GAMMA 0x080   /* vintage GammaTable: clamped so it cannot wash out */

/* a choice's value: NULL = the value is ABSENT (deleted) = the stack's own default */
typedef struct ctl_choice {
    const char *label;
    const char *value;
} ctl_choice;

typedef struct ctl_row {
    int         id;
    unsigned    lanes;
    int         tab;
    int         store;
    const char *name;
    int         kind;
    int         when;
    unsigned    flags;
    const ctl_choice *choices;
    float       fmin, fmax, fdef;
    const char *group;
    const char *label;
    const char *help;
    const char *vdflt;      /* vintage choice rows: the value its driver assumes when absent */
} ctl_row;

enum {
    CTL_ID_VSYNC = 1, CTL_ID_QUEUE, CTL_ID_GLIDE_REFRESH, CTL_ID_LOD_DITHER,
    CTL_ID_ALPHA_DITHER, CTL_ID_OVERLAY, CTL_ID_GLIDE_GAMMA, CTL_ID_APP_GAMMA,
    CTL_ID_NO_PLUGIN, CTL_ID_GL_GAMMA, CTL_ID_GL_DITHER, CTL_ID_GL_LODBIAS, CTL_ID_AA,
    CTL_ID_DESK_REFRESH, CTL_ID_2D_TEXT, CTL_ID_2D_PAT, CTL_ID_2D_LINE,
    /* vintage lane */
    CTL_ID_V_CLOCK = 100, CTL_ID_V_VSYNC_GLIDE, CTL_ID_V_VSYNC_D3D, CTL_ID_V_REFRESH,
    CTL_ID_V_DESK_GAMMA, CTL_ID_V_GLIDE_GAMMA, CTL_ID_V_SLIAA, CTL_ID_V_BAND,
    CTL_ID_V_OVERLAY, CTL_ID_V_DITHER, CTL_ID_V_LODBIAS, CTL_ID_V_FIFO, CTL_ID_V_TILED,
    CTL_ID_V_LOG
};

/* ---- where things live ---------------------------------------------------------- */

/* minihwc.c getRegPath(), the NT 5.x branch - pinned to that source by
 * tests/python/test_3dfxctl_settings.py */
#define CTL_KEY_3DFXVS_DEV0  "SYSTEM\\CurrentControlSet\\Services\\3dfxvs\\Device0"
#define CTL_KEY_GLIDE_3DFXVS "SYSTEM\\CurrentControlSet\\Services\\3dfxvs\\Device0\\glide"
#define CTL_KEY_GLIDE_BANSHEE "SYSTEM\\CurrentControlSet\\Services\\banshee\\Device0\\glide"
#define CTL_KEY_DIAG         "SYSTEM\\CurrentControlSet\\Services\\vcrmp\\Diag"
#define CTL_KEY_USER_ENV     "Environment"
#define CTL_KEY_SYSTEM_ENV   "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"
#define CTL_KEY_V_DEV0       CTL_KEY_3DFXVS_DEV0
#define CTL_KEY_V_D3D        "SYSTEM\\CurrentControlSet\\Services\\3dfxvs\\Device0\\D3D"
#define CTL_KEY_V_GLIDE      CTL_KEY_GLIDE_3DFXVS

/* The key our Glide reads on XP, decided the way it decides it. */
CTL_FN const char *ctl_glide_regpath(int has_3dfxvs_device0)
{
    return has_3dfxvs_device0 ? CTL_KEY_GLIDE_3DFXVS : CTL_KEY_GLIDE_BANSHEE;
}

/* The ONLY Diag values the panel may write: read by the kernel at every
 * SLI_AA_REQUEST (SliAA, vcrmp_multi.c sli_aa_allowed) or at every
 * IOCTL_VCR_INFO, i.e. every new PDEV (the Accel2D* three, vcrmp.c). Boot-time
 * switches (D3D32, Reset3D, Accel2D, D3D, TexPortFlush, AllowPoke, Disable ...)
 * would need a reboot, and the diagnostics (SliPersistAll, SliAAVendorRecipe,
 * SliAAReadback, FlipDeadline, DebugPort, EdidFilter, Ddc, Mon*) are for
 * supervised runs - none of them is ever written. */
CTL_FN int ctl_diag_name_ok(const char *name)
{
    static const char *const ok[] = { "SliAA", "Accel2DText", "Accel2DPattern", "Accel2DLine" };
    unsigned i;
    if (!name)
        return 0;
    for (i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (strcmp(name, ok[i]) == 0)
            return 1;
    return 0;
}

/* ---- the choices ---------------------------------------------------------------- */

static const ctl_choice ctl_c_vsync[] = {
    { "Default - OpenGL off, Glide: the game decides", NULL },
    { "Off - never wait (fastest, may tear)", "0" },
    { "On - every retrace (no tearing)", "1" },
    { "On - every 2nd retrace (half rate)", "2" },
    { NULL, NULL } };
static const ctl_choice ctl_c_queue[] = {
    { "Default - OpenGL 2, Glide games 3", NULL },
    { "0 - wait for each frame (lowest input lag, slowest)", "0" },
    { "1 frame", "1" },
    { "2 frames", "2" },
    { "3 frames (smoothest)", "3" },
    { NULL, NULL } };
static const ctl_choice ctl_c_onoff[] = {
    { "Off (default)", NULL },
    { "On", "1" },
    { NULL, NULL } };
static const ctl_choice ctl_c_alpha[] = {
    { "Sharper - no dither subtraction (default)", NULL },
    { "Smoother - dither subtraction on", "3" },
    { NULL, NULL } };
static const ctl_choice ctl_c_overlay[] = {
    { "Optimal - 2x2 below 1024 wide, else 4x1 (default)", NULL },
    { "4x1 filter at every resolution", "2" },
    { "2x2 filter wherever the board allows (4x1 in SLI)", "3" },
    { "Off - no filter (the 16-bit dither shows)", "-1" },
    { NULL, NULL } };
static const ctl_choice ctl_c_appgamma[] = {
    { "Games may set their own gamma (default)", NULL },
    { "Lock gamma to the value above", "0" },
    { NULL, NULL } };
static const ctl_choice ctl_c_noplugin[] = {
    { "Play the 3dfx splash plugin (default)", NULL },
    { "Skip the splash plugin", "1" },
    { NULL, NULL } };
static const ctl_choice ctl_c_gldither[] = {
    { "4x4 - smoother gradients (default)", NULL },
    { "2x2 - Glide's own pattern", "0" },
    { NULL, NULL } };
static const ctl_choice ctl_c_lodbias[] = {
    { "Sharp - a -0.5 bias (default)", NULL },
    { "Sharper - -1.0 (may shimmer)", "-1.0" },
    { "Neutral - exactly what the game asks", "0" },
    { "Softer - +0.5", "0.5" },
    { NULL, NULL } };
static const ctl_choice ctl_c_2d[] = {
    { "Off - GDI draws it (default)", NULL },
    { "On - the 2D engine draws it", "1" },
    { NULL, NULL } };

/* vintage lane (carried over from the first 3dfxctl) */
static const ctl_choice ctl_c_v_clock[] = {
    { "Auto (BIOS / stock 166 MHz)", "0" }, { "143 MHz (conservative)", "143" },
    { "150 MHz", "150" }, { "166 MHz (stock)", "166" }, { "183 MHz (overclock)", "183" },
    { "200 MHz (overclock)", "200" }, { NULL, NULL } };
static const ctl_choice ctl_c_v_vsync_glide[] = {
    { "Software controlled (app decides)", "-1" }, { "Off (no vblank wait)", "0" },
    { "On - every vblank (recommended)", "1" }, { "On - every 2nd vblank (half)", "2" },
    { NULL, NULL } };
static const ctl_choice ctl_c_v_vsync_d3d[] = {
    { "On - every vblank (recommended)", "1" }, { "Off", "0" },
    { "On - every 2nd vblank", "2" }, { NULL, NULL } };
static const ctl_choice ctl_c_v_refresh[] = {
    { "Auto (driver picks highest safe)", NULL }, { "60 Hz", "60" }, { "70 Hz", "70" },
    { "72 Hz", "72" }, { "75 Hz", "75" }, { "85 Hz", "85" }, { NULL, NULL } };
static const ctl_choice ctl_c_v_sliaa[] = {
    { "Single chip (no SLI/AA)", "0" }, { "2-way SLI", "2" }, { "4-way SLI", "5" },
    { "4-way SLI + 2x AA (experimental)", "6" }, { "2-way SLI + 4x AA (experimental)", "7" },
    { "8x AA (experimental)", "8" }, { NULL, NULL } };
static const ctl_choice ctl_c_v_overlay[] = {
    { "Off", "0" }, { "2x2 below 1024 (default)", "1" }, { "4x1 always", "2" },
    { "2x2 always", "3" }, { NULL, NULL } };
static const ctl_choice ctl_c_v_dither[] = {
    { "Off", "0" }, { "Optimal (default)", "1" }, { "Sharper", "2" }, { "Smoother", "3" },
    { NULL, NULL } };
static const ctl_choice ctl_c_v_tiled[] = {
    { "On (tiled - default)", "1" }, { "Off (linear)", "0" }, { NULL, NULL } };
static const ctl_choice ctl_c_v_onoff[] = { { "On", "1" }, { "Off", "0" }, { NULL, NULL } };

/* ---- THE TABLE -------------------------------------------------------------------
 * One row per control. Row format (parsed by the tests - keep the first line of
 * each row on one line):
 *   { id, lanes, tab, store, "NAME", kind, when, flags, choices, fmin, fmax, fdef,
 *     "group", "label", "help" }
 */
static const ctl_row ctl_rows[] = {
    /* ===== OUR stack (vcr-kmd + h5 Glide + MesaFX ICD) =====
     * 3D & Glide */
    { CTL_ID_VSYNC, CTL_LANE_VCR, CTL_TAB_3D, CTL_ST_ENV, "FX_GLIDE_SWAPINTERVAL", CTL_K_CHOICE, CTL_WHEN_EXPLORER, CTL_F_R_GLIDE | CTL_F_R_ICD, ctl_c_vsync, 0, 0, 0,
      "Frame timing", "Vertical sync",
      "Wait for the monitor's vertical retrace before showing a frame. On removes tearing; "
      "Off is fastest. Default leaves OpenGL games unsynced (our OpenGL driver's own default) "
      "and lets a Glide game choose. Written to your environment because the OpenGL driver "
      "reads it nowhere else." },
    { CTL_ID_QUEUE, CTL_LANE_VCR, CTL_TAB_3D, CTL_ST_ENV, "FX_GLIDE_SWAPPENDINGCOUNT", CTL_K_CHOICE, CTL_WHEN_EXPLORER, CTL_F_R_GLIDE | CTL_F_R_ICD, ctl_c_queue, 0, 0, 0,
      "Frame timing", "Frames queued ahead",
      "How many finished frames may wait for the screen. Fewer = less input lag, more = "
      "smoother under load. Glide never queues more than 6." },
    { CTL_ID_GLIDE_REFRESH, CTL_LANE_VCR, CTL_TAB_3D, CTL_ST_GLIDE, "FX_GLIDE_REFRESH", CTL_K_GLIDE_REFRESH, CTL_WHEN_LAUNCH, CTL_F_R_GLIDE | CTL_F_R_ICD, NULL, 0, 0, 0,
      "Frame timing", "Fullscreen refresh rate",
      "Auto: OpenGL games get the highest rate your monitor offers at each resolution, a "
      "Glide game the rate it asks for. A fixed rate overrides both; only rates the driver "
      "lists for this monitor are offered, and at a resolution where the monitor lacks the "
      "rate Glide falls back to the driver's default rate." },
    { CTL_ID_LOD_DITHER, CTL_LANE_VCR, CTL_TAB_3D, CTL_ST_GLIDE, "FX_GLIDE_LOD_DITHER", CTL_K_CHECK, CTL_WHEN_LAUNCH, CTL_F_R_GLIDE, ctl_c_onoff, 0, 0, 0,
      "Image quality", "Mipmap dithering",
      "Dithers between texture detail levels, hiding the seam where a surface switches to "
      "a smaller mipmap. Applies to Glide and OpenGL games." },
    { CTL_ID_ALPHA_DITHER, CTL_LANE_VCR, CTL_TAB_3D, CTL_ST_GLIDE, "SSTH3_ALPHADITHERMODE", CTL_K_CHOICE, CTL_WHEN_LAUNCH, CTL_F_R_GLIDE, ctl_c_alpha, 0, 0, 0,
      "Image quality", "16-bit alpha blending",
      "Smoother turns on dither subtraction, which removes the dither pattern that blended "
      "surfaces (smoke, glass) pick up in 16-bit colour. No effect in 32-bit." },
    { CTL_ID_OVERLAY, CTL_LANE_VCR, CTL_TAB_3D, CTL_ST_GLIDE, "SSTH3_OVERLAYMODE", CTL_K_CHOICE, CTL_WHEN_LAUNCH, CTL_F_R_GLIDE, ctl_c_overlay, 0, 0, 0,
      "Image quality", "16-bit video filter",
      "The scan-out filter that smooths 16-bit dithering on screen. 32-bit output is never "
      "filtered. With all chips in SLI the board uses 4x1 unless the filter is Off." },
    { CTL_ID_GLIDE_GAMMA, CTL_LANE_VCR, CTL_TAB_3D, CTL_ST_GLIDE, "SSTH3_RGAMMA|SSTH3_GGAMMA|SSTH3_BGAMMA", CTL_K_FLOAT, CTL_WHEN_LAUNCH, CTL_F_R_GLIDE | CTL_F_TRIPLE, NULL, 0.50f, 3.00f, 1.00f,
      "Brightness", "Glide gamma",
      "The gamma Glide loads when a game opens the board (1.00 = unchanged, higher = "
      "brighter). A game that sets its own gamma replaces it, unless gamma is locked below." },
    { CTL_ID_APP_GAMMA, CTL_LANE_VCR, CTL_TAB_3D, CTL_ST_GLIDE, "FX_GLIDE_USE_APP_GAMMA", CTL_K_CHECK, CTL_WHEN_LAUNCH, CTL_F_R_GLIDE, ctl_c_appgamma, 0, 0, 0,
      "Brightness", "Lock gamma to the Glide gamma",
      "Ignore every gamma change a game makes - including our OpenGL driver's own ramp - so "
      "the Glide gamma above applies to every Glide and OpenGL game." },
    { CTL_ID_NO_PLUGIN, CTL_LANE_VCR, CTL_TAB_3D, CTL_ST_ENV, "FX_GLIDE_NO_PLUGIN", CTL_K_CHECK, CTL_WHEN_EXPLORER, CTL_F_R_GLIDE | CTL_F_NEEDS_PLUGIN, ctl_c_noplugin, 0, 0, 0,
      "Other", "Skip the 3dfx splash plugin",
      "Do not load 3dfxspl3.dll, the plugin that plays the 3dfx logo animation when a game "
      "opens the board. Only offered when the plugin is installed: without it there is no "
      "splash to skip." },
    /* Anti-aliasing & SLI */
    { CTL_ID_AA, CTL_LANE_VCR, CTL_TAB_AA, CTL_ST_GLIDE, "SSTH3_SLI_AA_CONFIGURATION", CTL_K_AA, CTL_WHEN_LAUNCH, CTL_F_R_GLIDE | CTL_F_R_ICD, NULL, 0, 0, 0,
      "Chips and anti-aliasing", "SLI / anti-aliasing",
      "How the VSA-100 chips share the work. SLI splits the screen between the chips; the "
      "anti-aliasing modes make the chips draw the same picture at offset sample positions. "
      "Anti-aliasing is EXPERIMENTAL on this driver stack." },
    /* OpenGL */
    { CTL_ID_GL_GAMMA, CTL_LANE_VCR, CTL_TAB_GL, CTL_ST_ENV, "FX_GAMMA", CTL_K_FLOAT, CTL_WHEN_EXPLORER, CTL_F_R_ICD, NULL, 0.50f, 2.50f, 1.30f,
      "OpenGL image", "OpenGL gamma",
      "The gamma ramp our OpenGL driver loads for every OpenGL game (1.30 by default, "
      "because 16-bit output looks dark without it; 1.00 = no correction). Replaced by the "
      "Glide gamma when gamma is locked on the 3D tab." },
    { CTL_ID_GL_DITHER, CTL_LANE_VCR, CTL_TAB_GL, CTL_ST_ENV, "FX_DITHER", CTL_K_CHOICE, CTL_WHEN_EXPLORER, CTL_F_R_ICD, ctl_c_gldither, 0, 0, 0,
      "OpenGL image", "OpenGL dithering",
      "The dither pattern for 16-bit colour. 4x4 bands less; 2x2 is what Glide uses when "
      "nobody asks." },
    { CTL_ID_GL_LODBIAS, CTL_LANE_VCR, CTL_TAB_GL, CTL_ST_ENV, "FX_LOD_BIAS", CTL_K_CHOICE, CTL_WHEN_EXPLORER, CTL_F_R_ICD, ctl_c_lodbias, 0, 0, 0,
      "OpenGL image", "Texture sharpness",
      "A mipmap LOD bias for OpenGL games that set none. Negative is sharper (the classic "
      "3dfx trick against 16-bit softness); too negative shimmers." },
    /* Display & 2D */
    { CTL_ID_DESK_REFRESH, CTL_LANE_VCR, CTL_TAB_DISPLAY, CTL_ST_DISPLAY, "", CTL_K_DESK_REFRESH, CTL_WHEN_NOW, 0, NULL, 0, 0, 0,
      "Desktop", "Desktop refresh rate",
      "The refresh rate of the Windows desktop at its current resolution. Only rates the "
      "driver lists for this monitor are offered. Applied at once, paced, and put back after "
      "15 seconds unless you keep it." },
    { CTL_ID_2D_TEXT, CTL_LANE_VCR, CTL_TAB_DISPLAY, CTL_ST_DIAG, "Accel2DText", CTL_K_CHECK, CTL_WHEN_NOW, CTL_F_R_KERNEL | CTL_F_EXPERIMENTAL, ctl_c_2d, 0, 0, 0,
      "2D acceleration (experimental)", "Text on the 2D engine",
      "Draws text with the 2D engine. Correct on the emulated test bed but slower there than "
      "the software path; not yet measured on this board." },
    { CTL_ID_2D_PAT, CTL_LANE_VCR, CTL_TAB_DISPLAY, CTL_ST_DIAG, "Accel2DPattern", CTL_K_CHECK, CTL_WHEN_NOW, CTL_F_R_KERNEL | CTL_F_EXPERIMENTAL, ctl_c_2d, 0, 0, 0,
      "2D acceleration (experimental)", "Pattern fills on the 2D engine",
      "Hatched brushes and the grey of a drag rectangle drawn by the 2D engine. Correct on "
      "the test bed; not yet run on this board." },
    { CTL_ID_2D_LINE, CTL_LANE_VCR, CTL_TAB_DISPLAY, CTL_ST_DIAG, "Accel2DLine", CTL_K_CHECK, CTL_WHEN_NOW, CTL_F_R_KERNEL | CTL_F_EXPERIMENTAL, ctl_c_2d, 0, 0, 0,
      "2D acceleration (experimental)", "Straight lines on the 2D engine",
      "Horizontal and vertical lines drawn by the 2D engine. Correct on the test bed; not "
      "yet run on this board." },

    /* ===== the VINTAGE 3dfxvs driver (first 3dfxctl, unchanged values) ===== */
    { CTL_ID_V_CLOCK, CTL_LANE_VINTAGE, CTL_TAB_DISPLAY, CTL_ST_V_DEV0, "GraphicsClocking", CTL_K_CHOICE, CTL_WHEN_REBOOT, CTL_F_DWORD, ctl_c_v_clock, 0, 0, 0,
      "Clock", "Core / graphics clock",
      "VSA-100 core PLL. Auto keeps the BIOS clock; above 166 MHz is overclocking.", "0" },
    { CTL_ID_V_VSYNC_GLIDE, CTL_LANE_VINTAGE, CTL_TAB_3D, CTL_ST_V_GLIDE, "FX_GLIDE_SWAPINTERVAL", CTL_K_CHOICE, CTL_WHEN_REBOOT, 0, ctl_c_v_vsync_glide, 0, 0, 0,
      "Frame timing", "Vertical sync (Glide / OpenGL)",
      "Wait for vblank on buffer swap.", "1" },
    { CTL_ID_V_VSYNC_D3D, CTL_LANE_VINTAGE, CTL_TAB_3D, CTL_ST_V_D3D, "SSTH3_SWAPINTERVAL", CTL_K_CHOICE, CTL_WHEN_REBOOT, 0, ctl_c_v_vsync_d3d, 0, 0, 0,
      "Frame timing", "Vertical sync (Direct3D)",
      "D3D flip waits for N vblanks.", "1" },
    { CTL_ID_V_REFRESH, CTL_LANE_VINTAGE, CTL_TAB_3D, CTL_ST_V_GLIDE, "FX_GLIDE_REFRESH", CTL_K_CHOICE, CTL_WHEN_REBOOT, 0, ctl_c_v_refresh, 0, 0, 0,
      "Frame timing", "Full-screen refresh override",
      "Auto removes the override so the driver picks the refresh per resolution." },
    { CTL_ID_V_DESK_GAMMA, CTL_LANE_VINTAGE, CTL_TAB_DISPLAY, CTL_ST_V_DEV0, "GammaTable", CTL_K_V_GAMMA, CTL_WHEN_REBOOT, CTL_F_DESKTOP_GAMMA, NULL, 0.30f, 3.00f, 1.00f,
      "Gamma", "Desktop gamma",
      "2D desktop brightness (1.00 = neutral), clamped so it cannot wash out the desktop." },
    { CTL_ID_V_GLIDE_GAMMA, CTL_LANE_VINTAGE, CTL_TAB_3D, CTL_ST_V_DEV0, "GlideGammaTable", CTL_K_V_GAMMA, CTL_WHEN_REBOOT, 0, NULL, 0.30f, 3.00f, 1.30f,
      "Brightness", "3D / Glide gamma",
      "In-game brightness for Glide/OpenGL (1.30 = 3dfx default)." },
    { CTL_ID_V_SLIAA, CTL_LANE_VINTAGE, CTL_TAB_AA, CTL_ST_V_D3D, "SSTH3_SLI_AA_CONFIGURATION", CTL_K_CHOICE, CTL_WHEN_REBOOT, 0, ctl_c_v_sliaa, 0, 0, 0,
      "Chips and anti-aliasing", "SLI / anti-aliasing",
      "How the chips cooperate. AA modes need enough video memory.", "2" },
    { CTL_ID_V_BAND, CTL_LANE_VINTAGE, CTL_TAB_AA, CTL_ST_V_D3D, "SSTH3_SLI_BAND_HEIGHT", CTL_K_V_INT, CTL_WHEN_REBOOT, 0, NULL, 0, 128, 0,
      "Chips and anti-aliasing", "SLI band height (scanlines)",
      "2..128; 0 = driver-computed." },
    { CTL_ID_V_OVERLAY, CTL_LANE_VINTAGE, CTL_TAB_3D, CTL_ST_V_D3D, "SSTH3_OVERLAYMODE", CTL_K_CHOICE, CTL_WHEN_REBOOT, 0, ctl_c_v_overlay, 0, 0, 0,
      "Image quality", "Video overlay filter",
      "Scan-out scaling/filter for the displayed image.", "1" },
    { CTL_ID_V_DITHER, CTL_LANE_VINTAGE, CTL_TAB_3D, CTL_ST_V_D3D, "SSTH3_ALPHADITHERMODE", CTL_K_CHOICE, CTL_WHEN_REBOOT, 0, ctl_c_v_dither, 0, 0, 0,
      "Image quality", "Alpha dither",
      "16 bpp alpha dithering quality.", "1" },
    { CTL_ID_V_LODBIAS, CTL_LANE_VINTAGE, CTL_TAB_3D, CTL_ST_V_D3D, "SSTH3_LOD_BIAS", CTL_K_V_INT, CTL_WHEN_REBOOT, 0, NULL, -32, 31, 0,
      "Image quality", "Texture LOD bias",
      "Sharper (negative) / blurrier (positive) mip selection. 0 = neutral." },
    { CTL_ID_V_FIFO, CTL_LANE_VINTAGE, CTL_TAB_ADV, CTL_ST_V_DEV0, "CmdfifoSize", CTL_K_V_INT, CTL_WHEN_REBOOT, CTL_F_DWORD, NULL, 0, 1048576, 0,
      "Performance", "Command FIFO size (bytes)",
      "0 = driver default. Rounded to 4 KB, clamped 64 KB..1 MB." },
    { CTL_ID_V_TILED, CTL_LANE_VINTAGE, CTL_TAB_ADV, CTL_ST_V_DEV0, "TiledMode", CTL_K_CHOICE, CTL_WHEN_REBOOT, CTL_F_DWORD, ctl_c_v_tiled, 0, 0, 0,
      "Performance", "Framebuffer tiling",
      "Tiled is faster; Off forces a linear framebuffer (compatibility).", "1" },
    { CTL_ID_V_LOG, CTL_LANE_VINTAGE, CTL_TAB_ADV, CTL_ST_V_DEV0, "Retro3dfxLog", CTL_K_CHOICE, CTL_WHEN_REBOOT, 0, ctl_c_v_onoff, 0, 0, 0,
      "Diagnostics", "Verbose registry-ring log",
      "Extra lifecycle logging to the RLog flight-recorder ring.", "0" },
    { 0, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, NULL, NULL, NULL }
};

CTL_FN const ctl_row *ctl_row_by_id(int id)
{
    const ctl_row *r;
    for (r = ctl_rows; r->name; r++)
        if (r->id == id)
            return r;
    return NULL;
}

/* ---- the SLI / anti-aliasing configurations -----------------------------------------
 * SSTH3_SLI_AA_CONFIGURATION as our Glide maps it (h5sliaa.h h5SliAaConfigEnv):
 *   0 single chip  1 single chip + 2x AA  2/5 SLI  3/6 2x AA  4/7 4x AA  8 8x AA
 * and as our ICD maps it on a 4-chip board (fxapi.c): 8 -> 8x, 7 -> 4x, 6 -> 2x,
 * anything else -> 4-way SLI without AA. So on four chips 3 and 4 make the ICD
 * render without AA while Glide forces AA onto its surface: only 6/7/8 mean the
 * same thing to both, and only those are offered there. */

/* 1 = this value turns anti-aliasing on (Glide: aaSample > 0) */
CTL_FN int ctl_aa_is_aa(long cfg)
{
    return cfg == 1 || cfg == 3 || cfg == 4 || cfg == 6 || cfg == 7 || cfg == 8;
}

/* the "all chips in SLI, no AA" value for a board of n chips */
CTL_FN long ctl_aa_sli_value(unsigned nchips)
{
    return nchips >= 4 ? 5 : nchips == 2 ? 2 : 0;
}

#define CTL_AA_VALIDATED    1       /* proven on this board on our stack */
#define CTL_AA_UNTESTED     2       /* no AA, but never run on this board size on our stack */
#define CTL_AA_EXPERIMENTAL 3       /* anti-aliasing: needs the explicit confirmation */
#define CTL_AA_NOT_OFFERED  4

typedef struct ctl_aa_info {
    long        cfg;
    int         status;             /* CTL_AA_* */
    const char *label;
    const char *evidence;
} ctl_aa_info;

/* What a configuration is on a board of n chips, and why. 0 = n unknown. */
CTL_FN void ctl_aa_describe(long cfg, unsigned nchips, ctl_aa_info *o)
{
    o->cfg = cfg;
    o->status = CTL_AA_NOT_OFFERED;
    o->label = "unknown value";
    o->evidence = "not a configuration Glide knows (Glide treats it as SLI without AA)";
    if (nchips >= 4) {
        switch (cfg) {
        case 0:
            o->status = CTL_AA_VALIDATED;
            o->label = "Single chip - 1 of 4 VSA-100s";
            o->evidence = "validated on .124: Quake II 146.8 fps on our whole stack (2026-09-26)";
            return;
        case 2:
        case 5:
            o->status = CTL_AA_VALIDATED;
            o->label = "SLI - all four chips (recommended)";
            o->evidence = "validated on .124: fill 1124.6 Mpix/s = AmigaMerlin's, Quake II "
                          "201 fps at 640x480 (2026-09-26)";
            return;
        case 6:
            o->status = CTL_AA_EXPERIMENTAL;
            o->label = "2x anti-aliasing (EXPERIMENTAL)";
            o->evidence = "runs (618.6 Mpix/s) but draws a ghost / double image - two "
                          "misaligned copies (supervised, 2026-09-27)";
            return;
        case 7:
            o->status = CTL_AA_EXPERIMENTAL;
            o->label = "4x anti-aliasing (EXPERIMENTAL)";
            o->evidence = "FROZE THE WHOLE PC inside Glide's open (2026-09-26, confounded by "
                          "an earlier session; not re-run since)";
            return;
        case 8:
            o->status = CTL_AA_EXPERIMENTAL;
            o->label = "8x anti-aliasing (EXPERIMENTAL)";
            o->evidence = "never run on this board";
            return;
        case 1:
            o->label = "single chip + 2x AA";
            o->evidence = "refused by our guarded Glide on a board of more than two chips; an "
                          "unguarded Glide froze .124 with it (2026-09-26)";
            return;
        case 3:
        case 4:
            o->label = cfg == 3 ? "2x AA (two-chip value)" : "4x AA (two-chip value)";
            o->evidence = "a two-chip value: on four chips our OpenGL driver renders without "
                          "AA while Glide forces AA - use 6/7 instead";
            return;
        }
        return;
    }
    if (nchips == 2) {
        switch (cfg) {
        case 0:
            o->status = CTL_AA_UNTESTED;
            o->label = "Single chip";
            o->evidence = "not yet run on a two-chip board on our stack";
            return;
        case 2:
        case 5:
            o->status = CTL_AA_UNTESTED;
            o->label = "SLI - both chips";
            o->evidence = "not yet run on a two-chip board on our stack";
            return;
        case 3:
            o->status = CTL_AA_EXPERIMENTAL;
            o->label = "2x anti-aliasing (EXPERIMENTAL)";
            o->evidence = "never run on a two-chip board on our stack";
            return;
        case 4:
            o->status = CTL_AA_EXPERIMENTAL;
            o->label = "4x anti-aliasing (EXPERIMENTAL)";
            o->evidence = "never run on a two-chip board on our stack";
            return;
        case 1:
        case 6:
        case 7:
        case 8:
            o->label = "AA value for another board size";
            o->evidence = "our OpenGL driver and Glide disagree on this value for two chips";
            return;
        }
        return;
    }
    if (nchips == 1) {
        switch (cfg) {
        case 0:
        case 2:
        case 5:
            o->status = CTL_AA_UNTESTED;
            o->label = "Single chip";
            o->evidence = "the only non-AA mode of a one-chip board";
            return;
        case 1:
            o->status = CTL_AA_EXPERIMENTAL;
            o->label = "2x anti-aliasing (EXPERIMENTAL)";
            o->evidence = "never run on a one-chip board on our stack";
            return;
        }
        o->label = "AA value for another board size";
        o->evidence = "a multi-chip value";
        return;
    }
    o->label = "board size unknown";
    o->evidence = "the driver did not say how many chips the board has - nothing is offered";
}

/* The configurations the panel offers, in the order it lists them: the
 * validated/non-AA ones always, the experimental AA ones only when allowed. */
CTL_FN int ctl_aa_choices(unsigned nchips, int allow_experimental, long *out, int max)
{
    static const long order4[] = { 5, 0, 6, 7, 8 };
    static const long order2[] = { 2, 0, 3, 4 };
    static const long order1[] = { 0, 1 };
    const long *ord;
    int n, i, k = 0;
    ctl_aa_info a;
    if (nchips >= 4) {
        ord = order4;
        n = 5;
    } else if (nchips == 2) {
        ord = order2;
        n = 4;
    } else if (nchips == 1) {
        ord = order1;
        n = 2;
    } else {
        return 0;
    }
    for (i = 0; i < n && k < max; i++) {
        ctl_aa_describe(ord[i], nchips, &a);
        if (a.status == CTL_AA_NOT_OFFERED)
            continue;
        if (a.status == CTL_AA_EXPERIMENTAL && !allow_experimental)
            continue;
        out[k++] = ord[i];
    }
    return k;
}

/* 1 = two values of SSTH3_SLI_AA_CONFIGURATION give this board the same
 * non-AA chip mode (absent = Glide's default 2): absent/2/5 are "all chips in
 * SLI" on 2 and 4 chips, and absent/0/2/5 are all "the one chip" on one. An
 * AA value is only ever the same as itself. A preset uses it to leave a value
 * alone that already means what it would write. */
CTL_FN int ctl_aa_same_mode(int a_present, long a, int b_present, long b, unsigned nchips)
{
    long x = a_present ? a : 2, y = b_present ? b : 2;
    if (x == y)
        return 1;
    if (ctl_aa_is_aa(x) || ctl_aa_is_aa(y))
        return 0;
    if (nchips == 1)
        return (x == 0 || x == 2 || x == 5) && (y == 0 || y == 2 || y == 5);
    return (x == 2 || x == 5) && (y == 2 || y == 5);
}

/* The writes one AA choice turns into. The Diag\SliAA kill switch follows the
 * configuration: armed with an AA mode, deleted with anything else - so it is
 * never left armed behind a non-AA mode by this panel. */
typedef struct ctl_aa_plan {
    int         write_cfg;          /* 1 = write cfg_value (NULL = delete the value) */
    char        cfg_value[8];
    int         cfg_absent;
    int         sliaa;              /* 1 = set 1, -1 = delete the value, 0 = leave it */
    const char *refused;            /* non-NULL: write NOTHING, say this */
} ctl_aa_plan;

/* new_cfg < 0 = "the driver default" (delete the value: Glide's own 2 = SLI). */
CTL_FN void ctl_aa_make_plan(unsigned nchips, long new_cfg, int confirmed, int cur_sliaa,
                             ctl_aa_plan *p)
{
    ctl_aa_info a;
    memset(p, 0, sizeof *p);
    if (new_cfg < 0) {
        p->write_cfg = 1;
        p->cfg_absent = 1;
        p->sliaa = cur_sliaa ? -1 : 0;
        return;
    }
    ctl_aa_describe(new_cfg, nchips, &a);
    if (a.status == CTL_AA_NOT_OFFERED) {
        p->refused = "that configuration is not offered for this board (see the list)";
        return;
    }
    if (ctl_aa_is_aa(new_cfg) && !confirmed) {
        p->refused = "an experimental anti-aliasing mode needs the explicit confirmation";
        return;
    }
    p->write_cfg = 1;
    p->cfg_value[0] = (char)('0' + (int)new_cfg);
    p->cfg_value[1] = 0;
    p->sliaa = ctl_aa_is_aa(new_cfg) ? 1 : (cur_sliaa ? -1 : 0);
}

/* ---- presets --------------------------------------------------------------------
 * A preset fills the controls; nothing is written until Apply. NO preset
 * selects an anti-aliasing mode or touches the kernel AA switch: AA is
 * experimental on this board and needs a person's explicit confirmation. */
enum { CTL_PRESET_DEFAULTS, CTL_PRESET_QUALITY, CTL_PRESET_SPEED, CTL_NPRESETS };

static const char CTL_KEEP[] = "(keep)";        /* leave the control as it is */
static const char CTL_SLI[] = "(sli)";          /* all chips in SLI, for this board */
#define CTL_ABSENT ((const char *)0)            /* the stack's default: delete the value */

typedef struct ctl_preset_row {
    int         id;
    const char *v[CTL_NPRESETS];                /* defaults, quality, speed */
} ctl_preset_row;

static const ctl_preset_row ctl_presets[] = {
    /* id                     defaults     quality      speed      */
    { CTL_ID_VSYNC,         { CTL_ABSENT, "1",         "0"        } },
    { CTL_ID_QUEUE,         { CTL_ABSENT, CTL_ABSENT,  "3"        } },
    { CTL_ID_GLIDE_REFRESH, { CTL_ABSENT, CTL_KEEP,    CTL_KEEP   } },
    { CTL_ID_LOD_DITHER,    { CTL_ABSENT, "1",         CTL_ABSENT } },
    { CTL_ID_ALPHA_DITHER,  { CTL_ABSENT, "3",         CTL_ABSENT } },
    { CTL_ID_OVERLAY,       { CTL_ABSENT, CTL_ABSENT,  CTL_ABSENT } },
    { CTL_ID_GLIDE_GAMMA,   { CTL_ABSENT, CTL_KEEP,    CTL_KEEP   } },
    { CTL_ID_APP_GAMMA,     { CTL_ABSENT, CTL_KEEP,    CTL_KEEP   } },
    { CTL_ID_NO_PLUGIN,     { CTL_ABSENT, CTL_KEEP,    "1"        } },
    { CTL_ID_AA,            { CTL_ABSENT, CTL_SLI,     CTL_SLI    } },
    { CTL_ID_GL_GAMMA,      { CTL_ABSENT, CTL_KEEP,    CTL_KEEP   } },
    { CTL_ID_GL_DITHER,     { CTL_ABSENT, CTL_ABSENT,  CTL_ABSENT } },
    { CTL_ID_GL_LODBIAS,    { CTL_ABSENT, CTL_ABSENT,  CTL_ABSENT } },
    { CTL_ID_DESK_REFRESH,  { CTL_KEEP,   CTL_KEEP,    CTL_KEEP   } },
    { CTL_ID_2D_TEXT,       { CTL_ABSENT, CTL_KEEP,    CTL_KEEP   } },
    { CTL_ID_2D_PAT,        { CTL_ABSENT, CTL_KEEP,    CTL_KEEP   } },
    { CTL_ID_2D_LINE,       { CTL_ABSENT, CTL_KEEP,    CTL_KEEP   } },
    { 0, { 0, 0, 0 } }
};

#define CTL_PRESET_KEEP 0
#define CTL_PRESET_SET  1

/* What preset `p` does to the control of row `id`: CTL_PRESET_KEEP, or
 * CTL_PRESET_SET with *val (CTL_ABSENT = the stack's default). A row the table
 * does not list is kept. CTL_SLI becomes this board's SLI value. */
CTL_FN int ctl_preset_value(int p, int id, unsigned nchips, const char **val, char *buf,
                            unsigned buflen)
{
    const ctl_preset_row *r;
    if (p < 0 || p >= CTL_NPRESETS)
        return CTL_PRESET_KEEP;
    for (r = ctl_presets; r->id; r++) {
        if (r->id != id)
            continue;
        if (r->v[p] == CTL_KEEP)
            return CTL_PRESET_KEEP;
        if (r->v[p] == CTL_SLI) {
            if (!nchips || buflen < 2)
                return CTL_PRESET_KEEP;         /* unknown board: leave it */
            buf[0] = (char)('0' + (int)ctl_aa_sli_value(nchips));
            buf[1] = 0;
            *val = buf;
            return CTL_PRESET_SET;
        }
        *val = r->v[p];
        return CTL_PRESET_SET;
    }
    return CTL_PRESET_KEEP;
}

/* ---- refresh rates: only what the driver enumerates ------------------------------- */

typedef struct ctl_mode {
    unsigned w, h, bpp, hz;
} ctl_mode;

/* The rates the driver lists for exactly (w, h, bpp), highest first, no
 * duplicates. 0 and 1 Hz are the driver's "default" sentinels, never a rate. */
CTL_FN int ctl_desk_rates(const ctl_mode *m, int n, unsigned w, unsigned h, unsigned bpp,
                          unsigned *out, int max)
{
    int i, j, k = 0;
    for (i = 0; i < n; i++) {
        unsigned hz = m[i].hz;
        if (m[i].w != w || m[i].h != h || m[i].bpp != bpp || hz <= 1)
            continue;
        for (j = 0; j < k && out[j] != hz; j++)
            ;
        if (j < k || k >= max)
            continue;
        out[k++] = hz;
    }
    for (i = 1; i < k; i++)                     /* insertion sort, descending */
        for (j = i; j > 0 && out[j] > out[j - 1]; j--) {
            unsigned t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return k;
}

/* 1 = (w, h, bpp, hz) is a mode the driver enumerates */
CTL_FN int ctl_mode_listed(const ctl_mode *m, int n, unsigned w, unsigned h, unsigned bpp,
                           unsigned hz)
{
    int i;
    for (i = 0; i < n; i++)
        if (m[i].w == w && m[i].h == h && m[i].bpp == bpp && m[i].hz == hz && hz > 1)
            return 1;
    return 0;
}

/* The mode to pass through so the display driver builds a NEW surface at the
 * current mode (what the Accel2D* switches need: vcrdd reads them in
 * DrvEnableSurface). Measured on XP SP3 with vcr-kmd (QEMU test bed,
 * 2026-09-28): ChangeDisplaySettingsEx(CDS_RESET) at the SAME mode is only a
 * DrvAssertMode(FALSE)/(TRUE) pair on the same PDEV - no DrvEnableSurface, so
 * the switches are not read. A real mode change is. So: another mode the
 * driver LISTS, as close as possible - the same resolution and depth at the
 * nearest lower refresh (else the nearest higher), else the same resolution at
 * the other of 16/32 bpp (the same refresh if listed, else its highest not
 * above the current one, else its lowest). The resolution never changes, so
 * the desktop's layout does not either. 0 = no such mode: nothing is switched. */
CTL_FN int ctl_bounce_mode(const ctl_mode *m, int n, const ctl_mode *cur, ctl_mode *out)
{
    int i, found = 0;
    unsigned best = 0, other;
    /* 1. same resolution + depth, another refresh: nearest below, else nearest above */
    for (i = 0; i < n; i++)
        if (m[i].w == cur->w && m[i].h == cur->h && m[i].bpp == cur->bpp && m[i].hz > 1 &&
            m[i].hz < cur->hz && m[i].hz > best)
            best = m[i].hz;
    if (!best) {
        for (i = 0; i < n; i++)
            if (m[i].w == cur->w && m[i].h == cur->h && m[i].bpp == cur->bpp && m[i].hz > 1 &&
                m[i].hz > cur->hz && (!best || m[i].hz < best))
                best = m[i].hz;
    }
    if (best) {
        *out = *cur;
        out->hz = best;
        return 1;
    }
    /* 2. same resolution, the other of 16/32 bpp */
    if (cur->bpp != 16 && cur->bpp != 32)
        return 0;
    other = cur->bpp == 32 ? 16 : 32;
    if (ctl_mode_listed(m, n, cur->w, cur->h, other, cur->hz)) {
        *out = *cur;
        out->bpp = other;
        return 1;
    }
    for (i = 0; i < n; i++)
        if (m[i].w == cur->w && m[i].h == cur->h && m[i].bpp == other && m[i].hz > 1 &&
            m[i].hz <= cur->hz && m[i].hz > best) {
            best = m[i].hz;
            found = 1;
        }
    if (!found)
        for (i = 0; i < n; i++)
            if (m[i].w == cur->w && m[i].h == cur->h && m[i].bpp == other && m[i].hz > 1 &&
                (!found || m[i].hz < best)) {
                best = m[i].hz;
                found = 1;
            }
    if (!found)
        return 0;
    *out = *cur;
    out->bpp = other;
    out->hz = best;
    return 1;
}

/* A fullscreen refresh override for Glide: each rate the driver lists at a
 * 16/32-bit mode of at least 512x384, with the smallest and largest resolution
 * it is listed at (for the label). Highest first. */
typedef struct ctl_rate_span {
    unsigned hz;
    unsigned minw, minh, maxw, maxh;
} ctl_rate_span;

CTL_FN int ctl_glide_rates(const ctl_mode *m, int n, ctl_rate_span *out, int max)
{
    int i, j, k = 0;
    for (i = 0; i < n; i++) {
        const ctl_mode *x = &m[i];
        if (x->hz <= 1 || (x->bpp != 16 && x->bpp != 32) || x->w < 512 || x->h < 384)
            continue;
        for (j = 0; j < k && out[j].hz != x->hz; j++)
            ;
        if (j == k) {
            if (k >= max)
                continue;
            out[k].hz = x->hz;
            out[k].minw = out[k].maxw = x->w;
            out[k].minh = out[k].maxh = x->h;
            k++;
            continue;
        }
        if (x->w * x->h < out[j].minw * out[j].minh) {
            out[j].minw = x->w;
            out[j].minh = x->h;
        }
        if (x->w * x->h > out[j].maxw * out[j].maxh) {
            out[j].maxw = x->w;
            out[j].maxh = x->h;
        }
    }
    for (i = 1; i < k; i++)
        for (j = i; j > 0 && out[j].hz > out[j - 1].hz; j--) {
            ctl_rate_span t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return k;
}

/* ---- values ------------------------------------------------------------------------ */

/* strict decimal float: [-]digits[.digits], nothing else; 1 = ok */
CTL_FN int ctl_parse_float(const char *s, float *out)
{
    const char *p = s;
    int digits = 0;
    double v = 0, scale = 1;
    int neg = 0;
    if (!s || !*s)
        return 0;
    if (*p == '-' || *p == '+')
        neg = (*p++ == '-');
    for (; *p >= '0' && *p <= '9'; p++, digits++)
        v = v * 10 + (*p - '0');
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++, digits++) {
            scale /= 10;
            v += (*p - '0') * scale;
        }
    if (*p || !digits)
        return 0;
    *out = (float)(neg ? -v : v);
    return 1;
}

/* A float control's value as it is written: "absent" at the stack default
 * (within half a step of 0.01), else two decimals. buf >= 16. */
CTL_FN const char *ctl_float_value(float v, float dflt, char *buf)
{
    long c;
    float d = v - dflt;
    if (d < 0.005f && d > -0.005f)
        return CTL_ABSENT;
    c = (long)(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
    if (c < 0)
        c = 0;                                  /* no control here goes below zero */
    if (c > 999)
        c = 999;                                /* nor to 10.00 */
    buf[0] = (char)('0' + c / 100);
    buf[1] = '.';
    buf[2] = (char)('0' + (c / 10) % 10);
    buf[3] = (char)('0' + c % 10);
    buf[4] = 0;
    return buf;
}

/* ---- the card ------------------------------------------------------------------------ */

CTL_FN const char *ctl_card_name(unsigned device, unsigned nchips)
{
    switch (device) {
    case 0x0003: return "3dfx Voodoo Banshee";
    case 0x0004: return "3dfx Voodoo Banshee";
    case 0x0005: return "3dfx Voodoo3";
    case 0x0009:
        return nchips >= 4 ? "3dfx Voodoo5 6000" : nchips == 2 ? "3dfx Voodoo5 5500"
                                                              : "3dfx Voodoo4 4500";
    }
    return "3dfx graphics card";
}

#endif /* CTL_LOGIC_H */

/* test_qbinds.c - TRUE-SOURCE: compiles the REAL agent/shared/qbinds.h, the
 * decisions behind agent/src/qbinds.c (agent 1.97.0, QBINDS).
 *
 * THE OLD BEHAVIOUR: the staged Quake II autoexec.cfg opened its WASD block
 * with `unbindall`, which runs AFTER config.cfg and re-binds only ~24 keys -
 * so the weapon keys 1-0, [ ] inventory, ENTER, the arrows and F1-F4 were
 * unbound on every launch (.243's config.cfg, 2026-09-29: no weapon keys at
 * all). THE FIX: an agent-owned FLEETKEY.CFG that binds the fleet keys and
 * unbinds nothing, exec'd from the staged autoexec.
 *
 * The chain tests run the REAL default.cfg files (Quake 1.08 ID1/PAK0.PAK,
 * Quake II 3.20 baseq2/pak1.pak - the one the engine takes over pak0's) and the
 * staged autoexec block through qbinds.h's own command splitter, and assert BOTH
 * the old buggy result and the fixed one.
 */
#include "munit.h"
#include <string.h>

#include "../../agent/shared/qbinds.h"

/* ---- fixtures, verbatim (generated from the files named above) ---------- */

/* Quake II 3.20: baseq2/pak1.pak default.cfg (3,078 B, md5 d7e5139bc68f78efdecbdd88e6547acf) */
static const char q2_default_cfg[] =
    "//\r\n"
    "// KEY BINDINGS\r\n"
    "//\r\n"
    "\r\n"
    "unbindall\r\n"
    "\r\n"
    "bind ' \"inven_drop\"\r\n"
    "bind 1 \"use Blaster\"\r\n"
    "bind 2 \"use Shotgun\"\r\n"
    "bind 3 \"use Super Shotgun\"\r\n"
    "bind 4 \"use Machinegun\"\r\n"
    "bind 5 \"use Chaingun\"\r\n"
    "bind 6 \"use Grenade Launcher\"\r\n"
    "bind 7 \"use Rocket Launcher\"\r\n"
    "bind 8 \"use HyperBlaster\"\r\n"
    "bind 9 \"use Railgun\"\r\n"
    "bind 0 \"use BFG10K\"\r\n"
    "\r\n"
    "//\r\n"
    "// CHARACTER CONTROLS\r\n"
    "//\r\n"
    "\r\n"
    "bind    CTRL\t\t+attack\r\n"
    "bind    ALT\t\t+strafe\r\n"
    "\r\n"
    "bind    ,           +moveleft\r\n"
    "bind    .           +moveright\r\n"
    "bind    DEL         +lookdown\r\n"
    "bind    PGDN        +lookup\r\n"
    "bind    END         centerview\r\n"
    "bind    z           +lookdown\r\n"
    "bind    a           +lookup\r\n"
    "bind\tc\t\t\t+movedown\r\n"
    "\r\n"
    "bind    TAB         inven\r\n"
    "bind    ENTER       invuse\r\n"
    "bind    [           invprev\r\n"
    "bind    ]           invnext\r\n"
    "bind    '           invdrop\r\n"
    "bind\tBACKSPACE   invdrop\t\r\n"
    "\r\n"
    "bind\t/\t\t\tweapnext\r\n"
    "\r\n"
    "bind\tg \t\t\tuse grenades\r\n"
    "bind\ts \t\t\tuse silencer\r\n"
    "bind\tq\t\t\tuse quad damage\r\n"
    "bind\tb\t\t\tuse rebreather\r\n"
    "bind\te\t\t\tuse environment suit\r\n"
    "bind\ti\t\t\tuse invulnerability\r\n"
    "bind \tp\t\t\tuse power shield\r\n"
    "bind\tx\t\t\tscore\r\n"
    "\r\n"
    "//waves\r\n"
    "bind\th\t\"wave 0\"\r\n"
    "bind \tj\t\"wave 1\"\r\n"
    "bind\tk\t\"wave 2\"\r\n"
    "bind\tl\t\"wave 3\"\r\n"
    "bind \tu \t\"wave 4\"\r\n"
    "\r\n"
    "bind    SHIFT \t\t+speed\r\n"
    "\r\n"
    "bind    UPARROW     +forward\r\n"
    "bind    DOWNARROW   +back\r\n"
    "bind    LEFTARROW   +left\r\n"
    "bind    RIGHTARROW  +right\r\n"
    "\r\n"
    "bind    SPACE       +moveup \r\n"
    "\r\n"
    "//\r\n"
    "// MOUSE OPTIONS\r\n"
    "//\r\n"
    "\r\n"
    "bind    \\           +mlook\r\n"
    "\r\n"
    "//\r\n"
    "// CLIENT ENVIRONMENT COMMANDS\r\n"
    "//\r\n"
    "\r\n"
    "bind    PAUSE       \"pause\"\r\n"
    "bind    ESCAPE      \"togglemenu\"\r\n"
    "bind    ~           \"toggleconsole\"\r\n"
    "bind    `           \"toggleconsole\"\r\n"
    "\r\n"
    "bind    F1          \"cmd help\"\r\n"
    "bind\tF2\t\t\t\"menu_savegame\"\r\n"
    "bind\tF3\t\t\t\"menu_loadgame\"\r\n"
    "bind\tF4\t\t\t\"menu_keys\"\r\n"
    "bind\tF5\t\t\t\"menu_startserver\"\r\n"
    "bind\tF6\t\t\t\"echo Quick Saving...; wait; save quick\"\r\n"
    "bind\tF9\t\t\t\"echo Quick Loading...; wait; load quick\"\r\n"
    "bind\tF10\t\t\t\"menu_quit\"\r\n"
    "bind\tF12\t\t\t\"screenshot\"\r\n"
    "\r\n"
    "bind    t           \"messagemode\"\r\n"
    "\r\n"
    "bind    +           \"sizeup\"\r\n"
    "bind    =           \"sizeup\"\r\n"
    "bind    -           \"sizedown\"\r\n"
    "\r\n"
    "bind    INS         +klook\r\n"
    "\r\n"
    "//\r\n"
    "// MOUSE BUTTONS\r\n"
    "//\r\n"
    "\r\n"
    "bind    MOUSE3      +forward\r\n"
    "bind    MOUSE1      +attack\r\n"
    "bind    MOUSE2      +strafe\r\n"
    "\r\n"
    "//\r\n"
    "// DEFAULT CVARS\r\n"
    "//\r\n"
    "\r\n"
    "set viewsize\t\t100\r\n"
    "set\tvid_fullscreen\t1\r\n"
    "set win_noalttab\t0\r\n"
    "set sensitivity     3\r\n"
    "set crosshair\t\t1\r\n"
    "set cl_run\t\t\t0\r\n"
    "set hand\t\t\t0\r\n"
    "set m_pitch         0.022\r\n"
    "set m_yaw           0.022\r\n"
    "set m_forward       1\r\n"
    "set m_side          0.8\r\n"
    "set lookspring\t\t1\r\n"
    "set lookstrafe\t\t0\r\n"
    "\r\n"
    "\r\n"
    "//----------------------------------------------\r\n"
    "//\r\n"
    "// userinfo\r\n"
    "//\r\n"
    "\r\n"
    "set name Player\r\n"
    "set skin male/grunt\r\n"
    "\r\n"
    "//----------------------------------------------\r\n"
    "\r\n"
    "//\r\n"
    "//\tDEMO STUFF\r\n"
    "//\r\n"
    "alias   d1      \"demomap idlog.cin ; set nextserver d2\"\r\n"
    "alias   d2      \"demomap demo1.dm2 ; set nextserver d3\"\r\n"
    "alias   d3      \"demomap idlog.cin ; set nextserver d4\"\r\n"
    "alias   d4      \"demomap demo2.dm2 ; set nextserver d1\"\r\n"
    "\r\n"
    "//\r\n"
    "// newgame command\r\n"
    "//\r\n"
    "alias   newgame \" killserver ; maxclients 1 ; deathmatch 0 ; map *ntro.cin+base1\"\r\n"
    "\r\n"
    "//\r\n"
    "// quake2 +set dedicated 1\r\n"
    "// will run this command if no other +map is included\r\n"
    "//\r\n"
    "alias\tdedicated_start\t\"map base1\"\r\n"
    "\r\n"
    "\r\n";

/* Quake 1.08: ID1/PAK0.PAK default.cfg (1,914 B) */
static const char q1_default_cfg[] =
    "//\r\n"
    "// load keybindings\r\n"
    "//\r\n"
    "// commands with a leading + will also be called for key up events with\r\n"
    "// the + changed to a -\r\n"
    "unbindall\r\n"
    "\r\n"
    "//\r\n"
    "// character controls\r\n"
    "//\r\n"
    "\r\n"
    "bind\tALT\t\t\t\t+strafe\r\n"
    "\r\n"
    "bind\t,\t\t\t\t+moveleft\r\n"
    "bind\t.\t\t\t\t+moveright\r\n"
    "bind\tDEL\t\t\t\t+lookdown\r\n"
    "bind\tPGDN\t\t\t+lookup\r\n"
    "bind\tEND\t\t\t\tcenterview\r\n"
    "bind\tz\t\t\t\t+lookdown\r\n"
    "bind\ta\t\t\t\t+lookup\r\n"
    "\r\n"
    "bind\td\t\t\t\t+moveup\r\n"
    "bind\tc\t\t\t\t+movedown\r\n"
    "bind\tSHIFT\t\t\t+speed\r\n"
    "bind\tCTRL\t\t\t+attack\r\n"
    "bind\tUPARROW\t\t\t+forward\r\n"
    "bind\tDOWNARROW\t\t+back\r\n"
    "bind\tLEFTARROW\t\t+left\r\n"
    "bind\tRIGHTARROW\t\t+right\r\n"
    "\r\n"
    "bind\tSPACE\t\t\t+jump \r\n"
    "bind\tENTER\t\t\t+jump\r\n"
    "\r\n"
    "bind\tTAB\t\t\t\t+showscores\r\n"
    "\r\n"
    "bind\t1\t\t\t\t\"impulse 1\"\r\n"
    "bind\t2\t\t\t\t\"impulse 2\"\r\n"
    "bind\t3\t\t\t\t\"impulse 3\"\r\n"
    "bind\t4\t\t\t\t\"impulse 4\"\r\n"
    "bind\t5\t\t\t\t\"impulse 5\"\r\n"
    "bind\t6\t\t\t\t\"impulse 6\"\r\n"
    "bind\t7\t\t\t\t\"impulse 7\"\r\n"
    "bind\t8\t\t\t\t\"impulse 8\"\r\n"
    "\r\n"
    "bind\t0\t\t\t\t\"impulse 0\"\r\n"
    "\r\n"
    "bind\t/\t\t\t\t\"impulse 10\"\t\t// change weapon\r\n"
    "\r\n"
    "// zoom\r\n"
    "alias zoom_in \"sensitivity 2;fov 90;wait;fov 70;wait;fov 50;wait;fov 30;wait;fov 10;wait;fov 5;bind F11 zoom_out\"\r\n"
    "alias zoom_out \"sensitivity 4;fov 5;wait;fov 10;wait;fov 30;wait;fov 50;wait;fov 70;wait;fov 90;bind F11 zoom_in; sensitivity 3\"\r\n"
    "bind F11 zoom_in\r\n"
    "\r\n"
    "// Function keys\r\n"
    "bind\tF1\t\t\t\"help\"\r\n"
    "bind\tF2\t\t\t\"menu_save\"\r\n"
    "bind\tF3\t\t\t\"menu_load\"\r\n"
    "bind\tF4\t\t\t\"menu_options\"\r\n"
    "bind\tF5\t\t\t\"menu_multiplayer\"\r\n"
    "bind\tF6\t\t\t\"echo Quicksaving...; wait; save quick\"\r\n"
    "bind\tF9\t\t\t\"echo Quickloading...; wait; load quick\"\r\n"
    "bind\tF10\t\t\t\"quit\"\r\n"
    "bind    F12                     \"screenshot\"\r\n"
    "\r\n"
    "// mouse options\r\n"
    "bind\t\\\t\t\t\t+mlook\r\n"
    "\r\n"
    "//\r\n"
    "// client environment commands\r\n"
    "//\r\n"
    "bind\tPAUSE\t\t\t\"pause\"\r\n"
    "bind\tESCAPE\t\t\"togglemenu\"\r\n"
    "bind\t~\t\t\t\t\"toggleconsole\"\r\n"
    "bind\t`\t\t\t\t\"toggleconsole\"\r\n"
    "\r\n"
    "bind\tt\t\t\t\t\"messagemode\"\r\n"
    "\r\n"
    "bind\t+\t\t\t\t\"sizeup\"\r\n"
    "bind\t=\t\t\t\t\"sizeup\"\r\n"
    "bind\t-\t\t\t\t\"sizedown\"\r\n"
    "\r\n"
    "bind\tINS\t\t\t+klook\r\n"
    "\r\n"
    "//\r\n"
    "// mouse buttons\r\n"
    "//\r\n"
    "bind\tMOUSE1\t\t+attack\r\n"
    "bind\tMOUSE2\t\t+forward\r\n"
    "bind\tMOUSE3\t\t+mlook\r\n"
    "\r\n"
    "//\r\n"
    "// default cvars\r\n"
    "//\r\n"
    "viewsize \t\t100\r\n"
    "gamma \t\t\t1.0\r\n"
    "volume \t\t\t0.7\r\n"
    "sensitivity \t3\r\n"
    "\r\n";

/* The bind block of the staged Quake2Win9x/baseq2/autoexec.cfg - Quake2Complete's
 * baseq2/rogue/xatrix copies carry the same block (md5 ed61dfcb...) - read 2026-10-01. */
static const char q2_old_autoexec[] =
    "// ---- movement: WASD, plus F ------------------------------------------------\r\n"
    "unbindall\r\n"
    "bind w \"+forward\"\r\n"
    "bind s \"+back\"\r\n"
    "bind a \"+moveleft\"\r\n"
    "bind d \"+moveright\"\r\n"
    "bind f \"invuse\"                 // Quake II has no +use; invuse is its action key\r\n"
    "bind SPACE \"+moveup\"            // jump\r\n"
    "bind CTRL \"+movedown\"           // crouch\r\n"
    "bind SHIFT \"+speed\"\r\n"
    "\r\n"
    "// ---- free look -------------------------------------------------------------\r\n"
    "// Quake II has no `freelook` cvar - +mlook IS the mechanism, and putting it in\r\n"
    "// autoexec is what makes it permanent rather than a key you have to hold.\r\n"
    "+mlook\r\n"
    "set lookstrafe \"0\"              // mouse left/right TURNS, it does not strafe\r\n"
    "set lookspring \"0\"              // do not snap the view back when you stop moving\r\n"
    "set m_pitch \"0.022\"             // stock sensitivity, not inverted\r\n"
    "set sensitivity \"5\"\r\n"
    "set cl_run \"1\"                  // always run; nobody wants 1997 walk speed\r\n"
    "\r\n"
    "// ---- weapons and the rest --------------------------------------------------\r\n"
    "bind MOUSE1 \"+attack\"\r\n"
    "bind MOUSE2 \"+moveup\"\r\n"
    "bind MWHEELUP \"weapnext\"\r\n"
    "bind MWHEELDOWN \"weapprev\"\r\n"
    "bind q \"weapprev\"\r\n"
    "bind e \"weapnext\"\r\n"
    "bind TAB \"inven\"\r\n"
    "bind r \"cmd help\"\r\n"
    "bind ESCAPE \"togglemenu\"\r\n"
    "bind ` \"toggleconsole\"\r\n"
    "bind t \"messagemode\"\r\n"
    "bind y \"messagemode2\"\r\n"
    "bind F5 \"save quick\"\r\n"
    "bind F9 \"load quick\"\r\n";

/* ---- a tiny Quake bind table, driven by qbinds.h's OWN command splitter ---- */
#define SIM_MAX 160
typedef struct {
    char key[QB_ARG_LEN];
    char cmd[256];
} sim_bind_t;
typedef struct {
    sim_bind_t b[SIM_MAX];
    int n;
} sim_t;

static int sim_find(const sim_t *s, const char *k)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (qb_streq_ci(s->b[i].key, k))
            return i;
    return -1;
}

static const char *sim_get(const sim_t *s, const char *k)
{
    int i = sim_find(s, k);
    return i < 0 ? NULL : s->b[i].cmd;
}

/* Key_Bind_f: everything after the key, re-joined with single spaces. */
static void sim_bind(sim_t *s, char argv[QB_MAX_ARGS][QB_ARG_LEN], int argc)
{
    int i = sim_find(s, argv[1]), a;
    if (i < 0) {
        if (s->n >= SIM_MAX)
            return;
        i = s->n++;
        strcpy(s->b[i].key, argv[1]);
    }
    s->b[i].cmd[0] = 0;
    for (a = 2; a < argc; a++) {
        if (a > 2)
            strcat(s->b[i].cmd, " ");
        strcat(s->b[i].cmd, argv[a]);
    }
}

static void sim_unbind(sim_t *s, const char *k)
{
    int i = sim_find(s, k);
    if (i >= 0)
        s->b[i] = s->b[--s->n];
}

/* Run a config the way Cbuf_Execute does. `exec fleetkey.cfg` runs `fleetkey`
 * in place - Cbuf_InsertText puts the file ahead of the rest of the caller. */
static void sim_exec(sim_t *s, const char *text, const char *fleetkey)
{
    qb_cmdit_t it;
    char cmd[1024];
    char argv[QB_MAX_ARGS][QB_ARG_LEN];
    int line;
    qb_cmdit_init(&it, text, strlen(text));
    while (qb_cmdit_next(&it, cmd, sizeof(cmd), &line)) {
        int argc = qb_tokenize(cmd, argv);
        if (!argc)
            continue;
        if (qb_streq_ci(argv[0], "unbindall"))
            s->n = 0;
        else if (qb_streq_ci(argv[0], "bind") && argc >= 3)
            sim_bind(s, argv, argc);
        else if (qb_streq_ci(argv[0], "unbind") && argc >= 2)
            sim_unbind(s, argv[1]);
        else if (qb_streq_ci(argv[0], "exec") && argc >= 2 &&
                 qb_streq_ci(argv[1], QB_EXEC_ARG) && fleetkey)
            sim_exec(s, fleetkey, NULL);
    }
}

/* want == NULL: the key must be unbound. */
static int bind_is(const sim_t *s, const char *k, const char *want)
{
    const char *g = sim_get(s, k);
    if (!want)
        return g == NULL;
    return g != NULL && strcmp(g, want) == 0;
}
#define CHECK_BIND(s, k, want) CHECK(bind_is((s), (k), (want)), "key " k " should be " #want)

/* What the staged autoexec becomes once the library carries the exec line
 * (the Phase 2 library change): no unbindall, no inline block. */
static const char q2_new_autoexec[] =
    "// autoexec.cfg - fleet defaults for Quake II\r\n"
    "set vid_fullscreen \"1\"\r\n"
    "bind F11 \"screenshot\"\r\n"
    "exec fleetkey.cfg\r\n"
    "exec fleetres.cfg\r\n";

static const char q1_new_autoexec[] =
    "// NO SEMICOLONS IN COMMENTS\r\n"
    "bind F11 \"screenshot\"\r\n"
    "gamma 0.7\r\n"
    "exec fleetkey.cfg\r\n";

/* ---- the bodies ------------------------------------------------------------ */

static void check_body_rules(const char *name, const char *b)
{
    const char *p = b;
    size_t len = strlen(b);
    int lines = 0;
    CHECK(len > 0 && len < 2048, name);
    CHECK(strchr(b, ';') == NULL, "a ';' splits a Quake command even inside a comment");
    CHECK(len >= 2 && b[len - 2] == '\r' && b[len - 1] == '\n', "the body ends in CRLF");
    while (*p) {
        const char *e = strchr(p, '\n'), *c;
        int q = 0;
        CHECK(e != NULL, "every line ends in a newline");
        if (!e)
            break;
        CHECK(e > p && e[-1] == '\r', "every line is CRLF, never a bare LF");
        for (c = p; c < e; c++)
            if (*c == '"')
                q++;
        CHECK(!(q & 1), "an even number of quotes on every line");
        for (c = p; c + 1 < e; c++)
            if (c[0] == '/' && c[1] == '/') {
                const char *d;
                for (d = c; d < e; d++)
                    CHECK(*d != '"', "no double quote inside a comment");
                break;
            }
        lines++;
        p = e + 1;
    }
    CHECK(lines >= 2, name);
}

TEST(every_body_obeys_the_quake_config_rules)
{
    int pr;
    for (pr = 0; pr < QB_NPROFILES; pr++) {
        check_body_rules(qb_profiles[pr].name, qb_profiles[pr].body);
        check_body_rules(qb_profiles[pr].name, qb_profiles[pr].off);
    }
    CHECK(strncmp(qb_body_q1, "// fleetkey 1 q1\r\n", 18) == 0, "q1 header");
    CHECK(strncmp(qb_body_q2, "// fleetkey 1 q2\r\n", 18) == 0, "q2 header");
    CHECK(strncmp(qb_body_q1_off, "// fleetkey 1 q1 off\r\n", 22) == 0, "q1 off header");
    CHECK(strncmp(qb_body_q2_off, "// fleetkey 1 q2 off\r\n", 22) == 0, "q2 off header");
    /* 8.3, because DOS QUAKE.EXE reads ID1 too */
    CHECK(strlen(QB_FILE) == 12 && QB_FILE[8] == '.', "FLEETKEY.CFG is an 8.3 name");
    CHECK(strlen(QB_TMP) == 12 && QB_TMP[8] == '.', "FLEETKEY.TMP is an 8.3 name");
}

TEST(the_bodies_are_r4_a4_as_the_orchestrator_decided)
{
    char k[QB_MAX_KEYS][QB_ARG_LEN];
    static const char *const q2[] = { "w", "s", "a", "d", "SPACE", "CTRL", "SHIFT", "f",
        "MOUSE1", "MOUSE2", "e", "q", "MWHEELUP", "MWHEELDOWN", "TAB", "r", "y", "F5", "F9" };
    static const char *const q1[] = { "w", "s", "a", "d", "SPACE", "SHIFT", "MOUSE1",
        "MOUSE2", "e", "q", "MWHEELUP", "MWHEELDOWN", "y" };
    int n, i;
    n = qb_body_keys(qb_body_q2, k);
    CHECK_EQ_I(n, (int)(sizeof(q2) / sizeof(q2[0])));
    for (i = 0; i < n && i < (int)(sizeof(q2) / sizeof(q2[0])); i++)
        CHECK(strcmp(k[i], q2[i]) == 0, "q2 binds R4's keys in R4's order");
    n = qb_body_keys(qb_body_q1, k);
    CHECK_EQ_I(n, (int)(sizeof(q1) / sizeof(q1[0])));
    for (i = 0; i < n && i < (int)(sizeof(q1) / sizeof(q1[0])); i++)
        CHECK(strcmp(k[i], q1[i]) == 0, "q1 binds R4's keys in R4's order");
    /* q2 keeps CTRL = crouch and f = invuse; q1 leaves CTRL to default.cfg and binds no f */
    CHECK(strstr(qb_body_q2, "bind CTRL \"+movedown\"") != NULL, "q2 CTRL crouches");
    CHECK(strstr(qb_body_q2, "bind f \"invuse\"") != NULL, "q2 f is invuse");
    CHECK(strstr(qb_body_q1, "CTRL") == NULL, "q1 does not touch CTRL");
    CHECK(strstr(qb_body_q1, "bind f ") == NULL, "q1 binds no f");
    CHECK(strstr(qb_body_q1, "cl_forwardspeed \"400\"") != NULL, "q1 always-run");
    CHECK(strstr(qb_body_q2, "set cl_run \"1\"") != NULL, "q2 always-run");
    CHECK(strstr(qb_body_q2, "unbindall") == NULL && strstr(qb_body_q1, "unbindall") == NULL,
          "a body never unbinds what it does not bind");
    /* the off bodies bind nothing */
    CHECK_EQ_I(qb_body_keys(qb_body_q1_off, k), 0);
    CHECK_EQ_I(qb_body_keys(qb_body_q2_off, k), 0);
}

TEST(the_switch)
{
    CHECK_EQ_I(qb_mode(0, 0), QB_MODE_WASD);            /* absent = enforce */
    CHECK_EQ_I(qb_mode(1, 1), QB_MODE_WASD);
    CHECK_EQ_I(qb_mode(1, 0), QB_MODE_OFF);             /* the neutral body */
    CHECK_EQ_I(qb_mode(1, 2), QB_MODE_HANDS_OFF);       /* not touched at all */
    CHECK_EQ_I(qb_mode(1, 7), QB_MODE_WASD);            /* a typo never turns it off */
    {
        size_t len = 99;
        CHECK(qb_body(QB_PROFILE_Q2, QB_MODE_HANDS_OFF, &len) == NULL && len == 0,
              "hands off writes nothing");
        CHECK(qb_body(QB_PROFILE_Q2, QB_MODE_OFF, &len) == qb_body_q2_off, "off = neutral body");
        CHECK(qb_body(QB_PROFILE_Q1, QB_MODE_WASD, &len) == qb_body_q1 && len == strlen(qb_body_q1),
              "wasd = the q1 body");
        CHECK(qb_body(9, QB_MODE_WASD, &len) == NULL, "an unknown profile has no body");
    }
}

TEST(file_state_is_byte_exact)
{
    const char *w = qb_body_q2;
    size_t wl = strlen(w);
    char buf[2048];
    CHECK_EQ_I(qb_file_state(0, NULL, 0, w, wl), QB_FILE_MISSING);
    CHECK_EQ_I(qb_file_state(1, w, wl, w, wl), QB_FILE_CURRENT);
    memcpy(buf, w, wl);
    buf[wl] = ' ';                                       /* a trailing space */
    CHECK_EQ_I(qb_file_state(1, buf, wl + 1, w, wl), QB_FILE_STALE);
    memcpy(buf, w, wl);
    buf[17] = 'X';                                       /* one byte different */
    CHECK_EQ_I(qb_file_state(1, buf, wl, w, wl), QB_FILE_STALE);
    CHECK_EQ_I(qb_file_state(1, "", 0, w, wl), QB_FILE_STALE);  /* empty file */
}

TEST(the_titles_are_quake_1_and_2_only)
{
    const qb_title_t *t;
    t = qb_title_find("quake2complete");
    CHECK(t && t->profile == QB_PROFILE_Q2, "case-insensitive");
    CHECK(t && t->dirs[0] && t->dirs[1] && strcmp(t->dirs[1], "ctf") == 0,
          "ThreeWave CTF gets its own file - it had no binds at all");
    t = qb_title_find("Quake1");
    CHECK(t && t->profile == QB_PROFILE_Q1 && strcmp(t->dirs[0], "ID1") == 0, "Quake1 ID1");
    t = qb_title_find("Quake2Win9x");
    CHECK(t && t->profile == QB_PROFILE_Q2 && strcmp(t->dirs[0], "baseq2") == 0, "Quake2Win9x");
    CHECK(qb_title_find("HexenII") == NULL, "Hexen II is not in scope yet");
    CHECK(qb_title_find("SiNGold") == NULL, "SiN is not in scope yet");
    CHECK(qb_title_find("Quake3") == NULL, "Quake III is not a QBINDS title");
    CHECK_EQ_I(QB_NTITLES, 3);
}

/* ---- the splitter is the engine's ------------------------------------------ */

TEST(a_semicolon_in_a_comment_still_splits_the_line)
{
    /* E10, measured on .243: the staged comment "always run; nobody wants ..."
     * printed `Unknown command "nobody"` at every start. */
    qb_cmdit_t it;
    char cmd[256];
    char argv[QB_MAX_ARGS][QB_ARG_LEN];
    int line, argc;
    const char *t = "set cl_run \"1\"   // always run; nobody wants 1997\r\nbind x \"a;b\"\r\n";
    qb_cmdit_init(&it, t, strlen(t));
    CHECK(qb_cmdit_next(&it, cmd, sizeof(cmd), &line) && line == 1, "first command");
    argc = qb_tokenize(cmd, argv);
    CHECK(argc == 3 && strcmp(argv[2], "1") == 0, "the set itself");
    CHECK(qb_cmdit_next(&it, cmd, sizeof(cmd), &line) && line == 1, "the split-off tail");
    argc = qb_tokenize(cmd, argv);
    CHECK(argc >= 1 && strcmp(argv[0], "nobody") == 0, "runs as a command: nobody");
    CHECK(qb_cmdit_next(&it, cmd, sizeof(cmd), &line) && line == 2, "next line");
    argc = qb_tokenize(cmd, argv);
    CHECK(argc == 3 && strcmp(argv[2], "a;b") == 0, "a ';' inside quotes does not split");
    CHECK(!qb_cmdit_next(&it, cmd, sizeof(cmd), &line), "end");
}

/* ---- the chains, run on the real default.cfg files -------------------------- */

TEST(quake2_the_old_staged_block_loses_the_weapon_keys_the_new_chain_keeps_them)
{
    static sim_t s;
    /* OLD (the staged autoexec until Phase 2): unbindall after config.cfg */
    s.n = 0;
    sim_exec(&s, q2_default_cfg, NULL);
    CHECK_BIND(&s, "1", "use Blaster");                 /* default.cfg gave them */
    sim_exec(&s, "", NULL);                             /* config.cfg: a fresh box */
    sim_exec(&s, q2_old_autoexec, NULL);
    CHECK_BIND(&s, "w", "+forward");
    CHECK_BIND(&s, "1", NULL);                          /* OLD-BUGGY: lost */
    CHECK_BIND(&s, "0", NULL);
    CHECK_BIND(&s, "[", NULL);
    CHECK_BIND(&s, "]", NULL);
    CHECK_BIND(&s, "ENTER", NULL);
    CHECK_BIND(&s, "UPARROW", NULL);
    CHECK_BIND(&s, "F1", NULL);

    /* NEW: autoexec execs FLEETKEY.CFG; nothing unbinds */
    s.n = 0;
    sim_exec(&s, q2_default_cfg, NULL);
    sim_exec(&s, "", NULL);
    sim_exec(&s, q2_new_autoexec, qb_body_q2);
    CHECK_BIND(&s, "w", "+forward");
    CHECK_BIND(&s, "a", "+moveleft");                   /* default: +lookup */
    CHECK_BIND(&s, "d", "+moveright");
    CHECK_BIND(&s, "s", "+back");                       /* default: use silencer */
    CHECK_BIND(&s, "e", "weapnext");                    /* default: use environment suit */
    CHECK_BIND(&s, "q", "weapprev");                    /* default: use quad damage */
    CHECK_BIND(&s, "CTRL", "+movedown");                /* default: +attack */
    CHECK_BIND(&s, "f", "invuse");
    CHECK_BIND(&s, "MOUSE2", "+moveup");                /* default: +strafe */
    CHECK_BIND(&s, "1", "use Blaster");                 /* FIXED: kept */
    CHECK_BIND(&s, "0", "use BFG10K");
    CHECK_BIND(&s, "[", "invprev");
    CHECK_BIND(&s, "]", "invnext");
    CHECK_BIND(&s, "ENTER", "invuse");
    CHECK_BIND(&s, "UPARROW", "+forward");
    CHECK_BIND(&s, "F1", "cmd help");
    CHECK_BIND(&s, "F11", "screenshot");

    /* a player's saved config.cfg is honoured for every key FLEETKEY leaves alone */
    s.n = 0;
    sim_exec(&s, q2_default_cfg, NULL);
    sim_exec(&s, "bind g \"use Hand Grenade\"\r\nbind w \"+back\"\r\n", NULL);
    sim_exec(&s, q2_new_autoexec, qb_body_q2);
    CHECK_BIND(&s, "g", "use Hand Grenade");
    CHECK_BIND(&s, "w", "+forward");                    /* re-applied at every launch */

    /* QuakeBinds=0: the neutral body changes nothing */
    s.n = 0;
    sim_exec(&s, q2_default_cfg, NULL);
    sim_exec(&s, q2_new_autoexec, qb_body_q2_off);
    CHECK_BIND(&s, "a", "+lookup");
    CHECK_BIND(&s, "w", NULL);
}

TEST(quake1_wasd_on_top_of_id_defaults_ctrl_still_fires)
{
    static sim_t s;
    s.n = 0;
    sim_exec(&s, q1_default_cfg, NULL);                 /* quake.rc: default.cfg */
    sim_exec(&s, "", NULL);                             /* config.cfg */
    sim_exec(&s, q1_new_autoexec, qb_body_q1);          /* autoexec.cfg */
    CHECK_BIND(&s, "w", "+forward");
    CHECK_BIND(&s, "d", "+moveright");                  /* default: +moveup */
    CHECK_BIND(&s, "a", "+moveleft");                   /* default: +lookup */
    CHECK_BIND(&s, "SPACE", "+jump");
    CHECK_BIND(&s, "CTRL", "+attack");                  /* kept: a DOS box may have no mouse */
    CHECK_BIND(&s, "MOUSE2", "+jump");                  /* default: +forward */
    CHECK_BIND(&s, "e", "impulse 10");
    CHECK_BIND(&s, "q", "impulse 12");
    CHECK_BIND(&s, "1", "impulse 1");                   /* id's weapon keys survive */
    CHECK_BIND(&s, "UPARROW", "+forward");
    CHECK_BIND(&s, "F11", "screenshot");                /* the library's own bind */
    CHECK(sim_get(&s, "f") == NULL, "q1 binds no f");
}

/* ---- the autoexec chain check ---------------------------------------------- */

TEST(autoexec_scan)
{
    qb_scan_t sc;
    const char *t;

    qb_autoexec_scan(q2_new_autoexec, strlen(q2_new_autoexec), QB_PROFILE_Q2, &sc);
    CHECK(sc.exec_live && sc.exec_line == 4, "exec found on line 4");
    CHECK_EQ_I(sc.unbindall, QB_UB_NONE);
    CHECK(!sc.quote_trap && sc.nrebind == 0, "clean");
    CHECK(qb_effective(1, &sc), "effective with the WASD body in place");
    CHECK(!qb_effective(0, &sc), "not effective without it");

    /* the old staged block: no exec, unbindall */
    qb_autoexec_scan(q2_old_autoexec, strlen(q2_old_autoexec), QB_PROFILE_Q2, &sc);
    CHECK(!sc.exec_live && !sc.exec_commented, "no exec at all");
    CHECK_EQ_I(sc.unbindall, QB_UB_BEFORE);
    CHECK(strcmp(qb_exec_name(1, &sc), "missing") == 0, "exec: missing");
    CHECK(strcmp(qb_exec_name(0, &sc), "no_autoexec") == 0, "no autoexec.cfg at all");

    t = "// exec fleetkey.cfg\r\n";
    qb_autoexec_scan(t, strlen(t), QB_PROFILE_Q2, &sc);
    CHECK(!sc.exec_live && sc.exec_commented, "commented out");
    CHECK(strcmp(qb_exec_name(1, &sc), "commented") == 0, "exec: commented");

    t = "EXEC \"FleetKey.CFG\"\r\n";
    qb_autoexec_scan(t, strlen(t), QB_PROFILE_Q2, &sc);
    CHECK(sc.exec_live, "quoted, any case");

    t = "set x 1 // a; exec fleetkey.cfg\r\n";
    qb_autoexec_scan(t, strlen(t), QB_PROFILE_Q2, &sc);
    CHECK(sc.exec_live, "the engine runs an exec after a ';' in a comment - so does the scan");

    t = "exec fleetkey.cfg\r\nunbindall\r\n";
    qb_autoexec_scan(t, strlen(t), QB_PROFILE_Q2, &sc);
    CHECK_EQ_I(sc.unbindall, QB_UB_AFTER);
    CHECK(!qb_effective(1, &sc), "an unbindall after the exec undoes it");

    t = "exec fleetkey.cfg\r\nbind w \"+back\"\r\nbind 1 \"use Blaster\"\r\nunbind SPACE\r\nbind w\r\n";
    qb_autoexec_scan(t, strlen(t), QB_PROFILE_Q2, &sc);
    CHECK_EQ_I(sc.nrebind, 2);                          /* w and SPACE, not 1, not `bind w` */
    CHECK(strcmp(sc.rebinds[0], "w") == 0 && strcmp(sc.rebinds[1], "SPACE") == 0, "which keys");
    CHECK(!qb_effective(1, &sc), "a fleet key rebound after the exec");

    t = "bind x \"foo\r\nexec fleetkey.cfg\r\n";
    qb_autoexec_scan(t, strlen(t), QB_PROFILE_Q2, &sc);
    CHECK(sc.quote_trap && sc.quote_line == 1, "an open quote before the exec");
    CHECK(!qb_effective(1, &sc), "a quote trap can swallow the exec");

    t = "exec fleetkey.cfg\r\nbind x \"foo\r\n";
    qb_autoexec_scan(t, strlen(t), QB_PROFILE_Q2, &sc);
    CHECK(!sc.quote_trap, "an open quote AFTER the exec cannot reach it");

    /* Q1's key set: CTRL is not a fleet key there */
    t = "exec fleetkey.cfg\r\nbind CTRL \"+jump\"\r\n";
    qb_autoexec_scan(t, strlen(t), QB_PROFILE_Q1, &sc);
    CHECK_EQ_I(sc.nrebind, 0);
    qb_autoexec_scan(t, strlen(t), QB_PROFILE_Q2, &sc);
    CHECK_EQ_I(sc.nrebind, 1);
}

MUNIT_MAIN("QBINDS - the fleet Quake key layout (agent/shared/qbinds.h, agent 1.97.0)",
    RUN(every_body_obeys_the_quake_config_rules);
    RUN(the_bodies_are_r4_a4_as_the_orchestrator_decided);
    RUN(the_switch);
    RUN(file_state_is_byte_exact);
    RUN(the_titles_are_quake_1_and_2_only);
    RUN(a_semicolon_in_a_comment_still_splits_the_line);
    RUN(quake2_the_old_staged_block_loses_the_weapon_keys_the_new_chain_keeps_them);
    RUN(quake1_wasd_on_top_of_id_defaults_ctrl_still_fires);
    RUN(autoexec_scan);
)

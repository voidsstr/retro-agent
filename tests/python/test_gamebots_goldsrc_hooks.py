"""Regression: the GoldSrc adapter's Metamod hooks must match the real HLSDK.

scripts/gamebots/adapters/goldsrc/retrobot_engine.cpp has never had a
compiler run over it in this repo's own test host (no 32-bit toolchain,
see that directory's README "Build status"), so a signature mistake in a
dllapi hook is otherwise undetectable by `tests/run_all.sh` -- it would only
surface the day someone with a working toolchain builds it, or worse, the
day it's loaded into a live game server with the wrong argument types.

This was not hypothetical. The first real compile of this file (done outside
this repo, with a manually-assembled 32-bit toolchain, see the commit this
test landed in) produced:

    error: 'clientdata_t' does not name a type; did you mean 'clientdata_s'?
    error: invalid conversion from
        'void (*)(const edict_t*, int, int*)'
      to 'void (*)(const edict_s*, int, clientdata_s*)' [-fpermissive]

The root cause: `clientdata_t` (declared in HLSDK's common/entity_state.h) was
never reachable through this file's include chain, so the compiler's error
recovery silently treated the unknown type name as `int`, and the resulting
`RB_UpdateClientData_Post(const edict_t*, int, int*)` would have been
registered into metamod's `DLL_FUNCTIONS` POST-hook table anyway if the build
had used a more permissive/older compiler -- undefined behaviour reading a
`clientdata_t*` through an `int*` pointer inside a live game server, from a
hook that fires every frame for every connected player. This test pins BOTH
sides: the fixed signature is present, and the specific broken one (`int*` in
the third parameter) is not.

Companion fix, same root cause class: dlls/util.h has NO include guard (by
HLSDK's own design -- a .cpp is meant to include it exactly once, directly).
This file both included it directly AND pulled it in transitively via
<meta_api.h> -> dllapi.h -> sdk_util.h -> <util.h>, so it was parsed twice in
one translation unit and every default-argument/class declaration in it
became a hard "redefinition" error. Pinned here too, since it is exactly the
kind of "looked fine, cost a whole build" mistake a later refactor could
reintroduce without anyone noticing until the next real compile.

Third companion: every metamod plugin must compile its OWN copy of metamod's
SDK utility routines (UTIL_LogPrintf, etc) from metamod-hl1/metamod/sdk_util.cpp
-- metamod ships no shared runtime for plugins to link against. Missing this
builds and *links* a .so successfully (a shared object is allowed unresolved
symbols at build time) but the plugin fails to dlopen at runtime with
"undefined symbol: UTIL_LogPrintf". Confirmed with an actual dlopen smoke
test against the built retrobot.so. Pinned here at the Makefile level since
there is no HLSDK checkout in this repo to compile against and verify it
directly.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GOLDSRC = REPO / "scripts" / "gamebots" / "adapters" / "goldsrc"
ENGINE_CPP = GOLDSRC / "retrobot_engine.cpp"
MAKEFILE = GOLDSRC / "Makefile"


def _source():
    return ENGINE_CPP.read_text()


def test_engine_source_exists():
    assert ENGINE_CPP.is_file(), (
        "scripts/gamebots/adapters/goldsrc/retrobot_engine.cpp is missing -- "
        "this whole test file is about pinning invariants in it"
    )


def test_update_client_data_hook_has_the_real_signature():
    """Matches HLSDK engine/eiface.h's
    `void (*pfnUpdateClientData)(const struct edict_s *ent, int sendweapons,
    struct clientdata_s *cd)` field-for-field: same argument count, same
    const-ness, same pointee types (edict_t is a typedef for struct edict_s,
    clientdata_t for struct clientdata_s)."""
    src = _source()
    m = re.search(
        r"RB_UpdateClientData_Post\s*\(([^)]*)\)\s*\n?\s*\{",
        src,
    )
    assert m, "RB_UpdateClientData_Post's definition was not found at all"
    params = m.group(1)

    assert "const edict_t" in params, (
        "first parameter must stay `const edict_t *ent` -- eiface.h declares "
        "it const"
    )
    assert re.search(r"\bint\s+sendweapons\b", params), (
        "second parameter must stay a plain `int sendweapons`"
    )
    assert "clientdata_t" in params, (
        "third parameter must be `clientdata_t *cd` -- this is the exact "
        "field that broke: clientdata_t comes from HLSDK's "
        "common/entity_state.h, which nothing in this file's original "
        "include chain reached, so the compiler treated the unknown type "
        "name as `int` instead of failing loudly at the RIGHT place"
    )


def test_update_client_data_hook_is_not_the_known_broken_signature():
    """The literal broken signature the first real compile produced. If this
    ever matches again, the entity_state.h include regressed."""
    src = _source()
    m = re.search(
        r"RB_UpdateClientData_Post\s*\(([^)]*)\)\s*\n?\s*\{",
        src,
    )
    assert m
    params = m.group(1)
    # The broken build had a bare `int *cd` (or `int* cd`) for the third
    # parameter where `clientdata_t *cd` belongs.
    broken = re.search(r",\s*int\s*\*\s*cd\b", params)
    assert not broken, (
        "RB_UpdateClientData_Post's third parameter regressed to the "
        "'clientdata_t silently became int' broken signature -- see this "
        "file's module docstring for the exact compiler error this produces"
    )


def test_entity_state_header_is_included():
    """The actual fix: entity_state.h is what declares clientdata_t (and
    pulls in weapon_data_t via weaponinfo.h) -- nothing in extdll.h's own
    include chain reaches it."""
    src = _source()
    assert re.search(r'#include\s*[<"]entity_state\.h[>"]', src), (
        "retrobot_engine.cpp must #include <entity_state.h> -- it is the "
        "only thing in the HLSDK that declares clientdata_t, and its "
        "absence is exactly what turned 'clientdata_t *cd' into 'int *cd' "
        "under the compiler's error recovery (see this file's module "
        "docstring)"
    )


def test_util_h_is_not_included_directly():
    """dlls/util.h has no #ifndef include guard -- by HLSDK's own design, a
    .cpp includes it exactly once, directly. <meta_api.h> already pulls it
    in transitively (meta_api.h -> dllapi.h -> sdk_util.h -> <util.h>), so
    this file must never ALSO include it directly: doing so parses an
    unguarded header twice in one translation unit, and every default
    argument and class declaration in it becomes a hard 'redefinition'
    error. Confirmed by hitting exactly that the first time a compiler saw
    this file."""
    src = _source()
    direct_includes = re.findall(r'^\s*#include\s*[<"]util\.h[>"]', src, re.M)
    assert not direct_includes, (
        "retrobot_engine.cpp must not #include <util.h> directly -- it "
        "arrives already, transitively, via <meta_api.h>; a direct include "
        "double-parses a header with no include guard and fails to build "
        "(see this file's module docstring for the exact errors)"
    )


def test_util_logprintf_redeclared_with_the_real_const_signature():
    """metamod-hl1's own dlls/util.h declares
    `UTIL_LogPrintf(char *fmt, ...)` (non-const), but the only actual
    implementation any plugin links against -- metamod-hl1/metamod/
    sdk_util.cpp -- defines `UTIL_LogPrintf(const char *fmt, ...)`. In C++
    those are two different overloads (different mangled names). A call
    site that only sees dlls/util.h's declaration links "successfully" (a
    .so is allowed unresolved symbols at build time) but fails to dlopen at
    runtime with "undefined symbol: _Z14UTIL_LogPrintfPcz". This file must
    redeclare the real (const) signature so every call site binds to the
    overload that actually exists. Confirmed with an actual dlopen smoke
    test against the built retrobot.so."""
    src = _source()
    assert re.search(
        r"UTIL_LogPrintf\s*\(\s*const\s+char\s*\*\s*fmt\s*,\s*\.\.\.\s*\)",
        src,
    ), (
        "retrobot_engine.cpp must redeclare "
        "`UTIL_LogPrintf(const char *fmt, ...)` (matching sdk_util.cpp's "
        "real, const-qualified definition) -- without it, every "
        "UTIL_LogPrintf(\"...\") call site binds to dlls/util.h's "
        "non-const declaration, which has no matching definition anywhere "
        "a plugin links against, and the .so fails to dlopen"
    )


def test_makefile_compiles_metamods_own_sdk_util_cpp():
    """metamod ships no shared runtime for plugins to link against -- every
    plugin compiles its OWN copy of metamod's SDK utility routines
    (UTIL_LogPrintf and friends) from metamod-hl1/metamod/sdk_util.cpp.
    metamod-hl1's own stub_plugin lists sdk_util.cpp as one of ITS sources
    for exactly this reason. Missing it is invisible at build time (the
    functions are only USED, never redefined, in retrobot_engine.cpp) and
    only shows up as a dlopen-time "undefined symbol" -- confirmed with an
    actual dlopen smoke test against the built retrobot.so."""
    mk = MAKEFILE.read_text()
    assert "sdk_util.cpp" in mk, (
        "the Makefile must compile metamod-hl1/metamod/sdk_util.cpp into "
        "retrobot.so -- without it, UTIL_LogPrintf and the other metamod "
        "SDK utility routines are declared but never defined anywhere the "
        "plugin links against"
    )
    assert re.search(r"\$\(BUILD\)/sdk_util\.o", mk), (
        "sdk_util.cpp must actually be compiled into an object that lands "
        "in $(OBJS) / the final link -- listing the source without a build "
        "rule feeding the link step does not fix the missing symbol"
    )


def test_bot_registration_happens_at_createfakeclient_time():
    """Live-test finding (48/1.1.2.7/Stdio engine build): pfnClientPutInServer
    NEVER fired for a pfnCreateFakeClient()-created edict, confirmed by an
    unconditional entry log on that hook that never printed a single line
    across multiple bot creations on a live throwaway HLDS -- while the same
    UTIL_LogPrintf call worked fine from other dllapi hooks in the same run,
    and the DLL_FUNCTIONS table order was independently re-verified against
    eiface.h. Net effect before the fix: a bot was created (visible in
    `status`) but never registered into g_bots[], so RB_StartFrame_Post's
    loop saw zero bots, gb_exchange() was never called with a nonzero batch,
    and the policy server logged zero adapter connections -- forever, with
    no error anywhere. Registering immediately after pfnCreateFakeClient
    returns (in RB_Cmd_AddBot) does not depend on that callback firing at
    all; RB_ClientPutInServer_Post still calls the same (idempotent)
    rb_register_bot() for engine builds where it DOES fire."""
    src = _source()
    m = re.search(r"static void RB_Cmd_AddBot\s*\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
    assert m, "RB_Cmd_AddBot's definition was not found"
    body = m.group(1)
    assert "rb_register_bot(" in body, (
        "RB_Cmd_AddBot must call rb_register_bot() itself, immediately "
        "after pfnCreateFakeClient() succeeds -- waiting for "
        "RB_ClientPutInServer_Post to do it is exactly the bug that "
        "produced zero policy-server connections on a live 48/1.1.2.7/"
        "Stdio HLDS (see this test's docstring)"
    )
    assert re.search(r"rb_register_bot\s*\(\s*pEntity\s*\)", src), (
        "RB_ClientPutInServer_Post must still call rb_register_bot() too "
        "(idempotent -- see rb_register_bot's own re-registration guard) "
        "for engine builds where that hook DOES fire for a fake client"
    )


def test_bot_edict_is_reresolved_by_index_not_cached_across_frames():
    """Live-test finding: the adapter ran a fake client for 8+ minutes
    across multiple forced round restarts (mp_restartgame, sv_restartround)
    plus the natural mp_roundtime expiry with no crash once this was fixed
    -- but an earlier live run of this adapter crashed HLDS with a
    segfault roughly 8 seconds after a "Round_End" log line, while a
    fakeclient that had never successfully joined a team sat in the
    registry holding a bare cached edict_t* across frames. Re-resolving
    the edict fresh from its stable index every time it is used (rather
    than trusting a pointer copied at registration time) is the fix --
    see rb_resolve_bot_edict()'s own comment for why this matters even
    though GoldSrc's edict array itself is never reallocated."""
    src = _source()
    assert "rb_resolve_bot_edict" in src, (
        "retrobot_engine.cpp must define rb_resolve_bot_edict() -- see "
        "this test's docstring for the crash it exists to prevent"
    )
    # The specific broken pattern a previous version of this file used:
    # trusting a bare cached ->ed pointer's ->free field directly, with no
    # re-resolution and no check that it still belongs to a fake client.
    broken = re.search(r"!\s*b->ed\s*\|\|\s*b->ed->free", src)
    assert not broken, (
        "RB_StartFrame_Post regressed to checking a cached b->ed pointer "
        "directly instead of calling rb_resolve_bot_edict() -- see this "
        "test's docstring for the crash this reintroduces"
    )
    m = re.search(r"static void RB_StartFrame_Post\s*\(void\)\s*\{(.*?)\n\}", src, re.S)
    assert m, "RB_StartFrame_Post's definition was not found"
    assert "rb_resolve_bot_edict(" in m.group(1), (
        "RB_StartFrame_Post's per-bot loop must resolve each bot's edict "
        "via rb_resolve_bot_edict() before touching it"
    )
    m2 = re.search(r"static void RB_Cmd_Debug\s*\(void\)\s*\{(.*?)\n\}", src, re.S)
    assert m2, "RB_Cmd_Debug's definition was not found"
    assert "rb_resolve_bot_edict(" in m2.group(1), (
        "RB_Cmd_Debug must also resolve via rb_resolve_bot_edict() -- an "
        "admin diagnostic command dereferencing a stale cached pointer is "
        "the same bug in a different call site"
    )


def test_silent_failures_are_reported_once_on_transition_not_per_frame():
    """The literal ask that came out of live testing: "a bot that exists but
    is skipped every frame should say so once (not per frame) -- the same
    report-only-on-transition pattern the Quake III adapter uses"
    (adapters/quake3/gb_adapter.c's gb_reported_state). Pins three
    latches: the exchange-level policy-server-availability state, the
    per-bot alive/dead state, and the specific stuck-unassigned warning
    that would have made the original silent bug (a bot connected but
    never registered, later: registered but never leaving team 0) visible
    in the server log the first time, instead of requiring
    `retrobot_debug` to be run by hand to notice at all."""
    src = _source()
    assert "g_gb_reported_state" in src, (
        "expected a report-on-transition latch for the policy-server "
        "exchange result, matching adapters/quake3/gb_adapter.c's "
        "gb_reported_state"
    )
    assert "reported_alive" in src, (
        "expected a per-bot report-on-transition latch for the alive/dead "
        "skip in RB_StartFrame_Post's loop"
    )
    assert "rb_maybe_report_stuck" in src, (
        "expected the stuck-unassigned-after-a-grace-period warning "
        "(rb_maybe_report_stuck) -- this is what makes a bot that never "
        "leaves team 0 (the exact silent failure hit during live testing) "
        "show up in the server log on its own"
    )
    assert "reported_stuck_unassigned" in src, (
        "rb_maybe_report_stuck must latch so it reports exactly once per "
        "bot, not every frame"
    )

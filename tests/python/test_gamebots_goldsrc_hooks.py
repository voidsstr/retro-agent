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

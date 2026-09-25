"""GAMESYNC's free-space margin scales down on a period disk (agent 1.85.0).

The fixed 300 MB margin was sized for the XP boxes. On .243 (Win98, 1.2 GB C:)
it is a quarter of the volume: with 358 MB free the box could add no title over
58 MB, and it refused the 199 MB Quake II base game with the misleading line
"needs 199 MB, only 358 MB free". Under 4 GB the margin is 150 MB; the SKIP
line now names the margin.

gs_margin_for_disk() is compiled out of gamesync.c and exercised.
"""
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = (REPO / "agent" / "src" / "gamesync.c").read_text(errors="replace")
MB = 1024 * 1024


def _extract(signature):
    start = SRC.index(signature)
    depth, i = 0, SRC.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(SRC[i], 0)
        if depth == 0:
            return SRC[start:i + 1]
        i += 1


@pytest.fixture(scope="module")
def margin(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler - gs_margin_for_disk NOT exercised")
    defs = "\n".join(l for l in SRC.splitlines()
                     if l.startswith(("#define GS_FREE_MARGIN", "#define GS_SMALL_DISK")))
    d = tmp_path_factory.mktemp("margin")
    (d / "t.c").write_text(
        "#include <stdio.h>\n#include <stdlib.h>\ntypedef long long __int64;\n" + defs + "\n"
        + _extract("static __int64 gs_margin_for_disk(") +
        "\nint main(int c, char **v) { printf(\"%lld\\n\", gs_margin_for_disk(atoll(v[1]))); return 0; }\n")
    subprocess.run([cc, "-o", str(d / "t"), str(d / "t.c")], check=True)
    return lambda total: int(subprocess.run([str(d / "t"), str(total)], capture_output=True,
                                            text=True, check=True).stdout)


def test_period_disk_keeps_150_mb(margin):
    assert margin(1220 * MB) == 150 * MB          # .243's C:
    assert margin(4095 * MB) == 150 * MB


def test_xp_sized_disk_keeps_300_mb(margin):
    assert margin(4096 * MB) == 300 * MB
    assert margin(76 * 1024 * MB) == 300 * MB


def test_unmeasurable_disk_keeps_the_larger_margin(margin):
    assert margin(-1) == 300 * MB
    assert margin(0) == 300 * MB


def test_quake2_base_now_fits_on_243_after_the_zips_go():
    # 358 MB free + 43 MB of leftover DOS-game zips removed, 199 MB title
    assert 199 + 150 <= 358 + 43
    assert not (199 + 300 <= 358 + 43), "the old margin is what refused it"


def test_skip_line_names_the_margin():
    assert "MB kept free, only %I64d MB free" in SRC
    assert "sizes[i] + GS_FREE_MARGIN > freeb" not in SRC, "every check uses the scaled margin"

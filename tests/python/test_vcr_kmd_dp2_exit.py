"""vcr-kmd D3D HAL: a DX5 execute buffer's D3DOP_EXIT ends the DP2 stream
(clean-room lane, 2026-09-28).

DP2 reuses the execute buffer's opcode numbers (POINT/LINE/TRIANGLE/STATERENDER
= 1/2/3/8), so a DX5 execute-buffer title - Jedi Knight: Dark Forces II - hands
the driver a stream that ends in D3DOP_EXIT (11). The walker answered
D3DERR_COMMAND_UNPARSED at it: 47 times in one run on .124 (V5 6000), each 4
bytes before the end of its buffer, and D3DIM's fallback then crashed JK.EXE
(rep movsd to 0xbf7ffffc under D3DRealloc). EXIT must end the walk with DD_OK.
"""
import re
from pathlib import Path

SRC = (Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "vcr-kmd" / "display"
       / "vcrdd_d3d.c").read_text()


def test_exit_is_opcode_11():
    assert re.search(r"#define VCR_D3DOP_EXIT 11\b", SRC)


def test_exit_ends_the_walk_successfully_before_the_unknown_path():
    case = SRC.index("case VCR_D3DOP_EXIT:")
    body = SRC[case:SRC.index("case D3DNTDP2OP_EXT:", case)]
    assert "return DD_OK;" in body
    assert "COMMAND_UNPARSED" not in body.split("*/")[-1]
    # it sits in the same switch as the default that answers UNPARSED
    assert case < SRC.index("return D3DNTERR_COMMAND_UNPARSED;", case)

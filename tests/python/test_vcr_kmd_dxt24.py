"""vcr-kmd D3DBigTex: DXT2 and DXT4 are listed as DXT3's and DXT5's hardware
formats (2026-09-28).

DXT2/DXT4 are DXT3/DXT5 blocks with premultiplied colour - the same bits, and
the VSA-100's compressed formats 2 (DXT2/3) and 3 (DXT4/5). Halo 1.10 stores
its compressed alpha textures as DXT2/DXT4; with only DXT1/3/5 listed they
came back NULL and its main menu drew white on .124. With the aliases the menu
renders textured.
"""
from pathlib import Path

SRC = (Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "vcr-kmd" / "display"
       / "vcrdd_d3d.c").read_text()


def test_dxt2_and_dxt4_map_to_the_dxt3_and_dxt5_hardware_formats():
    fn = SRC[SRC.index("static ULONG dxt_class(DWORD fcc)"):]
    fn = fn[:fn.index("\n}\n")]
    assert "fcc == FCC_DXT2 || fcc == FCC_DXT3 ? TF_CMP_DXT23" in fn
    assert "fcc == FCC_DXT4 || fcc == FCC_DXT5 ? TF_CMP_DXT45" in fn


def test_every_list_carries_all_five():
    fourcc = SRC[SRC.index("ULONG VcrDdD3dFourCC("):]
    fourcc = fourcc[:fourcc.index("\n}\n")]
    for n in "12345":
        assert f"FCC_DXT{n};" in fourcc
    assert "return 5;" in fourcc
    for n in "12345":
        assert f"texfmt_fcc(&g_texfmt[g_gd.dwNumTextureFormats++], FCC_DXT{n});" in SRC
    # 3 16 bpp + A8R8G8B8 + 5 DXT = 9 entries
    assert "static DDSURFACEDESC g_texfmt[9];" in SRC


def test_no_path_tests_dxt3_or_dxt5_by_name_any_more():
    body = SRC.replace("#define FCC_DXT3", "").replace("#define FCC_DXT5", "")
    body = body[:body.index("static ULONG dxt_class")] + body[body.index("ULONG VcrDdD3dFourCC("):]
    # outside dxt_class and the two lists, DXT3/5 must not be special-cased
    for line in body.splitlines():
        if ("FCC_DXT3" in line or "FCC_DXT5" in line) and "texfmt_fcc" not in line and "codes[" not in line:
            raise AssertionError("a DXT3/5-only check left: " + line.strip())

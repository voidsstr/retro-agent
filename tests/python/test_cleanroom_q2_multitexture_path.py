"""voodoo-cleanroom ICD 0.1.71-0.1.73: Quake II's single-pass (SGIS) path.

Quake II with GL_SGIS_multitexture ran 4x SLOWER than two-pass on the V5 6000
(50.8 vs 213 fps, 640x480, 4 chips) - the "fixed CPU wall" 0.1.57/0.1.58 could
not locate on the Voodoo 2. The 0.1.67 sampling profiler named it in one run,
and three fixes took single-pass to 184.2 fps at 640x480 and 162.6 at
1024x768 (two-pass: 228.5 / 132.1). Every fix here is pixel-identical on the
box (scripts/benchmarks/icd_frame_compare.py).

* 0.1.71: glTexSubImage2D re-sent the WHOLE mip level (55 % of CPU). Now only
  the changed rows, via a rewritten fxTMReloadSubMipMapLevel (Glide3 LOD, and
  a first-row pointer in BYTES - the old one was in 16-bit units, half the
  offset for a 32-bit texture).
* 0.1.72: fx_glSelectTextureSGIS read an env var on every call (XP msvcrt's
  locale-aware getenv: ~27 % of the frame).
* 0.1.73: glActiveTexture/glClientActiveTexture flushed the buffered vertices
  on every unit switch - two per surface in Quake II.
"""
import re
from pathlib import Path

PATCH = Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "patches" / "mesafx-voodoo2-icd.patch"


def _post(name):
    text = PATCH.read_text(errors="replace")
    m = re.search(rf"^diff --git a/{re.escape(name)} .*?(?=^diff --git |\Z)", text, re.S | re.M)
    assert m, f"{name} not in the ICD patch"
    return "\n".join(l[1:] for l in m.group(0).splitlines()
                     if l[:1] in "+ " and not l.startswith("+++"))


def _fn(src, header):
    return src.split(header, 1)[1].split("\n}", 1)[0]


def test_texsubimage_reloads_only_the_changed_rows():
    t = _post("src/mesa/drivers/glide/fxddtex.c")
    blk = t.split('fullTexSub = getenv("FX_FULL_TEXSUB") != NULL;', 1)[1][:500]
    assert "fxTMReloadSubMipMapLevel(fxMesa, texObj, level, yoffset, height);" in blk
    # rescaled or compressed levels keep the full reload
    assert "!texImage->IsCompressed" in blk and "mml->wScale == 1 && mml->hScale == 1" in blk


def test_partial_reload_uses_glide3_lod_and_a_byte_row_pointer():
    m = _post("src/mesa/drivers/glide/fxtexman.c")
    body = _fn(m, "fxTMReloadSubMipMapLevel(fxMesaContext fxMesa,")
    assert "lodlevel = ti->info.largeLodLog2 - (level - ti->minLevel);" in body
    assert "(GLubyte *) texImage->Data" in body and "TexFormat->TexelBytes" in body
    assert "(GLushort *) texImage->Data + yoffset" not in body       # the 16-bit-unit bug
    assert body.count("grTexDownloadMipMapLevelPartial(") == 5       # 1 + split 2 + both 2


def test_sgis_select_reads_its_env_var_once():
    w = _post("src/mesa/drivers/glide/fxwgl.c")
    body = _fn(w, "static void APIENTRY fx_glSelectTextureSGIS(GLenum target)")
    assert "static int noClientTex = -1;" in body
    assert body.count('getenv("FX_SGIS_NO_CLIENTTEX")') == 1
    assert "if (noClientTex < 0)" in body


def test_unit_selection_marks_state_dirty_instead_of_flushing():
    t = _post("src/mesa/main/texstate.c")
    assert "if (lazy_unit_select())\n      ctx->NewState |= _NEW_TEXTURE;\n   else\n      FLUSH_VERTICES(ctx, _NEW_TEXTURE);" in t
    assert "      if (ctx->Array.ActiveTexture == texUnit)\n         return;\n      ctx->NewState |= _NEW_ARRAY;" in t
    assert '_mesa_getenv("MESA_NO_LAZY_UNIT_SELECT")' in t


def test_sub_rect_uploads_only_on_a_glide_with_the_fixed_row_ext():
    """0.1.74: patches narrower than the level go as sub-rows via
    grTexDownloadMipMapLevelPartialRowExt - ONLY when the Glide advertises
    RETRO3DFX_PARTIALROW (other Glides carry the min_s alignment bug), with a
    stdcall pointer (the ext is _grTexDownloadMipMapLevelPartialRowExt@44)."""
    m = _post("src/mesa/drivers/glide/fxtexman.c")
    assert 'strstr(ext, " RETRO3DFX_PARTIALROW ")' in m
    assert "typedef FxBool (FX_CALL *fxPartialRowProc)" in m
    body = _fn(m, "fxTMReloadSubRect(fxMesaContext fxMesa, struct gl_texture_object *tObj,")
    assert "if (!row || !ti->validated || !ti->isInTM)\n      return GL_FALSE;" in body
    assert "(GLuint) t * pitch" in body          # the ext wants the START of row t
    t = _post("src/mesa/drivers/glide/fxddtex.c")
    assert "!fxTMReloadSubRect(fxMesa, texObj, level, xoffset, yoffset, width, height))" in t

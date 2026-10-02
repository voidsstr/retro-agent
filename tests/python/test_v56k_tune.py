"""v56k_tune.set_cvars: the per-box game tuning edits the games' own config files.

Quake II writes CRLF; the first version of the line pattern ended at `$`
without allowing the CR, read every Quake II cvar as missing and would have
APPENDED a duplicate of each (caught by the dry run, 2026-10-02)."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("v56k_tune", ROOT / "scripts" / "benchmarks" / "v56k_tune.py")
tune = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tune)


def test_crlf_lines_are_changed_in_place_and_keep_their_cr():
    text = 'set gl_ext_palettedtexture "1"\r\nset gl_texturemode "GL_LINEAR_MIPMAP_NEAREST"\r\n'
    new, ch = tune.set_cvars(text, tune.Q2)
    assert ch == [("gl_ext_palettedtexture", "1", "0"), ("gl_texturemode", "GL_LINEAR_MIPMAP_NEAREST", tune.TRILINEAR)]
    assert new == 'set gl_ext_palettedtexture "0"\r\nset gl_texturemode "GL_LINEAR_MIPMAP_LINEAR"\r\n'


def test_missing_cvar_is_appended_with_the_files_own_verb():
    new, ch = tune.set_cvars('seta r_picmip "2"\n', {"r_picmip": "0", "r_vertexLight": "0"})
    assert new == 'seta r_picmip "0"\nseta r_vertexLight "0"\n'
    new, _ = tune.set_cvars('set vid_ref "gl"\r\n', {"gl_texturemode": tune.TRILINEAR})
    assert new.endswith('set gl_texturemode "GL_LINEAR_MIPMAP_LINEAR"\r\n')


def test_a_tuned_file_changes_nothing():
    once, _ = tune.set_cvars('set gl_ext_palettedtexture "1"\r\n', tune.Q2)
    twice, ch = tune.set_cvars(once, tune.Q2)
    assert ch == [] and twice == once


def test_renderer_values_with_spaces_and_brackets_round_trip():
    r = tune.RENDERER_FMT.format(ver="0.1.82")
    text = 'seta r_lastValidRenderer "Mesa Glide v0.62 Voodoo5 6000 (tm) [voodoo-cleanroom 0.1.80]"\n'
    new, ch = tune.set_cvars(text, {"r_lastValidRenderer": r})
    assert new == f'seta r_lastValidRenderer "{r}"\n' and len(ch) == 1

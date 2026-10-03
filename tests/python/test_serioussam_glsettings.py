"""The First Encounter's GLSettings.lst knows the clean-room ICD (2026-10-03).

TFE matches a 3dfx card by a "3Dfx*" VENDOR; the MesaFX ICD says "Brian Paul".
Unmatched, TFE took its generic "*Voodoo*" line (the Voodoo Graphics profile)
and, through Default.ini -> Initial.ini, ogl_bExclusive = 0 - a WS_CHILD
canvas that Glide's DirectDraw exclusive mode refuses. scripts/fleet/
stage-serioussam.py adds "Brian Paul" entries ahead of the "3Dfx*" block whose
scripts include the card's own TFE profile and set ogl_bExclusive = 1.
"""
import importlib.util
import os

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIB = '/mnt/retro-share/Files/Games-Library/SeriousSamFirstEncounter'


def _ss():
    spec = importlib.util.spec_from_file_location(
        'stage_serioussam_gl', os.path.join(REPO, 'scripts', 'fleet', 'stage-serioussam.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


SAMPLE = ('\r\n"ATI*"     "*"                  "*" "ATI chipset"                    "ATI-R128.ini"\r\n\r\n'
          '"3Dfx*"    "*Voodoo5*"          "*" "3Dfx Voodoo5"                   "3Dfx-V5.ini"\r\n'
          '"*"        "*Voodoo*"           "*" "Voodoo Graphics"                "3Dfx-V1.ini"\r\n'
          '"*"        "*"                  "*" "unrecognized 3D accelerator"    "Default.ini"\r\n')


def test_the_mesa_entries_go_before_every_voodoo_line_and_only_once():
    m = _ss()
    out = m.tfe_glsettings_lst(SAMPLE)
    lines = out.split('\r\n')
    first_mesa = next(i for i, l in enumerate(lines) if l.startswith('"Brian Paul"'))
    assert first_mesa < next(i for i, l in enumerate(lines) if l.startswith('"3Dfx*"'))
    assert first_mesa < next(i for i, l in enumerate(lines) if '"*Voodoo*"' in l and l.startswith('"*"'))
    assert m.tfe_glsettings_lst(out) == out                    # idempotent
    assert out.count('"Brian Paul"') == len(m.TFE_GLSET_ENTRIES)
    assert '\r\n' in out and '\n' not in out.replace('\r\n', '')   # CRLF kept
    # a Voodoo5 / Voodoo4 maps to the V5 profile, the catch-all comes last
    mesa = [l for l in lines if l.startswith('"Brian Paul"')]
    assert mesa[0].startswith('"Brian Paul" "*Voodoo5*"') and mesa[0].endswith('"3Dfx-V5-Mesa.ini"')
    assert mesa[-1].startswith('"Brian Paul" "*Voodoo*" ')


def test_every_mesa_script_includes_the_cards_profile_and_sets_exclusive():
    m = _ss()
    named = {l.split('"')[-2] for l in m.TFE_GLSET_ENTRIES}
    files = {os.path.basename(k): v for k, v in m.TFE_GLSET_SCRIPTS.items()}
    assert named == set(files)
    for name, body in files.items():
        base = name.replace('-Mesa', '')
        assert 'include "Scripts\\GLSettings\\%s";' % base in body, name
        assert body.rstrip().endswith('ogl_bExclusive = 1;'), name
        assert body.index('include') < body.index('ogl_bExclusive = 1;')   # after the profile


def test_the_staged_tree_carries_them():
    if not os.path.isdir(LIB):
        pytest.skip('SHARE NOT MOUNTED - cannot check the staged GLSettings.lst')
    m = _ss()
    with open(os.path.join(LIB, 'Scripts', 'GLSettings', 'GLSettings.lst'), 'rb') as f:
        lst = f.read().decode('latin1')
    assert m.tfe_glsettings_lst(lst) == lst, 'GLSettings.lst lacks the Mesa Glide entries'
    for rel, body in m.TFE_GLSET_SCRIPTS.items():
        with open(os.path.join(LIB, *rel.split('/')), 'rb') as f:
            assert f.read() == body.replace('\n', '\r\n').encode('latin1'), rel

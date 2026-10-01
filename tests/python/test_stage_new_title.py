"""scripts/fleet/stage-new-title.py - a NEW library title appears WHOLE or not at all.

retro-autodeploy answers a new top-level library directory with GAMESYNC RESET +
START on every box within ~90 s, and GAMESYNC never deletes - so a title must
never be visible half-uploaded. The tool uploads to _incoming/<Title> (a
support dir the agent skips), verifies the whole tree, and promotes it with one
rename. First used 2026-10-01 for WCProphecy (49 files, 1,792,595,623 bytes,
verified before and after the rename).
"""
import importlib.util
import os

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
spec = importlib.util.spec_from_file_location(
    'stage_new_title', os.path.join(REPO, 'scripts', 'fleet', 'stage-new-title.py'))
snt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snt)


@pytest.mark.parametrize('name,ok', [('WCProphecy', True), ('DOS-ScreamerRally', True),
                                     ('Flight-WC.v2', True), ('_incoming', False), ('', False),
                                     ('a/b', False), ('has space', False), ('..', False)])
def test_title_names(name, ok):
    assert bool(snt.NAME.match(name)) is ok


def test_verify_tree_catches_missing_changed_and_extra(tmp_path):
    local = tmp_path / 'local'
    share = tmp_path / 'share'
    (local / 'SUB').mkdir(parents=True)
    (share / 'SUB').mkdir(parents=True)
    for root in (local, share):
        (root / 'A.TXT').write_bytes(b'one')
        (root / 'SUB' / 'B.BIN').write_bytes(b'two')
    files = snt.local_files(str(local))
    assert snt.verify_tree(str(share), files, lambda m: None) == []
    (share / 'SUB' / 'B.BIN').write_bytes(b'TWO')               # same size, other bytes
    assert any(b.startswith('differs') for b in snt.verify_tree(str(share), files, lambda m: None))
    (share / 'SUB' / 'B.BIN').write_bytes(b'two')
    (share / 'EXTRA').write_bytes(b'x')                           # a file the tree does not have
    assert any('file count' in b for b in snt.verify_tree(str(share), files, lambda m: None))
    os.remove(share / 'EXTRA')
    os.remove(share / 'A.TXT')
    assert any(b.startswith('missing') for b in snt.verify_tree(str(share), files, lambda m: None))


def test_refuses_an_existing_title(tmp_path, monkeypatch):
    lib = tmp_path / 'Files' / 'Games-Library' / 'Quake1'
    lib.mkdir(parents=True)
    tree = tmp_path / 'tree'
    tree.mkdir()
    (tree / 'x').write_bytes(b'1')
    monkeypatch.setattr(snt, 'MNT', str(tmp_path))
    with pytest.raises(SystemExit) as e:
        snt.main([str(tree), 'QUAKE1', '--dry-run'])               # case-insensitive
    assert 'already exists' in str(e.value)

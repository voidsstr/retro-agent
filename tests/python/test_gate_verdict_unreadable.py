"""GAMESYNC refuses a run whose published gate verdicts cannot be READ (agent 1.97.1).

Only "the file is not there" may fall back to the local rules: those cannot see
an operator override, and on the W98BUILD VM (2026-10-01, SMB error 53) the
fallback planned 18 GB of titles the profile had ejected. The decision is
agent/shared/verdictread.h (native/test_verdictread.c); this pins the wiring.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GS = open(os.path.join(REPO, 'agent', 'src', 'gamesync.c'), encoding='latin-1').read()


def test_gate_init_reports_an_unreadable_file():
    body = GS[GS.index('static int gs_gate_init(const char *library)'):]
    body = body[:body.index('static void gs_gate_free(void)')]
    assert 'gs_slurp_vr(path' in body and 'gs_slurp(path' not in body
    assert 'VR_TRIES' in body and 'return -1;' in body
    # the "not published" log line is reachable only for VR_ABSENT
    assert body.index('if (vr == VR_UNREADABLE)') < body.index('not published for this')


def test_the_run_refuses_before_it_reads_or_copies_anything():
    i = GS.index('if (gs_gate_init(library) < 0) {')
    tail = GS[i:]
    ret = tail.index('return;')
    assert 'GS_FAILED' in tail[:ret] and 'NOT SYNCING' in tail[:ret]
    # the refusal comes before the monitor probe, the library listing and any copy
    assert i < GS.index('gameres_probe();', i)
    assert i < GS.index('gs_set_msg("enumerating library");', i)
    assert len(re.findall(r'gs_gate_init\(', GS)) == 2   # the definition and this one call

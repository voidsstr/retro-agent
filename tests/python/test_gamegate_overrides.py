"""gamegate operator overrides (scripts/gamegate/overrides.txt, 2026-09-28).

A driver switch can change what a card offers where HWPROFILE cannot see it:
.124's V5 6000 runs Halo only with vcr-kmd's D3D32 + D3DBigTex armed, while
the rules (rightly, from what they can see) say "fixed < sm1.x". An override
is keyed on the hardware profile and title, applied after the rules, and
published into that box's verdict file.
"""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from gamegate import gamegate as g      # noqa: E402
from gamegate import rules              # noqa: E402


class P:
    profile_hash = "dec95d42ad08a34d"


class Other:
    profile_hash = "0000000000000000"


def test_the_shipped_file_parses_and_names_124s_halo():
    ov = g.load_overrides()
    assert ("dec95d42ad08a34d", "halo") in ov
    assert ov[("dec95d42ad08a34d", "halo")][0] == rules.VERDICT_VALUE["run"]


def test_an_override_touches_only_its_own_profile_and_title():
    ov = {("dec95d42ad08a34d", "halo"): (rules.VERDICT_VALUE["run"], "why")}
    no = rules.Decision(verdict=rules.VERDICT_VALUE["no"], limiting="gpu_feature_level")
    d = g.apply_override(P, "Halo", no, ov)
    assert d.verdict == rules.VERDICT_VALUE["run"] and d.decided_by == "override"
    assert "operator override" in d.reason
    assert g.apply_override(Other, "Halo", no, ov) is no
    assert g.apply_override(P, "Halo2", no, ov) is no


def test_a_malformed_line_is_an_error_not_a_silent_skip(tmp_path):
    f = tmp_path / "o.txt"
    f.write_text("dec95d42ad08a34d\tHalo\tyes please\tbad verdict\n")
    with pytest.raises(SystemExit):
        g.load_overrides(f)
    f.write_text("dec95d42ad08a34d Halo run spaces not tabs\n")
    with pytest.raises(SystemExit):
        g.load_overrides(f)


def test_comments_and_blank_lines_are_ignored(tmp_path):
    f = tmp_path / "o.txt"
    f.write_text("# note\n\n")
    assert g.load_overrides(f) == {}

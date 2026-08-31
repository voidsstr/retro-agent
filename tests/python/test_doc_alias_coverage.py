"""Every title in the LAN document must map to a library directory.

The document names titles the way a person would ("Serious Sam: The First
Encounter"); the matrix is keyed on the Games-Library DIRECTORY name
("SeriousSamFirstEncounter"). `DOC_ALIASES` connects the two.

WHY A TABLE AND NOT A NORMALISATION RULE. Those two strings differ by
punctuation *and* a dropped "The", so no case/punctuation-stripping rule
connects them. The only thing that would is a fuzzy match, and a fuzzy match
here silently mis-attributes a verification to the wrong title -- which is
strictly worse than not matching at all, because the failure is invisible.

WHAT WENT WRONG. Serious Sam was withdrawn, then recovered and LAN-proved on
three boxes. The document was updated correctly and the database held three
`verified_two_box` rows, but no alias existed, so `doc --check` reported the
title as unmapped and its three verifications as un-written-up. Both halves
were fine; only the join was missing.

The design that made this findable is worth keeping: an unmapped title is
REPORTED, never skipped. A parser that quietly dropped unknown rows would have
removed those verifications from the matrix while every view still looked
healthy.
"""
import importlib.util
import os

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "compat.py")
LAN_DOC = os.path.join(REPO, "docs", "lan-multiplayer-status.md")

spec = importlib.util.spec_from_file_location("compat_mod", SRC)
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)


def test_every_title_named_in_the_lan_document_is_mapped():
    if not os.path.exists(LAN_DOC):
        pytest.skip("SKIPPED LOUDLY: %s absent - alias coverage NOT checked"
                    % LAN_DOC)
    with open(LAN_DOC, encoding="utf-8", errors="replace") as f:
        _facts, unmapped = compat._mp_from_doc(f.read())
    assert not unmapped, (
        "these titles appear in docs/lan-multiplayer-status.md and map to no "
        "library directory, so their verifications never reach the matrix:\n"
        "  %s\n\nAdd them to DOC_ALIASES in scripts/fleet/compat.py."
        % "\n  ".join(sorted(set(unmapped))))


def test_serious_sam_specifically_is_mapped_both_ways():
    """The one that caught this, and the one most likely to regress.

    It was withdrawn, recovered, and re-staged; a future rewrite of that
    section could easily re-word the title.
    """
    for name, want in (
            ("serious sam: the first encounter", "SeriousSamFirstEncounter"),
            ("serious sam: the second encounter", "SeriousSamSecondEncounter")):
        assert compat.DOC_ALIASES.get(name) == want, (
            "%r no longer maps to %s; its LAN proofs would silently stop "
            "reaching the matrix" % (name, want))


def test_an_unmapped_title_is_reported_rather_than_skipped():
    """The property that made the bug visible at all."""
    facts, unmapped = compat._mp_from_doc(
        "## VERIFIED LAN\n\n"
        "| Title | Engine | Proven on |\n|---|---|---|\n"
        "| Totally Not A Real Game | id Tech 3 | `.123` + `.240` |\n")
    assert "Totally Not A Real Game" in unmapped, (
        "an unrecognised title is being silently dropped. That removes "
        "verifications from the matrix while every view still looks healthy - "
        "the exact failure this parser was written strict to avoid.")
    assert not facts, "an unmapped title must not produce facts either"

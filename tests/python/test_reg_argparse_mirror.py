"""The registry-parsing test's VERBATIM copy must stay identical to the source.

tests/native/test_reg_argparse.c exercises `reg_next_arg()` and
`reg_rest_arg()` from agent/src/registry.c. Those functions cannot be linked
into a host test cheaply — registry.c pulls in the Win32 registry API, the
socket layer, the JSON writer and the logger — so the test carries a copy,
following the precedent of tests/native/test_icon_bay.c.

A copied implementation is only worth anything while it matches. This pins the
two together: if someone edits the parser in registry.c and not the test, the
test would keep passing against stale code and would quietly stop protecting
anything. That failure mode is worse than having no test, because it reads as
coverage.

It also asserts the original bug cannot come back. The parser used to be
sscanf("%31s %511s %255s"), and %s stops at whitespace, so any key path with a
space was silently cut in half:

    REGREAD HKLM SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion
      -> path "SOFTWARE\\Microsoft\\Windows", value "NT\\CurrentVersion"

which fails as "Cannot open key" — indistinguishable from the key not existing.
"Windows NT" is one of the commonest paths on the system.
"""

import os
import re

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "agent", "src", "registry.c")
MIRROR = os.path.join(REPO, "tests", "native", "test_reg_argparse.c")

FUNCS = ("static int reg_next_arg(", "static void reg_rest_arg(")


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _extract(text, signature):
    """Grab a whole function body starting at its signature line."""
    i = text.find(signature)
    assert i != -1, "%s not found" % signature
    j = text.index("\n}\n", i) + len("\n}\n")
    return text[i:j]


@pytest.mark.parametrize("signature", FUNCS)
def test_mirror_matches_source(signature):
    src = _extract(_read(SRC), signature)
    mirror = _extract(_read(MIRROR), signature)
    assert src == mirror, (
        "tests/native/test_reg_argparse.c's copy of %s has drifted from "
        "agent/src/registry.c. Re-copy it verbatim — a test that mirrors stale "
        "code passes while protecting nothing." % signature.strip("static ()")
    )


def _strip_comments(text):
    """Drop /* */ and // so a comment ABOUT the bug is not read as the bug.

    registry.c documents the old sscanf() parse in a comment, precisely so
    nobody reintroduces it. Scanning the raw file would match that comment and
    fail forever, which trains people to delete the test rather than fix the
    code.
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def test_the_truncating_sscanf_is_gone():
    """The old parse must not come back in any registry handler."""
    src = _strip_comments(_read(SRC))
    bad = re.findall(r'sscanf\s*\([^)]*%\d*s[^)]*\)', src)
    assert not bad, (
        "agent/src/registry.c parses arguments with sscanf(\"%%s\") again: %r. "
        "%%s stops at whitespace, which silently truncates any key path "
        "containing a space (e.g. 'Windows NT') and reports it as a missing "
        "key." % bad
    )


def test_quoted_paths_are_supported():
    src = _read(SRC)
    assert "reg_next_arg" in src and "reg_rest_arg" in src, (
        "the quote-aware argument helpers are gone from registry.c"
    )
    # REGREAD must try the whole remainder as a path before assuming the last
    # token is a value name — that is what makes the unquoted "Windows NT" case
    # work rather than merely the quoted one.
    m = re.search(r"void handle_regread\(.*?\n\}\n", src, re.S)
    assert m, "handle_regread() not found"
    body = m.group(0)
    assert "RegOpenKeyExA" in body and "strrchr" in body, (
        "handle_regread() must probe the joined path first and only then split "
        "at the LAST space; without that, an unquoted path containing a space "
        "is still misread as path + value"
    )


@pytest.mark.parametrize("handler", ["handle_regread", "handle_regdelete", "handle_regwrite"])
def test_every_handler_uses_the_new_parser(handler):
    src = _read(SRC)
    m = re.search(r"void %s\(.*?\n\}\n" % handler, src, re.S)
    assert m, "%s() not found" % handler
    assert "reg_next_arg" in m.group(0) or "reg_rest_arg" in m.group(0), (
        "%s() does not use the quote-aware parser, so it still truncates paths "
        "at the first space" % handler
    )

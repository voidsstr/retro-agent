"""No product key, CD key or serial may be committed to this repo.

WHY THIS EXISTS.  Every secret the fleet depends on lives in Azure Key Vault
`nsc-secrets-kv`; the repo is supposed to carry only the NAME of a secret and
the command that fetches it.  That convention holds today -- `git log -p --all`
was swept on 2026-08-30 and no key-shaped literal has ever been committed -- but
it holds only as long as nothing quietly breaks it.  A key pasted into a script
"just to test it" is permanent: rewriting published history is a whole
negotiation, so the cheap moment to catch it is before the commit.

CRYING WOLF IS THE FAILURE MODE HERE.  A guard that fires on hashes, GUIDs and
version strings gets muted within a week and then protects nothing.  So every
pattern below is anchored to a SHAPE THAT ONLY KEYS HAVE, placeholders are
excluded explicitly, and binaries are skipped rather than guessed at.

The strongest check is the last one: it fetches each `fleet-gamekey-*` secret
from the vault and greps the tree for that literal value, which catches any
shape at all.  It SKIPS LOUDLY when `az` is unavailable -- a silent skip would
let the guard rot into decoration.
"""
import os
import re
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# This file necessarily contains the patterns it hunts for, and SECRETS.md
# documents the rotation command with an all-X placeholder in it.
SELF = os.path.relpath(os.path.abspath(__file__), REPO)
ALLOWED_FILES = {
    SELF,
    "tests/python/test_no_committed_secrets.py",
    "scripts/pxe/SECRETS.md",
}

# A literal is a placeholder, not a leak, when it looks like one.
PLACEHOLDER = re.compile(
    r"^(?:[X]+|[x]+|[A-Z])(?:[-][X]+|[-][x]+|[-][A-Z])*$"
    r"|^(?:AAAAA|11111|00000|12345)",
)


def _tracked_text_files():
    out = subprocess.run(
        ["git", "-C", REPO, "ls-files", "-z"],
        capture_output=True, text=True, check=True).stdout
    for rel in out.split("\0"):
        if not rel:
            continue
        path = os.path.join(REPO, rel)
        if not os.path.isfile(path) or os.path.islink(path):
            continue
        if os.path.getsize(path) > 4 * 1024 * 1024:
            continue
        try:
            with open(path, "rb") as fh:
                head = fh.read(8192)
            if b"\0" in head:            # binary -- skip rather than guess
                continue
            with open(path, "r", errors="replace") as fh:
                yield rel, fh.read()
        except OSError:
            continue


# Shapes that essentially only CD/product keys have.  Each was chosen because
# the fleet actually owns a key of that shape.
KEY_SHAPES = [
    # Microsoft product key: five groups of five.  A GUID is 8-4-4-4-12 and a
    # hash has no dashes, so neither can reach this.
    ("Microsoft product key (5x5)",
     re.compile(r"\b[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}\b")),
    # Unreal Engine CD key (UT2004): four groups of five.
    ("Unreal-engine CD key (4x5)",
     re.compile(r"\b[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}\b")),
    # Sierra/Gearbox-style serial (Opposing Force): five groups of four.
    ("Sierra-style serial (5x4)",
     re.compile(r"\b[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}\b")),
    # WON Half-Life CD key: 4-5-4 digits.  Distinctive enough not to collide
    # with a date, a version or a phone number.
    ("WON Half-Life CD key (4-5-4 digits)",
     re.compile(r"\b\d{4}-\d{5}-\d{4}\b")),
]


# Values that are fleet-wide CONVENTIONS on an isolated LAN, documented as
# deliberately not secret in CLAUDE.md ("Deliberately NOT secret - documented,
# not hidden"). They are vaulted so the record exists, but they appear in the
# tree by design and must never be reported as leaks.
CONVENTIONAL_NOT_SECRET = {
    "password",              # the console account on every box; XP auto-login
                             # requires it in cleartext, and ~20 scripts assume it
    "retro-agent-secret",    # the agent's shared secret, compiled in as the default
    "retroadmin",            # the game servers' rcon password
    "retro-vanilla",
    "retro-noblood",
    "admin",
}


def test_no_key_shaped_literal_is_committed():
    hits = []
    for rel, text in _tracked_text_files():
        if rel in ALLOWED_FILES:
            continue
        for label, rx in KEY_SHAPES:
            for m in rx.finditer(text):
                lit = m.group(0)
                if PLACEHOLDER.match(lit):
                    continue
                line = text.count("\n", 0, m.start()) + 1
                # Report the SHAPE and the location, never the value itself --
                # a failing test's output is itself a place a key must not land.
                hits.append("%s:%d  %s  (%d chars, masked)"
                            % (rel, line, label, len(lit)))
    assert not hits, (
        "key-shaped literals are committed to this repo. Put the value in "
        "nsc-secrets-kv (see .claude/skills/fleet-keyvault/SKILL.md) and reference "
        "it by NAME here. Do NOT rewrite published git history without asking the "
        "user first.\n  " + "\n  ".join(hits))


def test_no_engine_key_file_is_tracked():
    """q3key / sof2key / cdkey are plain-text CD-key files.  They belong in the
    staged library on the share and in the vault, never in git."""
    out = subprocess.run(["git", "-C", REPO, "ls-files"],
                         capture_output=True, text=True, check=True).stdout
    bad = [p for p in out.split("\n")
           if os.path.basename(p).lower() in
           {"q3key", "sof2key", "cdkey", "jakey", "jokey", "rtcwkey", "efkey"}]
    assert not bad, "engine CD-key files are tracked in git: %s" % bad


def test_no_private_key_or_azure_connection_string_is_committed():
    markers = [
        ("openssh/pem private key", re.compile(r"-----BEGIN (?:OPENSSH|RSA|EC|DSA|PGP) PRIVATE KEY-----")),
        ("azure storage connection string", re.compile(r"AccountKey=[A-Za-z0-9+/]{40,}")),
        ("anthropic api key", re.compile(r"sk-ant-[A-Za-z0-9]{20,}")),
        ("aws access key id", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ]
    hits = []
    for rel, text in _tracked_text_files():
        if rel in ALLOWED_FILES:
            continue
        for label, rx in markers:
            if rx.search(text):
                hits.append("%s  %s" % (rel, label))
    assert not hits, "credential material is committed: %s" % hits


def test_the_vaulted_secrets_do_not_appear_in_the_tree():
    """The catch-all: compare against the REAL values.  Shape-matching can only
    catch shapes we thought of; this catches anything at all.

    WIDENED 2026-08-31 from `fleet-gamekey-*` to **every `fleet-*` secret**.
    The vault stopped being only game keys: `fleet-cloudflare-api-token` and the
    R2 key pair are live credentials to an internet-facing service, so a leak
    of one reaches past this isolated LAN in a way a 1999 CD key never could --
    and they are exactly the kind of value that gets pasted into a config while
    debugging. A guard scoped to the *old* contents of the vault protects least
    where it now matters most.

    Two consequences worth stating, because both are deliberate:
      * `fleet-cloudflare-r2-s3-endpoint` is NOT secret and is checked anyway.
        Committing it is harmless, so if this ever fires on that one, just
        exclude it -- do not weaken the prefix.
      * A short or highly generic secret value would drown this in false
        positives; the length floor below is what keeps that from happening.

    Skips loudly (never silently) when the vault cannot be reached -- offline,
    or `az` not logged in -- because a guard that quietly passes is worse than
    no guard.
    """
    if not shutil.which("az"):
        pytest.skip("SKIPPED (az not installed) - the strongest secret guard did NOT run")
    probe = subprocess.run(["az", "account", "show", "-o", "none"],
                           capture_output=True, text=True)
    if probe.returncode != 0:
        pytest.skip("SKIPPED (az not logged in) - the strongest secret guard did NOT run; "
                    "run `az login` to arm it")

    listing = subprocess.run(
        ["az", "keyvault", "secret", "list", "--vault-name", "nsc-secrets-kv",
         "--query", "[?starts_with(name,'fleet-')].name", "-o", "tsv"],
        capture_output=True, text=True, timeout=180)
    if listing.returncode != 0:
        pytest.skip("SKIPPED (vault unreachable) - the strongest secret guard did NOT run")

    names = [n.strip() for n in listing.stdout.split("\n") if n.strip()]
    assert names, "no fleet-* secrets in nsc-secrets-kv - has the vault been emptied?"
    # The widening is the point of this test; assert it actually happened
    # rather than trusting the query string to stay correct.
    assert any(n.startswith("fleet-gamekey") for n in names), (
        "no fleet-gamekey-* secrets returned - the listing query has regressed")

    values = {}
    for name in names:
        r = subprocess.run(
            ["az", "keyvault", "secret", "show", "--vault-name", "nsc-secrets-kv",
             "--name", name, "--query", "value", "-o", "tsv"],
            capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            continue
        v = r.stdout.strip()
        # Too short to grep for without drowning in coincidences.
        if len(v) < 8:
            continue
        # A secret whose VALUE is a deliberately-public fleet convention must
        # not be grepped for: `password` and `retro-agent-secret` appear in
        # almost every source file as ordinary words, struct fields and
        # documented defaults. Widening the prefix immediately lit up 20+ files
        # on `fleet-nas-...-password`, whose value is the literal word
        # "password" -- CLAUDE.md's "Deliberately NOT secret" section says so
        # explicitly. Reporting those would be a permanent red light, and a
        # guard that cries wolf gets ignored, which is how the real one gets
        # missed. Excluded BY VALUE, not by name, so a genuine secret can never
        # be skipped just because someone named it badly.
        if v.lower() in CONVENTIONAL_NOT_SECRET:
            continue
        values[name] = v

    hits = []
    for rel, text in _tracked_text_files():
        for name, v in values.items():
            if v in text or v.replace("-", "") in text:
                hits.append("%s contains the literal value of %s" % (rel, name))
    assert not hits, (
        "a vaulted secret's literal value is committed to this repo:\n  "
        + "\n  ".join(hits)
        + "\nReference it by vault NAME. Tell the user before rewriting history.")

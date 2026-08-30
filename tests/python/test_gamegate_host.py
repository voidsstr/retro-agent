"""Host-side gate: the cache key, the LLM reply validator, and the escalation gate.

Three things here have a failure mode that is invisible in normal operation,
which is why each gets a test rather than a comment:

1. THE CACHE KEY. It exists so an LLM is consulted once per (machine, title),
   not once per run. Key it on anything that varies and it misses every time -
   indistinguishable from working, just slower and more expensive, forever.

2. THE MALFORMED-REPLY PATH. A model that returns junk must leave the
   deterministic verdict standing. If a bad reply defaulted to "run", a broken
   or swapped model would silently become the most permissive gate on the fleet
   and nothing would show it.

3. THE ESCALATION GATE. Only a MARGINAL verdict may reach the model. If a `run`
   or a `no` ever escalated, every title on every box would become an ollama
   round trip - the exact cost the deterministic rules exist to avoid.
"""

import json
import os
import sys
import tempfile

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "scripts"))

from gamegate import cache as cache_mod   # noqa: E402
from gamegate import library as lib_mod   # noqa: E402
from gamegate import llm as llm_mod       # noqa: E402
from gamegate import rules                # noqa: E402


@pytest.fixture
def db():
    fd, path = tempfile.mkstemp(suffix=".db")
    os.close(fd)
    os.unlink(path)
    c = cache_mod.Cache(path)
    yield c
    c.close()
    if os.path.exists(path):
        os.unlink(path)


def _decision(verdict=rules.V_MARGINAL, by="llm"):
    return rules.Decision(verdict=verdict, limiting="cpu_mhz",
                          reason="close", decided_by=by, confidence=0.7)


# --- 1. the cache key -------------------------------------------------------

def test_cache_hits_on_the_same_machine_and_misses_on_a_different_one(db):
    db.put("aaaa", "UT2004", "", 1, "qwen3:14b", _decision())
    assert db.get("aaaa", "UT2004", "", 1, "qwen3:14b") is not None

    # A different hardware profile is a different machine, even for the same
    # title - that is the whole point of keying on hardware.
    assert db.get("bbbb", "UT2004", "", 1, "qwen3:14b") is None
    # A corrected requires.json bumps its version and must invalidate.
    assert db.get("aaaa", "UT2004", "", 2, "qwen3:14b") is None
    # Another model's opinion is not this model's.
    assert db.get("aaaa", "UT2004", "", 1, "gemma3:12b") is None
    # A per-shortcut verdict is its own entry.
    assert db.get("aaaa", "UT2004", "Play.bat", 1, "qwen3:14b") is None


def test_rule_verdicts_survive_a_model_change(db):
    """A rule verdict is reproducible arithmetic and belongs to no model.
    Storing it under a model would throw away every deterministic answer the
    moment someone passed --model, which would be pure waste."""
    db.put("aaaa", "Quake1", "", 1, "qwen3:14b",
           _decision(rules.V_RUN, by="rule"))
    row = db.get("aaaa", "Quake1", "", 1, "some-other-model")
    assert row is not None
    assert row["decided_by"] == "rule"
    assert row["model"] == ""


def test_forget_llm_keeps_the_arithmetic(db):
    db.put("aaaa", "Quake1", "", 1, "m", _decision(rules.V_RUN, by="rule"))
    db.put("aaaa", "UT2004", "", 1, "m", _decision(by="llm"))
    dropped = db.forget(only_llm=True)
    assert dropped == 1
    assert db.get("aaaa", "Quake1", "", 1, "m") is not None
    assert db.get("aaaa", "UT2004", "", 1, "m") is None


def test_hit_and_miss_counters_are_real(db):
    assert db.stats()["hits"] == 0
    db.get("nope", "x", "", 0, "m")
    assert db.stats()["misses"] == 1
    db.put("aaaa", "x", "", 0, "m", _decision())
    db.get("aaaa", "x", "", 0, "m")
    assert db.stats()["hits"] == 1


# --- 2. the malformed-reply path -------------------------------------------

@pytest.mark.parametrize("raw", [
    "",                                   # empty (qwen3.6:27b really does this)
    "not json at all",
    "{",                                  # truncated
    '{"verdict":"maybe"}',                # outside the enum
    '{"verdict":"RUN "}',                 # not a value we accept
    '["run"]',                            # right word, wrong shape
    'null',
    '{"confidence":0.9,"reason":"fine"}',  # no verdict at all
])
def test_a_malformed_reply_never_becomes_a_verdict(raw):
    """Every one of these must return None so the caller keeps the rule
    verdict. A bad reply turning into "run" would make a broken model the most
    permissive possible gate, silently."""
    fallback = _decision(rules.V_MARGINAL, by="rule")
    assert llm_mod.Judge._parse(raw, fallback) is None


def test_a_valid_reply_is_taken_and_attributed():
    fallback = _decision(rules.V_MARGINAL, by="rule")
    fallback.missing_caps = rules.CAP_DISC_MOUNT
    d = llm_mod.Judge._parse(
        '{"verdict":"no","confidence":0.8,"limiting_factor":"cpu_mhz",'
        '"reason":"UE2 on a 845 MHz P3 is a slideshow"}', fallback)
    assert d.verdict == rules.V_NO
    assert d.decided_by == "llm"
    assert d.confidence == 0.8
    # The capability finding is the RULES' fact and must survive the model's
    # answer - the model is not asked about it and cannot overrule it.
    assert d.missing_caps == rules.CAP_DISC_MOUNT


def test_confidence_is_clamped_not_trusted():
    """gemma4:26b really returned -1.0 in the model bench. A confidence outside
    0..1 must be clamped rather than stored and later compared against."""
    fallback = _decision(by="rule")
    for raw_conf, want in (("-1.0", 0.0), ("5", 1.0), ('"garbage"', 0.0)):
        d = llm_mod.Judge._parse(
            '{"verdict":"run","confidence":%s,"limiting_factor":"none",'
            '"reason":"x"}' % raw_conf, fallback)
        assert d is not None and d.confidence == want


def test_llm_unavailable_keeps_the_rule_verdict_and_says_so():
    """A model that cannot be reached must change nothing except the message.
    The marker matters: a verdict that silently came from a failed call is
    indistinguishable from a considered one."""
    j = llm_mod.Judge(model="nonexistent", host="http://127.0.0.1:1", retries=0,
                      timeout=1)
    fallback = rules.Decision(verdict=rules.V_MARGINAL, limiting="cpu_mhz",
                              reason="CPU below minimum", decided_by="rule")
    p = rules.Profile(cpu_mhz=845, ram_mb=511)
    r = rules.parse_requirements({"min_cpu_mhz": 1000}, "T")
    d = j.judge(p, r, fallback)
    assert d.verdict == rules.V_MARGINAL
    assert d.decided_by == "rule"
    assert "LLM unavailable" in d.reason
    assert j.failures == 1


# --- 3. the escalation gate -------------------------------------------------

class _CountingJudge:
    def __init__(self):
        self.asked = []

    def judge(self, profile, req, fallback):
        self.asked.append(req.title)
        return rules.Decision(verdict=rules.V_RUN, reason="model says fine",
                              decided_by="llm", confidence=0.9)


def _title(name, doc):
    from pathlib import Path
    t = lib_mod.Title(name=name, path=Path("/nonexistent"), shortcuts=[])
    t.requires_raw = doc
    return t


def test_only_marginal_reaches_the_model(db):
    """A gate that phones an LLM to conclude a Pentium III cannot run Doom 3 is
    a bad gate. Both ends of the range must be answered by arithmetic."""
    from gamegate.gamegate import decide_title
    p = rules.Profile(profile_hash="hhhh", cpu_mhz=845, ram_mb=511, vram_mb=32,
                      gpu_level=rules.GPU_TNL, os_level=rules.OS_WINXP,
                      cpu_features=rules.CPU_SSE)
    j = _CountingJudge()

    clear_run = _title("Quake1", {"requirements_version": 1,
                                  "min_cpu_mhz": 90, "min_ram_mb": 16})
    clear_no = _title("Doom3", {"requirements_version": 1,
                                "min_cpu_mhz": 1500,
                                "gpu_feature_level": "sm2.0"})
    borderline = _title("UT2004", {"requirements_version": 1,
                                   "min_cpu_mhz": 1000, "min_ram_mb": 128})

    d, _ = decide_title(p, clear_run, "", db, j, "m")
    assert d.verdict == rules.V_RUN and d.decided_by == "rule"
    d, _ = decide_title(p, clear_no, "", db, j, "m")
    assert d.verdict == rules.V_NO and d.decided_by == "rule"
    assert j.asked == [], "the rules must answer the clear cases alone"

    d, _ = decide_title(p, borderline, "", db, j, "m")
    assert j.asked == ["UT2004"]
    assert d.decided_by == "llm"


def test_a_title_with_no_requirements_never_costs_a_model_call(db):
    """A title that declares nothing has nothing for a model to reason about.
    Asking anyway would burn one call per title per box for no information."""
    from gamegate.gamegate import decide_title
    p = rules.Profile(profile_hash="hhhh", cpu_mhz=845, ram_mb=511)
    j = _CountingJudge()
    d, _ = decide_title(p, _title("Mystery", None), "", db, j, "m")
    assert d.verdict == rules.V_RUN
    assert j.asked == []


def test_the_second_run_is_a_cache_hit_and_asks_nothing(db):
    from gamegate.gamegate import decide_title
    p = rules.Profile(profile_hash="hhhh", cpu_mhz=845, ram_mb=511, vram_mb=32,
                      gpu_level=rules.GPU_TNL, os_level=rules.OS_WINXP)
    j = _CountingJudge()
    t = _title("UT2004", {"requirements_version": 1, "min_cpu_mhz": 1000})

    d1, hit1 = decide_title(p, t, "", db, j, "m")
    d2, hit2 = decide_title(p, t, "", db, j, "m")
    assert hit1 is False and hit2 is True
    assert len(j.asked) == 1, "a cached verdict must not re-ask the model"
    assert d1.verdict == d2.verdict and d2.decided_by == "llm"

    # --refresh must genuinely re-ask.
    _d3, hit3 = decide_title(p, t, "", db, j, "m", refresh=True)
    assert hit3 is False and len(j.asked) == 2


# --- the verdict file, round trip ------------------------------------------

def test_verdict_file_round_trips_and_only_no_blocks():
    p = rules.Profile(profile_hash="deadbeefdeadbeef", hostname="ADMIN",
                      ip="192.168.1.124")
    rows = [
        ("Quake1", rules.Decision(verdict=rules.V_RUN, reason="meets it")),
        ("UT2004", rules.Decision(verdict=rules.V_MARGINAL, limiting="cpu_mhz",
                                  reason="close", decided_by="llm")),
        ("Doom 3", rules.Decision(verdict=rules.V_NO,
                                  limiting="gpu_feature_level",
                                  reason="two levels short")),
    ]
    text = rules.format_verdict_file(p, rows, "qwen3:14b", "2026-08-30T00:00:00")
    back = rules.parse_verdict_file(text)
    assert back["Quake1"][0] == rules.V_RUN
    assert back["UT2004"][0] == rules.V_MARGINAL
    assert back["Doom 3"][0] == rules.V_NO       # a spaced title survives
    # Comments and the header are not decisions.
    assert "# gamegate v1" not in back
    # A truncated file yields fewer decisions, never a wrong one - every
    # missing line means "deploy", which is the safe direction.
    half = text[:len(text) // 2]
    partial = rules.parse_verdict_file(half)
    assert set(partial).issubset(set(back))


def test_launch_txt_is_read_with_the_agents_1023_byte_limit(tmp_path):
    """The agent reads launch.txt with ONE 1023-byte read, so a shortcut
    declared past that byte does not exist on any box. A linter that read the
    whole file would call such a title fine - which is how a title silently
    loses half of itself."""
    d = tmp_path / "Big"
    d.mkdir()
    filler = "\n".join(f"# {'x' * 60}" for _ in range(20))
    (d / "launch.txt").write_text(
        "first.exe\tFirst\n" + filler + "\nlast.exe\tLast\n")
    t = lib_mod.load_title(d)
    assert "first.exe" in t.shortcuts
    assert "last.exe" not in t.shortcuts, \
        "a line past 1023 bytes must be invisible here too"

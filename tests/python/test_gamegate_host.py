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


def test_published_file_declares_its_own_scope():
    """A verdict file must state how many titles it covers.

    The 2026-08-30 clobber: a per-title publisher wrote a ONE-row file over the
    complete 38-row one on seven of eight boxes. It parsed perfectly - right
    header, right columns, one valid verdict - so nothing reported it, and the
    nine ollama adjudications (the only verdicts a fleet box cannot recompute
    for itself) were lost silently. The gate kept working, by local rules, which
    is exactly what hid it.

    A file cannot stop itself being replaced. It can declare its scope, so the
    next reader can see the claim shrink.
    """
    from gamegate import rules

    p = rules.Profile(ip="10.0.0.1", hostname="BOX", profile_hash="abc123",
                      cpu_brand="test", cpu_mhz=1000, ram_mb=512,
                      gpu_name="test gpu", vram_mb=64,
                      gpu_level=rules.GPU_SM2, os_level=rules.OS_WINXP,
                      os_product="Windows XP")

    def dec(v, name="x", reason="r", by="rule"):
        d = rules.Decision()
        d.verdict, d.reason, d.decided_by = v, reason, by
        return d

    rows = [("Quake1", dec(rules.V_RUN)),
            ("FarCry", dec(rules.V_NO)),
            ("UT2004", dec(rules.V_MARGINAL, by="llm"))]
    text = rules.format_verdict_file(p, rows, "qwen3:14b", "2026-08-30T13:00:00")

    assert "# titles=3" in text, "the file must declare its own scope"
    body = [l for l in text.splitlines() if l and not l.startswith("#")]
    assert len(body) == 3
    # and the declaration must match what is actually there, or it is worthless
    declared = int([l for l in text.splitlines()
                    if l.startswith("# titles=")][0].split("=")[1])
    assert declared == len(body)

    # It must survive a round trip through the reader.
    got = rules.parse_verdict_file(text)
    assert set(got) == {"Quake1", "FarCry", "UT2004"}

    # A one-row file is well formed -- that is the whole danger -- but its
    # declared scope is now 1, so a reader comparing against the library size
    # can see it covers almost nothing.
    one = rules.format_verdict_file(p, [("Halo", dec(rules.V_NO))],
                                    "qwen3:14b", "2026-08-30T13:18:52")
    assert "# titles=1" in one
    assert len(rules.parse_verdict_file(one)) == 1


def test_a_rule_marginal_is_reescalated_not_served_forever(tmp_path):
    """A cached `marginal` that only ever saw the rules must not be a dead end.

    THE BUG (2026-08-30): decide_title returned on a cache hit BEFORE the
    escalation gate. So a marginal recorded while ollama was down - or under
    --no-llm - was served back forever and the model was never consulted. And
    because such a row is typed `rule`, --refresh-llm could not reach it either:
    that flag drops llm rows by design. The only recovery was --refresh, which
    discards every verdict for the box.

    That made every routine re-publish silently strip the model's reasoning -
    the one part of a verdict file a fleet box cannot recompute for itself.
    """
    import sys, os
    sys.path.insert(0, os.path.join(REPO, "scripts"))
    from gamegate import rules, cache as cache_mod
    from gamegate import gamegate as gg

    class FakeTitle:
        name = "UT2004"
        shortcuts = []
        def requirements(self, shortcut=""):
            return rules.parse_requirements(
                {"requirements_version": 1, "min_cpu_mhz": 1000}, "UT2004")

    class CountingJudge:
        model = "fake"
        def __init__(self):
            self.calls = 0
        def judge(self, profile, req, fallback):
            self.calls += 1
            d = rules.Decision()
            d.verdict, d.limiting = rules.V_MARGINAL, fallback.limiting
            d.reason, d.decided_by = "model reasoned about it", "llm"
            return d

    # 845 MHz against a 1000 MHz floor: inside the band, so MARGINAL.
    prof = rules.Profile(ip="10.0.0.9", hostname="BOX", profile_hash="deadbeef",
                         cpu_mhz=845, ram_mb=512, gpu_level=rules.GPU_SM2,
                         os_level=rules.OS_WINXP, os_product="Windows XP")
    c = cache_mod.Cache(str(tmp_path / "gg.db"))
    t = FakeTitle()

    # 1. ollama down: the rules stand, and the fail-open record IS cached.
    d1, hit1 = gg.decide_title(prof, t, "", c, None, "fake", use_llm=False)
    assert d1.verdict == rules.V_MARGINAL and d1.decided_by == "rule"
    assert hit1 is False

    # 2. ollama back. The cached row must NOT short-circuit the escalation.
    judge = CountingJudge()
    d2, _ = gg.decide_title(prof, t, "", c, judge, "fake", use_llm=True)
    assert judge.calls == 1, ("a rule-derived marginal was served from cache "
                              "and never adjudicated - the dead end is back")
    assert d2.decided_by == "llm" and d2.reason == "model reasoned about it"

    # 3. Now it IS decided, so it must be a real hit - no repeat model calls,
    #    or every plan would re-bill every marginal on every run.
    d3, hit3 = gg.decide_title(prof, t, "", c, judge, "fake", use_llm=True)
    assert judge.calls == 1, "an adjudicated verdict must be cached"
    assert hit3 is True and d3.decided_by == "llm"

    # 4. A settled RUN verdict is never re-escalated either.
    fast = rules.Profile(ip="10.0.0.8", hostname="F", profile_hash="cafe",
                         cpu_mhz=3000, ram_mb=2048, gpu_level=rules.GPU_SM3,
                         os_level=rules.OS_WINXP, os_product="Windows XP")
    gg.decide_title(fast, t, "", c, judge, "fake", use_llm=True)
    before = judge.calls
    gg.decide_title(fast, t, "", c, judge, "fake", use_llm=True)
    assert judge.calls == before
    c.close()

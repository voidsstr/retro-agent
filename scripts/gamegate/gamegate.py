#!/usr/bin/env python3
"""gamegate - decide which staged games a fleet machine should actually receive.

    python3 scripts/gamegate/gamegate.py profile <ip>          what is that box
    python3 scripts/gamegate/gamegate.py plan <ip> [...]        decide, do not publish
    python3 scripts/gamegate/gamegate.py publish <ip> [...]     decide and publish
    python3 scripts/gamegate/gamegate.py lint                   check the library's requires.json
    python3 scripts/gamegate/gamegate.py cache [--stats|--forget-llm|--clear]

THE SPLIT, and why it is where it is. A Pentium III cannot call a language
model and the RTX 5090 cannot see inside a fleet box, so the work divides
strictly by where it can run:

  agent (C)   collects HWPROFILE, and carries the deterministic rules so a
              freshly imaged box gates its own GAMESYNC with no host involved.
  host (here) plans the whole fleet, escalates ONLY the borderline band to
              ollama, caches every verdict, and publishes a per-profile file
              the agent honours.

DETERMINISTIC RULES RUN FIRST AND DECIDE ALONE wherever they can. Only a
genuinely marginal case reaches the model; a gate that phones an LLM to
conclude "a Pentium III cannot run Doom 3" is a bad gate.

FAIL-OPEN, on purpose: absent data never blocks a title. See rules.py.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(HERE.parent))

from gamegate import cache as cache_mod          # noqa: E402
from gamegate import library as library_mod      # noqa: E402
from gamegate import llm as llm_mod              # noqa: E402
from gamegate import rules                       # noqa: E402

SECRET = os.environ.get("RETRO_SECRET", "retro-agent-secret")

C = {
    "run": "\033[32m", "marginal": "\033[33m", "no": "\033[31m",
    "dim": "\033[2m", "bold": "\033[1m", "off": "\033[0m",
}
if not sys.stdout.isatty():
    C = {k: "" for k in C}


# --------------------------------------------------------------------------
# talking to a box
# --------------------------------------------------------------------------

async def _fetch_hwprofile(ip, port=9898, timeout=20.0):
    from client.retro_protocol import RetroConnection
    conn = RetroConnection(ip, port)
    # 12s, not the library default: .171 answers slowly enough that a shorter
    # timeout drops it from sweeps entirely (CLAUDE.md).
    await conn.connect(SECRET, timeout=12.0)
    try:
        text = await conn.command_text("HWPROFILE", timeout=timeout)
    finally:
        await conn.close()
    return text


def get_profile(ip, cache=None):
    """Fetch and parse HWPROFILE. Raises with a usable message if the agent is
    too old to have the command - that is a fixable state, not a mystery."""
    text = asyncio.run(_fetch_hwprofile(ip))
    try:
        data = json.loads(text)
    except ValueError:
        raise SystemExit(
            f"{ip}: HWPROFILE did not return JSON. If this agent predates "
            f"v1.71.0 it has no HWPROFILE command - update it first.\n"
            f"  got: {text[:200]}")
    p = rules.Profile.from_hwprofile(data, ip=ip)
    if cache:
        cache.remember_profile(p, text)
    return p, data


# --------------------------------------------------------------------------
# deciding
# --------------------------------------------------------------------------

def decide_title(profile, title, shortcut, cache, judge, model,
                 use_llm=True, refresh=False):
    """One decision, cache-aware. Returns (Decision, cache_hit)."""
    req = title.requirements(shortcut)
    key_shortcut = shortcut or ""

    if cache and not refresh:
        row = cache.get(profile.profile_hash, title.name, key_shortcut,
                        req.version, model)
        if row is not None:
            d = rules.Decision(
                verdict=rules.VERDICT_VALUE.get(row["verdict"], rules.V_RUN),
                limiting=row["limiting"], reason=row["reason"],
                missing_caps=row["missing_caps"],
                decided_by=row["decided_by"], confidence=row["confidence"])
            # A MARGINAL THAT ONLY EVER SAW THE RULES IS NOT A DECISION - it is
            # the rules saying "I cannot call this one", which is the exact
            # input the escalation gate exists to consume. Caching it as though
            # it were an answer made it a DEAD END: the hit returned before the
            # gate, so the model was never consulted, and because the row is
            # typed `rule` even --refresh-llm could not reach it (that flag
            # drops llm rows by design). The only recovery was --refresh, which
            # throws away every verdict on the box.
            #
            # This is not a rare corner: it is what every run records whenever
            # ollama is down or --no-llm is passed, and the fail-open path is
            # SUPPOSED to be routine. So the row is still stored - losing the
            # record would be worse - but a later run that CAN adjudicate treats
            # it as unfinished and escalates. Self-healing, and no flag needed.
            if not (d.verdict == rules.V_MARGINAL
                    and d.decided_by != "llm"
                    and use_llm and judge is not None
                    and req.has_opinion()):
                return d, True

    d = rules.decide(profile, req)

    # The escalation gate. ONLY a marginal verdict, and only when the title
    # actually declared something - a title with no requirements has nothing
    # for a model to reason about, and asking anyway would burn a call per
    # title per box for no information.
    if (d.verdict == rules.V_MARGINAL and use_llm and judge is not None
            and req.has_opinion()):
        d = judge.judge(profile, req, d)

    if cache:
        cache.put(profile.profile_hash, title.name, key_shortcut,
                  req.version, model, d)
    return d, False


def plan(profile, titles, cache, judge, model, use_llm=True, refresh=False):
    """Decide every title, and every shortcut that has its own rules.

    The TITLE decision governs the copy. A shortcut decision only ever
    suppresses that one shortcut - see the capability note in rules.py.
    """
    rows = []
    for t in titles:
        d, hit = decide_title(profile, t, "", cache, judge, model,
                              use_llm, refresh)
        per_shortcut = []
        req = t.requirements()
        if req.shortcuts:
            for sc in (t.shortcuts or []):
                sd, shit = decide_title(profile, t, sc, cache, judge, model,
                                        use_llm, refresh)
                if sd.verdict != d.verdict or sd.missing_caps != d.missing_caps:
                    per_shortcut.append((sc, sd, shit))
        rows.append((t, d, hit, per_shortcut))
    return rows


# --------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------

def print_profile(p, data):
    print(f"{C['bold']}{p.hostname or p.ip}{C['off']}  ({p.ip})")
    print(f"  profile_hash  {p.profile_hash}")
    cpu = data.get("cpu", {})
    print(f"  cpu           {cpu.get('brand') or cpu.get('vendor')} "
          f"[{cpu.get('vendor')} family {cpu.get('family')} model "
          f"{cpu.get('model')} stepping {cpu.get('stepping')}]")
    print(f"                {p.cpu_mhz} MHz ({cpu.get('mhz_source')}), "
          f"{p.cpu_count} core(s), {' '.join(p.feature_names())}")
    print(f"  ram           {p.ram_mb} MB")
    g = data.get("gpu", {})
    print(f"  gpu           {g.get('name')} "
          f"[{g.get('pci_ven')}:{g.get('pci_dev')}] "
          f"{p.vram_mb} MB, level "
          f"{rules.GPU_LEVEL_NAME.get(p.gpu_level, '?')}")
    print(f"                driver {g.get('driver_version') or '?'} "
          f"({g.get('driver_date') or '?'}), via {g.get('source')}")
    o = data.get("os", {})
    print(f"  os            {o.get('product')} {o.get('version')} "
          f"{o.get('service_pack')} [level {o.get('level')}], DirectX "
          f"{(data.get('directx') or {}).get('major')}")
    caps = data.get("capabilities", {}) or {}
    dm = caps.get("disc_mount")
    print(f"  disc mount    {'yes (' + str(caps.get('disc_mount_evidence')) + ')' if dm else 'NO - ' + rules.CAPABILITY_REMEDY[rules.CAP_DISC_MOUNT]}")
    print(f"  free on C:    {p.free_mb} MB")


def print_plan(profile, rows, cache, judge):
    print(f"\n{C['bold']}{profile.hostname or profile.ip}{C['off']} "
          f"({profile.profile_hash})  {profile.describe()}\n")
    width = max((len(t.name) for t, _, _, _ in rows), default=10)
    n = {"run": 0, "marginal": 0, "no": 0}
    blocked = []
    nofile = []
    for t, d, hit, subs in rows:
        n[d.name] += 1
        mark = "cached" if hit else d.decided_by
        print(f"  {C[d.name]}{d.name:<9}{C['off']} {t.name:<{width}}  "
              f"{C['dim']}{mark:<6}{C['off']} {d.reason}")
        for cap in d.missing_cap_names():
            blocked.append((t.name, "", cap))
        for sc, sd, _ in subs:
            print(f"      {C['dim']}shortcut{C['off']} {sc}: "
                  f"{C[sd.name]}{sd.name}{C['off']} {sd.reason}")
            for cap in sd.missing_cap_names():
                blocked.append((t.name, sc, cap))
        if not t.has_requirements_file:
            nofile.append(t.name)
        if t.requires_error:
            print(f"      {C['no']}requires.json problem{C['off']}: "
                  f"{t.requires_error}")

    print(f"\n  {n['run']} run, {n['marginal']} marginal, {n['no']} skipped")
    if cache:
        s = cache.stats()
        print(f"  cache: {s['hits']} hit / {s['misses']} miss "
              f"({s['entries']} entries, {s['rule']} rule, {s['llm']} llm)")
    if judge:
        print(f"  llm calls: {judge.calls}"
              + (f", {judge.failures} FAILED (rule verdict stood)"
                 if judge.failures else ""))
        # A box with marginal rows and ZERO model calls is indistinguishable
        # from a healthy run, and for a long time it was one: a rule-derived
        # marginal cached as an answer meant the model was never consulted
        # again. Say it, and say which flag actually reaches it, because
        # --refresh-llm cannot (those rows are typed `rule` by design).
        unadjudicated = [t.name for t, d, _h, _s in rows
                         if d.verdict == rules.V_MARGINAL
                         and d.decided_by != "llm"]
        if unadjudicated:
            print(f"  {C['marginal']}NOTE{C['off']} {len(unadjudicated)} "
                  f"marginal verdict(s) were NOT adjudicated by the model "
                  f"({', '.join(unadjudicated[:6])}"
                  f"{', ...' if len(unadjudicated) > 6 else ''}) - "
                  f"ollama unreachable, --no-llm, or the title declares no "
                  f"requirement worth reasoning about")
    if blocked:
        print(f"\n  {C['marginal']}blocked but REMEDIABLE{C['off']} - the "
              f"hardware is fine, the box is missing software:")
        for title, sc, cap in blocked:
            where = f"{title} / {sc}" if sc else title
            remedy = rules.CAPABILITY_REMEDY.get(
                rules.CAPABILITIES.get(cap, 0), "?")
            print(f"    {where}: needs {cap} -> {remedy}")
    if nofile:
        print(f"\n  {C['dim']}no requires.json (not gated): "
              f"{', '.join(nofile)}{C['off']}")


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def cmd_profile(args):
    c = cache_mod.Cache(args.db)
    p, data = get_profile(args.ip, c)
    if args.json:
        print(json.dumps(data, indent=2))
    else:
        print_profile(p, data)
    c.close()
    return 0


def _judge_for(args):
    if args.no_llm:
        return None
    j = llm_mod.Judge(model=args.model, host=args.ollama)
    if not j.available():
        print(f"{C['marginal']}warning{C['off']}: ollama model "
              f"{args.model!r} not available at {args.ollama} - marginal "
              f"cases will keep their deterministic verdict", file=sys.stderr)
        return None
    return j


def cmd_plan(args, publish=False):
    titles = library_mod.load_library(args.library)
    if args.title:
        want = {t.lower() for t in args.title}
        titles = [t for t in titles if t.name.lower() in want]
    c = cache_mod.Cache(args.db)
    judge = _judge_for(args)
    rc = 0
    for ip in args.ip:
        try:
            p, _ = get_profile(ip, c)
        except Exception as exc:
            print(f"{C['no']}{ip}: {exc}{C['off']}", file=sys.stderr)
            rc = 1
            continue
        if args.refresh_llm:
            c.forget(profile_hash=p.profile_hash, only_llm=True)
        rows = plan(p, titles, c, judge, args.model,
                    use_llm=not args.no_llm, refresh=args.refresh)
        print_plan(p, rows, c, judge)
        if publish:
            # ONLY THE TITLE-LEVEL VERDICT IS PUBLISHED. A per-shortcut
            # line is not a shape the agent parses, so shortcut suppression
            # stays a deterministic on-box decision made from requires.json.
            decided = [(t.name, d) for t, d, _, _ in rows]

            # A NARROWED PUBLISH MERGES; IT NEVER REPLACES. With --title, `rows`
            # covers only the named titles, and writing that straight out
            # discards every other verdict in the file. That is not theoretical:
            # on 2026-08-30 a one-title publish left a single-row file on seven
            # of eight boxes, and because the survivor was perfectly well formed
            # nothing reported it for hours - taking nine ollama adjudications,
            # the one thing a fleet box cannot recompute, with it.
            if args.title:
                keep = library_mod.read_verdict_file(p.profile_hash,
                                                     args.library)
                if keep:
                    fresh = {name for name, _d in decided}
                    merged = [(nm, dd) for nm, dd in keep if nm not in fresh]
                    decided = sorted(merged + decided, key=lambda r: r[0])
                    print(f"  merged {len(decided) - len(rows)} existing "
                          f"verdict(s) with {len(rows)} re-decided")

            text = rules.format_verdict_file(
                p, decided, args.model,
                datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S"))
            path = library_mod.write_verdict_file(p.profile_hash, text,
                                                  args.library)
            print(f"\n  published {path} ({len(decided)} verdicts)")
    c.close()
    return rc


def cmd_lint(args):
    """Check every requires.json in the library.

    Deploy-blocking problems are FAIL; everything else is a warning - the same
    line validate-staged-library.py draws, and for the same reason: a validator
    that cries wolf trains people to ignore it.
    """
    titles = library_mod.load_library(args.library)
    fails = warns = 0
    for t in titles:
        if not t.has_requirements_file:
            print(f"  {C['dim']}warn{C['off']}  {t.name}: no requires.json - "
                  f"not gated. Write one stating no floor if that is correct, "
                  f"so 'checked' and 'nobody looked' stay distinguishable.")
            warns += 1
            continue
        if t.requires_error:
            print(f"  {C['no']}FAIL{C['off']}  {t.name}: {t.requires_error}")
            fails += 1
            continue
        req = t.requirements()
        raw = t.requires_raw or {}
        known = {"requirements_version", "title", "year", "notes",
                 "min_cpu_mhz", "min_ram_mb", "min_vram_mb", "disk_mb",
                 "gpu_feature_level", "cpu_features", "min_os", "max_os",
                 "requires_capabilities", "shortcuts"}
        for key in raw:
            if key not in known and not key.startswith("_"):
                print(f"  {C['dim']}warn{C['off']}  {t.name}: unknown key "
                      f"{key!r} (ignored by both implementations)")
                warns += 1
        if not raw.get("requirements_version"):
            print(f"  {C['no']}FAIL{C['off']}  {t.name}: no "
                  f"requirements_version - the host cache is keyed on it, so "
                  f"without it a corrected number can never invalidate a "
                  f"cached verdict")
            fails += 1
        if ("gpu_feature_level" in raw
                and req.min_gpu_level == rules.GPU_UNKNOWN):
            print(f"  {C['no']}FAIL{C['off']}  {t.name}: unknown "
                  f"gpu_feature_level {raw['gpu_feature_level']!r} - reads as "
                  f"'no opinion', so the floor you wrote does nothing")
            fails += 1
        for key, valid in (("min_os", rules.OS_LEVELS),
                           ("max_os", rules.OS_LEVELS)):
            if key in raw and str(raw[key]).lower() not in valid:
                print(f"  {C['no']}FAIL{C['off']}  {t.name}: unknown {key} "
                      f"{raw[key]!r}")
                fails += 1
        for name in raw.get("cpu_features", []) or []:
            if str(name).lower() not in rules.FEATURES:
                print(f"  {C['no']}FAIL{C['off']}  {t.name}: unknown cpu "
                      f"feature {name!r} - silently ignored, so the "
                      f"requirement does nothing")
                fails += 1
        for name in raw.get("requires_capabilities", []) or []:
            if str(name).lower() not in rules.CAPABILITIES:
                print(f"  {C['no']}FAIL{C['off']}  {t.name}: unknown "
                      f"capability {name!r}")
                fails += 1
        # A shortcut rule that names a target launch.txt does not have can
        # never fire, and looks exactly like a rule that works.
        for sc in (raw.get("shortcuts") or {}):
            if not t.shortcuts:
                continue
            if sc.lower() not in {s.lower() for s in t.shortcuts}:
                print(f"  {C['no']}FAIL{C['off']}  {t.name}: shortcuts key "
                      f"{sc!r} is not a launch.txt target - it can never "
                      f"apply. launch.txt has: {t.shortcuts}")
                fails += 1
    print(f"\n  {len(titles)} titles, {fails} FAIL, {warns} warn")
    return 1 if fails else 0


def cmd_cache(args):
    c = cache_mod.Cache(args.db)
    if args.clear:
        print(f"  dropped {c.forget()} verdict(s)")
    elif args.forget_llm:
        print(f"  dropped {c.forget(only_llm=True)} llm verdict(s), "
              f"rule verdicts kept")
    else:
        s = c.stats()
        print(f"  {args.db or cache_mod.DEFAULT_DB}")
        print(f"  {s['entries']} verdicts ({s['rule']} rule, {s['llm']} llm)")
        for row in c.profiles():
            print(f"    {row['profile_hash']}  {row['hostname']:<18} "
                  f"{row['summary']}")
    c.close()
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--db", default=None, help="verdict cache path")
    ap.add_argument("--library", default=None, help="staged library root")
    ap.add_argument("--model", default=llm_mod.DEFAULT_MODEL)
    ap.add_argument("--ollama", default=llm_mod.DEFAULT_HOST)
    ap.add_argument("--no-llm", action="store_true",
                    help="deterministic rules only; marginal stays marginal")
    ap.add_argument("--refresh", action="store_true",
                    help="ignore every cached verdict and re-decide")
    ap.add_argument("--refresh-llm", action="store_true",
                    help="drop cached LLM verdicts, keep rule verdicts")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("profile"); p.add_argument("ip"); \
        p.add_argument("--json", action="store_true")
    p = sub.add_parser("plan"); p.add_argument("ip", nargs="+"); \
        p.add_argument("--title", action="append")
    p = sub.add_parser("publish"); p.add_argument("ip", nargs="+"); \
        p.add_argument("--title", action="append")
    sub.add_parser("lint")
    p = sub.add_parser("cache")
    p.add_argument("--clear", action="store_true")
    p.add_argument("--forget-llm", action="store_true")

    args = ap.parse_args(argv)
    if args.cmd == "profile":
        return cmd_profile(args)
    if args.cmd == "plan":
        return cmd_plan(args, publish=False)
    if args.cmd == "publish":
        return cmd_plan(args, publish=True)
    if args.cmd == "lint":
        return cmd_lint(args)
    if args.cmd == "cache":
        return cmd_cache(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())

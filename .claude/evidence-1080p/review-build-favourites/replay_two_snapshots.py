"""Two-pass offline replay of the favourites writer against captured box files
and two DB snapshots taken 20 minutes apart (read-only copies)."""
import sqlite3, sys, os, time, re, pathlib, datetime
WT = "/home/voidsstr/development/retro-agent/.claude/worktrees/lcd1080-patches"
sys.path.insert(0, WT + "/scripts/gameindex")
import db, favorites

SP = "/tmp/claude-1000/-home-voidsstr-development-retro-agent/51aba7ad-1963-4ba5-80a1-3b4e9fa29fcb/scratchpad"
EV = pathlib.Path("/home/voidsstr/development/retro-agent/.claude/evidence-1080p/favourites")

def open_snap(name):
    c = sqlite3.connect(SP + "/" + name); c.row_factory = sqlite3.Row
    t = c.execute("select max(last_seen) from servers").fetchone()[0]
    ts = time.mktime(time.strptime(t, "%Y-%m-%d %H:%M:%S")) + 30
    return c, ts

def pick(con, ts, engine, accepts, incumbents, key):
    real = time.time
    db.time.time = lambda: ts
    try:
        pol = favorites.policy_for(key)
        return db.best_servers(con, pol["servers_from"], limit=pol["slots"],
                               accepts=pol.get("accepts"), local_only=pol["local_only"],
                               strict_accepts=pol["strict_accepts"],
                               incumbent=lambda r: favorites.identity(engine, r) in incumbents)
    finally:
        db.time.time = real

RULES = [
    (re.compile(r"Quake3-TeamArena_baseq3_autoexec\.cfg$|Quake3_baseq3_autoexec\.cfg$", re.I), "q3", "ioquake3"),
    (re.compile(r"_baseq2_autoexec\.cfg$", re.I), "q2", "quake2"),
    (re.compile(r"UnrealTournament(436)?_System_UnrealTournament\.ini$", re.I), "unreal", "ut99"),
    (re.compile(r"Unreal_?Gold_System_Unreal\.ini$", re.I), "unreal", "unreal"),
    (re.compile(r"UT2004_System_UT2004\.ini$", re.I), "ut2k4", "ut2004"),
    (re.compile(r"serverbrowser\.vdf$", re.I), "goldsrc", "cs16"),
]

A, ta = open_snap("gameservers-snapshot.db")
B, tb = open_snap("gameservers-snapshot-B.db")
tot = {"p1write": 0, "p2write": 0, "files": 0}
for boxdir in sorted(p for p in EV.iterdir() if p.is_dir()):
    for f in sorted(boxdir.iterdir()):
        if f.name.startswith("run2_") or "bench" in f.name.lower() or "JediAcademy" in f.name:
            continue
        for rx, engine, key in RULES:
            if rx.search(f.name):
                break
        else:
            continue
        existing = f.read_bytes().decode("latin-1")
        inc = favorites.incumbents(engine, existing)
        s1 = pick(A, ta, engine, None, inc, key)
        t1, _ = favorites.render(engine, s1, existing, key=key)
        w1 = not favorites.same_favourites(engine, existing, t1)
        # the box now holds t1 (CRLF on disk, read back as such)
        box1 = t1.replace("\n", "\r\n")
        inc2 = favorites.incumbents(engine, box1)
        s2 = pick(B, tb, engine, None, inc2, key)
        t2, _ = favorites.render(engine, s2, box1, key=key)
        w2 = not favorites.same_favourites(engine, box1, t2)
        a1 = sorted(favorites.incumbents(engine, t1)); a2 = sorted(favorites.incumbents(engine, t2))
        tot["files"] += 1; tot["p1write"] += w1; tot["p2write"] += w2
        extra = ""
        if w2:
            extra = " changed: -%s +%s" % (sorted(set(a1) - set(a2)), sorted(set(a2) - set(a1)))
        print("%-14s %-62s %-7s p1=%-5s p2=%-5s n=%d%s" % (boxdir.name, f.name[:62], engine,
              "WRITE" if w1 else "same", "WRITE" if w2 else "same", len(a2), extra))
print(tot)

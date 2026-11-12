"""Replay consecutive real DB snapshots (one per service pass) against the
captured box files, with the builder's modules and the reviewed modules, and
count the rewrites each would have made after the first pass."""
import importlib.util, sqlite3, sys, time, re, pathlib, glob, os
SP = "/tmp/claude-1000/-home-voidsstr-development-retro-agent/51aba7ad-1963-4ba5-80a1-3b4e9fa29fcb/scratchpad"
WT = "/home/voidsstr/development/retro-agent/.claude/worktrees/lcd1080-patches/scripts/gameindex"
BV = "/home/voidsstr/development/retro-agent/.claude/evidence-1080p/review-build-favourites/builder-version"
EV = pathlib.Path("/home/voidsstr/development/retro-agent/.claude/evidence-1080p/favourites")

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

versions = {
    "builder": (load("bdb", BV + "/db.py"), load("bfav", BV + "/favorites.py")),
    "reviewed": (load("ndb", WT + "/db.py"), load("nfav", WT + "/favorites.py")),
}
snaps = [SP + "/gameservers-snapshot.db"] + sorted(glob.glob(SP + "/snaps/snap-*.db"))
def snap_time(p):
    c = sqlite3.connect(p); t = c.execute("select max(last_seen) from servers where is_local=0").fetchone()[0]; c.close()
    return t
snaps = sorted(snaps, key=snap_time)
print("passes:", [snap_time(p)[11:] for p in snaps])

RULES = [
    (re.compile(r"(Quake3-TeamArena|Quake3)_baseq3_autoexec\.cfg$", re.I), "q3", "ioquake3"),
    (re.compile(r"_baseq2_autoexec\.cfg$", re.I), "q2", "quake2"),
    (re.compile(r"UnrealTournament(436)?_System_UnrealTournament\.ini$", re.I), "unreal", "ut99"),
    (re.compile(r"Unreal_?Gold_System_Unreal\.ini$", re.I), "unreal", "unreal"),
    (re.compile(r"UT2004_System_UT2004\.ini$", re.I), "ut2k4", "ut2004"),
    (re.compile(r"serverbrowser\.vdf$", re.I), "goldsrc", "cs16"),
]
files = []
for boxdir in sorted(p for p in EV.iterdir() if p.is_dir()):
    for f in sorted(boxdir.iterdir()):
        if f.name.startswith("run2_") or "JediAcademy" in f.name:
            continue
        for rx, engine, key in RULES:
            if rx.search(f.name):
                files.append((boxdir.name, f, engine, key)); break

for vname, (db, fav) in versions.items():
    tot_first = tot_later = 0
    per_pass = [0] * len(snaps)
    for box, f, engine, key in files:
        state = f.read_bytes().decode("latin-1")
        for i, sp in enumerate(snaps):
            con = sqlite3.connect(sp); con.row_factory = sqlite3.Row
            t = snap_time(sp)
            ts = time.mktime(time.strptime(t, "%Y-%m-%d %H:%M:%S")) + 30
            real = time.time; db.time.time = lambda: ts
            try:
                pol = fav.policy_for(key)
                inc = fav.incumbents(engine, state)
                kw = dict(limit=pol["slots"], accepts=pol.get("accepts"), local_only=pol["local_only"],
                          strict_accepts=pol["strict_accepts"],
                          incumbent=lambda r, e=engine, s=inc: fav.identity(e, r) in s)
                servers = db.best_servers(con, pol["servers_from"], **kw)
            finally:
                db.time.time = real
                con.close()
            text, _ = fav.render(engine, servers, state, key=key)
            if not fav.same_favourites(engine, state, text):
                per_pass[i] += 1
                state = text.replace("\n", "\r\n")
    print("%-9s writes per pass: %s   (first pass %d, every later pass together %d, files %d)"
          % (vname, per_pass, per_pass[0], sum(per_pass[1:]), len(files)))

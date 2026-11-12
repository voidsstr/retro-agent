import importlib.util, sqlite3, sys, time, glob, pathlib
sys.dont_write_bytecode = True
SP = "/tmp/claude-1000/-home-voidsstr-development-retro-agent/51aba7ad-1963-4ba5-80a1-3b4e9fa29fcb/scratchpad"
WT = "/home/voidsstr/development/retro-agent/.claude/worktrees/lcd1080-patches/scripts/gameindex"
BV = "/home/voidsstr/development/retro-agent/.claude/evidence-1080p/review-build-favourites/builder-version"
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
V = {"builder": (load("bdb", BV + "/db.py"), load("bfav", BV + "/favorites.py")),
     "reviewed": (load("ndb", WT + "/db.py"), load("nfav", WT + "/favorites.py"))}
def st(p):
    c = sqlite3.connect(p); t = c.execute("select max(last_seen) from servers where is_local=0").fetchone()[0]; c.close(); return t
snaps = sorted([SP + "/gameservers-snapshot.db"] + glob.glob(SP + "/snaps/snap-*.db"), key=st)
f = pathlib.Path("/home/voidsstr/development/retro-agent/.claude/evidence-1080p/favourites/192.168.1.240/C_Games_Quake3-TeamArena_baseq3_autoexec.cfg")
for vn, (db, fav) in V.items():
    state = f.read_bytes().decode("latin-1")
    print("==", vn)
    for sp in snaps:
        t = st(sp); ts = time.mktime(time.strptime(t, "%Y-%m-%d %H:%M:%S")) + 30
        con = sqlite3.connect(sp); con.row_factory = sqlite3.Row
        real = time.time; db.time.time = lambda: ts
        try:
            pol = fav.policy_for("ioquake3"); inc = fav.incumbents("q3", state)
            servers = db.best_servers(con, "q3", limit=16, accepts={"baseq3"}, incumbent=lambda r: fav.identity("q3", r) in inc)
            ages = {r["addr"]: (ts - time.mktime(time.strptime(r["last_seen"], "%Y-%m-%d %H:%M:%S")))//60 for r in con.execute("select addr,last_seen from servers where engine='q3'")}
            pl = {r["addr"]: r["players"] for r in con.execute("select addr,players from servers where engine='q3'")}
        finally:
            db.time.time = real
        text, _ = fav.render("q3", servers, state, key="ioquake3")
        a, b = fav.incumbents("q3", state), fav.incumbents("q3", text)
        same = fav.same_favourites("q3", state, text)
        print("  %s %s" % (t[11:], "same" if same else "WRITE  -%s  +%s" % (
            [(x, "age %s min" % ages.get(x, "pruned"), "players %s" % pl.get(x)) for x in sorted(a - b)],
            [(x, "age %s" % ages.get(x), "players %s" % pl.get(x)) for x in sorted(b - a)])))
        if not same: state = text.replace("\n", "\r\n")

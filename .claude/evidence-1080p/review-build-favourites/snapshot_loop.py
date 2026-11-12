import sqlite3, time, os
SP = "/tmp/claude-1000/-home-voidsstr-development-retro-agent/51aba7ad-1963-4ba5-80a1-3b4e9fa29fcb/scratchpad/snaps"
last = None
deadline = time.time() + 60 * 60
while time.time() < deadline:
    src = sqlite3.connect("file:/home/voidsstr/.retro-fleet/gameservers.db?mode=ro", uri=True)
    try:
        m = src.execute("select max(last_seen) from servers where is_local=0").fetchone()[0]
        if m != last:
            dst = sqlite3.connect(os.path.join(SP, "snap-%s.db" % m.replace(" ", "_").replace(":", "")))
            src.backup(dst); dst.close()
            last = m
            print("snapshot", m, flush=True)
    except Exception as e:
        print("error", e, flush=True)
    finally:
        src.close()
    time.sleep(60)

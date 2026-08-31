#!/usr/bin/env python3
r"""Push the compatibility snapshot to the nsc-assistant dashboard.

    python3 scripts/fleet/compat-publish.py                       # to Azure
    python3 scripts/fleet/compat-publish.py --url http://localhost:8000
    python3 scripts/fleet/compat-publish.py --dry-run             # print, send nothing

THE DIRECTION OF TRAVEL IS THE POINT. The LAN database is the source of truth
and the dashboard is a read-only view of a snapshot it is HANDED. Nothing in
the fleet ever calls the dashboard, and no retro box can be affected by it
being down - a hard rule in CLAUDE.md, because a machine that needs a cloud
service to launch a game is a machine that stops working when the internet
does.

NO SECRET LEAVES THE LAN. The payload is scanned by
`compat.py::_assert_no_secrets` before it is sent, and the send is REFUSED
outright rather than scrubbed: silently stripping a key would leave the key
sitting in the local database with nobody warned it got there. Fleet secrets
live in Azure Key Vault `nsc-secrets-kv`.

The publish token is read from `$RETRO_COMPAT_PUBLISH_TOKEN` (or Key Vault
secret `fleet-dashboard-publish-token` when `--from-vault` is given). It is
never accepted on the command line: argv lands in shell history, in `ps`, and
in transcripts.
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import compat        # noqa: E402
import compat_db as C  # noqa: E402

DEFAULT_URL = os.getenv("RETRO_DASHBOARD_URL",
                        "https://nsc-dashboard.azurecontainerapps.io")


def token_from_vault():
    try:
        out = subprocess.run(
            ["python3", os.path.join(HERE, "keyvault.py"), "get",
             "fleet-dashboard-publish-token"],
            capture_output=True, text=True, timeout=60)
    except Exception as e:
        raise SystemExit("cannot reach Key Vault: %s" % e)
    if out.returncode != 0:
        raise SystemExit(
            "Key Vault has no `fleet-dashboard-publish-token`. Create it with\n"
            "  python3 scripts/fleet/keyvault.py set "
            "fleet-dashboard-publish-token --stdin\n"
            "and set the same value as RETRO_COMPAT_PUBLISH_TOKEN on the "
            "dashboard.\n%s" % out.stderr.strip())
    return out.stdout.strip()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--db")
    ap.add_argument("--from-vault", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--out", help="also write the payload here")
    a = ap.parse_args(argv)

    con = C.connect(a.db)
    try:
        rows = compat._matrix_rows(con, include_withdrawn=True)
        doc = {
            "generated": C.now(), "schema": 1,
            "legend": {"deploy": list(C.DEPLOY_STATES),
                       "runs": list(C.RUN_STATES), "mp": list(C.MP_STATES),
                       "renderer": list(C.RENDERERS)},
            "boxes": [dict(r) for r in con.execute(
                "SELECT ip,hostname,os,cpu,cpu_mhz,ram_mb,gpu,gpu_class,"
                "accelerators,display_mode,agent_version,state,note,measured_at"
                " FROM compat_box ORDER BY ip")],
            "titles": [dict(r) for r in con.execute(
                "SELECT title,display_name,engine,parent,kind,mp_design,"
                "in_library,note FROM compat_title ORDER BY title")],
            "matrix": rows,
            "conflicts": [dict(r) for r in con.execute(
                "SELECT * FROM v_compat_conflict")],
            "ingest_log": [dict(r) for r in con.execute(
                "SELECT * FROM compat_ingest ORDER BY id DESC LIMIT 25")],
        }
    finally:
        con.close()

    blob = json.dumps(doc)
    compat._assert_no_secrets(blob)          # refuses, never scrubs
    if a.out:
        open(a.out, "w").write(json.dumps(doc, indent=1))
        print("wrote %s" % a.out)
    print("%d cells, %d boxes, %d titles, %d bytes"
          % (len(doc["matrix"]), len(doc["boxes"]), len(doc["titles"]),
             len(blob)))
    if a.dry_run:
        print("--dry-run: nothing sent")
        return 0

    token = token_from_vault() if a.from_vault else \
        os.getenv("RETRO_COMPAT_PUBLISH_TOKEN", "")
    if not token:
        print("no publish token. Set RETRO_COMPAT_PUBLISH_TOKEN, or pass "
              "--from-vault to read `fleet-dashboard-publish-token`.\n"
              "The dashboard refuses anonymous uploads by design.",
              file=sys.stderr)
        return 2

    req = urllib.request.Request(
        a.url.rstrip("/") + "/api/compat/snapshot", data=blob.encode(),
        headers={"Content-Type": "application/json", "X-Compat-Token": token},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            print("published: %s" % r.read().decode()[:300])
    except urllib.error.HTTPError as e:
        # Report the dashboard's own words. A publisher that prints its own
        # reassuring summary of someone else's error is how a failed publish
        # gets believed.
        print("PUBLISH FAILED %s: %s" % (e.code, e.read().decode()[:400]),
              file=sys.stderr)
        return 1
    except Exception as e:
        print("PUBLISH FAILED: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
r"""Read and write this project's secrets in Azure Key Vault **nsc-secrets-kv**.

WHY THIS EXISTS.  Every secret the fleet depends on -- the XP product key, the
per-title game CD keys -- lives in `nsc-secrets-kv`, and until now fetching one
meant remembering a four-flag `az keyvault secret show` incantation.  Worse, the
obvious shorthand is actively dangerous:

    KEY=$(az keyvault secret show ... --query value -o tsv)   # <-- DON'T

because when `az` is not logged in, or the vault denies you, that pipeline
prints an error to stderr, exits non-zero **into a variable assignment that
swallows it**, and leaves `KEY` empty.  The caller then writes an empty product
key into `winnt.sif` or an empty CD key into an `install.reg`, and the failure
surfaces hours later on a box, as a dialog nobody is standing in front of.
That is exactly the "the tool reported success" failure mode CLAUDE.md is full
of, so this helper **fails loudly and distinguishes the states**:

  * exit 3  -- `az` is missing, or nobody is logged in            (fix: `az login`)
  * exit 4  -- logged in, but no such secret in the vault         (fix: check the name)
  * exit 5  -- logged in, secret may exist, but access is denied  (fix: vault access policy)
  * exit 6  -- the vault is unreachable / some other az failure

Three states, never two.  "Not logged in", "no such secret" and "no access"
have different fixes and must never render the same.

IT NEVER PUTS A SECRET ON A COMMAND LINE.  `set` reads the value from a file or
from stdin, never from argv, because argv lands in shell history, in `ps`, and
in this repo's own transcripts.  `get` writes the value to stdout and nothing
else -- no banner, no trailing newline unless you ask -- so it is safe to
capture, and it writes diagnostics to stderr where a capture cannot hide them.

    python3 scripts/fleet/keyvault.py list                 # every fleet-* secret
    python3 scripts/fleet/keyvault.py list --all           # every secret in the vault
    python3 scripts/fleet/keyvault.py get fleet-gamekey-ut2004
    python3 scripts/fleet/keyvault.py show fleet-gamekey-ut2004   # metadata, NO value
    python3 scripts/fleet/keyvault.py set fleet-gamekey-foo --file /tmp/k.txt \
        --content-type "..." --tag game="Foo" --tag verified=pending
    printf '%s' "$KEY" | python3 scripts/fleet/keyvault.py set fleet-gamekey-foo --stdin

WHEN NOT TO USE IT: never from a fleet box, and never at game-launch time.  A
staged `install.reg` carries the literal key because a Windows `.reg` cannot
indirect through a vault, and because a retro PC must never need the internet to
start a game.  The vault is the SYSTEM OF RECORD and the RECOVERY PATH, not a
runtime dependency.  See `.claude/skills/fleet-keyvault/SKILL.md`.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

VAULT = os.environ.get("RETRO_KEYVAULT", "nsc-secrets-kv")

# The AislePrompt project has its own, unrelated vault.  This helper must never
# reach into it -- naming it here so the boundary is in the code, not only in a
# doc somebody may not read.
FORBIDDEN_VAULTS = {"aisleprompt-kv"}

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_NO_AUTH = 3
EXIT_NOT_FOUND = 4
EXIT_FORBIDDEN = 5
EXIT_UNREACHABLE = 6


class VaultError(RuntimeError):
    def __init__(self, code, message, hint=""):
        super().__init__(message)
        self.code = code
        self.hint = hint


def _az(args, timeout=60):
    """Run `az` and return (rc, stdout, stderr).  Never logs stdout."""
    exe = shutil.which("az")
    if not exe:
        raise VaultError(
            EXIT_NO_AUTH,
            "the Azure CLI (`az`) is not installed or not on PATH",
            "install it, then `az login`",
        )
    try:
        p = subprocess.run(
            [exe] + args,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        raise VaultError(
            EXIT_UNREACHABLE,
            "`az` timed out after %ds -- the vault or the network is unreachable" % timeout,
        )
    return p.returncode, p.stdout, p.stderr


def _classify(rc, err, name=None):
    """Turn an `az` failure into one of the FOUR distinct states.

    The whole point of this function is that a caller can tell WHICH thing is
    wrong.  Collapsing these into a single "failed" is what lets an empty value
    reach a config file.
    """
    low = (err or "").lower()
    if any(s in low for s in (
        "please run 'az login'", 'please run "az login"', "az login",
        "no subscription found", "not logged in", "refresh token has expired",
        "interactive authentication is needed", "authenticationfailed",
    )):
        return VaultError(
            EXIT_NO_AUTH,
            "not logged in to Azure",
            "run `az login`, then retry",
        )
    if "secretnotfound" in low or "was not found in this key vault" in low or (
        "not found" in low and "secret" in low
    ):
        return VaultError(
            EXIT_NOT_FOUND,
            "no secret named %r in vault %r" % (name, VAULT),
            "`keyvault.py list` shows what is there; names are case-insensitive but hyphenated",
        )
    if "forbidden" in low or "does not have secrets get permission" in low or \
       "accessdenied" in low or "authorizationfailed" in low:
        return VaultError(
            EXIT_FORBIDDEN,
            "access denied reading %r from vault %r" % (name, VAULT),
            "the signed-in principal needs a Key Vault access policy / RBAC role; "
            "note this is NOT the same as the secret being absent",
        )
    if any(s in low for s in (
        "could not be resolved", "name or service not known", "temporary failure",
        "connection aborted", "max retries exceeded", "failed to establish a new connection",
        "vaultnotfound", "no such host",
    )):
        return VaultError(
            EXIT_UNREACHABLE,
            "vault %r is unreachable" % VAULT,
            "check DNS/network, and that the vault still exists",
        )
    return VaultError(
        EXIT_UNREACHABLE,
        "`az` failed (rc=%d): %s" % (rc, (err or "").strip().splitlines()[0] if err else "no output"),
    )


def _guard_vault():
    if VAULT in FORBIDDEN_VAULTS:
        raise VaultError(
            EXIT_USAGE,
            "refusing to touch vault %r -- it belongs to the AislePrompt project "
            "and is out of scope for the retro fleet" % VAULT,
        )


def ensure_login():
    """Raise VaultError(EXIT_NO_AUTH) unless `az` has a usable login."""
    rc, out, err = _az(["account", "show", "-o", "json"], timeout=30)
    if rc != 0:
        raise _classify(rc, err)
    try:
        json.loads(out)
    except ValueError:
        raise VaultError(EXIT_NO_AUTH, "`az account show` returned no account JSON",
                         "run `az login`")


def list_secrets(prefix="fleet-"):
    _guard_vault()
    ensure_login()
    rc, out, err = _az([
        "keyvault", "secret", "list", "--vault-name", VAULT,
        "--query", "[].{name:name,contentType:contentType,updated:attributes.updated,enabled:attributes.enabled}",
        "-o", "json",
    ], timeout=120)
    if rc != 0:
        raise _classify(rc, err)
    rows = json.loads(out or "[]")
    if prefix:
        rows = [r for r in rows if (r.get("name") or "").startswith(prefix)]
    rows.sort(key=lambda r: r.get("name") or "")
    return rows


def get_secret(name):
    """Return the secret's value as a str.  Raises VaultError, never returns ''."""
    _guard_vault()
    ensure_login()
    rc, out, err = _az([
        "keyvault", "secret", "show", "--vault-name", VAULT,
        "--name", name, "--query", "value", "-o", "tsv",
    ], timeout=60)
    if rc != 0:
        raise _classify(rc, err, name)
    # `-o tsv` appends a newline of its own; strip exactly that, not the value.
    value = out[:-1] if out.endswith("\n") else out
    if value == "":
        # An empty value is indistinguishable from a swallowed failure at the
        # call site, so refuse to hand one back rather than let it reach a config.
        raise VaultError(
            EXIT_NOT_FOUND,
            "secret %r came back EMPTY -- refusing to return it" % name,
            "a blank secret would be written into a config as if it were real; "
            "check the secret in the portal",
        )
    return value


def show_metadata(name):
    """Metadata only.  Deliberately never fetches the value."""
    _guard_vault()
    ensure_login()
    rc, out, err = _az([
        "keyvault", "secret", "show", "--vault-name", VAULT, "--name", name,
        "--query", "{name:name,contentType:contentType,tags:tags,"
                   "enabled:attributes.enabled,created:attributes.created,"
                   "updated:attributes.updated}",
        "-o", "json",
    ], timeout=60)
    if rc != 0:
        raise _classify(rc, err, name)
    return json.loads(out or "{}")


def set_secret(name, value, content_type=None, tags=None):
    """Write a secret.  `value` is passed through a 0600 temp FILE, never argv."""
    import tempfile
    _guard_vault()
    ensure_login()
    fd, path = tempfile.mkstemp(prefix="kv-", suffix=".tmp")
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w") as fh:
            fh.write(value)
        args = ["keyvault", "secret", "set", "--vault-name", VAULT,
                "--name", name, "--file", path, "--encoding", "utf-8",
                "--query", "id", "-o", "tsv"]
        if content_type:
            if len(content_type) > 255:
                raise VaultError(
                    EXIT_USAGE,
                    "--content-type is %d chars; Key Vault's limit is 255 and it "
                    "rejects the whole call with an unhelpful 'Property  has "
                    "invalid value'" % len(content_type),
                )
            args += ["--content-type", content_type]
        if tags:
            args += ["--tags"] + ["%s=%s" % (k, v) for k, v in tags]
        rc, out, err = _az(args, timeout=120)
        if rc != 0:
            raise _classify(rc, err, name)
        return (out or "").strip()
    finally:
        try:
            # Overwrite before unlinking; the value must not survive in free blocks.
            with open(path, "wb") as fh:
                fh.write(b"\0" * max(64, len(value.encode())))
                fh.flush()
                os.fsync(fh.fileno())
        except OSError:
            pass
        try:
            os.unlink(path)
        except OSError:
            pass


def _fail(e):
    sys.stderr.write("keyvault: %s\n" % e)
    if getattr(e, "hint", ""):
        sys.stderr.write("keyvault: hint: %s\n" % e.hint)
    return e.code


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="keyvault.py",
        description="Read/write retro-fleet secrets in Azure Key Vault %s." % VAULT,
    )
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("list", help="list secrets (fleet-* by default)")
    p.add_argument("--all", action="store_true", help="every secret, not just fleet-*")
    p.add_argument("--prefix", default=None, help="filter by name prefix")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("get", help="print one secret's VALUE to stdout")
    p.add_argument("name")
    p.add_argument("-n", "--no-newline", action="store_true",
                   help="do not append a trailing newline")

    p = sub.add_parser("show", help="print one secret's METADATA (never the value)")
    p.add_argument("name")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("set", help="write a secret from a file or stdin")
    p.add_argument("name")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--file", help="read the value from this file")
    src.add_argument("--stdin", action="store_true", help="read the value from stdin")
    p.add_argument("--content-type", default=None, help="<=255 chars: what it is and what reads it")
    p.add_argument("--tag", action="append", default=[], metavar="K=V",
                   help="repeatable; always include verified=<state>")
    p.add_argument("--keep-trailing-newline", action="store_true",
                   help="by default a single trailing newline is stripped")

    args = ap.parse_args(argv)
    if not args.cmd:
        ap.print_help()
        return EXIT_USAGE

    try:
        if args.cmd == "list":
            prefix = "" if args.all else (args.prefix if args.prefix is not None else "fleet-")
            rows = list_secrets(prefix)
            if args.json:
                print(json.dumps(rows, indent=2))
            elif not rows:
                sys.stderr.write("keyvault: no secrets match prefix %r in %s\n" % (prefix, VAULT))
                return EXIT_NOT_FOUND
            else:
                w = max(len(r["name"]) for r in rows)
                for r in rows:
                    ct = (r.get("contentType") or "").split(" - ")[0][:70]
                    print("%-*s  %s" % (w, r["name"], ct))
            return EXIT_OK

        if args.cmd == "get":
            value = get_secret(args.name)
            sys.stdout.write(value if args.no_newline else value + "\n")
            return EXIT_OK

        if args.cmd == "show":
            meta = show_metadata(args.name)
            if args.json:
                print(json.dumps(meta, indent=2))
            else:
                print("name        : %s" % meta.get("name"))
                print("contentType : %s" % (meta.get("contentType") or "-"))
                print("enabled     : %s" % meta.get("enabled"))
                print("updated     : %s" % meta.get("updated"))
                for k, v in sorted((meta.get("tags") or {}).items()):
                    print("tag %-8s: %s" % (k, v))
                print("(value not shown -- use `get` if you really need it)")
            return EXIT_OK

        if args.cmd == "set":
            if args.stdin:
                value = sys.stdin.read()
            else:
                with open(args.file, "r") as fh:
                    value = fh.read()
            if not args.keep_trailing_newline and value.endswith("\n"):
                value = value[:-1]
            if value == "":
                sys.stderr.write("keyvault: refusing to store an EMPTY value for %r\n" % args.name)
                return EXIT_USAGE
            tags = []
            for t in args.tag:
                if "=" not in t:
                    sys.stderr.write("keyvault: --tag must be K=V, got %r\n" % t)
                    return EXIT_USAGE
                k, v = t.split("=", 1)
                tags.append((k, v))
            if tags and not any(k == "verified" for k, _ in tags):
                sys.stderr.write(
                    "keyvault: warning: no verified= tag. Every fleet-* secret carries one "
                    "(pending / a date + what proved it), or nobody can tell a key that works "
                    "from one that was merely written down.\n")
            sid = set_secret(args.name, value, args.content_type, tags)
            sys.stderr.write("keyvault: stored %s\n" % args.name)
            print(sid)
            return EXIT_OK

    except VaultError as e:
        return _fail(e)
    except OSError as e:
        sys.stderr.write("keyvault: %s\n" % e)
        return EXIT_USAGE
    return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())

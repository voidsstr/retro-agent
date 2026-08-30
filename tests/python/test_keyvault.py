"""The Key Vault helper must fail LOUDLY, and must tell the three failures apart.

WHY THIS TEST EXISTS.  The natural shorthand for reading a secret is

    KEY=$(az keyvault secret show ... --query value -o tsv)

and it is a trap: when `az` is not logged in that assignment swallows the
error and leaves KEY empty, after which an empty product key goes into
`winnt.sif` or an empty CD key into an `install.reg`.  The defect surfaces
hours later, on a box, as a dialog nobody is standing in front of -- the same
"the tool reported success" shape as every other serious defect in this project.

So almost everything asserted here is a NEGATIVE path: that "not logged in",
"no such secret" and "access denied" produce three DIFFERENT exit codes (they
have three different fixes), that an empty value is refused rather than
returned, and that a secret value never reaches argv.

Nothing here touches Azure or the network -- `_az` is stubbed throughout.
"""
import importlib.util
import io
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "keyvault.py")

spec = importlib.util.spec_from_file_location("fleet_keyvault", SRC)
kv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kv)


# --- the classifier: three states, never two -------------------------------

NOT_LOGGED_IN = (
    "ERROR: Please run 'az login' to setup account.",
    "AuthenticationFailed: refresh token has expired",
    "ERROR: No subscription found. Run 'az account set' ...",
)
NO_SUCH_SECRET = (
    "(SecretNotFound) A secret with (name/id) fleet-nope was not found in this key vault.",
    "ERROR: Secret not found: fleet-nope",
)
NO_ACCESS = (
    "(Forbidden) The user, group or application does not have secrets get permission on key vault",
    "AuthorizationFailed: does not have authorization to perform action",
    "ERROR: AccessDenied",
)
UNREACHABLE = (
    "Max retries exceeded with url: /secrets/foo (Caused by NewConnectionError)",
    "ERROR: nsc-secrets-kv.vault.azure.net: Name or service not known",
    "(VaultNotFound) The vault was not found",
)


def test_not_logged_in_is_its_own_state():
    for err in NOT_LOGGED_IN:
        e = kv._classify(1, err)
        assert e.code == kv.EXIT_NO_AUTH, err
        assert "az login" in e.hint


def test_missing_secret_is_its_own_state():
    for err in NO_SUCH_SECRET:
        e = kv._classify(1, err, "fleet-nope")
        assert e.code == kv.EXIT_NOT_FOUND, err


def test_access_denied_is_its_own_state():
    """A denied read must NOT look like an absent secret -- different fix."""
    for err in NO_ACCESS:
        e = kv._classify(1, err, "fleet-x")
        assert e.code == kv.EXIT_FORBIDDEN, err
    assert kv.EXIT_FORBIDDEN != kv.EXIT_NOT_FOUND
    assert kv.EXIT_NO_AUTH != kv.EXIT_NOT_FOUND
    assert kv.EXIT_NO_AUTH != kv.EXIT_FORBIDDEN


def test_unreachable_vault_is_its_own_state():
    for err in UNREACHABLE:
        e = kv._classify(1, err, "fleet-x")
        assert e.code == kv.EXIT_UNREACHABLE, err


def test_unknown_failure_still_fails_and_quotes_az():
    e = kv._classify(9, "ERROR: something nobody has seen before")
    assert e.code == kv.EXIT_UNREACHABLE
    assert "rc=9" in str(e)


# --- get() must never hand back a blank ------------------------------------

def _stub(monkey_rc, monkey_out, monkey_err=""):
    def fake(args, timeout=60):
        if args[:2] == ["account", "show"]:
            return 0, '{"id":"x"}', ""
        return monkey_rc, monkey_out, monkey_err
    return fake


def test_get_strips_only_the_tsv_newline(tmp_path):
    kv._az = _stub(0, "ABCDE-12345\n")
    assert kv.get_secret("fleet-x") == "ABCDE-12345"


def test_get_refuses_an_empty_value():
    """An empty secret is indistinguishable from a swallowed failure at the
    call site, so it must raise rather than return ''."""
    kv._az = _stub(0, "\n")
    try:
        kv.get_secret("fleet-x")
    except kv.VaultError as e:
        assert e.code == kv.EXIT_NOT_FOUND
        assert "EMPTY" in str(e)
    else:
        raise AssertionError("an empty secret was returned instead of raising")


def test_get_propagates_no_auth_rather_than_returning_blank():
    kv._az = _stub(1, "", "ERROR: Please run 'az login' to setup account.")
    try:
        kv.get_secret("fleet-x")
    except kv.VaultError as e:
        assert e.code == kv.EXIT_NO_AUTH
    else:
        raise AssertionError("a not-logged-in az must not yield a value")


# --- set() must never put a secret on the command line ---------------------

def test_set_passes_the_value_by_file_not_argv():
    captured = {}

    def fake(args, timeout=60):
        if args[:2] == ["account", "show"]:
            return 0, '{"id":"x"}', ""
        captured["args"] = list(args)
        idx = args.index("--file")
        with open(args[idx + 1]) as fh:
            captured["file"] = fh.read()
        return 0, "https://v/secrets/fleet-x/1\n", ""

    kv._az = fake
    kv.set_secret("fleet-x", "SUPERSECRETVALUE", "what it is", [("verified", "pending")])
    assert captured["file"] == "SUPERSECRETVALUE"
    assert "SUPERSECRETVALUE" not in " ".join(captured["args"]), \
        "the secret reached argv, where it lands in shell history and ps"


def test_set_removes_the_temp_file():
    paths = {}

    def fake(args, timeout=60):
        if args[:2] == ["account", "show"]:
            return 0, '{"id":"x"}', ""
        paths["p"] = args[args.index("--file") + 1]
        return 0, "id\n", ""

    kv._az = fake
    kv.set_secret("fleet-x", "v", None, None)
    assert not os.path.exists(paths["p"]), "the temp file holding the secret survived"


def test_set_rejects_an_overlong_content_type():
    """Key Vault caps contentType at 255 and rejects the whole call with
    'Property  has invalid value', which says nothing about which property.
    Catch it here, with an error that names the real cause."""
    kv._az = _stub(0, "id\n")
    try:
        kv.set_secret("fleet-x", "v", "z" * 256, None)
    except kv.VaultError as e:
        assert e.code == kv.EXIT_USAGE
        assert "255" in str(e)
    else:
        raise AssertionError("a 256-char contentType was accepted")


# --- the aisleprompt-kv boundary lives in the code, not only in a doc ------

def test_the_aisleprompt_vault_is_refused():
    old = kv.VAULT
    try:
        kv.VAULT = "aisleprompt-kv"
        for fn in (lambda: kv.list_secrets(), lambda: kv.get_secret("x"),
                   lambda: kv.set_secret("x", "y")):
            try:
                fn()
            except kv.VaultError as e:
                assert e.code == kv.EXIT_USAGE
                assert "aisleprompt" in str(e).lower()
            else:
                raise AssertionError("aisleprompt-kv was not refused")
    finally:
        kv.VAULT = old


# --- the CLI surface -------------------------------------------------------

def test_cli_set_refuses_an_empty_stdin(capsys, monkeypatch):
    monkeypatch.setattr(sys, "stdin", io.StringIO("\n"))
    rc = kv.main(["set", "fleet-x", "--stdin"])
    assert rc == kv.EXIT_USAGE
    assert "EMPTY" in capsys.readouterr().err


def test_cli_show_never_prints_the_value(capsys):
    kv._az = _stub(0, '{"name":"fleet-x","contentType":"c","tags":{"verified":"pending"},'
                      '"enabled":true,"created":"c","updated":"u"}')
    assert kv.main(["show", "fleet-x"]) == kv.EXIT_OK
    out = capsys.readouterr().out
    assert "value" not in out.lower() or "not shown" in out
    assert "--query value" not in out


def test_cli_get_exit_code_is_the_classified_one(capsys):
    kv._az = _stub(1, "", "(Forbidden) does not have secrets get permission on key vault")
    assert kv.main(["get", "fleet-x"]) == kv.EXIT_FORBIDDEN
    err = capsys.readouterr().err
    assert "access denied" in err.lower()
    assert "NOT the same as the secret being absent" in err

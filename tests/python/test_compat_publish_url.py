"""The dashboard URL must be a real Container Apps FQDN.

WHY. `DEFAULT_URL` was `https://nsc-dashboard.azurecontainerapps.io` -- the app
name glued directly onto the service domain. Azure never issues that shape, so
it does not resolve, and every publish died with

    PUBLISH FAILED: <urlopen error [Errno -2] Name or service not known>

*after* building a correct 373 KB snapshot. It reads exactly like a network or
DNS fault on the dev host, and was reported as one. It was not: `getent hosts`
on the true FQDN resolves, and `az containerapp show` reports the app Running.

**A wrong constant that fails like an outage is worse than one that fails
loudly** -- it sends people to debug the wrong machine.

Real shape: `<app>.<environment>.<region>.azurecontainerapps.io`.

Offline and cheap: this asserts the SHAPE, never that the host resolves. A test
that needed DNS would fail on a disconnected dev host and teach people to skip
it -- and skipping is how the bad constant survived.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "compat-publish.py")


def _default_url():
    with open(SRC, encoding="utf-8", errors="replace") as f:
        src = f.read()
    m = re.search(r'DEFAULT_URL\s*=\s*os\.getenv\(\s*\n?\s*"RETRO_DASHBOARD_URL"\s*,\s*\n?\s*"([^"]+)"',
                  src)
    assert m, "could not find DEFAULT_URL in compat-publish.py"
    return m.group(1)


def test_the_default_url_has_an_environment_and_region_subdomain():
    url = _default_url()
    host = url.split("//", 1)[-1].rstrip("/")
    assert host.endswith(".azurecontainerapps.io"), host
    labels = host[: -len(".azurecontainerapps.io")].split(".")
    assert len(labels) >= 3, (
        "%r is not a Container Apps FQDN: it needs <app>.<environment>."
        "<region> before azurecontainerapps.io. The two-label form does not "
        "resolve and every publish then fails looking exactly like a DNS "
        "outage on this host." % host)
    assert labels[-1], "missing region label in %r" % host


def test_the_env_var_override_still_exists():
    """A pinned constant must stay overridable, or a redeploy needs a commit."""
    with open(SRC, encoding="utf-8", errors="replace") as f:
        assert "RETRO_DASHBOARD_URL" in f.read(), (
            "the env-var override is gone; the FQDN changes if the Container "
            "Apps environment is recreated")


def test_a_failed_publish_returns_non_zero():
    """Belt and braces on the thing that makes a failure visible.

    Measured caveat for whoever checks this by hand: `script | tail` reports
    TAIL's exit code. Use ${PIPESTATUS[0]} or do not pipe -- misreading that is
    how the exit handling was briefly blamed for a bug it did not have.
    """
    with open(SRC, encoding="utf-8", errors="replace") as f:
        src = f.read()
    assert "sys.exit(main())" in src, (
        "main()'s return value must reach the shell, or a failed publish exits 0")
    assert src.count("return 1") >= 2, (
        "the HTTPError and generic-exception paths must both return non-zero")

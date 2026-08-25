"""retro_brain_guard — the chat brain's fleet-scope policy.

The brain runs autonomously with permission_mode="bypassPermissions", so this
policy is the only thing between the model and the rest of the host. Two
prohibitions, both explicit user directives (2026-08-25): no cloud deployment,
and no changes to the SpecPicks or AislePrompt systems.

These tests exist because both failure directions are expensive. Too loose and
the brain can push to Azure or edit a live revenue site. Too tight and it can
no longer do the job it was widened for -- building and deploying the DOS
player to a fleet box.
"""
import asyncio
import pathlib
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "scripts"))

import retro_brain_guard as guard  # noqa: E402

CWD = str(REPO)
DEV = str(REPO.parent)


def denied(tool, ti):
    return guard.check_tool(tool, ti, cwd=CWD)


# --- no cloud deployment -----------------------------------------------------

@pytest.mark.parametrize("cmd", [
    "az webapp up --name specpicks",
    "aws s3 sync ./dist s3://bucket",
    "gcloud run deploy svc --image x",
    "kubectl apply -f k8s.yaml",
    "terraform apply -auto-approve",
    "flyctl deploy",
    "vercel --prod",
    "docker push registry.example.com/app:1",
    "npm publish",
    "twine upload dist/*",
])
def test_cloud_deploy_commands_are_denied(cmd):
    assert denied("Bash", {"command": cmd}), f"{cmd!r} must be blocked"


def test_cloud_binary_is_caught_behind_a_chain_or_a_prefix():
    # Checking only the first token is the obvious hole: `cd /tmp && az ...`
    # would sail straight through it.
    for cmd in ("cd /tmp && az webapp up",
                "sudo gcloud run deploy",
                "cat x | xargs kubectl apply -f -",
                "env FOO=1 aws s3 ls"):
        assert denied("Bash", {"command": cmd}), f"{cmd!r} must be blocked"


def test_site_repo_azure_deploy_scripts_are_denied():
    assert denied("Bash", {"command": "bash azure/deploy.sh"})
    assert denied("Bash", {"command": "docker build -f Dockerfile.azure ."})


def test_local_docker_and_builds_still_work():
    # The brain has to be able to BUILD. Only the push is a cloud action.
    for cmd in ("docker build -t app .",
                "make -C agent clean && make -C agent",
                "bash tests/run_all.sh",
                "wmake -f doschat.mk"):
        assert denied("Bash", {"command": cmd}) is None, f"{cmd!r} must be allowed"


# --- protected site repos ----------------------------------------------------

@pytest.mark.parametrize("path", [
    f"{DEV}/specpicks/src/simple-server.ts",
    f"{DEV}/aisleprompt/frontend/src/App.tsx",
    f"{DEV}/specpicks/prisma/schema.prisma",
])
def test_writing_a_site_repo_is_denied(path):
    assert denied("Write", {"file_path": path})
    assert denied("Edit", {"file_path": path})


def test_relative_and_traversal_paths_cannot_sneak_in():
    # A guard that only matched absolute paths would be trivially bypassed.
    assert denied("Edit", {"file_path": "../specpicks/src/x.ts"})
    assert denied("Edit", {"file_path": "../../development/aisleprompt/x.ts"})
    assert denied("Write", {"file_path": "./../specpicks/y.ts"})


def test_reading_a_site_repo_is_allowed():
    # Deliberate asymmetry: those repos are often the reference for how
    # something is done here, and reading changes nothing.
    p = f"{DEV}/specpicks/src/simple-server.ts"
    assert denied("Read", {"file_path": p}) is None
    assert denied("Bash", {"command": f"cat {p}"}) is None
    assert denied("Bash", {"command": f"grep -rn Prisma {DEV}/aisleprompt"}) is None


def test_mutating_a_site_repo_by_shell_is_denied():
    assert denied("Bash", {"command": f"sed -i s/a/b/ {DEV}/specpicks/src/x.ts"})
    assert denied("Bash", {"command": f"rm -rf {DEV}/aisleprompt/dist"})
    assert denied("Bash", {"command": f"echo hi > {DEV}/specpicks/x.txt"})


def test_a_name_that_merely_contains_the_repo_name_is_not_protected():
    # Substring matching would block retro-agent's own notes about the sites.
    ok = f"{DEV}/retro-agent/docs/specpicks-notes.md"
    assert denied("Write", {"file_path": ok}) is None


# --- the work the brain was widened FOR --------------------------------------

def test_fleet_application_work_is_allowed():
    for path in (f"{DEV}/retro-agent/scripts/dosgames/dosgame.c",
                 f"{DEV}/retro-agent/agent/doschat/doschat.c",
                 f"{DEV}/retro-agent/agent/src/gameindex.c",
                 f"{DEV}/retro-agent-private/scripts/x.sh",
                 f"{DEV}/nsc-assistant/agent/tools/retro_chat_daemon.py"):
        assert denied("Write", {"file_path": path}) is None, path


def test_fleet_deploy_commands_are_allowed():
    for cmd in ("python3 scripts/retro-wallpaper/deploy_rotation.py 192.168.1.143",
                "python3 provisioning/push_onboard.py 192.168.1.240",
                "curl --upload-file agent/retro_agent.exe -u u:p "
                "'smb://192.168.1.122/files/Utility/Retro%20Automation/retro_agent.exe'"):
        assert denied("Bash", {"command": cmd}) is None, cmd


# --- callback plumbing -------------------------------------------------------

def test_unbalanced_quotes_do_not_open_a_hole():
    # shlex.split raises on this; the fallback tokenizer must still catch it
    # rather than silently allowing the command.
    assert denied("Bash", {"command": 'az webapp up --name "unclosed'})


def test_pretooluse_hook_emits_a_deny_decision():
    # Driven with asyncio.run rather than pytest-asyncio: the repo's test env
    # has no async plugin, and adding one for two coroutines is not worth a
    # new dependency in a suite that is meant to run anywhere in under a second.
    hook = guard.build_pretooluse_hook(CWD)
    out = asyncio.run(hook({"tool_name": "Bash",
                            "tool_input": {"command": "az webapp up"}}, "id", None))
    spec = out["hookSpecificOutput"]
    assert spec["hookEventName"] == "PreToolUse"
    assert spec["permissionDecision"] == "deny"
    assert "cloud" in spec["permissionDecisionReason"].lower()


def test_pretooluse_hook_passes_allowed_calls_through_untouched():
    hook = guard.build_pretooluse_hook(CWD)
    out = asyncio.run(hook({"tool_name": "Bash",
                            "tool_input": {"command": "make -C agent"}}, "id", None))
    assert out == {}, "an allowed call must not carry a permission decision"

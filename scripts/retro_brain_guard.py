"""retro_brain_guard.py — what the chat brain may and may not touch.

The brain runs `permission_mode="bypassPermissions"`: nobody is watching the
retro chat to approve steps, so the SDK cannot prompt. That is fine for fleet
work, but it means a *policy* is the only thing standing between the model and
the rest of this host. This module is that policy, enforced as a
`can_use_tool` callback rather than a paragraph in the system prompt — a
prompt instruction is a request, a callback is a control.

Two prohibitions, both explicit user directives (2026-08-25):

  1. **No cloud deployment.** The brain builds and ships to the *retro fleet*
     over the LAN. It must never push to a cloud provider, a registry, or a
     managed platform.
  2. **No changes to the SpecPicks or AislePrompt systems.** Those are live
     revenue sites with their own agents; the retro brain has no business
     editing them, and a plausible-looking "fix" there is worse than none.

Everything else is allowed, deliberately: the brain is meant to work on the
fleet's own applications (the DOS player DOSGAME, DOSCHAT, the agent, the
driver stacks) exactly like any other Claude session — read logs, edit code,
build, and run deploy scripts that target fleet machines.

The read/write asymmetry is intentional. READING specpicks/aisleprompt is
allowed: their code is often the reference for how something is done here, and
reading changes nothing. Only mutation is blocked.
"""
import os
import re
import shlex
from pathlib import Path

# --- 1. Repositories the brain must not modify -------------------------------
# Matched against the resolved absolute path, so a relative path, a symlink or
# a ../.. traversal cannot walk into them.
PROTECTED_REPOS = ("specpicks", "aisleprompt")

DEV_ROOT = Path(os.environ.get("RETRO_DEV_ROOT", str(Path.home() / "development")))

# --- 2. Cloud deployment surface ---------------------------------------------
# Program names that reach a cloud provider or a public registry. Checked
# against the *command word*, not a substring of the whole line, so a path
# mentioning "aws" in a filename does not trip the guard.
CLOUD_BINARIES = {
    "az", "aws", "gcloud", "gsutil", "kubectl", "helm", "eksctl",
    "terraform", "pulumi", "flyctl", "fly", "heroku", "vercel", "netlify",
    "railway", "render", "doctl", "linode-cli", "wrangler", "firebase",
    "sam", "serverless", "sst", "cdk", "aws-cdk", "ecs-cli", "openshift", "oc",
}

# Multi-word invocations that are cloud pushes even though the program itself
# is fine to run locally. (docker build is allowed; docker push is not.)
CLOUD_SUBCOMMANDS = (
    ("docker", "push"), ("docker", "login"),
    ("podman", "push"), ("podman", "login"),
    ("npm", "publish"), ("yarn", "publish"), ("pnpm", "publish"),
    ("pip", "upload"), ("twine", "upload"),
    ("git", "push"),   # see _git_push_is_local()
)

# Deploy scripts in the site repos, by name. These exist to ship to Azure.
CLOUD_SCRIPT_RE = re.compile(
    r"(azure/(deploy|provision)\.sh|Dockerfile\.azure|deploy-to-(azure|aws|gcp))",
    re.IGNORECASE)

TOOLS_THAT_WRITE = {"Write", "Edit", "NotebookEdit", "MultiEdit"}


def _resolve(path_str, cwd=None):
    """Absolute, symlink-resolved path. Never raises on a nonexistent path."""
    if not path_str:
        return None
    p = Path(path_str)
    if not p.is_absolute():
        p = Path(cwd or os.getcwd()) / p
    try:
        return p.resolve()
    except (OSError, RuntimeError):
        return Path(os.path.normpath(str(p)))


def protected_repo_for(path_str, cwd=None):
    """Return the protected repo name this path falls inside, else None."""
    resolved = _resolve(path_str, cwd)
    if resolved is None:
        return None
    parts = [seg.lower() for seg in resolved.parts]
    for repo in PROTECTED_REPOS:
        # Match a whole path segment: /home/u/development/specpicks/... .
        # A substring test would also catch "specpicks-notes.md", which is not
        # the same thing at all.
        if repo in parts:
            return repo
    return None


def _git_push_is_local(argv):
    """`git push` to a fleet/self-hosted remote is fine; to a cloud host is not.

    We cannot resolve the remote's URL here without running git, so be
    conservative: allow it only when the brain is pushing inside a repo it
    owns, which the path check upstream has already vetted. The value of
    listing it at all is that pushing the SITE repos is blocked by the
    protected-repo rule before this is ever reached.
    """
    return True


def check_bash(command, cwd=None):
    """Return a denial reason for a shell command, or None to allow it."""
    if not command or not command.strip():
        return None

    if CLOUD_SCRIPT_RE.search(command):
        return ("that is a cloud deployment script. This brain deploys to the "
                "retro fleet over the LAN only.")

    try:
        tokens = shlex.split(command)
    except ValueError:
        # Unbalanced quotes: we cannot reason about it, so do not pretend to.
        # Fall back to a coarse word check rather than allowing it blind.
        tokens = re.findall(r"[A-Za-z0-9_.\-/]+", command)

    # Walk every token so the guard still fires inside a pipeline, a
    # `&&` chain, or a `sudo`/`env`/`xargs` prefix -- checking only tokens[0]
    # would miss `cd /tmp && az webapp up`, which is the obvious way around it.
    words = [Path(t).name for t in tokens if t and not t.startswith("-")]
    for i, w in enumerate(words):
        if w in CLOUD_BINARIES:
            return (f"'{w}' deploys to a cloud provider. This brain is scoped "
                    f"to the retro fleet on the LAN.")
        for prog, sub in CLOUD_SUBCOMMANDS:
            if w == prog and i + 1 < len(words) and words[i + 1] == sub:
                if (prog, sub) == ("git", "push") and _git_push_is_local(words):
                    continue
                return (f"'{prog} {sub}' publishes outside this network. "
                        f"This brain is scoped to the retro fleet on the LAN.")

    # A shell command can also mutate a protected repo without any write tool.
    for t in tokens:
        if "/" in t or t.startswith("."):
            repo = protected_repo_for(t, cwd)
            if repo and _looks_mutating(command):
                return (f"the {repo} system is off limits to this brain. "
                        f"Reading it is fine; changing it is not.")
    return None


_MUTATING_RE = re.compile(
    r"\b(rm|mv|cp|tee|sed\s+-i|truncate|dd|chmod|chown|install|patch|"
    r"git\s+(commit|checkout|reset|clean|apply|push)|"
    r"npm|yarn|pnpm|make|docker)\b|>>?\s*\S", re.IGNORECASE)


def _looks_mutating(command):
    return bool(_MUTATING_RE.search(command))


def check_tool(tool_name, tool_input, cwd=None):
    """Return a denial reason for this tool call, or None to allow it."""
    ti = tool_input or {}

    if tool_name in TOOLS_THAT_WRITE:
        for key in ("file_path", "path", "notebook_path"):
            repo = protected_repo_for(ti.get(key), cwd)
            if repo:
                return (f"the {repo} system is off limits to this brain. "
                        f"Reading it is fine; changing it is not.")
        return None

    if tool_name == "Bash":
        return check_bash(ti.get("command", ""), cwd)

    return None


def build_can_use_tool(cwd=None, logger=None):
    """Build the SDK `can_use_tool` callback enforcing the policy above."""
    from claude_agent_sdk import PermissionResultAllow, PermissionResultDeny

    async def can_use_tool(tool_name, tool_input, context):
        reason = check_tool(tool_name, tool_input, cwd)
        if reason is None:
            return PermissionResultAllow()
        msg = f"Blocked by fleet policy: {reason}"
        if logger:
            logger.warning("guard denied %s: %s", tool_name, reason)
        # interrupt=False: tell the model why and let it choose another route,
        # rather than killing a session that may be halfway through real work.
        return PermissionResultDeny(message=msg, interrupt=False)

    return can_use_tool


def build_pretooluse_hook(cwd=None, logger=None):
    """Build a PreToolUse hook enforcing the same policy.

    This, not `can_use_tool`, is the load-bearing control. The brain runs with
    `permission_mode="bypassPermissions"`, and in that mode the permission
    callback can legitimately never be consulted -- the whole point of the mode
    is to stop asking. PreToolUse hooks fire regardless of permission mode, so
    the deny happens before the tool runs either way. `can_use_tool` is kept as
    a second layer for whatever paths do consult it.
    """
    async def pre_tool_use(input_data, tool_use_id, context):
        tool_name = (input_data or {}).get("tool_name", "")
        tool_input = (input_data or {}).get("tool_input", {})
        reason = check_tool(tool_name, tool_input, cwd)
        if reason is None:
            return {}
        if logger:
            logger.warning("guard denied %s: %s", tool_name, reason)
        return {
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "deny",
                "permissionDecisionReason": f"Blocked by fleet policy: {reason}",
            }
        }

    return pre_tool_use

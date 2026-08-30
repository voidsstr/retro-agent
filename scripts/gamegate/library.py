"""Reading the staged library: requires.json and the launch.txt shortcut list.

The library lives on the SMB share at
    /mnt/retro-share/Files/Games-Library/<Title>/
and directories whose name starts with `_` are the library's own support
folders (_desktop, _patches), never titles - GAMESYNC skips them and so must
anything that enumerates alongside it, or the gate starts reporting a verdict
for the wallpaper directory.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path

from . import rules

DEFAULT_LIBRARY = Path("/mnt/retro-share/Files/Games-Library")
GATE_SUBDIR = "_gamegate"

#: The agent reads launch.txt with a single 1023-byte read, so a line past that
#: never becomes a shortcut. Mirrored here so the linter reports the same limit
#: the agent enforces rather than a different one.
LAUNCH_TXT_READ = 1023


@dataclass
class Title:
    name: str
    path: Path
    requires_raw: dict | None = None     # None = no file at all
    requires_error: str = ""
    shortcuts: list = None               # launch.txt first columns

    @property
    def has_requirements_file(self) -> bool:
        return self.requires_raw is not None

    def requirements(self, shortcut: str = "") -> rules.Requirements:
        return rules.parse_requirements(self.requires_raw, self.name, shortcut)


def _read_launch_txt(path: Path) -> list:
    """Return the launch.txt targets (first tab-separated column).

    Only the first LAUNCH_TXT_READ bytes are considered, because that is all
    the agent reads - a shortcut declared past that byte silently does not
    exist on any box, and a linter that read the whole file would call it fine.
    """
    f = path / "launch.txt"
    if not f.is_file():
        return []
    try:
        data = f.read_bytes()[:LAUNCH_TXT_READ]
    except OSError:
        return []
    out = []
    for line in data.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        out.append(line.split("\t")[0].strip())
    return out


def load_title(path: Path) -> Title:
    t = Title(name=path.name, path=path, shortcuts=_read_launch_txt(path))
    rj = path / "requires.json"
    if rj.is_file():
        try:
            t.requires_raw = json.loads(rj.read_text(encoding="utf-8",
                                                     errors="replace"))
            if not isinstance(t.requires_raw, dict):
                t.requires_error = "requires.json is not a JSON object"
                t.requires_raw = {}
        except (OSError, ValueError) as exc:
            # A broken file is reported, NOT silently treated as absent: the
            # gate fails open either way, but "somebody wrote a bad one" and
            # "nobody wrote one" need different follow-up.
            t.requires_error = f"unreadable requires.json: {exc}"
            t.requires_raw = {}
    return t


def load_library(root=None) -> list:
    root = Path(root) if root else DEFAULT_LIBRARY
    if not root.is_dir():
        raise FileNotFoundError(f"staged library not mounted: {root}")
    titles = []
    for entry in sorted(root.iterdir()):
        if not entry.is_dir() or entry.name.startswith("_") \
                or entry.name.startswith("."):
            continue
        titles.append(load_title(entry))
    return titles


def gate_dir(root=None) -> Path:
    root = Path(root) if root else DEFAULT_LIBRARY
    return root / GATE_SUBDIR


def verdict_path(profile_hash: str, root=None) -> Path:
    return gate_dir(root) / f"{profile_hash}.txt"


def write_verdict_file(profile_hash: str, text: str, root=None) -> Path:
    """Write it atomically-ish: a truncated file would be read by the agent as
    a shorter list of decisions, and every missing line silently means 'deploy'
    - so replace rather than rewrite in place."""
    d = gate_dir(root)
    d.mkdir(parents=True, exist_ok=True)
    final = d / f"{profile_hash}.txt"
    tmp = d / f".{profile_hash}.tmp"
    tmp.write_text(text, encoding="ascii", errors="replace")
    os.replace(tmp, final)
    return final

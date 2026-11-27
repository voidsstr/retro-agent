"""scripts/models/hfmirror.py - the invariants that keep a model mirror off the game library.

A write under Files/Games-Library is a new "title" to retro-autodeploy, which answers with
GAMESYNC RESET + START on every box, so the destination guard is a safety property, not
tidiness. The hash checks are what let the tool say a 5 GB shard landed intact.
"""
import hashlib
import importlib.util
import os

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "scripts", "models", "hfmirror.py")


@pytest.fixture(scope="module")
def hm():
    spec = importlib.util.spec_from_file_location("hfmirror", SRC)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_destination_is_always_under_ai_models(hm):
    assert hm.dest_path("openai/gpt-oss-120b", "model-00001-of-00015.safetensors") == \
        "Files/AI-Models/openai/gpt-oss-120b/model-00001-of-00015.safetensors"
    assert hm.dest_path("Qwen/X", "sub/dir/f.json").startswith("Files/AI-Models/Qwen/X/")


@pytest.mark.parametrize("repo,name", [
    ("../Games-Library/x", "f"),             # climbing out through the org
    ("org/..", "f"),
    ("org/name", "../../Games-Library/Evil/launch.txt"),
    ("org/name", "a/../../b"),
    ("org", "f"),                            # not org/name
    ("a/b/c", "f"),
    ("org/na;me", "f"),                      # smbclient -c separator
    ('org/na"me', "f"),
    ("org/name", ""),
    ("org/name", "dir//f"),
])
def test_destination_guard_refuses_escapes(hm, repo, name):
    with pytest.raises(ValueError):
        hm.dest_path(repo, name)


def test_excludes_match_repo_paths(hm):
    pats = ["original/*", "metal/*", "consolidated.safetensors"]
    assert hm.excluded("original/model--00001.safetensors", pats)
    assert hm.excluded("metal/model.bin", pats)
    assert hm.excluded("consolidated.safetensors", pats)
    assert not hm.excluded("model-00001-of-00015.safetensors", pats)
    assert not hm.excluded("config.json", [])


def test_lfs_files_are_checked_against_the_published_sha256(hm):
    data = b"x" * 1000
    good = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "blob": "ignored"}
    h = hm.Hasher(good)
    h.update(data)
    assert h.kind == "sha256" and h.ok()
    bad = dict(good, sha256="0" * 64)
    h = hm.Hasher(bad)
    h.update(data)
    assert not h.ok()


def test_small_files_are_checked_against_the_git_blob_sha1(hm):
    # git hash-object of "hello\n" - the blobId Hugging Face reports for a non-LFS file
    data = b"hello\n"
    f = {"size": len(data), "sha256": None, "blob": "ce013625030ba8dba906f756967f9e9ca394464a"}
    h = hm.Hasher(f)
    h.update(data)
    assert h.kind == "git-sha1" and h.ok()
    # and SHA256SUMS still gets a real SHA-256 for it
    assert h.sha256.hexdigest() == hashlib.sha256(data).hexdigest()


def test_a_file_with_no_published_hash_never_passes(hm):
    f = {"size": 3, "sha256": None, "blob": None}
    h = hm.Hasher(f)
    h.update(b"abc")
    assert not h.ok()


def test_the_nas_floor_defaults_to_500_gb(hm):
    # the share also holds the game library and the host backups
    if "HFMIRROR_NAS_FLOOR_GB" not in os.environ:
        assert hm.NAS_FLOOR == 500 * 10 ** 9


GGUF_REPO = [
    {"name": ".gitattributes", "size": 1}, {"name": "README.md", "size": 2},
    {"name": "UD-IQ4_XS/m-00001-of-00002.gguf", "size": 10},
    {"name": "UD-IQ4_XS/m-00002-of-00002.gguf", "size": 20},
    {"name": "UD-Q2_K_XL/m-00001-of-00001.gguf", "size": 30},
    {"name": "engram/table-BF16.gguf", "size": 40},
    {"name": "mmproj-F16.gguf", "size": 5}, {"name": "mmproj-BF16.gguf", "size": 6},
]


def test_include_takes_one_quant_out_of_a_gguf_repo(hm):
    inc = ["UD-IQ4_XS/*", "mmproj-F16.gguf", "README.md", ".gitattributes"]
    names = [f["name"] for f in hm.select(GGUF_REPO, inc)]
    assert names == [".gitattributes", "README.md", "UD-IQ4_XS/m-00001-of-00002.gguf",
                     "UD-IQ4_XS/m-00002-of-00002.gguf", "mmproj-F16.gguf"]
    # a quant published LATER cannot slip in, which is the whole point over excludes
    later = GGUF_REPO + [{"name": "UD-IQ4_NL/m-00001-of-00001.gguf", "size": 50}]
    assert "UD-IQ4_NL/m-00001-of-00001.gguf" not in [f["name"] for f in hm.select(later, inc)]


def test_exclude_applies_after_include_and_no_filter_keeps_everything(hm):
    assert [f["name"] for f in hm.select(GGUF_REPO, ["mmproj-*"], ["mmproj-BF16.gguf"])] == \
        ["mmproj-F16.gguf"]
    assert hm.select(GGUF_REPO) == GGUF_REPO


def test_add_pins_the_commit_and_records_the_plan(hm, tmp_path, monkeypatch):
    monkeypatch.setattr(hm, "STATE_DIR", str(tmp_path))
    monkeypatch.setattr(hm, "QUEUE", str(tmp_path / "queue.json"))
    monkeypatch.setattr(hm, "api_tree", lambda repo, rev="main": (
        {"sha": "f5d175de2cf7" + "0" * 28, "license": "other", "license_name": None,
         "last_modified": None, "gated": False}, list(GGUF_REPO)))

    class A:
        repo = ["unsloth/Qwen3.8-Flash-Next-GGUF"]
        include = ["UD-IQ4_XS/*", "mmproj-F16.gguf"]
        exclude = None
        revision = None
        note = "t"
    assert hm.cmd_add(A()) == 0
    it = hm.load_queue()["items"][0]
    assert it["revision"].startswith("f5d175de2cf7")      # pinned now, not "main" later
    assert it["planned_files"] == 3 and it["planned_bytes"] == 35
    assert it["include"] == ["UD-IQ4_XS/*", "mmproj-F16.gguf"]


def test_add_refuses_filters_that_keep_nothing(hm, tmp_path, monkeypatch):
    monkeypatch.setattr(hm, "STATE_DIR", str(tmp_path))
    monkeypatch.setattr(hm, "QUEUE", str(tmp_path / "queue.json"))
    monkeypatch.setattr(hm, "api_tree", lambda repo, rev="main": (
        {"sha": "a" * 40, "license": None, "license_name": None, "last_modified": None,
         "gated": False}, list(GGUF_REPO)))

    class A:
        repo = ["org/name"]
        include = ["Q9_K/*"]          # a typo: matches nothing
        exclude = None
        revision = None
        note = None
    with pytest.raises(SystemExit):
        hm.cmd_add(A())
    assert hm.load_queue()["items"] == []

#!/usr/bin/env python3
"""Re-derive files.tsv from the staged AvP tree - and say whether it still agrees.

files.tsv is what apply.py trusts, so it must be something anyone can re-derive
rather than a list typed by hand. This walks every file under
Games-Library/AliensVsPredator and decides, from the bytes alone:

  * obfuscated  = its raw head is no format we know AND its de-obfuscated head
                  is one (RFFL / REBCRIF1 / BIK / SMK), or it is ffinfo.txt and
                  de-obfuscates to the fast-file list. Each one must then pass
                  apply.structure_check, and match NakedAVP where NakedAVP lists it.
  * keep        = listed by NakedAVP and already matching it as staged.

It REFUSES to write a table whose counts differ from what the 2026-09-29 review
measured (154 = 145 NakedAVP-verified + 9 FMVs, 56 keep) unless --force: a new
count means the library changed, and that is for a person to look at.

    python3 make_table.py            # derive and diff against the committed files.tsv
    python3 make_table.py --write    # overwrite files.tsv (only with the expected counts)
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Loaded by path under a unique name: every patch directory has an apply.py, and
# a bare `import apply` would hand back whichever one was imported first.
_spec = importlib.util.spec_from_file_location("avp_patch_apply", os.path.join(HERE, "apply.py"))
apply = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(apply)

EXPECT = {"deobfuscate": 154, "deobfuscate_ref": 145, "keep": 56}


def derive(share_root=apply.SHARE_MNT):
    ref = apply.load_reference()
    root = apply.title_root(share_root)
    rows, seen_ref = [], set()
    for dp, _dn, fn in os.walk(root):
        for name in fn:
            full = os.path.join(dp, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            with open(full, "rb") as f:
                data = f.read()
            raw_cls = apply.classify_head(data[:16], data[:4096])
            kind = apply.file_kind(rel)
            plain = None
            if raw_cls == "?" and kind in ("rif", "rffl", "bink", "smk", "ffinfo"):
                d = apply.deobfuscate(data)
                dcls = apply.classify_head(d[:16], d[:4096])
                if (kind == "ffinfo" and dcls == "text") or dcls == kind:
                    plain = d
            rmd5 = ref.get(rel.lower())
            if plain is not None:
                pm = apply.md5_bytes(plain)
                apply.structure_check(kind, plain)          # raises if it is not whole
                if rmd5 and pm != rmd5:
                    raise SystemExit("%s: de-obfuscates to %s, NakedAVP says %s" % (rel, pm, rmd5))
                rows.append(apply.Row(action="deobfuscate", path=rel, size=len(data),
                                      original_md5=apply.md5_bytes(data), patched_md5=pm,
                                      reference_md5=rmd5 or "-",
                                      verify="nakedavp" if rmd5 else kind,
                                      raw_head16=data[:16].hex()))
            elif rmd5:
                m = apply.md5_bytes(data)
                if m != rmd5:
                    raise SystemExit("%s: staged md5 %s is neither NakedAVP's %s nor obfuscated" % (rel, m, rmd5))
                rows.append(apply.Row(action="keep", path=rel, size=len(data), original_md5=m,
                                      patched_md5="-", reference_md5=rmd5, verify="nakedavp",
                                      raw_head16=data[:16].hex()))
            if rmd5:
                seen_ref.add(rel.lower())
    missing = sorted(set(ref) - seen_ref)
    if missing:
        raise SystemExit("NakedAVP lists %d file(s) the tree does not have: %s" % (len(missing), missing[:5]))
    rows.sort(key=lambda r: (r.action != "deobfuscate", r.path.lower()))
    return rows


def counts(rows):
    return {"deobfuscate": sum(r.action == "deobfuscate" for r in rows),
            "deobfuscate_ref": sum(r.action == "deobfuscate" and r.reference_md5 != "-" for r in rows),
            "keep": sum(r.action == "keep" for r in rows)}


HEADER = """AvP Gold staged tree: which files apply.py de-obfuscates, and which it must leave alone.
Derived by make_table.py from /mnt/retro-share/Files/Games-Library/AliensVsPredator
on 2026-09-29 - do not hand-edit; re-run make_table.py and look at the diff.
action       deobfuscate = InstallShield-obfuscated as staged; keep = already equal to NakedAVP
path         relative to the title root, the share's exact case
original_md5 the file as staged today;  patched_md5 = after deobfuscate() ('-' for keep)
reference_md5  NakedAVP Gold md5 (nakedavp-gold-md5.txt) or '-' (the FMVs: none published)
verify       nakedavp | bink | smk - what proves the output
raw_head16   first 16 bytes as staged, hex"""


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args(argv)
    rows = derive()
    c = counts(rows)
    print("derived: %s (expected %s)" % (c, EXPECT))
    if os.path.exists(apply.TABLE_PATH):
        old = dict((r.path, r.as_dict()) for r in apply.load_table())
        new = dict((r.path, r.as_dict()) for r in rows)
        diff = sorted(set(old) ^ set(new)) + sorted(p for p in set(old) & set(new) if old[p] != new[p])
        print("differences from the committed files.tsv: %d" % len(diff))
        for p in diff[:20]:
            print("  %s" % p)
    if a.write:
        if c != EXPECT and not a.force:
            print("REFUSED: counts differ from the reviewed 154/145/56 - the library changed; look first")
            return 1
        apply.write_table(rows, apply.TABLE_PATH, HEADER)
        print("wrote %s" % apply.TABLE_PATH)
    return 0


if __name__ == "__main__":
    sys.exit(main())

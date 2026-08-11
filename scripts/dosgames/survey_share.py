#!/usr/bin/env python3
"""Survey the share's DOS game archives: read zip central directories over
gvfs (no full downloads) and classify each game's install/run pattern.

Output: JSON report (one record per archive) used to design dosgame.exe's
install scripting and the share catalog it consumes.
"""
import json, os, sys, zipfile, random, time

GV = "/run/user/1000/gvfs/smb-share:server=192.168.1.122,share=files/Games/DOS"
def _argv(i, default):
    try:
        return sys.argv[i]
    except IndexError:
        return default

OUT = _argv(1, "/tmp/dos_survey.json")
try:
    SAMPLE_PER_BUCKET = int(_argv(2, "60"))
except ValueError:
    SAMPLE_PER_BUCKET = 60

INSTALLER_NAMES = {"install.exe","install.bat","setup.exe","setup.bat",
                   "instal.exe","setsound.exe","sinstall.exe","uwinstal.exe"}
ARCHIVE_EXT = {".zip",".arj",".lha",".rar",".7z"}
IMG_EXT = {".iso",".bin",".cue",".img",".ima",".ccd",".sub",".mds",".mdf"}

def classify(names):
    """names: list of paths inside the zip (lowercase, / separated)."""
    top = set()
    exes, bats, coms, installers, images, nested = [], [], [], [], [], []
    for n in names:
        parts = n.split("/")
        top.add(parts[0] if len(parts) > 1 else "")
        base = parts[-1]
        ext = os.path.splitext(base)[1]
        depth = len(parts) - 1
        if ext == ".exe": exes.append((depth, n))
        elif ext == ".bat": bats.append((depth, n))
        elif ext == ".com": coms.append((depth, n))
        if base in INSTALLER_NAMES: installers.append(n)
        if ext in IMG_EXT: images.append(n)
        if ext in ARCHIVE_EXT: nested.append(n)
    kind = "unknown"
    if installers and images:
        kind = "cd-image+installer"
    elif images and not exes:
        kind = "cd-image"
    elif installers:
        kind = "installer"
    elif exes or coms or bats:
        kind = "ready-to-run"
    elif nested:
        kind = "nested-archive"
    return {
        "kind": kind,
        "n_files": len(names),
        "top_dirs": sorted(d for d in top if d)[:5],
        "installers": installers[:5],
        "exes_shallow": [n for d, n in sorted(exes)[:8]],
        # COM files are already used to classify an archive as ready-to-run
        # (above), so leaving them out of the record made gen_catalog drop
        # every COM-launched game: 0 of 2,982 catalog rows had a .COM main
        # program, which is impossible for a pre-1990 DOS library.
        "coms_shallow": [n for d, n in sorted(coms)[:5]],
        "bats_shallow": [n for d, n in sorted(bats)[:5]],
        "images": images[:4],
        "nested": nested[:3],
    }

def survey_zip(path):
    try:
        with open(path, "rb") as f:
            zf = zipfile.ZipFile(f)
            names = [i.filename.lower().replace("\\", "/")
                     for i in zf.infolist() if not i.is_dir()]
        return classify(names)
    except Exception as e:
        return {"kind": "error", "error": str(e)[:120]}

def gather():
    jobs = []
    # root-level archives
    for e in os.scandir(GV):
        if e.is_file() and e.name.lower().endswith(".zip"):
            jobs.append(("root", e.path))
        elif e.is_file():
            ext = os.path.splitext(e.name)[1].lower()
            if ext in IMG_EXT | {".rar", ".7z"}:
                jobs.append(("root-other", e.path))
    # collection buckets — random sample per bucket
    coll = os.path.join(GV, "DOS Games Collection")
    for bucket in sorted(os.listdir(coll)):
        bp = os.path.join(coll, bucket)
        if not os.path.isdir(bp): continue
        zips = [f for f in os.listdir(bp) if f.lower().endswith(".zip")]
        random.seed(42)
        for f in random.sample(zips, min(SAMPLE_PER_BUCKET, len(zips))):
            jobs.append(("collection/"+bucket, os.path.join(bp, f)))
    md = os.path.join(GV, "More Dos Games")
    zips = [f for f in os.listdir(md) if f.lower().endswith(".zip")]
    random.seed(42)
    for f in random.sample(zips, min(SAMPLE_PER_BUCKET, len(zips))):
        jobs.append(("more-dos-games", os.path.join(md, f)))
    return jobs

def main():
    jobs = gather()
    print(f"{len(jobs)} archives to survey", flush=True)
    out = []
    t0 = time.time()
    for i, (group, path) in enumerate(jobs):
        name = os.path.basename(path)
        ext = os.path.splitext(name)[1].lower()
        if ext == ".zip":
            rec = survey_zip(path)
        else:
            rec = {"kind": {"iso":"cd-image","bin":"cd-image","cue":"cd-cue",
                            "rar":"rar-archive","7z":"7z-archive"}.get(ext[1:], "other")}
        rec.update({"group": group, "name": name,
                    "size": os.path.getsize(path) if os.path.exists(path) else 0})
        out.append(rec)
        if (i+1) % 25 == 0:
            print(f"  {i+1}/{len(jobs)} ({time.time()-t0:.0f}s)", flush=True)
    with open(OUT, "w") as f:
        json.dump(out, f, indent=1)
    # summary
    from collections import Counter
    print(Counter(r["kind"] for r in out))
    print("wrote", OUT)

if __name__ == "__main__":
    main()

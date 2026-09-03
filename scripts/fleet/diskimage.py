#!/usr/bin/env python3
"""diskimage.py - capture and restore a retro machine's OS disk to the NAS.

WHY THIS EXISTS. Rebuilding a fleet box from the PXE installer takes ~an hour and
lands you with a fresh install, not the machine you had. Every driver fix, every
staged game, every registry repair is gone. An image is the difference between
"reinstall it" and "put it back".

WHY IT IMAGES AN ATTACHED DISK RATHER THAN A RUNNING MACHINE. Three measured
reasons, all of which killed the obvious designs:

  * The agent transport CANNOT carry an image. agent/shared/frameproto.h caps a
    frame at 32 MB and agent/src/files.c:125 HeapAllocs the whole payload before
    sending. Streaming a disk would mean a new transport in C, for XP and 98.
  * Imaging a LIVE Windows disk gives an inconsistent image. The pagefile and
    hiberfil alone add gigabytes of worthless bytes.
  * A PXE-booted Linux imager is the right long-term answer but needs an NBP
    (iPXE/pxelinux) that is not installed, and a kernel+initramfs small enough
    for boxes with 128-255 MB of RAM. That lane is future work; this one works
    today with a USB-SATA/IDE adapter and no new infrastructure.

So: pull the drive, attach it, run this. It is also the only route that will
ever work for the two boxes with no Linux NIC driver (HP 100VG, 3c509) and the
Pentium-classic Win98 box that cannot run a modern i686 kernel at all.

FORMATS. Default is ntfsclone: it copies only allocated clusters, so a 6 GB
volume holding 2.4 GB stores ~2.4 GB rather than 6, and it is exact for the
filesystem. Use --raw for a true byte-for-byte whole-disk image when you need
the partition table, boot sector and any non-NTFS partitions preserved verbatim
(a dual-boot box, or a disk whose MBR you are debugging). Raw is what "byte for
byte" means; ntfsclone is what you usually want.

SAFETY. Restore writes to a block device and that is irreversible, so:
  * --confirm is mandatory, and the device must be named again after it
  * the system disk and anything mounted are refused outright
  * the target must be at least as large as the source
  * a profile mismatch is refused unless --force-profile

Usage:
    diskimage.py list
    diskimage.py capture --device /dev/sdb [--name box] [--raw] [--note "..."]
    diskimage.py restore --image <id> --device /dev/sdb --confirm /dev/sdb
    diskimage.py verify  --image <id>
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

NAS_ROOT = os.environ.get("RETRO_IMAGE_ROOT",
                          "/mnt/retro-share/Files/OS/DiskImages")
# Capture to LOCAL disk first, then move. The NAS measures 6-22 MB/s and its
# CIFS mount is `soft`, so a two-hour write is two hours of exposure to an EIO
# that kills the capture with no resume. Local staging runs at source speed
# (~47 MB/s measured) and the NAS only sees one sequential copy of a finished,
# already-hashed file.
STAGE_ROOT = os.environ.get("RETRO_IMAGE_STAGE", "/var/tmp/diskimage-staging")
CHUNK = 8 * 1024 * 1024


# ---------------------------------------------------------------- helpers

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def die(msg, code=1):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(code)


def human(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0


def dev_size(dev):
    with open(dev, "rb") as fh:
        return fh.seek(0, os.SEEK_END) or fh.tell()


PROBE_MNT = "/tmp/.diskimage-probe"


def system_disks():
    """Every block device backing a mounted filesystem, plus its parents.

    Restoring over the host's own root disk would destroy this machine, so the
    check is deliberately broad: a whole disk counts as in use if ANY of its
    partitions is mounted.

    Our own read-only probe mount is excluded - otherwise reading the profile
    makes the disk look busy and capture refuses the disk it just probed.
    """
    busy = set()
    r = run(["lsblk", "-rno", "NAME,MOUNTPOINT,PKNAME"])
    for line in r.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 2 or not parts[1].strip():
            continue
        mnt = parts[1].strip()
        if mnt == PROBE_MNT:
            continue
        busy.add("/dev/" + parts[0])
        if len(parts) >= 3 and parts[2].strip():
            busy.add("/dev/" + parts[2].strip())
    return busy


def mounted(dev):
    r = run(["findmnt", "-rno", "SOURCE", dev])
    if r.returncode == 0 and r.stdout.strip():
        return True
    # a partition of it mounted counts too
    r = run(["lsblk", "-rno", "NAME,MOUNTPOINT", dev])
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) >= 2 and p[1].strip():
            return True
    return False


def first_ntfs_partition(dev):
    r = run(["lsblk", "-rno", "NAME,FSTYPE", dev])
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) >= 2 and p[1] == "ntfs":
            return "/dev/" + p[0]
    return None


# ------------------------------------------------------- hardware profile

def read_profile(part):
    """Derive the restore-compatibility key by reading the disk's own registry.

    This is deliberately NOT the agent's profile_hash: that folds in the OS
    version and the monitor's panel size (see agent/shared/gamegate.h), can only
    be computed from inside a running Windows, and changes if you swap a monitor.
    Neither is any use to a restore.

    What actually decides whether an image boots on a given machine is the
    storage stack and the HAL - see docs/pxe-failure-catalogue.md, where four
    separate causes of STOP 0x7B all come down to the boot storage driver. So
    those are what the key records.
    """
    prof = {"hal": None, "boot_storage": [], "mergeide": False,
            "windows": None, "computername": None}
    tmp = PROBE_MNT
    os.makedirs(tmp, exist_ok=True)
    mounted_here = False
    try:
        if not mounted(part):
            r = run(["mount", "-o", "ro", part, tmp])
            if r.returncode != 0:
                return prof, "could not mount read-only (need sudo?): " + r.stderr.strip()
            mounted_here = True
            root = tmp
        else:
            r = run(["findmnt", "-rno", "TARGET", part])
            root = r.stdout.strip().splitlines()[0]

        hive = None
        for cand in ("WINDOWS/system32/config/system",
                     "Windows/system32/config/system"):
            p = os.path.join(root, cand)
            if os.path.exists(p):
                hive = p
                break
        if not hive:
            return prof, "no Windows SYSTEM hive found"

        hal = os.path.join(os.path.dirname(os.path.dirname(hive)), "hal.dll")
        if os.path.exists(hal):
            with open(hal, "rb") as fh:
                prof["hal"] = hashlib.sha256(fh.read()).hexdigest()[:16]

        try:
            import hivex
        except ImportError:
            return prof, "python3-hivex not installed; profile is partial"

        h = hivex.Hivex(hive)

        def child(n, name):
            for c in h.node_children(n):
                if h.node_name(c).lower() == name.lower():
                    return c

        def dword(n, k):
            if not n:
                return None
            for v in h.node_values(n):
                if h.value_key(v).lower() == k.lower():
                    try:
                        return h.value_dword(v)
                    except Exception:
                        return None

        root_n = h.root()
        sel = child(root_n, "Select")
        cur = dword(sel, "Current") or 1
        cs = child(root_n, "ControlSet%03d" % cur)
        svcs = child(cs, "Services")
        # every storage miniport that loads at boot IS the compatibility key
        for name in ("atapi", "pciide", "intelide", "viaide", "aliide",
                     "cmdide", "toside", "nvatabus", "ultra", "iaStor",
                     "viamraid", "SI3112", "mv61xx"):
            n = child(svcs, name)
            if n is not None and dword(n, "Start") == 0:
                prof["boot_storage"].append(name)
        cddb = child(child(cs, "Control"), "CriticalDeviceDatabase")
        if cddb is not None:
            names = {h.node_name(c).lower() for c in h.node_children(cddb)}
            prof["mergeide"] = ("primary_ide_channel" in names
                                and "secondary_ide_channel" in names)
        prof["windows"] = "found"
    finally:
        if mounted_here:
            # An ignored umount failure leaks the mount, and capture's safety
            # check then refuses the very disk we just probed. Retry lazily and
            # say so if it still will not go.
            if run(["umount", tmp]).returncode != 0:
                if run(["umount", "-l", tmp]).returncode != 0:
                    print(f"  WARNING: could not unmount the probe at {tmp}")
    return prof, None


def profile_key(prof):
    """A short stable key. Images with the same key should boot the same box."""
    basis = "|".join([
        prof.get("hal") or "nohal",
        ",".join(sorted(prof.get("boot_storage") or [])) or "nostorage",
        "mergeide" if prof.get("mergeide") else "fixed",
    ])
    return hashlib.sha256(basis.encode()).hexdigest()[:12]


# -------------------------------------------------------------- capture

def cmd_capture(a):
    dev = a.device
    if not os.path.exists(dev):
        die(f"{dev} does not exist")
    if dev in system_disks():
        die(f"{dev} backs a mounted filesystem on THIS host - refusing to read "
            f"it as a fleet disk")

    size = dev_size(dev)
    part = first_ntfs_partition(dev)
    print(f"  device      {dev}  ({human(size)})")
    print(f"  ntfs part   {part or '(none found)'}")

    prof, warn = ({}, "raw capture: profile not read")
    if part:
        prof, warn = read_profile(part)
    if warn:
        print(f"  profile     WARNING: {warn}")
    key = profile_key(prof) if prof else "unknown"
    print(f"  profile key {key}")
    if prof.get("boot_storage"):
        print(f"    boot storage : {', '.join(prof['boot_storage'])}")
        mi = "yes" if prof.get("mergeide") else ("NO - image may only boot on "
                                                 "the controller it came from")
        print(f"    MergeIDE     : {mi}")

    name = a.name or f"disk-{os.path.basename(dev)}"
    stamp = time.strftime("%Y%m%d-%H%M%S")
    image_id = f"{name}-{stamp}"
    outdir = os.path.join(NAS_ROOT, key, image_id)

    if not os.path.isdir(NAS_ROOT):
        try:
            os.makedirs(NAS_ROOT, exist_ok=True)
        except OSError as e:
            die(f"cannot create {NAS_ROOT}: {e} (is the NAS mounted rw?)")

    stage = a.stage or STAGE_ROOT
    incoming = os.path.join(stage, image_id + ".incoming")
    os.makedirs(incoming, exist_ok=True)
    free = shutil.disk_usage(stage).free
    print(f"  staging     {incoming}")
    print(f"              {human(free)} free locally")

    method = "raw" if a.raw else ("ntfsclone" if part else "raw")
    src = dev if method == "raw" else part
    blob = os.path.join(incoming, "disk.raw.zst" if method == "raw"
                        else "part.ntfsclone.zst")

    print(f"\n  method      {method}  (source {src})")
    print(f"  writing to  {blob}")
    print("  this is slow - the NAS measures 6-22 MB/s\n")

    t0 = time.time()
    if method == "raw":
        # byte-for-byte, whole disk, partition table and boot sector included
        cmd = f"dd if={src} bs=8M status=progress conv=noerror,sync | zstd -3 -T0 -o {blob!r} -f"
    else:
        # A volume that was shut down uncleanly - which is EVERY disk pulled from
        # a machine with I/O errors, and the reason you are imaging it - makes
        # ntfsclone refuse. --force reads it anyway. The image is then a faithful
        # copy of a dirty filesystem: restore it and Windows runs autochk, exactly
        # as it would have on the original. That is a backup, not a repair.
        force = " --force" if a.force else ""
        cmd = (f"ntfsclone --save-image{force} --output - {src} "
               f"| zstd -3 -T0 -o {blob!r} -f")
    r = subprocess.run(["bash", "-o", "pipefail", "-c", cmd])
    if r.returncode != 0:
        shutil.rmtree(incoming, ignore_errors=True)
        extra = ""
        if method == "ntfsclone" and not a.force:
            extra = ("\n       If it said the volume is scheduled for a check or was "
                     "shut down uncleanly, re-run with --force (keeps the dirty flag, "
                     "which is right for a backup), or --raw to bypass NTFS entirely.")
        die(f"capture failed (exit {r.returncode}){extra}")
    elapsed = time.time() - t0
    stored = os.path.getsize(blob)
    rate = f"{human(stored/max(elapsed,1))}/s"

    print("\n  hashing...")
    hsh = hashlib.sha256()
    with open(blob, "rb") as fh:
        for b in iter(lambda: fh.read(CHUNK), b""):
            hsh.update(b)

    # the partition table is tiny and priceless for an ntfsclone restore
    sfd = run(["sfdisk", "--dump", dev]).stdout
    with open(os.path.join(incoming, "partition-table.sfdisk"), "w") as fh:
        fh.write(sfd)

    manifest = {
        "image_id": image_id,
        "created": stamp,
        "name": name,
        "note": a.note or "",
        "method": method,
        "source_device": dev,
        "source_partition": part,
        "device_bytes": size,
        "stored_bytes": stored,
        "sha256": hsh.hexdigest(),
        "blob": os.path.basename(blob),
        "profile_key": key,
        "profile": prof,
        "capture_seconds": round(elapsed, 1),
        "source_was_dirty": bool(a.force),
        "captured_by": "scripts/fleet/diskimage.py",
    }
    with open(os.path.join(incoming, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2)

    if a.local_only:
        final = os.path.join(stage, image_id)
        os.rename(incoming, final)
        print(f"\n  captured    {human(stored)} in {elapsed/60:.1f} min ({rate})")
        print(f"  image id    {image_id}")
        print(f"  LOCAL ONLY  {final}")
        print(f"  move it later with:  {sys.argv[0]} push --image {image_id}")
        return

    nas_incoming = outdir + ".incoming"
    print(f"\n  copying to the NAS (this is the slow part)...")
    t1 = time.time()
    os.makedirs(nas_incoming, exist_ok=True)
    for f in sorted(os.listdir(incoming)):
        shutil.copy2(os.path.join(incoming, f), os.path.join(nas_incoming, f))
    # verify the copy landed intact before we trust it and delete the local one
    print("  verifying the copy...")
    h2 = hashlib.sha256()
    with open(os.path.join(nas_incoming, os.path.basename(blob)), "rb") as fh:
        for b in iter(lambda: fh.read(CHUNK), b""):
            h2.update(b)
    if h2.hexdigest() != manifest["sha256"]:
        die(f"the copy on the NAS does not match the local image. Local copy kept "
            f"at {incoming}; NAS copy left at {nas_incoming} for inspection.")
    # rename last: a directory without .incoming is a COMPLETE, VERIFIED image
    os.rename(nas_incoming, outdir)
    shutil.rmtree(incoming, ignore_errors=True)
    move = time.time() - t1
    print(f"\n  captured    {human(stored)} in {elapsed/60:.1f} min ({rate})")
    print(f"  copied      to the NAS in {move/60:.1f} min "
          f"({human(stored/max(move,1))}/s)")
    print(f"  image id    {image_id}")
    print(f"  path        {outdir}")


# --------------------------------------------------------------- restore

def load_images():
    out = []
    if not os.path.isdir(NAS_ROOT):
        return out
    for key in sorted(os.listdir(NAS_ROOT)):
        kd = os.path.join(NAS_ROOT, key)
        if not os.path.isdir(kd):
            continue
        for iid in sorted(os.listdir(kd)):
            if iid.endswith(".incoming"):
                continue
            mp = os.path.join(kd, iid, "manifest.json")
            if os.path.exists(mp):
                try:
                    with open(mp) as fh:
                        m = json.load(fh)
                    m["_dir"] = os.path.join(kd, iid)
                    out.append(m)
                except Exception:
                    pass
    return out


def cmd_list(a):
    imgs = load_images()
    if not imgs:
        print(f"no images under {NAS_ROOT}")
        return
    print(f"{'IMAGE ID':<34} {'PROFILE':<13} {'METHOD':<10} {'STORED':>10}  NOTE")
    for m in imgs:
        print(f"{m['image_id']:<34} {m['profile_key']:<13} {m['method']:<10} "
              f"{human(m['stored_bytes']):>10}  {m.get('note','')}")


def find_image(image_id):
    for m in load_images():
        if m["image_id"] == image_id:
            return m
    die(f"no image {image_id!r} (run: diskimage.py list)")


def cmd_verify(a):
    m = find_image(a.image)
    blob = os.path.join(m["_dir"], m["blob"])
    print(f"  {blob}")
    hsh = hashlib.sha256()
    with open(blob, "rb") as fh:
        for b in iter(lambda: fh.read(CHUNK), b""):
            hsh.update(b)
    ok = hsh.hexdigest() == m["sha256"]
    print(f"  sha256 {'OK' if ok else 'MISMATCH'}")
    sys.exit(0 if ok else 2)


def cmd_restore(a):
    m = find_image(a.image)
    dev = a.device
    if a.confirm != dev:
        die(f"--confirm must repeat the target device exactly ({dev}). "
            f"Restore is irreversible.")
    if not os.path.exists(dev):
        die(f"{dev} does not exist")
    if dev in system_disks() or mounted(dev):
        die(f"{dev} is mounted or backs this host's filesystems - refusing")

    tgt = dev_size(dev)
    if tgt < m["device_bytes"]:
        die(f"target is {human(tgt)}, image needs at least "
            f"{human(m['device_bytes'])}")

    print(f"  image    {m['image_id']}  ({m['method']}, {human(m['stored_bytes'])})")
    print(f"  target   {dev}  ({human(tgt)})")
    print(f"  profile  {m['profile_key']}")

    if a.expect_profile and a.expect_profile != m["profile_key"]:
        if not a.force_profile:
            die(f"profile mismatch: image is {m['profile_key']}, you expected "
                f"{a.expect_profile}. An image restored onto a different storage "
                f"controller bugchecks 0x7B - see docs/pxe-failure-catalogue.md. "
                f"Use --force-profile only if you know why that is safe.")
        print("  WARNING: profile mismatch forced")

    blob = os.path.join(m["_dir"], m["blob"])
    print(f"\n  writing {blob}\n  -> {dev}\n  IRREVERSIBLE. Starting in 5s, ^C to abort.")
    time.sleep(5)

    if m["method"] == "raw":
        cmd = f"zstd -d -c {blob!r} | dd of={dev} bs=8M status=progress conv=fsync"
    else:
        pt = os.path.join(m["_dir"], "partition-table.sfdisk")
        if os.path.exists(pt):
            print("  restoring partition table first")
            r = run(["bash", "-c", f"sfdisk {dev} < {pt!r}"])
            if r.returncode != 0:
                die(f"sfdisk failed: {r.stderr.strip()}")
            run(["partprobe", dev])
            time.sleep(2)
        part = first_ntfs_partition(dev) or (dev + "1")
        print(f"  restoring filesystem into {part}")
        cmd = f"zstd -d -c {blob!r} | ntfsclone --restore-image --overwrite {part} -"
    r = subprocess.run(["bash", "-o", "pipefail", "-c", cmd])
    if r.returncode != 0:
        die(f"restore FAILED (exit {r.returncode}) - the target disk is now in an "
            f"unknown state and must be re-restored or reinstalled")
    subprocess.run(["sync"])
    print("\n  restore complete.")
    if not m.get("profile", {}).get("mergeide"):
        print("  NOTE: this image does NOT have the MergeIDE entries. It will only")
        print("        boot on the same storage controller it was captured from.")
        print("        See docs/pxe-failure-catalogue.md - 'Making an image portable'.")


def cmd_push(a):
    """Move a locally-staged image to the NAS, verifying before deleting."""
    stage = a.stage or STAGE_ROOT
    src = os.path.join(stage, a.image)
    mp = os.path.join(src, "manifest.json")
    if not os.path.exists(mp):
        die(f"no staged image at {src}")
    with open(mp) as fh:
        m = json.load(fh)
    outdir = os.path.join(NAS_ROOT, m["profile_key"], m["image_id"])
    if os.path.isdir(outdir):
        die(f"{outdir} already exists on the NAS")
    nas_incoming = outdir + ".incoming"
    os.makedirs(nas_incoming, exist_ok=True)
    print(f"  {src}\n  -> {outdir}")
    for f in sorted(os.listdir(src)):
        shutil.copy2(os.path.join(src, f), os.path.join(nas_incoming, f))
    print("  verifying...")
    h = hashlib.sha256()
    with open(os.path.join(nas_incoming, m["blob"]), "rb") as fh:
        for b in iter(lambda: fh.read(CHUNK), b""):
            h.update(b)
    if h.hexdigest() != m["sha256"]:
        die(f"copy does not match; left at {nas_incoming}")
    os.rename(nas_incoming, outdir)
    shutil.rmtree(src, ignore_errors=True)
    print(f"  done - local staging removed")


def cmd_staged(a):
    stage = a.stage or STAGE_ROOT
    if not os.path.isdir(stage):
        print(f"nothing staged in {stage}")
        return
    rows = [d for d in sorted(os.listdir(stage))
            if os.path.exists(os.path.join(stage, d, "manifest.json"))]
    if not rows:
        print(f"nothing staged in {stage}")
        return
    print(f"staged locally in {stage}:")
    for d in rows:
        with open(os.path.join(stage, d, "manifest.json")) as fh:
            m = json.load(fh)
        print(f"  {m['image_id']:<38} {human(m['stored_bytes']):>10}  "
              f"{'DIRTY' if m.get('source_was_dirty') else ''}")


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="list stored images")
    p.set_defaults(fn=cmd_list)

    p = sub.add_parser("capture", help="image an attached disk to the NAS")
    p.add_argument("--device", required=True, help="e.g. /dev/sdb (whole disk)")
    p.add_argument("--name", help="short machine name for the image id")
    p.add_argument("--raw", action="store_true",
                   help="true byte-for-byte whole-disk image (bigger, exact)")
    p.add_argument("--note", help="free text stored in the manifest")
    p.add_argument("--stage", help=f"local staging directory (default {STAGE_ROOT})")
    p.add_argument("--local-only", action="store_true",
                   help="capture locally and STOP - do not copy to the NAS yet")
    p.add_argument("--force", action="store_true",
                   help="image a volume that was shut down uncleanly. The image "
                        "keeps the dirty flag, so a restored copy runs autochk "
                        "on first boot - which is correct for a backup.")
    p.set_defaults(fn=cmd_capture)

    p = sub.add_parser("verify", help="re-hash a stored image")
    p.add_argument("--image", required=True)
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("restore", help="write a stored image back to a disk")
    p.add_argument("--image", required=True)
    p.add_argument("--device", required=True)
    p.add_argument("--confirm", required=True,
                   help="repeat the device exactly; restore is irreversible")
    p.add_argument("--expect-profile", help="refuse unless the image matches this key")
    p.add_argument("--force-profile", action="store_true")
    p.set_defaults(fn=cmd_restore)

    p = sub.add_parser("push", help="move a locally-staged image to the NAS")
    p.add_argument("--image", required=True)
    p.add_argument("--stage")
    p.set_defaults(fn=cmd_push)

    p = sub.add_parser("staged", help="list images staged locally, not yet on the NAS")
    p.add_argument("--stage")
    p.set_defaults(fn=cmd_staged)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()

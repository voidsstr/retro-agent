# Disk imaging — capture a fleet machine and put it back

`scripts/fleet/diskimage.py` captures a retro machine's OS disk to the NAS and
restores it. It is the difference between "reinstall it" (an hour, and you lose every
driver fix and staged game) and "put it back" (minutes, exactly as it was).

```bash
scripts/fleet/diskimage.py list
scripts/fleet/diskimage.py capture --device /dev/sdb --name NSC-9871C0E9964
scripts/fleet/diskimage.py verify  --image NSC-9871C0E9964-20260902-171500
scripts/fleet/diskimage.py restore --image <id> --device /dev/sdb --confirm /dev/sdb
```

## It images an ATTACHED disk, not a running machine

Pull the drive, put it in a USB-SATA/IDE adapter, run the tool. That sounds like the
crude option; it is the only one that works, and each alternative was rejected on
measurement rather than taste:

- **The agent cannot carry an image.** `agent/shared/frameproto.h:24` caps a frame at
  32 MB and `agent/src/files.c:125` `HeapAlloc`s the entire payload before sending.
  Streaming a disk means writing a new transport in C, for XP *and* 98.
- **A live Windows disk gives an inconsistent image**, and the pagefile plus
  `hiberfil.sys` add gigabytes of bytes nobody wants.
- **A PXE-booted Linux imager is the right long-term answer**, but needs an NBP
  (iPXE or pxelinux — neither installed) and a kernel+initramfs that fits boxes with
  128–255 MB of RAM. That lane is future work.

It is also the *only* route that will ever work for three machines: the HP 100VG box
(no `hp100` driver in modern kernels, and it has never PXE-booted in the server's
entire log), the 3c509 Win98 box (no `CONFIG_EL3` module), and the Pentium-classic
box whose CPU has no CMOV and so cannot run an i686 kernel at all.

## It needs sudo, and it stages locally first

`ntfsclone` and `dd` both read the block device directly, so **every capture runs under
`sudo`**. Not `sudo -E` - sudo rejects it and the tool does not need your environment.

```bash
sudo python3 scripts/fleet/diskimage.py capture --device /dev/sdb --name box --raw
```

The capture writes to `/var/tmp/diskimage-staging` at source speed (~47 MB/s measured),
hashes it there, then does ONE sequential copy to the NAS, **verifies the sha256 of the
copy**, and only then renames it into place and deletes the local staging.

That ordering is deliberate. The NAS measures 6-22 MB/s on a `soft` CIFS mount: streaming
a two-hour capture straight into it is two hours of exposure to an `EIO` that kills the
run with no resume. Staged, the fragile part is a plain copy of an already-hashed file,
and a failure means re-running `push`, not the capture.

- `--local-only` - capture and stop, do not touch the NAS yet
- `push --image <id>` - move a staged image later, same verification
- `staged` - list what is captured locally but not yet pushed

## Choosing a format when the filesystem is damaged

`ntfsclone` does its own consistency check and **refuses an inconsistent filesystem**,
even with `--force`. `--force` only gets past the dirty *flag*; a real inconsistency stops
it dead:

```
Totally 13 cluster accounting mismatches.   ("extra cluster in $Bitmap")
ERROR: Filesystem check failed!
```

That is correct behaviour - an image of an inconsistent filesystem is an inconsistent
image. But it means the machine you MOST want to back up, the one whose disk is failing,
is the one ntfsclone will not touch.

**Use `--raw` there.** `dd` never parses NTFS, so corruption is irrelevant and you get a
faithful byte-for-byte copy of the damage. Image first, repair second - never run
`chkdsk` over a storage path that is failing writes, because chkdsk rewrites metadata and
a failed metadata write loses volumes rather than clusters.

## Two formats, and "byte for byte" means `--raw`

| | what it stores | when |
|---|---|---|
| **ntfsclone** (default) | allocated clusters only — a 6 GB volume holding 2.4 GB stores ~2.4 GB | almost always |
| **`--raw`** | the whole disk, every sector, partition table and MBR included | dual-boot disks, non-NTFS partitions, or when debugging the boot sector |

Both are compressed with `zstd -3`. The fleet's raw disks total 2,334 GB but hold only
640 GB of data, so ntfsclone is roughly a 3.7× saving before compression.

`--raw` is the literal byte-for-byte image. Use it when the bytes outside the
filesystem matter.

## The profile key is about whether it will BOOT

Each image is filed under a **profile key** derived from the disk's own registry:

```
sha256( hal.dll hash | boot-start storage miniports | mergeide? )[:12]
```

That is deliberately *not* the agent's `profile_hash`, which folds in the OS version
and the monitor's panel size, changes when you swap a monitor, and can only be computed
from inside a running Windows — no use at all to a restore.

What actually decides whether an image boots on a given machine is the **storage stack
and the HAL**. [`pxe-failure-catalogue.md`](pxe-failure-catalogue.md) documents four
separate causes of `STOP 0x7B`, and every one of them is the boot storage driver. So
that is what the key records, and `restore` refuses a mismatch unless you pass
`--force-profile`.

### Make images portable first

An image whose registry lacks the MergeIDE entries will only boot on the controller it
came from. `capture` warns when it sees this. Apply the MergeIDE set (documented in the
failure catalogue) to the source disk *before* capturing, and the image becomes portable
across IDE controllers.

## Storage layout

```
/mnt/retro-share/Files/OS/DiskImages/
  <profile-key>/
    <name>-<YYYYmmdd-HHMMSS>/
      manifest.json              id, method, sizes, sha256, profile, timings
      part.ntfsclone.zst         (or disk.raw.zst for --raw)
      partition-table.sfdisk     needed to restore an ntfsclone image
```

A directory is written as `<id>.incoming` and **renamed only on success**, so anything
without that suffix is a complete, hashed image. There is 4.7 TB free.

## Safety

Restore writes to a block device and cannot be undone, so it refuses to run unless:

- `--confirm` repeats the target device **exactly**
- the device is not mounted and does not back any of this host's filesystems
- the target is at least as large as the source
- the profile matches, or `--force-profile` is given explicitly

`capture` additionally refuses to read a device that backs a mounted filesystem here —
that would be the dev host's own disk.

## Caveats worth knowing before you rely on it

- **The NAS is the bottleneck and it is inconsistent** — measured between 6 and 22 MB/s
  on an idle 0.2 ms LAN. A 60 GB raw capture is hours.
- **The CIFS mount is `soft`**, so a NAS hiccup returns `EIO` mid-capture. The
  `.incoming` rename means you will notice; there is no resume.
- **The NAS caps at SMB 2.0.2.** Modern kernels gate both `vers=1.0` and `vers=2.0`
  behind `CONFIG_CIFS_ALLOW_INSECURE_LEGACY`; this host has it, but an imaging
  initramfs built without it would have no route to the NAS at any dialect.
- **v1 is same-machine backup and restore.** Cross-machine cloning additionally raises
  SID, ComputerName and activation questions that are not addressed here.

## If you add a PXE restore lane later — read this first

`pxe_server.py` arms the boot hold **only** when the fetched file's basename equals
`cfg['bootfile']`. A machine served a *different* boot file never arms the hold, so it
is re-offered on every boot — an unbounded reinstall loop, and a silent one. That is the
same class of failure that destroyed a finished install on 2026-09-02. The comparison
must cover every servable boot file before a second one is added. There is a warning at
that line in the source.

Also note `bootfile_by_arch` is not a usable dispatch mechanism: every PXE client this
server has ever seen reports `arch=0`.

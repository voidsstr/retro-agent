"""Deterministic capability rules - the PYTHON MIRROR of agent/shared/gamegate.h.

Two copies of this logic exist on purpose. The agent must be able to gate a
title with nothing but the staged tree in front of it (a freshly PXE-imaged box
syncs its games before any host tool has ever seen it), and the host must be
able to plan and explain a whole fleet without waking eight machines. Neither
copy can be dropped.

A mirrored implementation is worth nothing once it drifts, so
tests/python/test_gamegate_mirror.py COMPILES gamegate.h and compares every
answer in this file against the C one, case by case, rather than trusting a
comment that says they match.

The division of labour, restated because it is the whole design:

    hard NO     an OS floor unmet, a required CPU instruction absent, a GPU two
                whole feature levels short. Arithmetic, no opinion needed.
    MARGINAL    inside GG_MARGIN_PCT of a published minimum, or a GPU exactly
                one level short. THIS is the only band an LLM ever sees.
    RUN         everything met.

A gate that asks a language model whether a Pentium III can run Doom 3 is a bad
gate; the rules have to answer that themselves, and the tests assert they do.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

# ---------------------------------------------------------------- CPU features
CPU_FPU, CPU_MMX, CPU_CMOV = 0x0001, 0x0002, 0x0004
CPU_SSE, CPU_SSE2, CPU_SSE3 = 0x0008, 0x0010, 0x0020
CPU_SSSE3, CPU_SSE41, CPU_3DNOW = 0x0040, 0x0080, 0x0100

FEATURES = {
    "fpu": CPU_FPU, "mmx": CPU_MMX, "cmov": CPU_CMOV, "sse": CPU_SSE,
    "sse2": CPU_SSE2, "sse3": CPU_SSE3, "ssse3": CPU_SSSE3,
    "sse4.1": CPU_SSE41, "3dnow": CPU_3DNOW,
}
FEATURE_NAME = {v: k for k, v in FEATURES.items()}

# ------------------------------------------------------------ GPU feature level
GPU_UNKNOWN, GPU_NONE, GPU_FIXED = -1, 0, 1
GPU_TNL, GPU_SM1, GPU_SM2, GPU_SM3 = 2, 3, 4, 5

GPU_LEVELS = {
    "none": GPU_NONE, "fixed": GPU_FIXED, "tnl": GPU_TNL,
    "sm1.x": GPU_SM1, "sm1": GPU_SM1, "sm2.0": GPU_SM2, "sm2": GPU_SM2,
    "sm3.0": GPU_SM3, "sm3": GPU_SM3,
}
GPU_LEVEL_NAME = {
    GPU_NONE: "none", GPU_FIXED: "fixed", GPU_TNL: "tnl",
    GPU_SM1: "sm1.x", GPU_SM2: "sm2.0", GPU_SM3: "sm3.0",
    GPU_UNKNOWN: "unknown",
}

# ------------------------------------------------------------------- OS floors
OS_UNKNOWN, OS_WIN9X, OS_WIN2K, OS_WINXP = 0, 1, 2, 3
OS_VISTA, OS_WIN7, OS_WIN8, OS_WIN10 = 4, 5, 6, 7

OS_LEVELS = {
    "win9x": OS_WIN9X, "win95": OS_WIN9X, "win98": OS_WIN9X,
    "win2k": OS_WIN2K, "winxp": OS_WINXP, "xp": OS_WINXP,
    "vista": OS_VISTA, "win7": OS_WIN7, "win8": OS_WIN8, "win10": OS_WIN10,
}
OS_LEVEL_NAME = {v: k for k, v in
                 (("win9x", OS_WIN9X), ("win2k", OS_WIN2K), ("winxp", OS_WINXP),
                  ("vista", OS_VISTA), ("win7", OS_WIN7), ("win8", OS_WIN8),
                  ("win10", OS_WIN10))}
OS_LEVEL_NAME[OS_UNKNOWN] = "unknown"

# ----------------------------------------------------------- Machine capabilities
CAP_DISC_MOUNT = 0x0001
CAPABILITIES = {"disc_mount": CAP_DISC_MOUNT}
CAPABILITY_NAME = {v: k for k, v in CAPABILITIES.items()}
CAPABILITY_REMEDY = {
    CAP_DISC_MOUNT: "install a virtual disc mounter (Daemon Tools)",
}

# ----------------------------------------------------------------- Verdicts
V_RUN, V_MARGINAL, V_NO = 0, 1, 2
VERDICT_NAME = {V_RUN: "run", V_MARGINAL: "marginal", V_NO: "no"}
VERDICT_VALUE = {v: k for k, v in VERDICT_NAME.items()}

MARGIN_PCT = 25

# ---------------------------------------------------------------------------
# The GPU table. MUST stay byte-for-byte equivalent to gg_gpu_table[] in
# agent/shared/gamegate.h; the mirror test walks every row of the C table and
# every row of this one, plus a sweep of ids in between.
#
# NVIDIA device ids are NOT ordered by generation - 0x0150 is a GeForce2 GTS
# (2000, DX7) and 0x0160 is a GeForce 6200 (2004, SM3.0) - so this is explicit
# ranges with no "anything above X" fallback. .124's card is 0x0150, which is
# precisely the id any such shortcut gets wrong.
# ---------------------------------------------------------------------------
GPU_TABLE = [
    (0x121A, 0x0001, 0x0009, GPU_FIXED),   # every 3dfx Voodoo: no hardware T&L

    (0x10DE, 0x0018, 0x0019, GPU_FIXED),   # RIVA 128
    (0x10DE, 0x0020, 0x0020, GPU_FIXED),   # RIVA TNT
    (0x10DE, 0x0028, 0x002F, GPU_FIXED),   # RIVA TNT2 / Vanta
    (0x10DE, 0x00A0, 0x00A0, GPU_FIXED),   # Aladdin TNT2
    (0x10DE, 0x0100, 0x0103, GPU_TNL),     # GeForce 256
    (0x10DE, 0x0110, 0x0113, GPU_TNL),     # GeForce2 MX
    (0x10DE, 0x0150, 0x0153, GPU_TNL),     # GeForce2 GTS/Ti/Ultra  <- .124
    (0x10DE, 0x01A0, 0x01A0, GPU_TNL),     # nForce IGP
    (0x10DE, 0x0170, 0x018F, GPU_TNL),     # GeForce4 MX - DX7, no shaders
    (0x10DE, 0x0200, 0x0203, GPU_SM1),     # GeForce3
    (0x10DE, 0x0250, 0x0253, GPU_SM1),     # GeForce4 Ti
    (0x10DE, 0x0280, 0x0289, GPU_SM1),     # GeForce4 Ti AGP8x
    (0x10DE, 0x0300, 0x0334, GPU_SM2),     # GeForce FX 5xxx
    (0x10DE, 0x0040, 0x004F, GPU_SM3),     # GeForce 6800
    (0x10DE, 0x0090, 0x009F, GPU_SM3),     # GeForce 7800
    (0x10DE, 0x00C0, 0x00CF, GPU_SM3),     # GeForce 6800 (NV41/42)
    (0x10DE, 0x00F0, 0x00FF, GPU_SM3),     # NV4x PCIe bridged
    (0x10DE, 0x0140, 0x014F, GPU_SM3),     # GeForce 6600
    (0x10DE, 0x0160, 0x016F, GPU_SM3),     # GeForce 6200
    (0x10DE, 0x01D0, 0x01DF, GPU_SM3),     # GeForce 7300/7400
    (0x10DE, 0x0290, 0x029F, GPU_SM3),     # GeForce 7900
    (0x10DE, 0x0390, 0x039F, GPU_SM3),     # GeForce 7600
    (0x10DE, 0x0400, 0x0429, GPU_SM3),     # GeForce 8500/8600/8400
    (0x10DE, 0x05E0, 0x05FF, GPU_SM3),     # GT200
    (0x10DE, 0x0600, 0x06FF, GPU_SM3),     # G92/G94/G98 incl. 8400 GS (06E4)
    (0x10DE, 0x0A00, 0x0FFF, GPU_SM3),     # GT21x / GF1xx
    (0x10DE, 0x1000, 0x3FFF, GPU_SM3),     # Fermi and later

    (0x1002, 0x4742, 0x4744, GPU_FIXED),   # Rage Pro
    (0x1002, 0x4C42, 0x4C4D, GPU_FIXED),   # Rage LT/Mobility
    (0x1002, 0x5041, 0x5046, GPU_FIXED),   # Rage 128
    (0x1002, 0x5245, 0x524C, GPU_FIXED),   # Rage 128 Pro
    (0x1002, 0x5144, 0x5157, GPU_TNL),     # Radeon 7x00 / RV100 / RV200
    (0x1002, 0x514C, 0x514D, GPU_SM1),     # Radeon 8500/9100
    (0x1002, 0x4242, 0x4242, GPU_SM1),     # All-in-Wonder 8500
    (0x1002, 0x4966, 0x496E, GPU_SM1),     # Radeon 9000
    (0x1002, 0x4144, 0x4154, GPU_SM2),     # Radeon 9500/9700
    (0x1002, 0x4164, 0x4174, GPU_SM2),     # R300 secondary
    (0x1002, 0x4E44, 0x4E56, GPU_SM2),     # Radeon 9800
    (0x1002, 0x5960, 0x5965, GPU_SM2),     # Radeon 9200
    (0x1002, 0x5B60, 0x5B7F, GPU_SM2),     # Radeon X300/X550
    (0x1002, 0x5D48, 0x5D6F, GPU_SM3),     # Radeon X800
    (0x1002, 0x7100, 0x71FF, GPU_SM3),     # Radeon X1000
    (0x1002, 0x7240, 0x729F, GPU_SM3),     # Radeon X1900
    (0x1002, 0x9400, 0x9FFF, GPU_SM3),     # HD 2000 and later
    (0x1002, 0x6600, 0x68FF, GPU_SM3),     # Evergreen / NI / SI

    (0x8086, 0x7121, 0x7125, GPU_FIXED),   # i810
    (0x8086, 0x1132, 0x1132, GPU_FIXED),   # i815
    (0x8086, 0x2562, 0x2572, GPU_FIXED),   # i845G / i865G  <- .171, NO T&L
    (0x8086, 0x2582, 0x2592, GPU_SM2),     # i915G
    (0x8086, 0x2772, 0x27AE, GPU_SM2),     # i945G
    (0x8086, 0x29A2, 0x29D2, GPU_SM3),     # G965 / G33 / G35
    (0x8086, 0x2E00, 0x2E92, GPU_SM3),     # G4x
    (0x8086, 0x0042, 0x0126, GPU_SM3),     # Ironlake / Sandy Bridge HD

    (0x5333, 0x0000, 0xFFFF, GPU_FIXED),   # S3
    (0x102B, 0x0000, 0xFFFF, GPU_FIXED),   # Matrox
    (0x1039, 0x0000, 0xFFFF, GPU_FIXED),   # SiS
    (0x1023, 0x0000, 0xFFFF, GPU_FIXED),   # Trident
    (0x100C, 0x0000, 0xFFFF, GPU_FIXED),   # Tseng
    (0x1013, 0x0000, 0xFFFF, GPU_FIXED),   # Cirrus Logic

    (0x15AD, 0x0000, 0xFFFF, GPU_FIXED),   # VMware SVGA
    (0x80EE, 0x0000, 0xFFFF, GPU_FIXED),   # VirtualBox
    (0x1234, 0x0000, 0xFFFF, GPU_FIXED),   # QEMU/Bochs
]


def gpu_level_from_pci(ven: int, dev: int) -> int:
    """Classify an adapter. UNKNOWN means 'no opinion' and never rejects."""
    if not ven:
        return GPU_UNKNOWN
    for v, lo, hi, level in GPU_TABLE:
        if v == ven and lo <= dev <= hi:
            return level
    return GPU_UNKNOWN


def os_level_from_version(major: int, minor: int, is_nt: bool) -> int:
    """Win9x is decided by the PLATFORM ID, not the version number: Windows 98
    is 4.10 and NT 4.0 is 4.0, and reading NT4 as 'Windows 95' would put a
    9x-only title on it."""
    if not is_nt:
        return OS_WIN9X
    if major == 5 and minor == 0:
        return OS_WIN2K
    if major == 5:
        return OS_WINXP
    if major == 6 and minor == 0:
        return OS_VISTA
    if major == 6 and minor == 1:
        return OS_WIN7
    if major == 6:
        return OS_WIN8
    if major >= 10:
        return OS_WIN10
    return OS_UNKNOWN


# ---------------------------------------------------------------------------


@dataclass
class Profile:
    """What a machine IS. Built from the agent's HWPROFILE reply.

    Every field here is stable across reboots. Nothing that varies - uptime,
    free RAM, free disk - may be added, or profile_hash stops being a usable
    cache key and every poll re-consults the LLM.
    """
    ip: str = ""
    hostname: str = ""
    profile_hash: str = ""
    cpu_vendor: str = ""
    cpu_brand: str = ""
    cpu_family: int = 0
    cpu_model: int = 0
    cpu_stepping: int = 0
    cpu_mhz: int = 0
    cpu_count: int = 0
    cpu_features: int = 0
    ram_mb: int = 0
    gpu_name: str = ""
    gpu_ven: int = 0
    gpu_dev: int = 0
    vram_mb: int = 0
    gpu_level: int = GPU_UNKNOWN
    os_level: int = OS_UNKNOWN
    os_product: str = ""
    os_major: int = 0
    os_minor: int = 0
    dx_major: int = 0
    caps: int = 0
    free_mb: int = 0

    @classmethod
    def from_hwprofile(cls, data: dict, ip: str = "") -> "Profile":
        cpu = data.get("cpu", {}) or {}
        gpu = data.get("gpu", {}) or {}
        os_ = data.get("os", {}) or {}
        caps = data.get("capabilities", {}) or {}
        disks = data.get("disk", []) or []

        def _hex(s):
            try:
                return int(str(s), 16) if str(s).lower().startswith("0x") \
                    else int(str(s) or 0)
            except (TypeError, ValueError):
                return 0

        # C: is where GAMESYNC puts games, so that is the volume that matters.
        free = 0
        for d in disks:
            if str(d.get("root", "")).upper().startswith("C"):
                free = int(d.get("free_mb", 0) or 0)
                break

        p = cls(
            ip=ip,
            hostname=data.get("hostname", ""),
            profile_hash=data.get("profile_hash", ""),
            cpu_vendor=cpu.get("vendor", ""),
            cpu_brand=cpu.get("brand", ""),
            cpu_family=int(cpu.get("family", 0) or 0),
            cpu_model=int(cpu.get("model", 0) or 0),
            cpu_stepping=int(cpu.get("stepping", 0) or 0),
            cpu_mhz=int(cpu.get("mhz", 0) or 0),
            cpu_count=int(cpu.get("count", 0) or 0),
            cpu_features=int(cpu.get("feature_bits", 0) or 0),
            ram_mb=int(data.get("ram_mb", 0) or 0),
            gpu_name=gpu.get("name", ""),
            gpu_ven=_hex(gpu.get("pci_ven", 0)),
            gpu_dev=_hex(gpu.get("pci_dev", 0)),
            vram_mb=int(gpu.get("vram_mb", 0) or 0),
            gpu_level=int(gpu.get("feature_level_num", GPU_UNKNOWN)),
            os_level=int(os_.get("level_num", OS_UNKNOWN) or 0),
            os_product=os_.get("product", ""),
            dx_major=int((data.get("directx", {}) or {}).get("major", 0) or 0),
            caps=int(caps.get("bits", 0) or 0),
            free_mb=free,
        )
        ver = (os_.get("version", "") or "").split(".")
        if len(ver) >= 2:
            try:
                p.os_major, p.os_minor = int(ver[0]), int(ver[1])
            except ValueError:
                pass
        return p

    def feature_names(self) -> list:
        return [n for b, n in sorted(FEATURE_NAME.items())
                if self.cpu_features & b]

    def describe(self) -> str:
        """One line for a human, and the MACHINE block handed to the LLM."""
        gpu = self.gpu_name or "unknown GPU"
        return (f"{self.cpu_brand or self.cpu_vendor} "
                f"{self.cpu_mhz} MHz x{self.cpu_count}, "
                f"{self.ram_mb} MB RAM, {gpu} "
                f"({self.gpu_ven:04X}:{self.gpu_dev:04X}, {self.vram_mb} MB, "
                f"{GPU_LEVEL_NAME.get(self.gpu_level, 'unknown')}), "
                f"{self.os_product}")


@dataclass
class Requirements:
    """What a title NEEDS. A zero/None field is 'no opinion' and never blocks."""
    version: int = 0
    min_cpu_mhz: int = 0
    min_ram_mb: int = 0
    min_vram_mb: int = 0
    disk_mb: int = 0
    req_features: int = 0
    req_caps: int = 0
    min_gpu_level: int = GPU_UNKNOWN
    min_os_level: int = OS_UNKNOWN
    max_os_level: int = OS_UNKNOWN
    notes: str = ""
    title: str = ""
    year: int = 0
    shortcuts: dict = field(default_factory=dict)
    #: True when a requires.json existed at all - "nobody wrote one" and
    #: "somebody checked and there is no floor" are different facts, and only
    #: this field keeps them apart.
    present: bool = False

    def has_opinion(self) -> bool:
        return bool(self.min_cpu_mhz or self.min_ram_mb or self.min_vram_mb
                    or self.req_features or self.req_caps
                    or self.min_gpu_level != GPU_UNKNOWN
                    or self.min_os_level != OS_UNKNOWN
                    or self.max_os_level != OS_UNKNOWN)

    def describe(self) -> dict:
        """The GAME block handed to the LLM. Deliberately the same words the
        schema uses, so the model is never asked to translate."""
        d = {"title": self.title}
        if self.year:
            d["year"] = self.year
        if self.min_cpu_mhz:
            d["min_cpu_mhz"] = self.min_cpu_mhz
        if self.min_ram_mb:
            d["min_ram_mb"] = self.min_ram_mb
        if self.min_vram_mb:
            d["min_vram_mb"] = self.min_vram_mb
        if self.min_gpu_level != GPU_UNKNOWN:
            d["gpu_feature_level"] = GPU_LEVEL_NAME[self.min_gpu_level]
        if self.req_features:
            d["cpu_features"] = [n for b, n in sorted(FEATURE_NAME.items())
                                 if self.req_features & b]
        if self.min_os_level != OS_UNKNOWN:
            d["min_os"] = OS_LEVEL_NAME[self.min_os_level]
        if self.notes:
            d["notes"] = self.notes
        return d


def _overlay(doc: dict, r: Requirements) -> None:
    """Apply whatever `doc` STATES onto r, leaving unstated fields alone.

    Presence, not value, decides - a shortcut writing "requires_capabilities":
    [] must be able to CLEAR a title-level requirement, and an empty list is
    indistinguishable from an absent one by value while meaning the opposite.
    """
    if "requirements_version" in doc:
        r.version = int(doc["requirements_version"] or 0)
    for key, attr in (("min_cpu_mhz", "min_cpu_mhz"),
                      ("min_ram_mb", "min_ram_mb"),
                      ("min_vram_mb", "min_vram_mb"),
                      ("disk_mb", "disk_mb")):
        if key in doc:
            setattr(r, attr, int(doc[key] or 0))
    if "cpu_features" in doc:
        r.req_features = 0
        for name in doc["cpu_features"] or []:
            r.req_features |= FEATURES.get(str(name).lower(), 0)
    if "requires_capabilities" in doc:
        r.req_caps = 0
        for name in doc["requires_capabilities"] or []:
            r.req_caps |= CAPABILITIES.get(str(name).lower(), 0)
    if "gpu_feature_level" in doc:
        r.min_gpu_level = GPU_LEVELS.get(
            str(doc["gpu_feature_level"] or "").lower(), GPU_UNKNOWN)
    if "min_os" in doc:
        r.min_os_level = OS_LEVELS.get(str(doc["min_os"] or "").lower(),
                                       OS_UNKNOWN)
    if "max_os" in doc:
        r.max_os_level = OS_LEVELS.get(str(doc["max_os"] or "").lower(),
                                       OS_UNKNOWN)
    for key in ("notes", "title"):
        if doc.get(key):
            setattr(r, key, str(doc[key]))
    if doc.get("year"):
        r.year = int(doc["year"])


def parse_requirements(doc, title: str = "", shortcut: str = "") -> Requirements:
    """Build Requirements from a parsed requires.json.

    `doc` of None means the file is absent - that is a different fact from an
    empty file, and `present` records which.
    """
    r = Requirements(title=title, present=doc is not None)
    if not doc:
        return r
    flat = {k: v for k, v in doc.items() if k != "shortcuts"}
    _overlay(flat, r)
    r.shortcuts = dict(doc.get("shortcuts") or {})
    if shortcut:
        # Windows filenames are case-insensitive and so is this lookup - a
        # case-sensitive compare would report "no rule for this shortcut" for a
        # rule sitting right there.
        low = shortcut.lower()
        for key, sub in r.shortcuts.items():
            if str(key).lower() == low and isinstance(sub, dict):
                _overlay(sub, r)
                break
    return r


@dataclass
class Decision:
    verdict: int = V_RUN
    limiting: str = ""
    reason: str = ""
    missing_caps: int = 0
    decided_by: str = "rule"
    confidence: float = 1.0

    @property
    def name(self) -> str:
        return VERDICT_NAME[self.verdict]

    def missing_cap_names(self) -> list:
        return [n for b, n in sorted(CAPABILITY_NAME.items())
                if self.missing_caps & b]


def _hard_short(have: int, need: int) -> bool:
    if not need or not have:
        return False
    if have >= need:
        return False
    return have * 100 < need * (100 - MARGIN_PCT)


def _soft_short(have: int, need: int) -> bool:
    if not need or not have:
        return False
    return have < need


def decide(p: Profile, r: Requirements) -> Decision:
    """The deterministic verdict. Mirrors gg_decide() in gamegate.h exactly.

    ORDER MATTERS: the hard binary disqualifications go first, so the reported
    limiting factor is the thing that actually stops the game. A box that is
    both short of RAM and running the wrong OS should be told about the OS -
    otherwise someone buys the wrong part.
    """
    d = Decision()
    if p is None or r is None:
        d.reason = "no data - not gated"
        return d

    # Recorded, never folded into the verdict: a missing mounter is an
    # installer away, and calling that "cannot run" says give up rather than
    # says fix the box.
    d.missing_caps = r.req_caps & ~p.caps

    if (r.min_os_level != OS_UNKNOWN and p.os_level != OS_UNKNOWN
            and p.os_level < r.min_os_level):
        d.verdict, d.limiting = V_NO, "os"
        d.reason = (f"OS too old for this title (have "
                    f"{OS_LEVEL_NAME.get(p.os_level)}, needs "
                    f"{OS_LEVEL_NAME.get(r.min_os_level)})")
        return d
    if (r.max_os_level != OS_UNKNOWN and p.os_level != OS_UNKNOWN
            and p.os_level > r.max_os_level):
        d.verdict, d.limiting = V_NO, "os"
        d.reason = (f"OS too new for this title (have "
                    f"{OS_LEVEL_NAME.get(p.os_level)}, max "
                    f"{OS_LEVEL_NAME.get(r.max_os_level)})")
        return d

    if r.req_features and p.cpu_features:
        missing = r.req_features & ~p.cpu_features
        if missing:
            first = min(b for b in FEATURE_NAME if missing & b)
            d.verdict, d.limiting = V_NO, "cpu_features"
            d.reason = "CPU lacks " + FEATURE_NAME[first]
            return d

    soft = None
    if r.min_gpu_level != GPU_UNKNOWN and p.gpu_level != GPU_UNKNOWN:
        gap = r.min_gpu_level - p.gpu_level
        if gap >= 2:
            d.verdict, d.limiting = V_NO, "gpu_feature_level"
            d.reason = (f"GPU too old for this title's renderer (have "
                        f"{GPU_LEVEL_NAME[p.gpu_level]}, needs "
                        f"{GPU_LEVEL_NAME[r.min_gpu_level]})")
            return d
        if gap == 1:
            soft = ("gpu_feature_level",
                    f"GPU one feature level short (have "
                    f"{GPU_LEVEL_NAME[p.gpu_level]}, needs "
                    f"{GPU_LEVEL_NAME[r.min_gpu_level]})")

    for field_name, have, need, unit, hard_msg, soft_msg in (
            ("cpu_mhz", p.cpu_mhz, r.min_cpu_mhz, "MHz",
             "CPU too slow", "CPU below minimum"),
            ("ram_mb", p.ram_mb, r.min_ram_mb, "MB",
             "not enough RAM", "RAM below minimum"),
            ("vram_mb", p.vram_mb, r.min_vram_mb, "MB",
             "not enough video RAM", "video RAM below minimum")):
        if _hard_short(have, need):
            d.verdict, d.limiting = V_NO, field_name
            d.reason = f"{hard_msg} (have {have} {unit}, needs {need})"
            return d

    # DISK IS HARD AND HAS NO MARGIN BAND: a tree either fits or it does not,
    # and "90% of Far Cry" is not a playable game. It is checked AFTER the
    # cpu/ram/vram floors so that a box which genuinely cannot run the title is
    # told that, rather than being sent to free up space it would then waste.
    #
    # FAILS OPEN on free_mb == 0, which is what an agent that could not measure
    # the volume reports - the same direction as every other absent field. The
    # agent's own GAMESYNC room check is the backstop that sees the REAL tree
    # size rather than this declared estimate; this test exists to refuse the
    # copy BEFORE an hour of SMB1 bandwidth is spent on a title that cannot fit.
    if r.disk_mb and p.free_mb and p.free_mb < r.disk_mb:
        d.verdict, d.limiting = V_NO, "disk"
        d.reason = (f"not enough free disk (have {p.free_mb} MB, "
                    f"needs {r.disk_mb})")
        return d

    if soft is None:
        for field_name, have, need, unit, _h, soft_msg in (
                ("cpu_mhz", p.cpu_mhz, r.min_cpu_mhz, "MHz", "",
                 "CPU below minimum"),
                ("ram_mb", p.ram_mb, r.min_ram_mb, "MB", "",
                 "RAM below minimum"),
                ("vram_mb", p.vram_mb, r.min_vram_mb, "MB", "",
                 "video RAM below minimum")):
            if _soft_short(have, need):
                d.verdict, d.limiting = V_MARGINAL, field_name
                d.reason = f"{soft_msg} (have {have} {unit}, needs {need})"
                return d

    if soft is not None:
        d.verdict, d.limiting, d.reason = V_MARGINAL, soft[0], soft[1]
        return d

    d.reason = "meets requirements"
    return d


# --------------------------------------------------------------------------
# Verdict file
# --------------------------------------------------------------------------

VERDICT_HEADER = "# gamegate v1"


def format_verdict_file(profile: Profile, rows, model: str,
                        generated: str) -> str:
    """Render <library>/_gamegate/<profile_hash>.txt.

    Tab separated for the same reason PREFER.TXT is: a title name contains
    spaces and a reason certainly does. ONLY a "no" line blocks a title, so a
    file cut off mid-write can never do worse than deploy everything.
    """
    out = [
        f"{VERDICT_HEADER} profile={profile.profile_hash} "
        f"host={profile.hostname or profile.ip} generated={generated}",
        f"# {profile.describe()}",
        f"# model={model}",
        "# <verdict>\t<title>\t<limiting>\t<reason>",
    ]
    for title, d in rows:
        out.append("\t".join((d.name, title, d.limiting or "-",
                              (d.reason or "").replace("\t", " ")
                              + f" [{d.decided_by}]")))
    return "\n".join(out) + "\n"


def parse_verdict_file(text: str) -> dict:
    """Read one back. Mirrors gg_verdict_parse(); unknown verdicts are ignored
    rather than treated as a rejection."""
    out = {}
    for line in text.splitlines():
        line = line.rstrip("\r")
        s = line.lstrip(" \t")
        if not s or s[0] in "#;":
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        verdict = VERDICT_VALUE.get(parts[0].strip())
        if verdict is None:
            continue
        title = parts[1]
        if not title:
            continue
        out[title] = (verdict, parts[2] if len(parts) > 2 else "",
                      parts[3] if len(parts) > 3 else "")
    return out


_HEX = re.compile(r"^0x", re.I)

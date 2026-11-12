#!/usr/bin/env python3
"""Regenerate the static evidence for the JKA / SoF2 / RTCW 1080p survey.

Read-only: reads the staged library at /mnt/retro-share and the share's
Game Updates folder, writes text files next to this script. Needs pefile +
capstone (both on the dev host).
"""
import os, re, struct, hashlib, zipfile, io, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

LIB = "/mnt/retro-share/Files/Games-Library"
UPD = "/mnt/retro-share/Files/Game Updates"
OUT = os.path.dirname(os.path.abspath(__file__))
md = Cs(CS_ARCH_X86, CS_MODE_32)


class PE:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        pe = pefile.PE(data=self.data, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.secs = [(s.Name.rstrip(b"\0").decode(), s.VirtualAddress,
                      s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData)
                     for s in pe.sections]
        self.subsys = "%d.%d" % (pe.OPTIONAL_HEADER.MajorSubsystemVersion,
                                 pe.OPTIONAL_HEADER.MinorSubsystemVersion)

    def va2off(self, v):
        r = v - self.base
        for n, va, vs, raw, rs in self.secs:
            if va <= r < va + max(vs, rs):
                return raw + (r - va)

    def cstr(self, v, n=80):
        o = self.va2off(v)
        if o is None:
            return None
        s = self.data[o:o + n].split(b"\0")[0]
        try:
            s = s.decode("ascii")
        except UnicodeDecodeError:
            return None
        return s if s and all(32 <= ord(c) < 127 or c in "\n\t" for c in s) else None

    def dis(self, start, end):
        o = self.va2off(start)
        out = []
        for ins in md.disasm(self.data[o:o + (end - start)], start):
            ann = ""
            for m in re.findall(r"0x[0-9a-f]{6,8}", ins.op_str):
                s = self.cstr(int(m, 16))
                if s and len(s) > 1:
                    ann += '   ; "%s"' % s.strip()
            if ins.mnemonic == "push" and re.fullmatch(r"0x[0-9a-f]{8}", ins.op_str):
                f = struct.unpack("<f", struct.pack("<I", int(ins.op_str, 16)))[0]
                if 0.001 < abs(f) < 1e6 and not ann:
                    ann = "   ; float %g" % f
            out.append("  %08x: %-7s %s%s" % (ins.address, ins.mnemonic, ins.op_str, ann))
        return "\n".join(out)

    def modetable(self, first_desc="Mode  0: 320x240", stride=16, n=40):
        b = first_desc.encode() + b"\0"
        i = self.data.find(b)
        va = None
        for n_, v, vs, raw, rs in self.secs:
            if raw <= i < raw + rs:
                va = self.base + v + (i - raw)
        ref = self.data.find(struct.pack("<I", va))
        rows = []
        for k in range(n):
            p, w, h = struct.unpack_from("<III", self.data, ref + stride * k)
            d = self.cstr(p)
            if not d or not d.startswith("Mode"):
                break
            rows.append("  %2d  %4d x %-4d  %s" % (k, w, h, d))
        return rows


def md5(path):
    return hashlib.md5(open(path, "rb").read()).hexdigest()


def write(name, text):
    with open(os.path.join(OUT, name), "w") as fh:
        fh.write(text if text.endswith("\n") else text + "\n")


# ---------------------------------------------------------------- binaries
bins = [
    ("JediAcademy", "jasp.exe"), ("JediAcademy", "jamp.exe"),
    ("SoldierOfFortune2", "SoF2.exe"), ("SoldierOfFortune2", "sof2mp.exe"),
    ("ReturnToCastleWolfenstein", "WolfSP.exe"),
    ("ReturnToCastleWolfenstein", "WolfMP.exe"),
]
lines = ["# staged engine binaries analysed (md5, size, PE SubsystemVersion, version string)"]
tables = []
for t, b in bins:
    p = os.path.join(LIB, t, b)
    pe = PE(p)
    ver = [m.group(0).decode() for m in re.finditer(
        rb"(Wolf 1\.[0-9A-Za-z.-]+|SOF2MP GOLD V[0-9.]+|JAmp: v[0-9.]+|JA: v[0-9.]+)", pe.data)]
    lines.append("%s/%s  md5=%s  size=%d  subsys=%s  %s" % (
        t, b, md5(p), len(pe.data), pe.subsys, sorted(set(ver))))
    stride = 12 if t == "JediAcademy" else 16
    tables.append("== %s/%s  (engine r_mode table; index = r_mode value)" % (t, b))
    tables += pe.modetable(stride=stride)
write("binaries.txt", "\n".join(lines))
write("engine-mode-tables.txt", "\n".join(tables))

# ---------------------------------------------------------------- SoF2 MP
pe = PE(os.path.join(LIB, "SoldierOfFortune2", "sof2mp.exe"))
txt = []
txt.append("sof2mp.exe (SOF2MP GOLD V1.03) - why r_mode -1 cannot reach the renderer\n")
txt.append("[1] R_Register: r_mode is registered through a 5-argument Cvar_Get with a\n"
           "    RANGE: Cvar_Get(\"r_mode\", \"3\", 0x2021, 3.0f, 10.0f). Flag 0x2000 = range-checked.\n"
           "    r_customwidth/height/aspect are registered with min=max=0 and flags 0x21 (no range).\n"
           "    Pushes are right-to-left, so the first two pushes are max, then min.")
txt.append(pe.dis(0x4ba5ff, 0x4ba6a7))
txt.append("\n    raw bytes at file offset 0xBA614: " +
           pe.data[0xBA614:0xBA623].hex(" ") +
           "\n    = push 10.0f ; push 3.0f ; push 0x2021   (pattern occurs exactly once in the file)")
txt.append("\n[2] Cvar_Get, existing-cvar path: if (flags & 0x2000) and value < min -> Cvar_Set2(name, \"%g\" of min, force);\n"
           "    if value > max -> set to max. So a -1 from the command line or any cfg becomes 3 = 640x480\n"
           "    at the moment the renderer registers r_mode, and 11 (856x480) becomes 10 (2048x1536).")
txt.append(pe.dis(0x44b3b2, 0x44b444))
txt.append("\n[3] R_GetModeInfo DOES carry the mode == -1 branch (r_customwidth [0x11a6498]->integer,\n"
           "    r_customheight [0x11a6384]->integer, r_customaspect [0x11a3648]->value) - it is dead code,\n"
           "    because the clamp above means r_mode is never -1 when GLW_SetMode calls it.")
txt.append(pe.dis(0x4b98a0, 0x4b9919))
txt.append("\n[4] GLW_LoadOpenGL reads r_mode ([0x11a6490]) for the mode it asks for; on failure it retries mode 3.")
txt.append(pe.dis(0x4dee74, 0x4deeee))
write("sof2mp-r_mode-clamp.disasm.txt", "\n".join(txt))

# ---------------------------------------------------------------- SoF2 SP
pe = PE(os.path.join(LIB, "SoldierOfFortune2", "SoF2.exe"))
txt = ["SoF2.exe (single player) - NO clamp, and the -1 branch is live\n",
       "[1] R_Register: plain 3-argument Cvar_Get(name, default, 0x21). r_mode default \"3\";\n"
       "    r_customwidth default \"1600\". No min/max anywhere.",
       pe.dis(0x10090b86, 0x10090c3a),
       "\n[2] R_GetModeInfo: mode == -1 -> r_customwidth ([0x10799810]), r_customheight ([0x1079980c]),\n"
       "    r_customaspect ([0x10799808]).",
       pe.dis(0x1008fc40, 0x1008fcb3),
       "\n[3] mode-set failure path: Cvar_SetValue(\"r_mode\", 3.0), r_fullscreen 1, r_colorbits 16, retry.",
       pe.dis(0x100b8810, 0x100b88b8)]
write("sof2sp-r_mode.disasm.txt", "\n".join(txt))

# ---------------------------------------------------------------- RTCW
txt = ["Return to Castle Wolfenstein (GOG 2.0.0.2 tree) - both engines honour r_mode -1;\n"
       "the first launch of a fresh tree is forced to r_mode 3 by Com_SetRecommended.\n"]
pe = PE(os.path.join(LIB, "ReturnToCastleWolfenstein", "WolfMP.exe"))
txt += ["== WolfMP.exe (Wolf 1.4-MP, Oct 28 2002)",
        "[1] R_Register: plain 3-arg Cvar_Get - r_mode (\"3\", 0x21) -> [0x1224318], r_customwidth\n"
        "    (\"1600\") -> [0x1224324], r_customheight (\"1024\") -> [0x12242d8], r_customaspect -> [0x1222570].",
        pe.dis(0x45ab07, 0x45ab8f),
        "\n[2] R_GetModeInfo: mode == -1 -> r_customwidth/height/aspect.",
        pe.dis(0x459c10, 0x459c8d),
        "\n[3] Com_SetRecommended: execs ONE of four cfgs - every one of them says `seta r_mode 3`.",
        pe.dis(0x420610, 0x4206ab),
        "\n[4] end of Com_Init: if com_recommendedSet == 0 (no config yet) and not safe mode ->\n"
        "    Com_SetRecommended(); Cbuf_ExecuteText(EXEC_APPEND, \"vid_restart\"); then com_recommendedSet 1.\n"
        "    This runs AFTER default.cfg / wolfconfig_mp.cfg / autoexec.cfg / Com_StartupVariable, so it\n"
        "    beats fleetres.cfg AND the launcher's +set r_mode on the first launch of a fresh tree.",
        pe.dis(0x420aeb, 0x420b2b),
        "\n    config order in Com_Init:",
        pe.dis(0x420740, 0x420786),
        "\n[5] the renderer's AssertCvarRange (fn 0x45b490, \"WARNING: cvar '%s' out of range\") is called\n"
        "    for exactly three cvars - r_picmip [0x122257c] 0..16, r_displayRefresh [0x1224304] 0..200,\n"
        "    r_znear [0x12242b4] 0.001..200. r_mode [0x1224318] is NOT range-checked anywhere.",
        pe.dis(0x45aa35, 0x45aa49), pe.dis(0x45ac1c, 0x45ac30), pe.dis(0x45acd5, 0x45acec)]
pe = PE(os.path.join(LIB, "ReturnToCastleWolfenstein", "WolfSP.exe"))
txt += ["\n== WolfSP.exe (Wolf 1.32, May 1 2002)",
        "[1] R_Register: r_mode -> [0x1184488], r_customwidth -> [0x1184494], r_customheight -> [0x1184448],\n"
        "    r_customaspect -> [0x11826c4]; plain 3-arg Cvar_Get, no range.",
        pe.dis(0x446d4c, 0x446dd9),
        "\n[2] R_GetModeInfo: mode == -1 branch present.",
        pe.dis(0x445e00, 0x445e7d),
        "\n[3] first-run: if com_recommendedSet == 0 -> Com_SetRecommended(1) (same four cfgs) ...",
        pe.dis(0x41d85f, 0x41d885),
        "    ... which ends with Cbuf_AddText(\"vid_restart\\n\") when its argument is true:",
        pe.dis(0x41d3fd, 0x41d412),
        "\n[4] AssertCvarRange (fn 0x447750) callers: r_picmip, r_picmip2, r_displayRefresh, r_znear\n"
        "    (call sites 0x446c79, 0x446c91, 0x446e75, 0x446f4e) - r_mode [0x1184488] is not among them."]
z = zipfile.ZipFile(os.path.join(LIB, "ReturnToCastleWolfenstein", "Main", "pak0.pk3"))
txt.append("\n== the four first-run cfgs (Main/pak0.pk3)")
for n in ("highVidhighCPU.cfg", "highVidlowCPU.cfg", "lowVidhighCPU.cfg", "lowVidlowCPU.cfg"):
    body = z.read(n).decode("latin-1").replace("\r", "")
    txt.append("--- %s\n%s" % (n, body.strip()))
write("rtcw-r_mode-and-first-run.disasm.txt", "\n".join(txt))


# ---------------------------------------------------------------- menus
def menu_items(pk3, pattern):
    z = zipfile.ZipFile(pk3)
    out = []
    for i in z.infolist():
        if not re.search(pattern, i.filename, re.I):
            continue
        body = z.read(i).decode("latin-1").replace("\r", "")
        for m in re.finditer(r'cvar\s+"(ui_r_mode|r_mode)"', body, re.I):
            # the cvarFloatList sits right before or right after the cvar line
            lo = body.rfind("itemDef", 0, m.start())
            hi = body.find("itemDef", m.end())
            item = body[lo:hi if hi > 0 else len(body)]
            fl = re.search(r"cvarFloatList\s*\{[^}]*\}", item, re.S)
            out.append("%s : %s\n    %s" % (os.path.basename(pk3), i.filename,
                                           re.sub(r"\s+", " ", fl.group(0)) if fl else "(no cvarFloatList)"))
    return out


txt = ["Jedi Academy (retail 1.01) - every menu that edits the video mode (ui_r_mode -> r_mode on Apply)"]
for p in ("assets1.pk3", "assets3.pk3"):
    txt += menu_items(os.path.join(LIB, "JediAcademy", "base", p), r"\.menu$")
s = zipfile.ZipFile(os.path.join(LIB, "JediAcademy", "base", "assets0.pk3")).read(
    "strings/English/MENUS.str").decode("latin-1").replace("\r", "")
txt.append("\nlabels (assets0.pk3 strings/English/MENUS.str):")
for m in re.finditer(r"REFERENCE\s+(\d+_X_\d+|CUSTOM)\s*\n\s*LANG_ENGLISH\s+(\"[^\"]*\")", s):
    txt.append("    @MENUS_%s = %s" % (m.group(1), m.group(2)))
write("jka-menu-modelists.txt", "\n".join(txt))

txt = ["Soldier of Fortune II Gold 1.03\n", "== MP (sof2mp.exe) - ui/setup_display.menu; update101.pk3 overrides mp.pk3"]
for p in ("mp.pk3", "update101.pk3"):
    txt += menu_items(os.path.join(LIB, "SoldierOfFortune2", "base", p), r"^ui/.*\.menu$")
z = zipfile.ZipFile(os.path.join(LIB, "SoldierOfFortune2", "base", "therest.pk3"))
rmf = z.read("menus/m_video.rmf").decode("latin-1").replace("\r", "")
m = re.search(r"<list \"&MENU_VIDEO_VIDMODES&\"[^>]*>", rmf, re.S)
txt.append("\n== SP (SoF2.exe) - Raven RMF menu, therest.pk3 menus/m_video.rmf")
txt.append("    " + re.sub(r"\s+", " ", m.group(0)))
for n in ("menus/video_init.cfg", "menus/video_apply.cfg"):
    b = z.read(n).decode("latin-1").replace("\r", "")
    txt.append("    %s: %s" % (n, [l for l in b.splitlines() if "r_mode" in l or "menu_mode" in l]))
st = zipfile.ZipFile(os.path.join(LIB, "SoldierOfFortune2", "base", "update101.pk3")).read(
    "Strip/menu_video.sp").decode("latin-1").replace("\r", "")
m = re.search(r"REFERENCE VIDMODES.*?TEXT_LANGUAGE1 (\"[^\"]*\")", st, re.S)
txt.append("    labels (update101.pk3 Strip/menu_video.sp VIDMODES): %s" % m.group(1))
write("sof2-menu-modelists.txt", "\n".join(txt))

txt = ["Return to Castle Wolfenstein (GOG) - menus that edit r_mode directly"]
for p in ("sp_pak1.pk3", "sp_pak3.pk3", "mp_pak1.pk3"):
    txt += menu_items(os.path.join(LIB, "ReturnToCastleWolfenstein", "Main", p),
                      r"^(ui|ui_mp)/(system|ingame_system)\.menu$")
write("rtcw-menu-modelists.txt", "\n".join(txt))

# ---------------------------------------------------------------- RTCW SP 1.42d (on the share)
zp = os.path.join(UPD, "RTCW", "rtcw-sp-1.42d-win32-bin.zip")
z = zipfile.ZipFile(zp)
txt = ["Knightmare's unofficial RTCW SP 1.42d, already on the share:", "  " + zp,
       "  md5=%s" % md5(zp), ""]
txt += ["  %9d  %s  %s" % (i.file_size, "%04d-%02d-%02d" % i.date_time[:3], i.filename) for i in z.infolist()]
exe = z.read("WolfSP.exe")
pe = pefile.PE(data=exe)
txt.append("\nWolfSP.exe 1.42d: SubsystemVersion %d.%d, linker %d.%d, imports %s" % (
    pe.OPTIONAL_HEADER.MajorSubsystemVersion, pe.OPTIONAL_HEADER.MinorSubsystemVersion,
    pe.OPTIONAL_HEADER.MajorLinkerVersion, pe.OPTIONAL_HEADER.MinorLinkerVersion,
    [e.dll.decode() for e in pe.DIRECTORY_ENTRY_IMPORT]))
modes = sorted(set(m.group(0).decode() for m in re.finditer(rb"Mode +\d+: [0-9x]+[^\0]*", exe)),
               key=lambda s: int(s.split(":")[0].split()[-1]))
txt.append("mode table: " + " | ".join(modes))
inner = zipfile.ZipFile(io.BytesIO(z.read("Main/sp_pak4.pk3")))
for n in ("ui/system.menu", "ui/ingame_system.menu"):
    b = inner.read(n).decode("latin-1").replace("\r", "")
    fl = re.search(r'cvar "r_mode"\s*cvarFloatList\s*\{[^}]*\}', b, re.S)
    txt.append("\n1.42d %s:\n    %s" % (n, re.sub(r"\s+", " ", fl.group(0)) if fl else "(r_mode list not found)"))
    txt.append("    custom fields: %s" % re.findall(r'cvar "(r_custom\w+)"', b))
txt.append("\nNAME COLLISION: the patch ships Main/sp_pak4.pk3 (%d bytes: ui/*.menu + maps/*.ai/script).\n"
           "The staged tree already has a DIFFERENT Main/sp_pak4.pk3:" % z.getinfo("Main/sp_pak4.pk3").file_size)
staged = os.path.join(LIB, "ReturnToCastleWolfenstein", "Main", "sp_pak4.pk3")
sz = zipfile.ZipFile(staged)
txt.append("  %s  %d bytes md5=%s, %d entries, e.g. %s" % (
    staged, os.path.getsize(staged), md5(staged), len(sz.infolist()),
    [i.filename for i in sz.infolist() if i.filename.endswith(".bsp")]))
txt.append("Extracting the patch as its readme says would REPLACE those map updates. Stage it under a\n"
           "name that sorts after sp_pak4.pk3 instead (e.g. sp_pak4b.pk3 / sp_pak5.pk3).")
write("rtcw-sp-1.42d-patch.txt", "\n".join(txt))
print("ok")

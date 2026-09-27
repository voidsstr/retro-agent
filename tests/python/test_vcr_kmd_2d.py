"""vcr-kmd 2D engine (display/vcrdd_2d.c) and the DirectDraw timing fixes.

Found and verified on the 86Box Voodoo3 test bed (tools/86box/, 2026-09-26)
with the self-checking labs: gdilab (GDI), ddlab (DirectDraw). Each invariant
below failed a lab before it was fixed:

- a RECTFILL's colour is the SOURCE operand (colorFore): with PATCOPY (0xF0)
  the engine filled from the uninitialised pattern registers - ddlab blt
  bad_fill 4096 of 4096.
- a source colour key selects the ROP in the rop register's byte 1; unset,
  keyed pixels were copied like any other.
- the sync before a CPU access must wait for the PCI FIFO to DRAIN as well as
  for busy to clear: an operation still in the FIFO has not started, and a
  status that counts only started work reads idle - gdilab engine/CPU
  interleave 170 bad; 0 after.
- status[6] is CLEAR during vertical retrace (Glide grSstVRetraceOn); read the
  other way, WaitForVerticalBlank waited for the END of the blank.
- a flip is latched at the next retrace: Flip/GetFlipStatus must say
  WASSTILLDRAWING until then - ddlab flip ran at 768 flips/s on a 60 Hz mode.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
G2D = (KMD / "display" / "vcrdd_2d.c").read_text()
DD = (KMD / "display" / "vcrdd_ddraw.c").read_text()
PUNT = (KMD / "display" / "vcrdd_punt.c").read_text()
MPDD = (KMD / "miniport" / "vcrmp_dd.c").read_text()
ESC = (KMD / "display" / "vcrdd_escape.c").read_text()


def func(src, sig):
    i = src.index(sig)
    return src[i:src.index("\n}\n", i)]


def test_a_fill_takes_its_colour_as_the_source_operand():
    fill = func(G2D, "BOOL VcrDd2dFill(")
    assert "CMD_RECTFILL | CMD_GO | CMD_ROP(ROP_SRCCOPY)" in fill
    assert "0xf0" not in fill.lower()
    assert "#define ROP_SRCCOPY         0xccu" in G2D


def test_a_source_colour_key_keeps_the_destination():
    assert "#define ROP_KEYED           (ROP_SRCCOPY | (ROP_DSTCOPY << 8) | (ROP_DSTCOPY << 16))" in G2D
    assert "#define ROP_DSTCOPY         0xaau" in G2D
    copy = func(G2D, "BOOL VcrDd2dCopy(")
    assert "wr(pd, G_ROP, ROP_KEYED);" in copy
    assert "G_COMMANDEX, ckey_flags ? CMDEX_SRC_CKEY : 0" in copy


def test_sync_waits_for_the_fifo_to_drain_not_only_for_busy():
    sync = func(G2D, "void VcrDd2dSync(")
    assert "(s & ST_BUSY) || (s & ST_FIFO_FREE) < pd->g2d_fifo_full" in sync
    # idle must be seen repeatedly, and the wait is bounded
    assert "idle < 3" in sync and "SPIN_CAP" in sync
    init = func(G2D, "void VcrDd2dInit(")
    assert "pd->g2d_fifo_full" in init            # learned, not assumed


def test_every_wait_is_bounded_and_failure_turns_acceleration_off():
    assert "#define SPIN_CAP" in G2D
    room = func(G2D, "BOOL VcrDdRoom(")
    assert "i < SPIN_CAP" in room and "give_up(" in room
    assert "pd->g2d_ok = 0;" in func(G2D, "static void give_up(")


def test_the_cpu_never_draws_before_the_engine_is_done():
    bits = func(PUNT, "static SURFOBJ *bits(")
    assert "VcrDd2dSync(pd);" in bits
    lock = func(DD, "static DWORD APIENTRY Dd_Lock(")
    assert "VcrDd2dSync(pd);" in lock
    # Glide takes the chip only after our queue is empty
    assert ESC.count("VcrDd2dSync(pd);") >= 2


def test_software_blits_sync_first():
    blt = func(DD, "static DWORD APIENTRY Dd_Blt(")
    # every software fallback that touches video memory is preceded by a sync
    for m in re.finditer(r"memmove|memset\(dp", blt):
        before = blt[:m.start()]
        assert before.rfind("VcrDd2dSync(pd);") > before.rfind("VcrDd2dCopy(") or \
            before.rfind("VcrDd2dSync(pd);") > before.rfind("VcrDd2dFill(")


def test_vertical_retrace_is_status_bit_6_clear():
    vb = MPDD[MPDD.index("case IOCTL_VCR_VBLANK:"):]
    vb = vb[:vb.index("return NO_ERROR;")]
    assert "(VcrRd(x, 0, VCR_R_STATUS) & VCR_STATUS_VRETRACE) ? 0 : 1" in vb
    assert "& VCR_STATUS_VRETRACE) ? 1 : 0" not in vb


def test_a_flip_is_pending_until_the_chip_latched_it():
    flip = func(DD, "static DWORD APIENTRY Dd_Flip(")
    assert "!flip_done(pd)" in flip and "DDERR_WASSTILLDRAWING" in flip
    # pending unless the app asked not to wait (DDFLIP_NOVSYNC - see
    # test_vcr_kmd_ddraw.py): a vsync'd flip still waits for the latch
    assert "vcr_flip_begin(&pd->flip, novsync," in flip
    assert re.search(r"if \(!novsync && !flip_done\(pd\)\)", flip)
    status = func(DD, "static DWORD APIENTRY Dd_GetFlipStatus(")
    assert "flip_done(pd) ? DD_OK : DDERR_WASSTILLDRAWING" in status
    done = func(DD, "static int flip_done(")
    # The glue that hands the rule its sample, pinned WHOLE: `have &&
    # !v.in_vblank` (retrace sense inverted - a flip begun in active display
    # done at the first poll that sees active display, up to a frame early)
    # and `1, v.in_vblank` (a failed read treated as a sample) both passed
    # every test while only the prefix was pinned (review, 2026-09-27).
    assert "return vcr_flip_poll(&pd->flip, have, have && v.in_vblank, now);" in done
    assert "have = flip_sample(pd, &v, &now);" in done
    # The rule itself moved to include/vcr_flip.h (2026-09-27), where
    # tests/native/test_vcr_kmd_flip.c drives it over a CRT timeline: a
    # retrace that began after the flip, or the deadline - by default the
    # nominal rate's frame + frame / 8, exactly as before the move.
    flip_h = (KMD / "include" / "vcr_flip.h").read_text()
    poll = func(flip_h, "static inline int vcr_flip_poll(")
    assert "s->seen_active" in poll and "s->deadline" in poll
    dl = func(flip_h, "static inline vcr_ticks vcr_flip_deadline(")
    assert "frame + frame / 8" in dl


def test_the_display_driver_gets_the_registers_from_the_standard_ioctl():
    mp = (KMD / "miniport" / "vcrmp.c").read_text()
    q = mp[mp.index("case IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES:"):]
    q = q[:q.index("break;\n    }")]
    # one buffer in and out: the request is copied before the answer is written
    assert q.index("VideoPortMoveMemory(&req, rp->InputBuffer") < q.index("out->VirtualAddress =")
    # system space only, and only the Voodoo has an engine
    assert "x->backend != VCR_HW_VOODOO || req.RequestedVirtualAddress" in q
    assert "case IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES:" in mp


def test_a_clipped_blit_goes_to_the_engine_rect_by_rect():
    """A windowed DirectDraw/Direct3D application presents with a CLIPPED
    blit (back buffer -> primary through the window's clip list). Handed back
    to the HEL, every frame is copied by the CPU out of video memory - the
    slowest path the card has. Verified on the 86Box Voodoo3: d3dprobe
    'present' reads the quad back from the screen through GDI."""
    blt = func(DD, "static DWORD APIENTRY Dd_Blt(")
    assert "if (p->IsClipped)\n        return clipped_blt(pd, p);" in blt
    c = func(DD, "static DWORD clipped_blt(")
    assert "p->prDestRects[i]" in c and "p->rOrigDest" in c and "p->rOrigSrc" in c
    assert "s == d" in c                     # one surface onto itself: the HEL
    assert "VcrDd2dCopy(" in c and "VcrDd2dFill(" in c


def test_the_readme_safety_section_says_what_the_driver_does():
    """The vcr-kmd README's Safety section said "No 2D engine, no command FIFO,
    no interrupts in this version" and "The desktop starts 1 MB into video
    memory" long after the 2D engine, the D3D setup unit and the top-of-memory
    desktop landed (2026-09-26) - a safety claim a reader would lean on. It now
    says what is true, and this pins the parts of it the code decides: every
    engine wait is capped and gives acceleration up, the miniport has no
    interrupt handler, and the desktop is placed at the top of memory."""
    readme = (KMD / "README.md").read_text()
    safety = readme[readme.index("## Safety"):readme.index("## Diag switches")]
    assert "No 2D engine" not in safety and "starts 1 MB into video" not in safety
    for claim in ("SPIN_CAP", "turns acceleration OFF", "no interrupt", "TOP of video memory",
                  "vcr_desktop_offset"):
        assert claim in safety, claim
    two_d = (KMD / "display" / "vcrdd_2d.c").read_text()
    assert "#define SPIN_CAP" in two_d
    give_up = func(two_d, "static void give_up(")
    assert "pd->g2d_ok = 0;" in give_up and "VCR_EV_DD_2D, 9," in give_up
    room = func(two_d, "BOOL VcrDdRoom(")
    assert "for (i = 0; i < SPIN_CAP; i++)" in room and "give_up(pd, 1, s);" in room
    three_d = (KMD / "display" / "vcrdd_3d.c").read_text()
    assert "VcrDdRoom(pd" in three_d
    for f in sorted((KMD / "miniport").glob("*.c")):
        src = f.read_text()
        assert "HwInterrupt" not in src and "VideoPortEnableInterrupt" not in src, f.name
    mp = (KMD / "miniport" / "vcrmp.c").read_text()
    assert "/* desktop at the top of memory: nothing below it belongs to GDI */" in mp

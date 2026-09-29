/* jklogic.h - the decision logic of jkmode.c, with no Win32 in it.
 *
 * Jedi Knight: Dark Forces II (JK.EXE 1.01) and Mysteries of the Sith (JKM.EXE)
 * store their display mode as an INDEX into a mode list they build themselves,
 * so "1920x1080" cannot be written into their registry key - only "entry 31 of
 * the SORTED list this box's driver produced today". jkmode.c rebuilds that list
 * the way the game does and writes the index; this header holds every rule it
 * applies, so tests/python/test_patch_jk-helper.py compiles it natively and
 * checks it without Windows, DirectDraw or the share.
 *
 * Every rule below cites the instruction it was read from. Addresses are JK.EXE
 * (md5 85f8f883bbb7a167429879c899be1803) / JKM.EXE (md5
 * 192d2be2bfbe5a1acc9f562657a7d9ea), the staged originals; apply.py --check
 * asserts the bytes at each of them.
 *
 *   device list      [0] "Window Display Device" (GUID JK_GUID_WINDOW), then one
 *                    entry per DirectDrawEnumerateA callback that survives
 *                    DirectDrawCreate + SetCooperativeLevel(0x11) + GetCaps +
 *                    SetCooperativeLevel(8). At most 16 entries in all, the
 *                    window device included (cmp [count],0x10 - JK 0x425c50,
 *                    MotS 0x428d80). A NULL lpGUID (the primary driver) is
 *                    stored as sixteen zero bytes.
 *   mode list        per device: pass 1 SetCooperativeLevel(JK 0x51 / MotS 0x11)
 *                    + EnumDisplayModes keeping only 320-wide modes (tagged
 *                    [ModeX]); pass 2 SetCooperativeLevel(0x11) + EnumDisplayModes
 *                    keeping every mode in DF2 - but in MotS NOT a mode whose
 *                    width is >= 1400 and whose lPitch equals its width, i.e. a
 *                    high-resolution 8-bpp mode (MotS 0x428f9e..0x428fb3: cmp
 *                    eax,0x578 / cmp eax,[lPitch] / ret without storing or
 *                    counting; DF2's callback has no such test, 0x425e6e).
 *                    ONE counter across both passes, hard cap 64 (cmp edi,0x40 -
 *                    JK 0x425e37/0x426036, MotS 0x428f67/0x429166), applied in
 *                    ENUMERATION order.
 *   SORTED           then the device-open routine SORTS the capped list
 *                    (JK 0x422797: qsort(0x866cc0, n, 0x54, 0x4249d0) via the
 *                    MSVC CRT qsort at 0x513060; MotS 0x4258da: 0x56c510 /
 *                    0x427b00, identical code) BEFORE the device record copies
 *                    it (JK 0x4277e6). The comparator puts a ModeX entry before
 *                    any other mode of a different width and after one of the
 *                    same width, and otherwise orders by bpp, then width, then
 *                    height. displayMode is an index into the SORTED list - so
 *                    on .123 DF2's 1920x1080x16 is entry 31, not the 45th mode
 *                    enumerated. jk_sort_modes() is an exact port of that qsort
 *                    (the comparator is not a strict weak order when ModeX and
 *                    ordinary 320-wide modes coexist, as on .240, so only the
 *                    same algorithm gives the same order).
 *   3D device list   per DirectDraw device: QueryInterface(IID_IDirect3D) +
 *                    EnumDevices, at most 4 (cmp ecx,4 - JK 0x42bd56, MotS same
 *                    code at 0x42ef10).
 *   3D device pick   the load routine (JK 0x414a86.., MotS 0x4173ff..) - see
 *                    jk_pick_3d(). Ticking "3D Acceleration" in Setup->Display
 *                    stores exactly that pair (JK 0x414f87, MotS 0x41793a).
 *   displayMode      NOT range-checked by the game: the load routine indexes the
 *                    list with it at once (JK 0x414ba6, MotS 0x41751f). A stale
 *                    index past the end of the list reads beyond the array.
 */
#ifndef JKLOGIC_H
#define JKLOGIC_H

#define JK_MODE_CAP   64    /* modes kept per device */
#define JK_DEV_CAP    16    /* device-list entries, the window device included */
#define JK_D3D_CAP    4     /* 3D devices kept per DirectDraw device */
#define JK_NO_3D      0x45  /* the game's "no 3D device" index */

typedef struct { unsigned char b[16]; } jk_guid;

/* {92A69F00-13FA-11D1-97C0-00A024293005}: list entry 0, the windowed device */
static const jk_guid JK_GUID_WINDOW = {{0x00,0x9f,0xa6,0x92,0xfa,0x13,0xd1,0x11,
                                        0x97,0xc0,0x00,0xa0,0x24,0x29,0x30,0x05}};
/* {87051A80-13FC-11D1-97C0-00A024293005}: stored as 3DDeviceGUID for "no 3D" */
static const jk_guid JK_GUID_NO3D   = {{0x80,0x1a,0x05,0x87,0xfc,0x13,0xd1,0x11,
                                        0x97,0xc0,0x00,0xa0,0x24,0x29,0x30,0x05}};

typedef struct {
    int modex;      /* stored by pass 1 (320-wide only) */
    int w, h;
    int bpp;        /* 8, dwRGBBitCount, or -1 (neither flag: the game keeps a stale value) */
} jk_mode;

typedef struct {
    int hw;         /* rec+0x00: the HW desc's dcmColorModel != 0 (0x42bddc) */
    int persp;      /* rec+0x04: dpcTriCaps.dwTextureCaps & 1 (PERSPECTIVE) */
    int zbuf;       /* rec+0x08: dwDeviceZBufferBitDepth != 0 */
    int alpha;      /* rec+0x10: dpcTriCaps.dwTextureCaps >> 2 & 1 (ALPHA) */
    int r14;        /* rec+0x14: shade caps: !(0x1000) && (0x2000) */
    int r18;        /* rec+0x18: (texture blend 8 && shade 0x4000) || r14 */
    int color;      /* rec+0x24: 2 if dcmColorModel has RGB, |1 if MONO */
    int depths;     /* rec+0x28: dwDeviceRenderBitDepth remapped; 0x10 = 16 bpp */
    jk_guid guid;   /* rec+0x21c: the lpGuid EnumDevices handed over */
    char name[64];
} jk_d3d;

typedef struct {
    int primary;    /* +0x10c: lpGUID was NULL */
    int has_guid;   /* +0x104: lpGUID was not NULL */
    int is3d;       /* +0x108: DDCAPS.dwCaps & DDCAPS_3D (driver caps) */
    jk_guid guid;   /* +0x288 */
    char desc[64], name[64];
    int nmodes;
    jk_mode modes[JK_MODE_CAP];
    int seen;            /* entries both passes offered to the list, UNCAPPED (diagnostic) */
    int target_seen_at;  /* uncapped position of the first target mode, -1 = never offered */
    int n3d;
    jk_d3d d3d[JK_D3D_CAP];
} jk_dev;

/* -------------------------------------------------------------------------- */

static int jk_guid_eq(const jk_guid *a, const jk_guid *b)
{
    int i;
    for (i = 0; i < 16; i++) if (a->b[i] != b->b[i]) return 0;
    return 1;
}

static unsigned jk_u32(const unsigned char *p, unsigned off)
{
    return (unsigned)p[off] | ((unsigned)p[off + 1] << 8) |
           ((unsigned)p[off + 2] << 16) | ((unsigned)p[off + 3] << 24);
}

/* The bits-per-pixel the mode callbacks store at mode+0x20 (JK 0x425e91..0x425ec7):
 * DDPF_PALETTEINDEXED8 (0x20) -> 8, else DDPF_RGB (0x40) -> dwRGBBitCount, else
 * nothing is written and the slot keeps a stale value - reported here as -1,
 * which never matches the 16 this tool looks for, and which makes the entry's
 * SORT position unknowable (jk_modes_unknown_bpp: jkmode.c then refuses). */
static int jk_mode_bpp(unsigned pf_flags, unsigned rgb_bits)
{
    if (pf_flags & 0x20) return 8;
    if (pf_flags & 0x40) return (int)rgb_bits;
    return -1;
}

/* One EnumDisplayModes callback, as the game stores it. pass 1 keeps only
 * 320-wide modes; pass 2 keeps everything, except that MotS (hires8_filter)
 * drops a mode with width >= 1400 whose lPitch equals its width - before it is
 * counted, so it takes no slot and moves every later index. `pitch` is the
 * lPitch DirectDraw handed the callback (DDSURFACEDESC +0x10); the test is on
 * the reported value, not on the bpp, so it must be the real one. The game
 * CANCELS the enumeration at the cap; this keeps counting past it without
 * storing, so a mode that exists but was cut (.240: 1920x1080x16 at position
 * 76) can be reported as such. The stored list is identical either way.
 * Returns 1 if the mode was stored. The list is in ENUMERATION order until
 * jk_sort_modes() runs. */
static int jk_mode_offer(jk_dev *d, int pass, int w, int h, int pitch,
                         unsigned pf_flags, unsigned rgb_bits, int tw, int th,
                         int hires8_filter)
{
    jk_mode m;
    int stored = 0;
    if (pass == 1 && w != 320) return 0;           /* 0x42605e: cmp dwWidth,0x140 */
    if (pass == 2 && hires8_filter && w >= 1400 && pitch == w)
        return 0;                                  /* MotS 0x428fa1: cmp eax,0x578 / cmp eax,ebx */
    m.modex = (pass == 1);
    m.w = w; m.h = h;
    m.bpp = jk_mode_bpp(pf_flags, rgb_bits);
    if (!m.modex && m.w == tw && m.h == th && m.bpp == 16 && d->target_seen_at < 0)
        d->target_seen_at = d->seen;
    if (d->seen < JK_MODE_CAP) {
        d->modes[d->nmodes++] = m;
        stored = 1;
    }
    d->seen++;
    return stored;
}

/* The 3D device record (JK 0x42bd50, MotS 0x42ef10, identical code). The HW
 * description is used when its dcmColorModel is non-zero, the HEL one otherwise.
 * Offsets are into D3DDEVICEDESC: +0x08 dcmColorModel, +0x80 dpcTriCaps.dwShadeCaps,
 * +0x84 dpcTriCaps.dwTextureCaps, +0x8c dpcTriCaps.dwTextureBlendCaps,
 * +0x9c dwDeviceRenderBitDepth, +0xa0 dwDeviceZBufferBitDepth. */
static void jk_d3d_from_desc(jk_d3d *r, const unsigned char *hw, const unsigned char *hel)
{
    const unsigned char *d;
    unsigned cm, tex, shade, blend, rbd;
    r->hw = jk_u32(hw, 0x08) != 0;
    d = r->hw ? hw : hel;
    cm = d[0x08];                                  /* mov cl, byte [ebp+0x158] */
    tex = jk_u32(d, 0x84);
    shade = jk_u32(d, 0x80);
    blend = jk_u32(d, 0x8c);
    rbd = jk_u32(d, 0x9c);
    r->color = 0;
    if (cm & 2) r->color = 2;
    if (cm & 1) r->color |= 1;
    r->persp = (int)(tex & 1);
    r->zbuf = jk_u32(d, 0xa0) != 0;
    r->alpha = (int)((tex >> 2) & 1);
    r->r14 = (!(shade & 0x1000) && (shade & 0x2000)) ? 1 : 0;
    r->r18 = (((blend & 8) && (shade & 0x4000)) || r->r14) ? 1 : 0;
    r->depths = 0;
    if (rbd & 0x4000) r->depths |= 0x01;           /* DDBD_1  */
    if (rbd & 0x2000) r->depths |= 0x02;           /* DDBD_2  */
    if (rbd & 0x1000) r->depths |= 0x04;           /* DDBD_4  */
    if (rbd & 0x0800) r->depths |= 0x08;           /* DDBD_8  */
    if (rbd & 0x0400) r->depths |= 0x10;           /* DDBD_16 */
    if (rbd & 0x0200) r->depths |= 0x20;           /* DDBD_24 */
    if (rbd & 0x0100) r->depths |= 0x40;           /* DDBD_32 */
}

/* One EnumDevices callback: the next record, or NULL once four are held - the
 * game then answers D3DENUMRET_CANCEL (JK 0x42bd56 cmp ecx,4 / jae). */
static jk_d3d *jk_d3d_add(jk_dev *d)
{
    if (d->n3d >= JK_D3D_CAP) return 0;
    return &d->d3d[d->n3d++];
}

/* The test both selection loops apply (JK 0x414abd..0x414ad5). */
static int jk_d3d_usable(const jk_d3d *r)
{
    return r->hw && r->persp && r->zbuf && (r->color & 2) && (r->depths & 0x10);
}

/* The first loop's extra test (JK 0x414ad7..0x414ae4). */
static int jk_d3d_usable_addon(const jk_d3d *r)
{
    return jk_d3d_usable(r) && (r->alpha || r->r14 || r->r18);
}

/* The device the game recommends for 3D, i.e. what ticking "3D Acceleration"
 * stores. Loop 1 prefers an ADD-ON 3D card (a secondary DirectDraw device with
 * DDCAPS_3D and a GUID - a Voodoo 2 behind a 2D card) whose 3D device also
 * passes jk_d3d_usable_addon(); loop 2 takes the first usable 3D device on any
 * device. `devs` holds the DirectDraw devices only, in the game's order (game
 * list index = array index + 1). Returns 1 and the pair, or 0: the game then
 * forces b3DAccel to 0 (JK 0x414b87). */
static int jk_pick_3d(const jk_dev *devs, int n, int *dev_out, int *d3d_out)
{
    int i, j;
    for (i = 0; i < n; i++) {
        const jk_dev *d = &devs[i];
        if (!d->is3d || d->primary || !d->has_guid) continue;
        for (j = 0; j < d->n3d; j++)
            if (jk_d3d_usable_addon(&d->d3d[j])) { *dev_out = i; *d3d_out = j; return 1; }
    }
    for (i = 0; i < n; i++)
        for (j = 0; j < devs[i].n3d; j++)
            if (jk_d3d_usable(&devs[i].d3d[j])) { *dev_out = i; *d3d_out = j; return 1; }
    return 0;
}

/* The game's mode comparator (JK 0x4249d0, MotS 0x427b00 - identical code).
 * A ModeX entry against a non-ModeX one looks ONLY at the widths: different
 * widths put the ModeX entry first, equal widths put it last. Otherwise bpp
 * (+0x20), then width (+0x08), then height (+0x0c), ascending, by subtraction. */
static int jk_mode_cmp(const jk_mode *a, const jk_mode *b)
{
    if (a->modex) {
        if (!b->modex) return a->w != b->w ? -1 : 1;     /* 0x4249e5..0x4249f5 */
    } else if (b->modex) {
        return b->w != a->w ? 1 : -1;                    /* 0x4249fe..0x424a0f */
    }
    if (a->bpp != b->bpp) return a->bpp - b->bpp;        /* 0x424a10 */
    if (a->w != b->w) return a->w - b->w;                /* 0x424a1e */
    return a->h - b->h;                                  /* 0x424a2c */
}

static void jk_mode_swap(jk_mode *a, jk_mode *b)         /* 0x513270: no-op when a == b */
{
    jk_mode t;
    if (a == b) return;
    t = *a; *a = *b; *b = t;
}

/* An exact port of the qsort both games link (JK 0x513060, MotS 0x56c510:
 * the MSVC 4-6 CRT qsort.c) with the game's comparator. Exact matters: with
 * ModeX and ordinary 320-wide modes in one list the comparator is not a strict
 * weak order (N320x200x32 < X320x200x8 < N640x480x8 < N320x200x32), and the
 * result is then whatever THIS algorithm produces on THIS input order.
 * tests/python/test_patch_jk-helper.py holds it to the game's own code run
 * under emulation. */
static void jk_shortsort(jk_mode *lo, jk_mode *hi)       /* 0x513210 */
{
    jk_mode *p, *max;
    while (hi > lo) {
        max = lo;
        for (p = lo + 1; p <= hi; p++)
            if (jk_mode_cmp(p, max) > 0) max = p;
        jk_mode_swap(max, hi);
        hi--;
    }
}

static void jk_sort_modes(jk_dev *d)
{
    jk_mode *lo, *hi, *mid, *loguy, *higuy;
    jk_mode *lostk[30], *histk[30];
    int stkptr = 0, size;
    if (d->nmodes < 2) return;                           /* 0x51306a: cmp num,2 / jb */
    lo = d->modes;
    hi = d->modes + (d->nmodes - 1);
recurse:
    size = (int)(hi - lo) + 1;
    if (size <= 8) {                                     /* 0x5130bd: cmp eax,8 (CUTOFF) */
        jk_shortsort(lo, hi);
    } else {
        mid = lo + size / 2;                             /* 0x513118: middle element as pivot */
        jk_mode_swap(mid, lo);
        loguy = lo;
        higuy = hi + 1;
        for (;;) {
            do { loguy++; } while (loguy <= hi && jk_mode_cmp(loguy, lo) <= 0);
            do { higuy--; } while (higuy > lo && jk_mode_cmp(higuy, lo) >= 0);
            if (higuy < loguy) break;
            jk_mode_swap(loguy, higuy);
        }
        jk_mode_swap(lo, higuy);
        if (higuy - 1 - lo >= hi - loguy) {              /* 0x513185: smaller half first */
            if (lo + 1 < higuy) { lostk[stkptr] = lo; histk[stkptr] = higuy - 1; ++stkptr; }
            if (loguy < hi) { lo = loguy; goto recurse; }
        } else {
            if (loguy < hi) { lostk[stkptr] = loguy; histk[stkptr] = hi; ++stkptr; }
            if (lo + 1 < higuy) { hi = higuy - 1; goto recurse; }
        }
    }
    --stkptr;
    if (stkptr >= 0) { lo = lostk[stkptr]; hi = histk[stkptr]; goto recurse; }
}

/* Stored entries whose bpp the game reads from a stale slot (neither
 * DDPF_PALETTEINDEXED8 nor DDPF_RGB). Their sort position - and so the index
 * of every mode after them - cannot be known from here: refuse, never guess. */
static int jk_modes_unknown_bpp(const jk_dev *d)
{
    int i, n = 0;
    for (i = 0; i < d->nmodes; i++) if (d->modes[i].bpp < 0) n++;
    return n;
}

/* The stored index of the requested mode at 16 bpp, or -1 - in the SORTED
 * list, so call it after jk_sort_modes(). ModeX entries are skipped - they
 * only ever exist at 320 wide. The first match wins. */
static int jk_find_mode(const jk_dev *d, int w, int h)
{
    int i;
    for (i = 0; i < d->nmodes; i++) {
        const jk_mode *m = &d->modes[i];
        if (!m->modex && m->w == w && m->h == h && m->bpp == 16) return i;
    }
    return -1;
}

/* The default device (JK 0x4245a0, MotS 0x4276d0): the first entry with the
 * longest run of matches against {+0x110 == 1, type == 1, primary, 3D,
 * has_guid}. Returns a GAME list index (0 = the window device, which scores 0). */
static int jk_default_device(const jk_dev *devs, int n)
{
    int best = 0, best_score = 0, i;
    for (i = 0; i < n; i++) {
        const jk_dev *d = &devs[i];
        int s = 2;                                   /* +0x110 and type are 1 for every DD device */
        if (d->primary) { s = 3; if (d->is3d) { s = 4; if (d->has_guid) return i + 1; } }
        if (s > best_score) { best_score = s; best = i + 1; }
    }
    return best;
}

/* Which game-list entry the load routine will index displayMode into, given
 * what the registry holds now (JK 0x414801..0x414855): the entry whose GUID
 * matches displayDeviceGUID when BOTH GUID values read, else the default. */
static int jk_resolve_device(const jk_dev *devs, int n, int guids_read, const jk_guid *dev_guid)
{
    int i;
    if (guids_read) {
        if (jk_guid_eq(dev_guid, &JK_GUID_WINDOW)) return 0;
        for (i = 0; i < n; i++) if (jk_guid_eq(dev_guid, &devs[i].guid)) return i + 1;
    }
    return jk_default_device(devs, n);
}

/* 1 when a stored displayMode would index past the end of the list the game
 * will use - the one case where deleting it is not a guess: the game does not
 * range-check it. 0 when it is in range, or when the answer is unknown (the
 * window device's list is a fixed table this tool does not model). */
static int jk_stored_mode_is_stale(const jk_dev *devs, int n, int guids_read,
                                   const jk_guid *dev_guid, unsigned stored_mode)
{
    int g = jk_resolve_device(devs, n, guids_read, dev_guid);
    if (g == 0) return 0;
    return stored_mode >= (unsigned)devs[g - 1].nmodes;
}

#endif

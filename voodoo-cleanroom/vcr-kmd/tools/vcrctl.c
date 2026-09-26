/*
 * vcrctl.c - the vcr-kmd harness on the box. One command per run, one JSON
 * object on stdout, run through the agent (`EXEC vcrctl ...`). Reusable tests
 * of specific things, and the golden-capture tool.
 *
 * Works on OUR driver (vcr escapes) and on ANY 3dfx driver that speaks the
 * HWCEXT protocol Glide uses - the vendor's included - because the register
 * commands reach the chip through the same GETLINEARADDR mapping Glide gets.
 * That is how golden captures are taken from the known-good driver:
 *
 *   vcrctl golden 1024 768 16 85     set the mode through GDI, then dump every
 *                                    IO register and the CRTC (via the MMIO
 *                                    VGA alias) - compare with our mode math
 *
 * Commands
 *   info                   our driver: what the miniport found (vcr_info)
 *   log [after]            our driver: flight-recorder entries (TSV lines)
 *   mark <text>            our driver: write a mark into the recorder
 *   bootok                 our driver: clear the boot-attempt counter
 *   reset-engine           our driver: reset a hung 3D engine / command stream
 *   snapshot               our driver: every register of every chip
 *   reg <hexoff>           our driver: read one memBase0 register
 *   crtc <hexidx>          our driver: read one CRTC register
 *   pci <target> <hexoff>  our driver: read PCI config (target 0-3, 16 = bridge)
 *   modes                  any driver: EnumDisplaySettings
 *   setmode W H BPP [HZ]   any driver: ChangeDisplaySettings, with the result
 *                          (paced; the mode is held until the floor has passed)
 *   hwc                    any driver: the Glide HWCEXT handshake + mapping
 *                          validation exactly as our Glide checks it
 *   hwcregs                any driver: IO registers + CRTC through the mapping
 *   golden W H BPP HZ      any driver: setmode, then hwcregs (paced, held)
 *   gdi                    any driver: GDI draw + read-back pattern test
 *   modetest W H BPP HZ    switch + GDI test + registers in ONE process (a
 *                          CDS_FULLSCREEN mode reverts when its process exits);
 *                          paced, held, and the restore is paced
 *   ddraw W H BPP [HZ]     any driver: Glide's route to fullscreen (DirectDraw
 *                          exclusive + SetDisplayMode + RestoreDisplayMode);
 *                          the mode is held for the floor, not flashed
 *   modeseq PACE_MS W H BPP HZ [W H BPP HZ ...]
 *                          modetest over a LIST in ONE process: no bounce back
 *                          to the desktop between modes (every switch re-syncs
 *                          the monitor, and a CRT clicks its relays at each
 *                          band change), at least max(PACE_MS,
 *                          MODESEQ_MIN_PACE_MS) between switches - the first
 *                          one included - whatever the caller asks, at most
 *                          MODESEQ_MAX_MODES modes, one restore at the end.
 *                          PACE_MS is decimal 0..30000 (VCR_PACE_MAX_MS);
 *                          anything else is refused before anything switches.
 *                          One at a time: a named mutex, and a second run
 *                          answers "busy":true without switching anything.
 *                          To stop it from the host, create C:\vcr\modeseq.stop:
 *                          it is seen before the next switch and deleted, and
 *                          the run restores once (paced) and reports
 *                          "stopped":true
 *   restore                ChangeDisplaySettings(NULL) - back to the registry
 *                          mode (paced)
 *   pace-mark              stamp the box's last-switch time NOW (vcr_pace.h)
 *                          and switch nothing. For the host, right after it
 *                          KILLS a tool that may have held a temporary mode
 *                          (PROCKILL, EXECW's tree-kill on timeout, taskkill
 *                          /f): XP reverted that mode as the process died, a
 *                          kill runs no exit hold, and without the stamp the
 *                          next switch is measured from the killed tool's last
 *                          recorded one - possibly straight after the revert
 *   pace-kill PID          the kill made AS a switch, instead of a kill and a
 *                          pace-mark: under the pace lock, a floor after the
 *                          latest stamp, TerminateProcess, up to 10 s for it
 *                          to be gone, the revert stamped 5 s ahead. Only a
 *                          vcrctl / ddlab / d3dprobe / glidelab process - never
 *                          pid 0 or 4, never itself, never the agent. Answers
 *                          {"cmd":"pace-kill","ok":..,"pid":N,"exited":..};
 *                          "ok":false with "pace lock busy" when the victim
 *                          (or another tool) is stuck holding the lock - then
 *                          nothing was killed, and the host kills it by no
 *                          other route (a plain kill reverts its mode
 *                          unpaced): the survivor is reported for a person
 *   dump [path]            the flight recorder + bugcheck out of a crash dump,
 *                          scanned on the box (dumps exceed the agent's frames)
 *   probe-vga [HEXBASE]    vcrprobe.sys: the whole VGA register file (legacy
 *                          ports, or the alias in a 3dfx I/O BAR at HEXBASE)
 *   probe-pci BUS DEV FN [raw]  vcrprobe.sys: 256 bytes of PCI config space
 *                          (raw = 0xCF8 cycles: the V5's slave functions)
 *   probe-mem HEXPHYS LEN  vcrprobe.sys: physical memory, read only (<= 4 KB)
 *   (hwcregs/golden add the VGA file when vcrprobe.sys is loaded)
 *
 * Every command that changes the display mode (setmode, golden, modetest,
 * ddraw, modeseq, restore) goes through vcr_pace.h: at least 3 s since the
 * last switch by ANY tool on the box, and a temporary mode held 3 s before it
 * is given back - by us, or by XP when this process exits or crashes. Each
 * switch is a monitor re-sync; on 2026-09-26 a sweep on .124 did ~250 of them
 * at two a second to a 1998 Sony CRT, and the user heard every relay click.
 * A switch the gate cannot pace (its lock held by a stuck tool, a stamp that
 * never stops moving) is NOT made: the command answers "ok":false with
 * "error":"pace lock busy" (or "pace floor never passed") and switches
 * nothing more - a mode it already holds is left for the exit hold, which
 * paces XP's revert. The one exit no process can pace is being killed: a
 * host kills a vcrctl (or a lab) with `vcrctl pace-kill`, or runs
 * `vcrctl pace-mark` after any other kill.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcr_types.h"
#include "vcr_ioctl.h"
#include "vcr_hwcext.h"
#include "vcr_regs.h"
#include "vcr_probe.h"
#include <tlhelp32.h>
/* vcr_pace.h's paced kill (pace-kill), which only this tool compiles */
#define VCR_PACE_WANT_KILL
#include "vcr_pace.h"

static HDC g_dc;

static int esc(ULONG code, void *in, int cin, void *out, int cout)
{
    return ExtEscape(g_dc, (int)code, cin, (LPCSTR)in, cout, (LPSTR)out);
}

static int fail(const char *cmd, const char *why)
{
    printf("{\"cmd\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n", cmd, why);
    return 1;
}

/* ---- our driver ------------------------------------------------------------- */

static int cmd_info(void)
{
    vcr_info v;
    int n, i;
    memset(&v, 0, sizeof v);
    n = esc(VCR_ESC_INFO, NULL, 0, &v, sizeof v);
    if (n <= 0)
        return fail("info", "VCR_ESC_INFO refused - not the vcr-kmd driver?");
    printf("{\"cmd\":\"info\",\"ok\":true,\"version\":\"%u.%u\",\"build\":%u,"
           "\"backend\":%u,\"vendor\":\"%04x\",\"device\":\"%04x\",\"subsys\":\"%08x\","
           "\"rev\":%u,\"bus\":%u,\"slot\":\"%x\",\"nchips\":%u,\"chip_slots\":[",
           v.version >> 16, v.version & 0xffff, v.build, v.backend, v.vendor, v.device,
           v.subsys, v.revision, v.bus, v.slot, v.nchips);
    for (i = 0; i < (int)v.nchips && i < VCR_MAX_CHIPS; i++)
        printf("%s\"%x\"", i ? "," : "", v.chip_slot[i]);
    printf("],\"mmio_phys\":[");
    for (i = 0; i < (int)v.nchips && i < VCR_MAX_CHIPS; i++)
        printf("%s\"%08x\"", i ? "," : "", v.mmio_phys[i]);
    printf("],\"lfb_phys\":\"%08x\",\"lfb_len\":%u,\"mmio_len\":%u,\"io_base\":\"%x\","
           "\"fb_per_chip\":%u,\"desktop_offset\":%u,\"cur_mode\":%d,\"cur\":\"%ux%ux%u@%u\","
           "\"stride\":%u,\"nmodes\":%u,\"boot_attempts\":%u,\"boot_good\":%u,"
           "\"sli_active\":%u,\"log_next_seq\":%u,\"flags\":%u,\"glide_chips\":%u,"
           "\"sli_chips\":%u,\"sli_result\":%d,\"clock_6k_hz\":%u,\"chip_bar0\":[",
           v.lfb_phys, v.lfb_len, v.mmio_len, v.io_base, v.fb_per_chip, v.desktop_offset,
           (int)v.cur_mode, v.cur_w, v.cur_h, v.cur_bpp, v.cur_hz, v.cur_stride, v.nmodes,
           v.boot_attempts, v.boot_good, v.sli_active, v.log_next_seq, v.flags,
           v.glide_chips, v.sli_chips, (int)v.sli_result, v.clock_6k_hz);
    for (i = 0; i < (int)v.nchips && i < VCR_MAX_CHIPS; i++)
        printf("%s\"%08x\"", i ? "," : "", v.slave_bar0[i]);
    v.mon_pnp[3] = 0;
    v.mon_name[15] = 0;
    printf("],\"edid_ok\":%u,\"mon_filter\":%u,\"monitor\":\"%s%04x %s\","
           "\"mon_h_khz\":[%u,%u],\"mon_v_hz\":[%u,%u],\"mon_max_pixclk_khz\":%u,\"edid\":\"",
           v.edid_ok, v.mon_filter, v.mon_pnp, v.mon_product, v.mon_name, v.mon_hmin_khz,
           v.mon_hmax_khz, v.mon_vmin_hz, v.mon_vmax_hz, v.mon_max_pixclk_khz);
    for (i = 0; i < (int)sizeof v.edid; i++)
        printf("%02x", v.edid[i]);
    /* whose limits mon_* are (VCR_MON_SRC_*: 1 EDID, 2 SAME, 3 ENVELOPE,
     * 4 DEFAULT, 0 none). A driver built before the field was appended fills
     * a shorter struct and says so in size: null then, not a 0 that would
     * read as "filter off" */
    if (v.size >= FIELD_OFFSET(vcr_info, mon_src) + sizeof v.mon_src)
        printf("\",\"mon_src\":%u}\n", v.mon_src);
    else
        printf("\",\"mon_src\":null}\n");
    return 0;
}

static void tsv_msg(const char *m)
{
    for (; *m; m++)
        putchar(*m == '\t' || *m == '\n' || *m == '\r' ? ' ' : *m);
}

static int cmd_log(ULONG after)
{
    static unsigned char buf[VCR_LOG_READ_RES_BYTES(256)];
    vcr_log_read_res *r = (vcr_log_read_res *)buf;
    vcr_log_read_req q;
    ULONG total = 0, i;
    for (;;) {
        q.after_seq = after;
        q.max = 256;
        if (esc(VCR_ESC_LOG_READ, &q, sizeof q, buf, sizeof buf) <= 0)
            return total ? 0 : fail("log", "VCR_ESC_LOG_READ refused");
        if (total == 0)
            printf("#vcrlog boot_count=%u version=%x entries=%u next_seq=%u created=%08x%08x\n",
                   r->hdr.boot_count, r->hdr.drv_version, r->hdr.nentries,
                   r->hdr.next_seq, r->hdr.created_hi, r->hdr.created_lo);
        for (i = 0; i < r->count; i++) {
            const vcr_log_entry *e = &r->e[i];
            printf("%u\t%u\t%u\t%u\t%u\t%u\t%08x\t%08x\t%08x\t%08x\t", e->seq, e->ms,
                   e->src, e->level, e->code, e->pid, e->a, e->b, e->c, e->d);
            tsv_msg(e->msg);
            putchar('\n');
        }
        total += r->count;
        if (r->count == 0 || r->last_seq == after)
            break;
        after = r->last_seq;
    }
    return 0;
}

static int cmd_mark(const char *text)
{
    vcr_log_write_req r;
    memset(&r, 0, sizeof r);
    r.code = 900;
    r.level = 2;
    r.src = VCR_SRC_TOOL;
    r.pid = GetCurrentProcessId();
    strncpy(r.msg, text, sizeof r.msg - 1);
    if (esc(VCR_ESC_LOG_MARK, &r, sizeof r, NULL, 0) <= 0)
        return fail("mark", "refused");
    printf("{\"cmd\":\"mark\",\"ok\":true}\n");
    return 0;
}

static int cmd_simple(const char *name, ULONG code)
{
    int n = esc(code, NULL, 0, NULL, 0);
    printf("{\"cmd\":\"%s\",\"ok\":%s}\n", name, n > 0 ? "true" : "false");
    return n > 0 ? 0 : 1;
}

static void print_bytes(const char *name, const vcr_u8 *b, int n)
{
    int i;
    printf("\"%s\":\"", name);
    for (i = 0; i < n; i++)
        printf("%02x", b[i]);
    printf("\"");
}

static int cmd_snapshot(void)
{
    static vcr_snapshot s;
    int c, i;
    if (esc(VCR_ESC_SNAPSHOT, NULL, 0, &s, sizeof s) <= 0)
        return fail("snapshot", "refused");
    printf("{\"cmd\":\"snapshot\",\"ok\":true,\"chips\":[");
    for (c = 0; c < (int)s.nchips && c < VCR_MAX_CHIPS; c++) {
        const vcr_chip_snapshot *k = &s.chip[c];
        printf("%s{\"io\":[", c ? "," : "");
        for (i = 0; i < 64; i++)
            printf("%s\"%08x\"", i ? "," : "", k->ioregs[i]);
        printf("],\"cfg\":[");
        for (i = 0; i < 64; i++)
            printf("%s\"%08x\"", i ? "," : "", k->cfg[i]);
        printf("],\"misc\":\"%02x\",", k->misc);
        print_bytes("crtc", k->crtc, sizeof k->crtc);
        printf(",");
        print_bytes("seq", k->seq, sizeof k->seq);
        printf(",");
        print_bytes("gfx", k->gfx, sizeof k->gfx);
        printf(",");
        print_bytes("attr", k->attr, sizeof k->attr);
        printf("}");
    }
    printf("]}\n");
    return 0;
}

static int cmd_regop(const char *name, ULONG kind, ULONG off, ULONG idx)
{
    vcr_reg_op op;
    memset(&op, 0, sizeof op);
    op.kind = kind;
    op.offset = off;
    op.vga_index = idx;
    if (esc(VCR_ESC_REG, &op, sizeof op, &op, sizeof op) <= 0)
        return fail(name, "refused");
    printf("{\"cmd\":\"%s\",\"ok\":true,\"offset\":\"%lx\",\"index\":\"%lx\",\"value\":\"%08x\"}\n",
           name, off, idx, op.value);
    return 0;
}

static int cmd_pci(ULONG target, ULONG off)
{
    vcr_pci_op op;
    memset(&op, 0, sizeof op);
    op.target = target;
    op.offset = off;
    op.size = 4;
    if (esc(VCR_ESC_PCI, &op, sizeof op, &op, sizeof op) <= 0)
        return fail("pci", "refused");
    printf("{\"cmd\":\"pci\",\"ok\":true,\"target\":%lu,\"offset\":\"%lx\",\"value\":\"%08x\"}\n",
           target, off, op.value);
    return 0;
}


/* the CLUT as the card holds it: dacAddr <- i, read dacData. Bank 0 is what a
 * non-bypassed desktop at 16/32 bpp looks every channel up in, so anything but
 * an identity ramp there is the scan-out's colours, not GDI's. */
static int cmd_clut(ULONG first, ULONG count)
{
    vcr_reg_op op;
    ULONG i, nonid = 0;
    printf("{\"cmd\":\"clut\",\"first\":%lu,\"entries\":[", first);
    for (i = first; i < first + count && i < 512; i++) {
        memset(&op, 0, sizeof op);
        op.kind = VCR_REG_MMIO32;
        op.offset = 0x50;                       /* dacAddr */
        op.value = i;
        op.write = 1;
        if (esc(VCR_ESC_REG, &op, sizeof op, &op, sizeof op) <= 0)
            return fail("clut", "dacAddr write refused");
        memset(&op, 0, sizeof op);
        op.kind = VCR_REG_MMIO32;
        op.offset = 0x54;                       /* dacData */
        if (esc(VCR_ESC_REG, &op, sizeof op, &op, sizeof op) <= 0)
            return fail("clut", "dacData read refused");
        if ((op.value & 0xffffff) != ((i & 0xff) * 0x010101))
            nonid++;
        printf("%s\"%06x\"", i > first ? "," : "", op.value & 0xffffff);
    }
    printf("],\"not_identity\":%lu}\n", nonid);
    return 0;
}

/* ---- vcrprobe.sys (loaded on the fly: sc start vcrprobe) --------------------- */

static HANDLE probe_open(void)
{
    return CreateFileA("\\\\.\\VcrProbe", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
}

static int probe_vga_at(vcr_probe_vga *v, ULONG base)
{
    HANDLE h = probe_open();
    DWORD got = 0;
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    memset(v, 0, sizeof *v);
    *(ULONG *)v = base;             /* in and out share the buffer */
    ok = DeviceIoControl(h, IOCTL_VCRPROBE_VGA, v, sizeof(ULONG), v, sizeof *v, &got, NULL);
    CloseHandle(h);
    return ok && got == sizeof *v;
}

static int probe_vga(vcr_probe_vga *v)
{
    return probe_vga_at(v, 0x300);
}

static void print_vga_json(const vcr_probe_vga *v)
{
    printf("{\"misc\":\"%02x\",", v->misc);
    print_bytes("seq", v->seq, sizeof v->seq);
    printf(",");
    print_bytes("crtc", v->crtc, sizeof v->crtc);
    printf(",");
    print_bytes("gfx", v->gfx, sizeof v->gfx);
    printf(",");
    print_bytes("attr", v->attr, sizeof v->attr);
    printf("}");
}

static int cmd_probe_vga(ULONG base)
{
    vcr_probe_vga v;
    if (!probe_vga_at(&v, base))
        return fail("probe-vga", "\\\\.\\VcrProbe not available (sc start vcrprobe)");
    printf("{\"cmd\":\"probe-vga\",\"ok\":true,\"base\":\"%lx\",\"vga\":", base);
    print_vga_json(&v);
    printf("}\n");
    return 0;
}

static int cmd_probe_pci(ULONG bus, ULONG dev, ULONG fn, ULONG raw)
{
    vcr_probe_pci q;
    HANDLE h = probe_open();
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE)
        return fail("probe-pci", "\\\\.\\VcrProbe not available (sc start vcrprobe)");
    memset(&q, 0, sizeof q);
    q.bus = bus;
    q.dev = dev;
    q.fn = fn;
    q.offset = 0;
    q.len = 256;
    q.raw = raw;
    if (!DeviceIoControl(h, IOCTL_VCRPROBE_PCI, &q, sizeof q, &q, sizeof q, &got, NULL)) {
        CloseHandle(h);
        return fail("probe-pci", "ioctl failed");
    }
    CloseHandle(h);
    printf("{\"cmd\":\"probe-pci\",\"ok\":true,\"bus\":%lu,\"dev\":%lu,\"fn\":%lu,\"raw\":%lu,"
           "\"got\":%u,", bus, dev, fn, raw, q.got);
    print_bytes("cfg", q.data, q.got > 256 ? 256 : (int)q.got);
    printf("}\n");
    return 0;
}

static int cmd_probe_mem(ULONG phys, ULONG len)
{
    static vcr_probe_mem q;
    HANDLE h = probe_open();
    DWORD got = 0;
    ULONG i;
    if (h == INVALID_HANDLE_VALUE)
        return fail("probe-mem", "\\\\.\\VcrProbe not available (sc start vcrprobe)");
    memset(&q, 0, sizeof q);
    q.phys = phys;
    q.len = len;
    if (!DeviceIoControl(h, IOCTL_VCRPROBE_MEM, &q, sizeof q, &q, sizeof q, &got, NULL)) {
        CloseHandle(h);
        return fail("probe-mem", "ioctl refused (alignment, length or address)");
    }
    CloseHandle(h);
    printf("{\"cmd\":\"probe-mem\",\"ok\":true,\"phys\":\"%08lx\",\"dwords\":[", phys);
    for (i = 0; i < len / 4; i++)
        printf("%s\"%08x\"", i ? "," : "", ((vcr_u32 *)q.data)[i]);
    printf("]}\n");
    return 0;
}


/* ---- crash dumps: read the recorder out of MEMORY.DMP ON THE BOX ------------
 * A kernel dump is tens to hundreds of MB and the agent's frames stop at 32 MB,
 * so the scan happens here (the same logic as tools/vcrdump.py): header for the
 * bugcheck, then every copy of the recorder's magic, the newest ring decoded in
 * the `vcrctl log` TSV format. */
static int read_at(HANDLE f, DWORD off, void *buf, DWORD len)
{
    DWORD got = 0;
    if (SetFilePointer(f, (LONG)off, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
        GetLastError() != NO_ERROR)
        return 0;
    return ReadFile(f, buf, len, &got, NULL) && got == len;
}

static int cmd_dump(const char *path)
{
    static unsigned char chunk[(1 << 20) + 16];
    unsigned char hdr[0x1000];
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, off = 0, best_off = 0, best_seq = 0, nrings = 0, i;
    vcr_log_header lh;

    if (f == INVALID_HANDLE_VALUE)
        return fail("dump", "cannot open the dump file");
    size = GetFileSize(f, NULL);
    if (read_at(f, 0, hdr, sizeof hdr) && !memcmp(hdr, "PAGEDUMP", 8))
        printf("#vcrdump file=%s size=%lu bugcheck=%08x p1=%08x p2=%08x p3=%08x p4=%08x "
               "dump_type=%u\n", path, size, *(vcr_u32 *)&hdr[0x28], *(vcr_u32 *)&hdr[0x2c],
               *(vcr_u32 *)&hdr[0x30], *(vcr_u32 *)&hdr[0x34], *(vcr_u32 *)&hdr[0x38],
               *(vcr_u32 *)&hdr[0xf88]);
    else
        printf("#vcrdump file=%s size=%lu no PAGEDUMP header\n", path, size);

    while (off < size) {
        DWORD want = size - off > (1u << 20) + 16 ? (1u << 20) + 16 : size - off, j;
        if (!read_at(f, off, chunk, want))
            break;
        for (j = 0; j + 16 <= want; j++) {
            if (chunk[j] != 'V' || memcmp(chunk + j, VCR_LOG_MAGIC, VCR_LOG_MAGIC_LEN))
                continue;
            if (!read_at(f, off + j, &lh, sizeof lh) || lh.version != VCR_LOG_VERSION ||
                lh.header_size != sizeof lh || lh.entry_size != sizeof(vcr_log_entry) ||
                !lh.nentries || lh.nentries > 65536)
                continue;
            nrings++;
            printf("#ring offset=%lu entries=%u next_seq=%u boot_count=%u\n", off + j,
                   lh.nentries, lh.next_seq, lh.boot_count);
            if (lh.next_seq >= best_seq) {
                best_seq = lh.next_seq;
                best_off = off + j;
            }
        }
        if (want <= 16)
            break;
        off += want - 16;               /* overlap: a magic across the boundary */
    }
    if (!nrings) {
        CloseHandle(f);
        printf("#no vcr-kmd flight recorder in this file%s\n",
               *(vcr_u32 *)&hdr[0xf88] == 4 ? " (a minidump holds no pool)" : "");
        return 1;
    }
    read_at(f, best_off, &lh, sizeof lh);
    printf("#vcrlog boot_count=%u version=%x entries=%u next_seq=%u created=%08x%08x\n",
           lh.boot_count, lh.drv_version, lh.nentries, lh.next_seq, lh.created_hi,
           lh.created_lo);
    {
        /* oldest surviving sequence first */
        vcr_u32 first = lh.next_seq > lh.nentries ? lh.next_seq - lh.nentries + 1 : 1, s;
        for (s = first; s <= lh.next_seq; s++) {
            vcr_log_entry e;
            i = (s - 1) % lh.nentries;
            if (!read_at(f, best_off + sizeof lh + i * sizeof e, &e, sizeof e) || e.seq != s)
                continue;
            e.msg[VCR_LOG_MSG_LEN - 1] = 0;
            printf("%u\t%u\t%u\t%u\t%u\t%u\t%08x\t%08x\t%08x\t%08x\t", e.seq, e.ms, e.src,
                   e.level, e.code, e.pid, e.a, e.b, e.c, e.d);
            tsv_msg(e.msg);
            putchar('\n');
        }
    }
    CloseHandle(f);
    return 0;
}

/* ---- any driver ----------------------------------------------------------------- */

static int cmd_modes(void)
{
    DEVMODEA dm;
    DWORD i;
    int first = 1;
    printf("{\"cmd\":\"modes\",\"ok\":true,\"modes\":[");
    for (i = 0;; i++) {
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsA(NULL, i, &dm))
            break;
        printf("%s\"%lux%lux%lu@%lu\"", first ? "" : ",", dm.dmPelsWidth, dm.dmPelsHeight,
               dm.dmBitsPerPel, dm.dmDisplayFrequency);
        first = 0;
    }
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    printf("],\"current\":\"%lux%lux%lu@%lu\"}\n", dm.dmPelsWidth, dm.dmPelsHeight,
           dm.dmBitsPerPel, dm.dmDisplayFrequency);
    return 0;
}

/* What set_mode answers when vcr_pace.h refused the switch (its lock busy, or
 * the floor never passed - g_vcr_pace_why): not a DISP_CHANGE_* value, and
 * nothing was switched, so there is nothing to give back either. */
#define CDS_NOT_MADE (-1000L)

/* The one place this tool switches INTO a mode, so setmode, golden, modetest
 * and modeseq are all paced by construction. The attempt is recorded even
 * when it FAILS: a driver can program the CRTC and then fail the switch, and
 * the monitor re-syncs just the same - an unrecorded one is how a retry loop
 * turns into the 2026-09-26 burst on .124. */
static LONG set_mode(DWORD w, DWORD h, DWORD bpp, DWORD hz)
{
    DEVMODEA dm;
    LONG r;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmBitsPerPel = bpp;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (hz) {
        dm.dmDisplayFrequency = hz;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    if (!vcr_pace_before_switch())
        return CDS_NOT_MADE;
    r = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
    vcr_pace_after_switch();            /* CDS_FULLSCREEN: XP reverts it at exit */
    return r;
}

/* After a give-back (ChangeDisplaySettings(NULL) or RestoreDisplayMode) that
 * the caller paced going in like any switch. One that FAILED leaves the
 * temporary mode up for XP to revert when this process exits - one more
 * re-sync - so it is recorded as still holding one, and vcr_pace.h's exit
 * hook waits out the floor before that revert. Never retried: a restore
 * failing in a loop is a burst of re-syncs, the very thing the 2026-09-26
 * .124 sweep did to the CRT. */
static void pace_gave_back(int ok)
{
    vcr_pace_after_switch_ex(!ok);
}

/* ,"error":"<why vcr_pace.h refused>" after a switch that was not made */
static void print_not_made(LONG r)
{
    if (r == CDS_NOT_MADE)
        printf(",\"error\":\"%s\"", g_vcr_pace_why);
}

static int cmd_setmode(DWORD w, DWORD h, DWORD bpp, DWORD hz)
{
    LONG r = set_mode(w, h, bpp, hz);
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    printf("{\"cmd\":\"setmode\",\"ok\":%s,\"result\":%ld,\"asked\":\"%lux%lux%lu@%lu\","
           "\"current\":\"%lux%lux%lu@%lu\"", r == DISP_CHANGE_SUCCESSFUL ? "true" : "false",
           r, w, h, bpp, hz, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel,
           dm.dmDisplayFrequency);
    print_not_made(r);
    printf("}\n");
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
}

/* the same check our Glide makes (minihwc.c): its own view, committed, mapped */
static int view_ok(ULONG va, ULONG min_len, char *why, int whylen)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!va) {
        _snprintf(why, whylen, "null");
        return 0;
    }
    if (!VirtualQuery((LPCVOID)(ULONG_PTR)va, &mbi, sizeof mbi)) {
        _snprintf(why, whylen, "VirtualQuery failed");
        return 0;
    }
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_MAPPED ||
        (ULONG)(ULONG_PTR)mbi.AllocationBase != va || mbi.RegionSize < min_len) {
        _snprintf(why, whylen, "state %lx type %lx base %p region %lu", mbi.State, mbi.Type,
                  mbi.AllocationBase, (unsigned long)mbi.RegionSize);
        return 0;
    }
    _snprintf(why, whylen, "ok");
    return 1;
}

static ULONG g_hwc_code;
static int hwc(ULONG which, vcr_hwc_req *rq, vcr_hwc_res *rs)
{
    static const ULONG codes[3] = { VCR_EXT_HWC, VCR_EXT_HWC_OLD, VCR_EXT_HWC_WXP };
    int i, n;
    rq->which = which;
    if (g_hwc_code)
        return esc(g_hwc_code, rq, sizeof *rq, rs, sizeof *rs);
    for (i = 0; i < 3; i++) {             /* Glide's own probe order */
        n = esc(codes[i], rq, sizeof *rq, rs, sizeof *rs);
        if (n > 0) {
            g_hwc_code = codes[i];
            return n;
        }
    }
    return 0;
}

typedef struct {
    ULONG base0, base1, device, fbram, nchips;
} hwc_state;

static int hwc_open(hwc_state *st, int verbose)
{
    vcr_hwc_req rq;
    vcr_hwc_res rs;
    char why0[96], why1[96];
    int n;

    memset(&rq, 0, sizeof rq);
    memset(&rs, 0, sizeof rs);
    rq.opt.deviceConfig.dc = (ULONG)(ULONG_PTR)g_dc;
    rq.opt.deviceConfig.devNo = 0;
    n = hwc(VCR_HWC_GETDEVICECONFIG, &rq, &rs);
    if (n <= 0 || rs.opt.deviceConfig.vendorID != VCR_PCI_VENDOR_3DFX) {
        if (verbose)
            printf("{\"cmd\":\"hwc\",\"ok\":false,\"step\":\"GETDEVICECONFIG\",\"ret\":%d,"
                   "\"vendor\":\"%x\"}\n", n, rs.opt.deviceConfig.vendorID);
        return 0;
    }
    st->device = rs.opt.deviceConfig.deviceID;
    st->fbram = rs.opt.deviceConfig.fbRam;
    st->nchips = rs.opt.deviceConfig.numChips;

    memset(&rq, 0, sizeof rq);
    memset(&rs, 0, sizeof rs);
    rq.opt.linearAddr.devNum = 0;
    rq.opt.linearAddr.pHandle = GetCurrentProcessId();
    n = hwc(VCR_HWC_GETLINEARADDR, &rq, &rs);
    st->base0 = rs.opt.linearAddr.baseAddresses[0];
    st->base1 = rs.opt.linearAddr.baseAddresses[1];
    if (n <= 0 || rs.resStatus != 1 || !st->base0) {
        if (verbose)
            printf("{\"cmd\":\"hwc\",\"ok\":false,\"step\":\"GETLINEARADDR\",\"ret\":%d,"
                   "\"resStatus\":%d}\n", n, rs.resStatus);
        return 0;
    }
    if (verbose) {
        int ok0 = view_ok(st->base0, 8u << 20, why0, sizeof why0);
        int ok1 = view_ok(st->base1, 1u << 20, why1, sizeof why1);
        printf("{\"cmd\":\"hwc\",\"ok\":%s,\"escape\":\"%lx\",\"device\":\"%04lx\","
               "\"fbRam\":%lu,\"numChips\":%lu,\"base0\":\"%08lx\",\"base0_check\":\"%s\","
               "\"base1\":\"%08lx\",\"base1_check\":\"%s\",\"status\":\"%08lx\","
               "\"vidProcCfg\":\"%08lx\"}\n",
               ok0 && ok1 ? "true" : "false", g_hwc_code, st->device, st->fbram, st->nchips,
               st->base0, why0, st->base1, why1,
               *(volatile ULONG *)(ULONG_PTR)(st->base0 + VCR_R_STATUS),
               *(volatile ULONG *)(ULONG_PTR)(st->base0 + VCR_R_VIDPROCCFG));
    }
    return 1;
}

static void hwc_close(void)
{
    vcr_hwc_req rq;
    vcr_hwc_res rs;
    memset(&rq, 0, sizeof rq);
    memset(&rs, 0, sizeof rs);
    rq.opt.unmapMemory.procHandle = GetCurrentProcessId();
    hwc(VCR_HWC_UNMAP_MEMORY, &rq, &rs);
}

static int cmd_hwc(void)
{
    hwc_state st;
    int ok = hwc_open(&st, 1);
    if (ok)
        hwc_close();
    return ok ? 0 : 1;
}

/* The IO registers through the mapping. NOT the VGA registers: measured on
 * the V5 6000 (2026-09-25), the MMIO alias of 0xb0-0xdf does not read back -
 * CRTC reads 0 and 0xcc returns the status byte - so VGA state is only
 * reachable through the I/O BAR, i.e. from the kernel (`vcrctl snapshot` on
 * our driver). */
static int cmd_hwcregs(const char *tag)
{
    hwc_state st;
    volatile UCHAR *b;
    int i;
    DEVMODEA dm;
    if (!hwc_open(&st, 0))
        return fail("hwcregs", "HWCEXT mapping refused");
    b = (volatile UCHAR *)(ULONG_PTR)st.base0;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    printf("{\"cmd\":\"hwcregs\",\"ok\":true,\"tag\":\"%s\",\"mode\":\"%lux%lux%lu@%lu\","
           "\"device\":\"%04lx\",\"io\":[", tag, dm.dmPelsWidth, dm.dmPelsHeight,
           dm.dmBitsPerPel, dm.dmDisplayFrequency, st.device);
    for (i = 0; i < 64; i++)
        printf("%s\"%08lx\"", i ? "," : "", *(volatile ULONG *)(b + i * 4));
    printf("]");
    {
        vcr_probe_vga v;
        if (probe_vga(&v)) {
            printf(",\"vga\":");
            print_vga_json(&v);
        }
    }
    printf("}\n");
    hwc_close();
    return 0;
}

static int cmd_golden(DWORD w, DWORD h, DWORD bpp, DWORD hz)
{
    char tag[64];
    LONG r = set_mode(w, h, bpp, hz);
    if (r == CDS_NOT_MADE) {
        printf("{\"cmd\":\"golden\",\"ok\":false,\"error\":\"%s\","
               "\"asked\":\"%lux%lux%lu@%lu\"}\n", g_vcr_pace_why, w, h, bpp, hz);
        return 1;
    }
    if (r != DISP_CHANGE_SUCCESSFUL) {
        printf("{\"cmd\":\"golden\",\"ok\":false,\"error\":\"ChangeDisplaySettings %ld\","
               "\"asked\":\"%lux%lux%lu@%lu\"}\n", r, w, h, bpp, hz);
        return 1;
    }
    Sleep(1500);
    _snprintf(tag, sizeof tag, "%lux%lux%lu@%lu", w, h, bpp, hz);
    return cmd_hwcregs(tag);
}

/* GDI draws a pattern, reads it back, and counts mismatches per colour band.
 * A mode switch makes the desktop repaint asynchronously, which can land
 * between the draw and the read-back (seen once in 112 modes in the VM), so
 * settle first and retry: a driver fault fails every attempt the same way. */
/* What a colour reads back as at the current depth: 16 bpp keeps 5-6-5 bits
 * and GDI expands them by shifting, so pure red reads 248,0,0 on ANY driver.
 * Tolerate exactly that quantisation - nothing looser. */
static ULONG g_first_bad_got, g_first_bad_want;
static int g_first_bad_x = -1, g_first_bad_y = -1;

static int color_ok(COLORREF got, COLORREF want, int bpp)
{
    int tol = bpp == 16 ? 8 : 0, i;
    for (i = 0; i < 3; i++) {
        int g = (got >> (8 * i)) & 0xff, w = (want >> (8 * i)) & 0xff;
        if (g > w || w - g > tol)
            return 0;
    }
    return 1;
}

/* Draw into OUR OWN topmost popup, never the bare screen DC: after a mode
 * switch Explorer repaints and re-arranges the desktop for seconds, and a
 * pattern FillRect'ed onto GetDC(NULL) was painted over before it could be
 * read back (every sample at (0,0) read the desktop background, on .124). */
static HWND test_window(void)
{
    static HWND w;
    MSG msg;
    if (!w) {
        w = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "STATIC", "",
                            WS_POPUP | WS_VISIBLE, 0, 0, 256, 64, NULL, NULL,
                            GetModuleHandleA(NULL), NULL);
        SetWindowPos(w, HWND_TOPMOST, 0, 0, 256, 64, SWP_SHOWWINDOW);
    }
    UpdateWindow(w);
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return w;
}

static int gdi_once(int band_bad[4])
{
    int bpp = GetDeviceCaps(g_dc, BITSPIXEL);
    HWND wnd = test_window();
    HDC wdc = GetDC(wnd), mem;
    HBITMAP bm;
    int x, y, bad = 0;
    static const COLORREF c[4] = { RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255),
                                   RGB(255, 255, 255) };
    for (y = 0; y < 4; y++) {
        HBRUSH br = CreateSolidBrush(c[y]);
        RECT r;
        r.left = 0;
        r.top = y * 16;
        r.right = 256;
        r.bottom = y * 16 + 16;
        FillRect(wdc, &r, br);
        DeleteObject(br);
        band_bad[y] = 0;
    }
    GdiFlush();
    mem = CreateCompatibleDC(wdc);
    bm = CreateCompatibleBitmap(wdc, 256, 64);
    SelectObject(mem, bm);
    BitBlt(mem, 0, 0, 256, 64, wdc, 0, 0, SRCCOPY);
    for (y = 0; y < 64; y += 3)
        for (x = 0; x < 256; x += 7)
            if (!color_ok(GetPixel(mem, x, y), c[y / 16], bpp)) {
                if (!bad) {
                    g_first_bad_got = GetPixel(mem, x, y);
                    g_first_bad_want = c[y / 16];
                    g_first_bad_x = x;
                    g_first_bad_y = y;
                }
                bad++;
                band_bad[y / 16]++;
            }
    DeleteDC(mem);
    DeleteObject(bm);
    ReleaseDC(wnd, wdc);
    return bad;
}

static int gdi_test(int band[4], int *attempts)
{
    int bad = 0, attempt;
    Sleep(400);
    for (attempt = 1; attempt <= 3; attempt++) {
        bad = gdi_once(band);
        if (!bad)
            break;
        Sleep(500);
    }
    *attempts = attempt > 3 ? 3 : attempt;
    return bad;
}

static int cmd_gdi(void)
{
    int band[4], bad = 0, attempt;
    Sleep(400);
    for (attempt = 1; attempt <= 3; attempt++) {
        bad = gdi_once(band);
        if (!bad)
            break;
        Sleep(500);
    }
    InvalidateRect(NULL, NULL, TRUE);
    printf("{\"cmd\":\"gdi\",\"ok\":%s,\"checked\":814,\"mismatches\":%d,\"attempts\":%d,"
           "\"band_mismatches\":[%d,%d,%d,%d]}\n", bad ? "false" : "true", bad,
           attempt > 3 ? 3 : attempt, band[0], band[1], band[2], band[3]);
    return bad ? 1 : 0;
}


/* The path Glide takes to its fullscreen mode (minihwc/win_mode.c):
 * DirectDrawCreate, EXCLUSIVE|FULLSCREEN, SetDisplayMode(w, h, bpp, refresh),
 * then RestoreDisplayMode. ddraw.dll is loaded at run time.
 * FOCUS: unlike the labs this needs no WM_ACTIVATEAPP handler. DirectDraw gives
 * the desktop back on a deactivation from its hook on the window's messages,
 * and this thread dispatches none between the switch in and the paced
 * restore (the hold is a Sleep inside vcr_pace.h), so a lost foreground cannot
 * make it switch behind the gate's back. Keep it that way: a message pump
 * added here needs the labs' handler too. */
typedef HRESULT (WINAPI *PFN_DDCREATEEX)(GUID *, LPVOID *, REFIID, IUnknown *);

static int cmd_ddraw(DWORD w, DWORD h, DWORD bpp, DWORD hz)
{
    static const GUID iid7 = { 0x15e65ec0, 0x3b9c, 0x11d2,
                               { 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b } };
    HMODULE lib = LoadLibraryA("ddraw.dll");
    PFN_DDCREATEEX create = lib ? (PFN_DDCREATEEX)GetProcAddress(lib, "DirectDrawCreateEx") : NULL;
    LPDIRECTDRAW7 dd = NULL;
    HWND wnd;
    HRESULT hr, hr_coop = 0, hr_mode = 0, hr_restore = 0;
    DEVMODEA during;
    int switched = 0, left = 0;         /* left: the mode is left up for the exit hold */
    const char *why = NULL;             /* why vcr_pace.h refused a switch */

    if (!create)
        return fail("ddraw", "no DirectDrawCreateEx");
    wnd = CreateWindowExA(WS_EX_TOPMOST, "STATIC", "vcrctl ddraw", WS_POPUP | WS_VISIBLE,
                          0, 0, 64, 64, NULL, NULL, GetModuleHandleA(NULL), NULL);
    hr = create(NULL, (LPVOID *)&dd, &iid7, NULL);
    if (FAILED(hr) || !dd) {
        DestroyWindow(wnd);
        printf("{\"cmd\":\"ddraw\",\"ok\":false,\"step\":\"create\",\"hr\":\"%08lx\"}\n", hr);
        return 1;
    }
    hr_coop = IDirectDraw7_SetCooperativeLevel(dd, wnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (SUCCEEDED(hr_coop)) {
        /* refused: no SetDisplayMode, and leaving exclusive mode below
         * changes no mode, since DirectDraw changed none */
        if (!vcr_pace_before_switch()) {
            why = g_vcr_pace_why;
        } else {
            hr_mode = IDirectDraw7_SetDisplayMode(dd, w, h, bpp, hz, 0);
            vcr_pace_after_switch();    /* recorded even if it failed: see set_mode */
            switched = 1;
        }
    }
    memset(&during, 0, sizeof during);
    during.dmSize = sizeof during;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &during);
    if (switched) {
        /* this was a fixed 500 ms: in and out of a mode inside a second, two
         * re-syncs the tube cannot finish. Now the mode is held for the floor.
         * Refused, it is not given back here by ANY route - RestoreDisplayMode,
         * leaving exclusive mode and the Release all would, unpaced - but left
         * for XP to revert at exit, after vcr_pace.h's exit hold */
        if (!vcr_pace_before_switch()) {
            why = g_vcr_pace_why;
            left = 1;
        } else {
            hr_restore = IDirectDraw7_RestoreDisplayMode(dd);
            pace_gave_back(SUCCEEDED(hr_restore));
            /* DirectDraw puts back a mode it changed when exclusive mode ends,
             * so after a FAILED restore, leaving exclusive mode can be a
             * second attempt at once - pace it as the switch it may be */
            if (FAILED(hr_restore) && !vcr_pace_before_switch()) {
                why = g_vcr_pace_why;
                left = 1;
            }
        }
    }
    if (!left) {
        IDirectDraw7_SetCooperativeLevel(dd, wnd, DDSCL_NORMAL);
        IDirectDraw7_Release(dd);
        if (FAILED(hr_restore))
            vcr_pace_after_switch();    /* state unknown: hold again before exit */
        DestroyWindow(wnd);
    }
    printf("{\"cmd\":\"ddraw\",\"ok\":%s,\"asked\":\"%lux%lux%lu@%lu\",\"coop\":\"%08lx\","
           "\"setmode\":\"%08lx\",\"restore\":\"%08lx\",\"during\":\"%lux%lux%lu@%lu\"",
           switched && !why && SUCCEEDED(hr_coop) && SUCCEEDED(hr_mode) && during.dmPelsWidth == w &&
           during.dmBitsPerPel == bpp ? "true" : "false",
           w, h, bpp, hz, hr_coop, hr_mode, hr_restore, during.dmPelsWidth,
           during.dmPelsHeight, during.dmBitsPerPel, during.dmDisplayFrequency);
    if (why)
        printf(",\"error\":\"%s\",\"left_for_exit\":%s", why, left ? "true" : "false");
    printf("}\n");
    return switched && !why && SUCCEEDED(hr_mode) ? 0 : 1;
}


/* One mode, proven inside ONE process: a CDS_FULLSCREEN mode is TEMPORARY and
 * XP restores the registry mode when the process that set it exits - measured
 * on .124: `setmode` then `modes` in a new process showed the old mode again.
 * A sweep that switched in one process and tested in the next tested the
 * desktop mode every time. So: switch, draw + read back, and read the
 * registers (VCR_ESC_SNAPSHOT) before this process ends. */

/* The flight recorder's next sequence number AFTER a mode: the host
 * attributes log entries to modes by these boundaries - and a switch that
 * FAILED is the one whose entries it most needs. */
static void print_log_next_seq(void)
{
    vcr_info v;
    memset(&v, 0, sizeof v);
    if (esc(VCR_ESC_INFO, NULL, 0, &v, sizeof v) > 0)
        printf(",\"log_next_seq\":%u", v.log_next_seq);
}

/* modetest_one's answer when vcr_pace.h refused the switch: nothing switched */
#define MODETEST_NOT_MADE 2

static int modetest_one(DWORD w, DWORD h, DWORD bpp, DWORD hz, int seq_index)
{
    static vcr_snapshot snap;
    int band[4], bad, attempts, i, have_snap;
    LONG r = set_mode(w, h, bpp, hz);
    DEVMODEA cur;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        printf("{\"cmd\":\"modetest\",\"ok\":false,\"index\":%d,\"result\":%ld,"
               "\"asked\":\"%lux%lux%lu@%lu\"", seq_index, r, w, h, bpp, hz);
        print_not_made(r);
        print_log_next_seq();
        printf("}\n");
        /* out NOW: a host timeout tree-kills a modeseq, and a line still in
         * the pipe buffer is a failed mode nobody hears about */
        fflush(stdout);
        return r == CDS_NOT_MADE ? MODETEST_NOT_MADE : 1;
    }
    bad = gdi_test(band, &attempts);
    have_snap = esc(VCR_ESC_SNAPSHOT, NULL, 0, &snap, sizeof snap) > 0;
    printf("{\"cmd\":\"modetest\",\"ok\":%s,\"asked\":\"%lux%lux%lu@%lu\","
           "\"current\":\"%lux%lux%lu@%lu\",\"gdi\":{\"mismatches\":%d,\"attempts\":%d,"
           "\"band_mismatches\":[%d,%d,%d,%d],\"first_bad\":[%d,%d,\"%06lx\",\"%06lx\"]}",
           (!bad && cur.dmPelsWidth == w && cur.dmPelsHeight == h && cur.dmBitsPerPel == bpp)
               ? "true" : "false", w, h, bpp, hz, cur.dmPelsWidth, cur.dmPelsHeight,
           cur.dmBitsPerPel, cur.dmDisplayFrequency, bad, attempts, band[0], band[1],
           band[2], band[3], g_first_bad_x, g_first_bad_y, g_first_bad_got, g_first_bad_want);
    if (have_snap) {
        const vcr_chip_snapshot *k = &snap.chip[0];
        printf(",\"snapshot\":{\"cmd\":\"snapshot\",\"ok\":true,\"chips\":[{\"io\":[");
        for (i = 0; i < 64; i++)
            printf("%s\"%08x\"", i ? "," : "", k->ioregs[i]);
        printf("],\"misc\":\"%02x\",", k->misc);
        print_bytes("crtc", k->crtc, sizeof k->crtc);
        printf("}]}");
    }
    print_log_next_seq();
    printf(",\"index\":%d}\n", seq_index);
    fflush(stdout);
    return bad ? 1 : 0;
}

static int cmd_modetest(DWORD w, DWORD h, DWORD bpp, DWORD hz)
{
    int rc = modetest_one(w, h, bpp, hz, 0);
    LONG r = CDS_NOT_MADE;
    if (rc == MODETEST_NOT_MADE)
        return 1;                       /* nothing switched: nothing to give back */
    /* holds the tested mode for the floor. Refused, the mode stays up for
     * XP to revert at exit, after vcr_pace.h's exit hold - never given back
     * unpaced */
    if (vcr_pace_before_switch()) {
        r = ChangeDisplaySettingsA(NULL, 0);
        pace_gave_back(r == DISP_CHANGE_SUCCESSFUL);
    }
    if (r != DISP_CHANGE_SUCCESSFUL) {
        /* a second line only when it matters: the box may be left in the
         * test mode, and the modetest line alone would not say so */
        printf("{\"cmd\":\"restore\",\"ok\":false,\"result\":%ld,\"after\":\"modetest\"", r);
        print_not_made(r);
        printf("}\n");
        rc = 1;
    }
    return rc;
}

/* Every mode switch makes the monitor lose and re-acquire sync; on a CRT a
 * change of horizontal-frequency band also clicks its mode relays and steps
 * the high voltage. A sweep of 123 modes with a return to the desktop after
 * each one was ~250 re-syncs at two a second (.124, 2026-09-26) - needless
 * wear on a 1998 tube. These limits are enforced HERE, in the tool that
 * switches, and every switch - the first, each mode, the restore - waits on
 * vcr_pace.h's floor, which is measured from the last switch made by ANY
 * process on the box. So no caller can fire switches faster than a person
 * would: not by a short PACE_MS, not by running modeseq back to back, not by
 * running two at once (the mutex below). */
#define MODESEQ_MIN_PACE_MS 3000
#define MODESEQ_MAX_MODES   16
/* the literal above is what the safety test reads; it must not undercut the
 * shared floor, or the usage text would promise a pace the gate overrides */
#if MODESEQ_MIN_PACE_MS < VCR_PACE_MIN_MS
#error "MODESEQ_MIN_PACE_MS is below vcr_pace.h's floor"
#endif

/* Stopping from the host. Killing the process (a PROCKILL, an EXECW timeout)
 * skips the exit hold: XP drops the test mode the instant the process dies,
 * possibly right after a switch - two re-syncs back to back. A file the loop
 * looks for before each switch lets the run end the civil way: finish the
 * mode it is in, hold it, restore once. The file is consumed so the next run
 * is not stopped by it; one left over from a finished run stops the next run
 * before its first switch, which fails safe (nothing switched, it says so). */
#define MODESEQ_STOP_FILE "C:\\vcr\\modeseq.stop"
/* One run at a time: two sweeps share the floor's timestamp file but race
 * between reading it and switching, and a host that times out and re-issues
 * a sweep while the first is still running would double the re-syncs. */
#define MODESEQ_MUTEX     "vcrctl-modeseq"

static int modeseq_stop_asked(void)
{
    if (GetFileAttributesA(MODESEQ_STOP_FILE) == INVALID_FILE_ATTRIBUTES)
        return 0;
    DeleteFileA(MODESEQ_STOP_FILE);
    return 1;
}

static int cmd_modeseq(int argc, char **argv)
{
    DWORD pace = 0, got;
    int n = (argc - 3) / 4, i, bad = 0, stopped = 0;
    LONG r = DISP_CHANGE_SUCCESSFUL;
    const char *refused = NULL;         /* why vcr_pace.h refused a switch */
    HANDLE one;
    if (n < 1 || (argc - 3) % 4)
        return fail("modeseq", "usage: modeseq PACE_MS W H BPP HZ [W H BPP HZ ...]");
    if (n > MODESEQ_MAX_MODES)
        return fail("modeseq", "too many modes for one run (each is a monitor re-sync)");
    /* strtoul took "-1" as 0xFFFFFFFF: a pace nobody meant, on every switch
     * of the run. Refused here, before anything switches */
    if (!vcr_pace_parse_ms(argv[2], &pace))
        return fail("modeseq", "PACE_MS must be decimal milliseconds, 0 to 30000");
    if (pace < MODESEQ_MIN_PACE_MS)
        pace = MODESEQ_MIN_PACE_MS;
    /* WAIT_ABANDONED is ours too: its owner has exited, and a run that
     * crashed is not a run in progress */
    one = CreateMutexA(NULL, FALSE, MODESEQ_MUTEX);
    got = one ? WaitForSingleObject(one, 0) : WAIT_FAILED;
    if (got != WAIT_OBJECT_0 && got != WAIT_ABANDONED) {
        if (one)
            CloseHandle(one);
        printf("{\"cmd\":\"modeseq\",\"ok\":false,\"busy\":true,\"error\":\"another modeseq "
               "is running - one sweep at a time\"}\n");
        return 1;
    }
    /* the caller's pace governs every wait in this process, the first
     * switch's included: the previous tool's last switch may be a moment ago */
    vcr_pace_set_min(pace);
    for (i = 0; i < n; i++) {
        char **m = argv + 3 + 4 * i;
        int go, t;
        /* wait out the floor BEFORE looking for the stop file, so a stop that
         * arrives during the wait is honoured before this switch, not after.
         * set_mode paces the same switch again inside modetest_one: nested in
         * this wait, it does not wait a second floor */
        go = vcr_pace_before_switch();
        if (modeseq_stop_asked()) {
            stopped = 1;
            /* waited for a switch it will not make: give the pace lock back
             * (a no-op when it was refused), or the next tool finds it
             * abandoned and waits a floor for nothing */
            vcr_pace_cancel();
            break;
        }
        if (!go) {
            /* the gate could not pace this switch: none is made, and the run
             * ends here - the mode it is in is given back below if the gate
             * lets it, else held for XP's revert by the exit hold */
            refused = g_vcr_pace_why;
            break;
        }
        t = modetest_one(atoi(m[0]), atoi(m[1]), atoi(m[2]), atoi(m[3]), i);
        if (t == MODETEST_NOT_MADE) {   /* cannot happen nested; said, not assumed */
            refused = g_vcr_pace_why;
            break;
        }
        bad += t;
    }
    /* stopped before the first mode: nothing of ours to give back, and a
     * restore would be one more re-sync for nothing */
    if (i) {
        /* the restore is a switch too: held, paced, and never retried - and
         * when the gate refuses it, not made at all (the exit hold paces the
         * revert XP makes instead) */
        if (vcr_pace_before_switch()) {
            stopped |= modeseq_stop_asked();    /* consume a stop that came in late */
            r = ChangeDisplaySettingsA(NULL, 0);
            pace_gave_back(r == DISP_CHANGE_SUCCESSFUL);
        } else {
            r = CDS_NOT_MADE;
            if (!refused)
                refused = g_vcr_pace_why;
        }
    }
    printf("{\"cmd\":\"modeseq\",\"ok\":%s,\"modes\":%d,\"ran\":%d,\"failed\":%d,"
           "\"stopped\":%s,\"pace_ms\":%lu,\"restore\":%ld",
           !bad && !stopped && !refused && r == DISP_CHANGE_SUCCESSFUL ? "true" : "false", n, i,
           bad, stopped ? "true" : "false", pace, r);
    if (refused)
        printf(",\"error\":\"%s\"", refused);
    printf("}\n");
    /* `one` is deliberately NOT released: after a failed restore the exit
     * hold still has to run, and a queued second run must not switch in the
     * moment XP reverts ours. Process exit releases it. */
    return bad || stopped || refused || r != DISP_CHANGE_SUCCESSFUL;
}

/* The host's half of the exit hold (see the usage above). Deliberately
 * without the pace lock: the stamp must carry the time of the kill's revert,
 * not the moment a lock came free, and a tool waiting under the lock reads
 * the stamp again after every sleep, so it still sees this one. */
static int cmd_pace_mark(void)
{
    if (!vcr_pace_mark())       /* vcr_pace.h has said why on stderr */
        return fail("pace-mark", "cannot write the switch stamp C:\\\\vcr\\\\lastswitch.dat");
    printf("{\"cmd\":\"pace-mark\",\"ok\":true}\n");
    return 0;
}

/* The image name of a live process, from a Toolhelp snapshot (XP has it).
 * 0 = no such process in the list. */
static int image_of(DWORD pid, char *out, int outlen)
{
    PROCESSENTRY32 pe;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    int found = 0;
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    memset(&pe, 0, sizeof pe);
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                _snprintf(out, outlen, "%s", pe.szExeFile);
                out[outlen - 1] = 0;
                found = 1;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static int pace_kill_answer(DWORD pid, int ok, int exited, const char *image, const char *why,
                            DWORD err)
{
    printf("{\"cmd\":\"pace-kill\",\"ok\":%s,\"pid\":%lu,\"exited\":%s",
           ok ? "true" : "false", pid, exited ? "true" : "false");
    if (image)
        printf(",\"image\":\"%s\"", image);
    if (why)
        printf(",\"error\":\"%s\"", why);
    if (err)
        printf(",\"win32_error\":%lu", err);
    printf("}\n");
    return ok ? 0 : 1;
}

/* A host that must stop a tool (its budget ran out, a sweep is being
 * abandoned) kills it THROUGH the gate instead of around it: a PROCKILL or
 * EXECW's tree-kill drops the tool's mode the instant it dies, possibly right
 * after a switch, and records nothing. Only our own mode-switching tools, so
 * this cannot be pointed at the agent or the system. The process is OPENED
 * before its image is checked: the open handle pins the pid, which then
 * cannot be recycled for another process between the check and the kill. */
static int cmd_pace_kill(const char *arg)
{
    DWORD pid = 0, err = 0;
    char image[MAX_PATH];
    const char *why, *p, *img;
    HANDLE proc;
    int r, exited = 0;
    for (p = arg; *p >= '0' && *p <= '9' && pid <= 0x0fffffffu; p++)
        pid = pid * 10 + (DWORD)(*p - '0');
    if (p == arg || *p)
        return pace_kill_answer(pid, 0, 0, NULL, "PID must be a decimal process id", 0);
    why = vcr_pace_kill_pid_refusal(pid, GetCurrentProcessId());
    if (why)                            /* pid 0, 4 or itself: refused before any open */
        return pace_kill_answer(pid, 0, 0, NULL, why, 0);
    proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!proc)
        return pace_kill_answer(pid, 0, 0, NULL, "cannot open the process (gone already?)",
                                GetLastError());
    img = image_of(pid, image, sizeof image) ? image : NULL;
    why = vcr_pace_kill_refusal(pid, GetCurrentProcessId(), img);
    if (why) {
        CloseHandle(proc);
        return pace_kill_answer(pid, 0, 0, img, why, 0);
    }
    r = vcr_pace_kill(proc, &exited, &err);
    CloseHandle(proc);
    if (r == 0)                         /* the gate refused: nothing was killed */
        return pace_kill_answer(pid, 0, 0, image, g_vcr_pace_why, 0);
    if (r < 0)
        return pace_kill_answer(pid, 0, 0, image, "TerminateProcess failed", err);
    /* killed, but not gone in 10 s (a thread stuck in the kernel): its mode
     * goes when it does, later than the stamp says - the host must not
     * switch until it is gone, then pace-mark */
    return pace_kill_answer(pid, exited, exited, image,
                            exited ? NULL : "terminated but not gone after 10 s", 0);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "info";
    int rc;
    /* EXEC runs us hidden: a Watson or critical-error box would sit where
     * nobody can dismiss it, holding a test mode on the monitor and the agent
     * command until its timeout. Fail at once and let the host see it. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    g_dc = GetDC(NULL);
    if (!g_dc)
        return fail(cmd, "GetDC failed");
    if (!strcmp(cmd, "info"))
        rc = cmd_info();
    else if (!strcmp(cmd, "log"))
        rc = cmd_log(argc > 2 ? strtoul(argv[2], NULL, 0) : 0);
    else if (!strcmp(cmd, "mark"))
        rc = cmd_mark(argc > 2 ? argv[2] : "mark");
    else if (!strcmp(cmd, "reset-engine"))
        rc = cmd_simple("reset-engine", VCR_ESC_RESET_ENGINE);
    else if (!strcmp(cmd, "bootok"))
        rc = cmd_simple("bootok", VCR_ESC_BOOT_OK);
    else if (!strcmp(cmd, "snapshot"))
        rc = cmd_snapshot();
    else if (!strcmp(cmd, "reg") && argc > 2)
        rc = cmd_regop("reg", VCR_REG_MMIO32, strtoul(argv[2], NULL, 16), 0);
    else if (!strcmp(cmd, "clut"))
        rc = cmd_clut(argc > 2 ? strtoul(argv[2], NULL, 0) : 0, argc > 3 ? strtoul(argv[3], NULL, 0) : 256);
    else if (!strcmp(cmd, "crtc") && argc > 2)
        rc = cmd_regop("crtc", VCR_REG_VGA_CRTC, 0, strtoul(argv[2], NULL, 16));
    else if (!strcmp(cmd, "pci") && argc > 3)
        rc = cmd_pci(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 16));
    else if (!strcmp(cmd, "modes"))
        rc = cmd_modes();
    else if (!strcmp(cmd, "setmode") && argc > 4)
        rc = cmd_setmode(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                         argc > 5 ? atoi(argv[5]) : 0);
    else if (!strcmp(cmd, "hwc"))
        rc = cmd_hwc();
    else if (!strcmp(cmd, "hwcregs"))
        rc = cmd_hwcregs(argc > 2 ? argv[2] : "current");
    else if (!strcmp(cmd, "golden") && argc > 5)
        rc = cmd_golden(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
    else if (!strcmp(cmd, "ddraw") && argc > 4)
        rc = cmd_ddraw(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                       argc > 5 ? atoi(argv[5]) : 0);
    else if (!strcmp(cmd, "dump"))
        rc = cmd_dump(argc > 2 ? argv[2] : "C:\\WINDOWS\\MEMORY.DMP");
    else if (!strcmp(cmd, "probe-vga"))
        rc = cmd_probe_vga(argc > 2 ? strtoul(argv[2], NULL, 16) : 0x300);
    else if (!strcmp(cmd, "probe-pci") && argc > 4)
        rc = cmd_probe_pci(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0),
                           strtoul(argv[4], NULL, 0), argc > 5 && !strcmp(argv[5], "raw"));
    else if (!strcmp(cmd, "probe-mem") && argc > 3)
        rc = cmd_probe_mem(strtoul(argv[2], NULL, 16), strtoul(argv[3], NULL, 0));
    else if (!strcmp(cmd, "modetest") && argc > 5)
        rc = cmd_modetest(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
    else if (!strcmp(cmd, "modeseq") && argc > 6)
        rc = cmd_modeseq(argc, argv);
    else if (!strcmp(cmd, "gdi"))
        rc = cmd_gdi();
    else if (!strcmp(cmd, "pace-mark"))
        rc = cmd_pace_mark();
    else if (!strcmp(cmd, "pace-kill") && argc > 2)
        rc = cmd_pace_kill(argv[2]);
    else if (!strcmp(cmd, "restore")) {
        LONG r = CDS_NOT_MADE;
        if (vcr_pace_before_switch()) {
            r = ChangeDisplaySettingsA(NULL, 0);
            pace_gave_back(r == DISP_CHANGE_SUCCESSFUL);
        }
        printf("{\"cmd\":\"restore\",\"ok\":%s,\"result\":%ld",
               r == DISP_CHANGE_SUCCESSFUL ? "true" : "false", r);
        print_not_made(r);
        printf("}\n");
        rc = r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
    } else
        rc = fail(cmd, "unknown command or missing arguments");
    ReleaseDC(NULL, g_dc);
    /* vcr_pace.h's exit hold can keep this process alive for the floor after
     * main returns; what it printed must already be out if the host's
     * timeout kills it there */
    fflush(stdout);
    return rc;
}

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
 *   snapshot               our driver: every register of every chip
 *   reg <hexoff>           our driver: read one memBase0 register
 *   crtc <hexidx>          our driver: read one CRTC register
 *   pci <target> <hexoff>  our driver: read PCI config (target 0-3, 16 = bridge)
 *   modes                  any driver: EnumDisplaySettings
 *   setmode W H BPP [HZ]   any driver: ChangeDisplaySettings, with the result
 *   hwc                    any driver: the Glide HWCEXT handshake + mapping
 *                          validation exactly as our Glide checks it
 *   hwcregs                any driver: IO registers + CRTC through the mapping
 *   golden W H BPP HZ      any driver: setmode, then hwcregs
 *   gdi                    any driver: GDI draw + read-back pattern test
 *   modetest W H BPP HZ    switch + GDI test + registers in ONE process (a
 *                          CDS_FULLSCREEN mode reverts when its process exits)
 *   ddraw W H BPP [HZ]     any driver: Glide's route to fullscreen (DirectDraw
 *                          exclusive + SetDisplayMode + RestoreDisplayMode)
 *   restore                ChangeDisplaySettings(NULL) - back to the registry mode
 *   dump [path]            the flight recorder + bugcheck out of a crash dump,
 *                          scanned on the box (dumps exceed the agent's frames)
 *   probe-vga [HEXBASE]    vcrprobe.sys: the whole VGA register file (legacy
 *                          ports, or the alias in a 3dfx I/O BAR at HEXBASE)
 *   probe-pci BUS DEV FN [raw]  vcrprobe.sys: 256 bytes of PCI config space
 *                          (raw = 0xCF8 cycles: the V5's slave functions)
 *   probe-mem HEXPHYS LEN  vcrprobe.sys: physical memory, read only (<= 4 KB)
 *   (hwcregs/golden add the VGA file when vcrprobe.sys is loaded)
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
           "\"sli_active\":%u,\"log_next_seq\":%u,\"flags\":%u}\n",
           v.lfb_phys, v.lfb_len, v.mmio_len, v.io_base, v.fb_per_chip, v.desktop_offset,
           (int)v.cur_mode, v.cur_w, v.cur_h, v.cur_bpp, v.cur_hz, v.cur_stride, v.nmodes,
           v.boot_attempts, v.boot_good, v.sli_active, v.log_next_seq, v.flags);
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

static LONG set_mode(DWORD w, DWORD h, DWORD bpp, DWORD hz)
{
    DEVMODEA dm;
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
    return ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
}

static int cmd_setmode(DWORD w, DWORD h, DWORD bpp, DWORD hz)
{
    LONG r = set_mode(w, h, bpp, hz);
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    printf("{\"cmd\":\"setmode\",\"ok\":%s,\"result\":%ld,\"asked\":\"%lux%lux%lu@%lu\","
           "\"current\":\"%lux%lux%lu@%lu\"}\n", r == DISP_CHANGE_SUCCESSFUL ? "true" : "false",
           r, w, h, bpp, hz, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel,
           dm.dmDisplayFrequency);
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
 * then RestoreDisplayMode. ddraw.dll is loaded at run time. */
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
    if (SUCCEEDED(hr_coop))
        hr_mode = IDirectDraw7_SetDisplayMode(dd, w, h, bpp, hz, 0);
    memset(&during, 0, sizeof during);
    during.dmSize = sizeof during;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &during);
    Sleep(500);
    hr_restore = IDirectDraw7_RestoreDisplayMode(dd);
    IDirectDraw7_SetCooperativeLevel(dd, wnd, DDSCL_NORMAL);
    IDirectDraw7_Release(dd);
    DestroyWindow(wnd);
    printf("{\"cmd\":\"ddraw\",\"ok\":%s,\"asked\":\"%lux%lux%lu@%lu\",\"coop\":\"%08lx\","
           "\"setmode\":\"%08lx\",\"restore\":\"%08lx\",\"during\":\"%lux%lux%lu@%lu\"}\n",
           SUCCEEDED(hr_coop) && SUCCEEDED(hr_mode) && during.dmPelsWidth == w &&
           during.dmBitsPerPel == bpp ? "true" : "false",
           w, h, bpp, hz, hr_coop, hr_mode, hr_restore, during.dmPelsWidth,
           during.dmPelsHeight, during.dmBitsPerPel, during.dmDisplayFrequency);
    return SUCCEEDED(hr_mode) ? 0 : 1;
}


/* One mode, proven inside ONE process: a CDS_FULLSCREEN mode is TEMPORARY and
 * XP restores the registry mode when the process that set it exits - measured
 * on .124: `setmode` then `modes` in a new process showed the old mode again.
 * A sweep that switched in one process and tested in the next tested the
 * desktop mode every time. So: switch, draw + read back, and read the
 * registers (VCR_ESC_SNAPSHOT) before this process ends. */
static int cmd_modetest(DWORD w, DWORD h, DWORD bpp, DWORD hz)
{
    static vcr_snapshot snap;
    int band[4], bad, attempts, i, have_snap;
    LONG r = set_mode(w, h, bpp, hz);
    DEVMODEA cur;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        printf("{\"cmd\":\"modetest\",\"ok\":false,\"result\":%ld,\"asked\":\"%lux%lux%lu@%lu\"}\n",
               r, w, h, bpp, hz);
        return 1;
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
    printf("}\n");
    ChangeDisplaySettingsA(NULL, 0);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "info";
    int rc;
    g_dc = GetDC(NULL);
    if (!g_dc)
        return fail(cmd, "GetDC failed");
    if (!strcmp(cmd, "info"))
        rc = cmd_info();
    else if (!strcmp(cmd, "log"))
        rc = cmd_log(argc > 2 ? strtoul(argv[2], NULL, 0) : 0);
    else if (!strcmp(cmd, "mark"))
        rc = cmd_mark(argc > 2 ? argv[2] : "mark");
    else if (!strcmp(cmd, "bootok"))
        rc = cmd_simple("bootok", VCR_ESC_BOOT_OK);
    else if (!strcmp(cmd, "snapshot"))
        rc = cmd_snapshot();
    else if (!strcmp(cmd, "reg") && argc > 2)
        rc = cmd_regop("reg", VCR_REG_MMIO32, strtoul(argv[2], NULL, 16), 0);
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
    else if (!strcmp(cmd, "gdi"))
        rc = cmd_gdi();
    else if (!strcmp(cmd, "restore")) {
        LONG r = ChangeDisplaySettingsA(NULL, 0);
        printf("{\"cmd\":\"restore\",\"ok\":%s,\"result\":%ld}\n",
               r == DISP_CHANGE_SUCCESSFUL ? "true" : "false", r);
        rc = r == DISP_CHANGE_SUCCESSFUL ? 0 : 1;
    } else
        rc = fail(cmd, "unknown command or missing arguments");
    ReleaseDC(NULL, g_dc);
    return rc;
}

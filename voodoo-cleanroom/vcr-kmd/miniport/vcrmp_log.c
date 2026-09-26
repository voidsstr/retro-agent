/*
 * vcrmp_log.c - the flight recorder in the kernel, and the registry
 * diagnostics that survive what the recorder cannot.
 *
 * Three layers, each for a different way to die:
 *
 *   1. The RING (non-paged pool, common/vcr_log.c): everything. Read live
 *      through the display driver's VCR_ESC_LOG_READ, or after a bugcheck out
 *      of the kernel memory dump (tools/vcrdump.py scans for the magic).
 *
 *   2. PHASES (registry `<service>\Diag`, flushed): the boot/mode-set
 *      milestones only. A hard hang loses the ring with the power cycle, but
 *      the last phase reached - and the phase history of that boot - are on
 *      disk. Read them back with the agent's REGREAD after the box returns.
 *
 *   3. The BOOT COUNTER (same key): incremented and flushed before the
 *      miniport touches the hardware, cleared by the stable-boot timer once
 *      the driver has run for a minute. When it reaches MaxBootAttempts the
 *      miniport declines the adapter, XP falls back to the VGA driver, and
 *      the box comes back reachable instead of boot-looping.
 *
 * Optional mirrors, off by default: DbgPrint (a kernel debugger), and a raw
 * byte port - 0xE9 is QEMU's debugcon, 0x3F8 a polled COM1.
 */
#include "vcrmp.h"

#define VCR_TAG     0x524b4356      /* 'VCKR' */

static vcr_log_ring *g_ring;
static UNICODE_STRING g_diag_path;
static WCHAR    g_diag_buf[256];
static ULONG    g_log_level = VCR_LV_DEBUG;
static ULONG    g_debug_port;
static ULONG    g_dbgprint;
static ULONGLONG g_t0;
ULONG VcrBootAttempts;

#define PHASE_SLOTS 64
static ULONG    g_phase_hist[PHASE_SLOTS * 4];  /* {ms, code, a, b} */
static ULONG    g_phase_n;

ULONG VcrMs(void)
{
    ULONGLONG t = KeQueryInterruptTime() - g_t0;   /* 100 ns units */
    return (ULONG)(t / 10000u);
}

/* ---- registry: <service>\Diag ------------------------------------------------ */

static HANDLE diag_open(void)
{
    OBJECT_ATTRIBUTES oa;
    HANDLE h = NULL;
    ULONG disp;
    if (!g_diag_path.Buffer)
        return NULL;
    InitializeObjectAttributes(&oa, &g_diag_path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    if (ZwCreateKey(&h, VCR_KEY_ALL_ACCESS, &oa, 0, NULL, VCR_REG_OPTION_NON_VOLATILE,
                    &disp) < 0)
        return NULL;
    return h;
}

ULONG VcrDiagGet(PCWSTR name, ULONG dflt)
{
    UCHAR buf[sizeof(VCR_KEY_VALUE_PARTIAL) + 8];
    VCR_KEY_VALUE_PARTIAL *kv = (VCR_KEY_VALUE_PARTIAL *)buf;
    UNICODE_STRING n;
    ULONG got = 0, v = dflt;
    HANDLE h;
    if (KeGetCurrentIrql() != VCR_PASSIVE_LEVEL || !(h = diag_open()))
        return dflt;
    RtlInitUnicodeString(&n, name);
    if (ZwQueryValueKey(h, &n, VCR_KeyValuePartialInformation, kv, sizeof buf, &got) >= 0 &&
        kv->DataLength >= 4)
        v = *(ULONG *)kv->Data;
    ZwClose(h);
    return v;
}

ULONG VcrDiagGetString(PCWSTR name, USHORT *out, ULONG maxchars)
{
    UCHAR buf[sizeof(VCR_KEY_VALUE_PARTIAL) + 128];
    VCR_KEY_VALUE_PARTIAL *kv = (VCR_KEY_VALUE_PARTIAL *)buf;
    UNICODE_STRING n;
    ULONG got = 0, i, nch = 0;
    HANDLE h;
    if (KeGetCurrentIrql() != VCR_PASSIVE_LEVEL || !(h = diag_open()))
        return 0;
    RtlInitUnicodeString(&n, name);
    if (ZwQueryValueKey(h, &n, VCR_KeyValuePartialInformation, kv, sizeof buf, &got) >= 0 &&
        kv->Type == 1 /* REG_SZ */) {
        const USHORT *s = (const USHORT *)kv->Data;
        for (i = 0; i < kv->DataLength / 2 && i + 1 < maxchars && s[i]; i++)
            out[nch++] = s[i];
    }
    if (maxchars)
        out[nch] = 0;
    ZwClose(h);
    return nch;
}

static void diag_write(HANDLE h, PCWSTR name, ULONG type, PVOID data, ULONG len)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    ZwSetValueKey(h, &n, 0, type, data, len);
}

void VcrDiagSet(PCWSTR name, ULONG value, BOOLEAN flush)
{
    HANDLE h;
    if (KeGetCurrentIrql() != VCR_PASSIVE_LEVEL || !(h = diag_open()))
        return;
    diag_write(h, name, 4 /* REG_DWORD */, &value, sizeof value);
    if (flush) {
        NTSTATUS st = ZwFlushKey(h);
        if (st < 0)
            VLOG(VCR_LV_WARN, VCR_EV_REG_FLUSH, st, 0, 0, 0, "ZwFlushKey failed");
    }
    ZwClose(h);
}

void VcrPhase(ULONG code, ULONG a, ULONG b, const char *what)
{
    HANDLE h;
    ULONG slot, ms = VcrMs();
    VLOG(VCR_LV_INFO, code, a, b, 0, 0, "PHASE %s", what);
    slot = (g_phase_n++ % PHASE_SLOTS) * 4;
    g_phase_hist[slot + 0] = ms;
    g_phase_hist[slot + 1] = code;
    g_phase_hist[slot + 2] = a;
    g_phase_hist[slot + 3] = b;
    if (KeGetCurrentIrql() != VCR_PASSIVE_LEVEL || !(h = diag_open()))
        return;
    diag_write(h, L"LastPhase", 4, &code, 4);
    diag_write(h, L"LastPhaseA", 4, &a, 4);
    diag_write(h, L"LastPhaseMs", 4, &ms, 4);
    diag_write(h, L"PhaseCount", 4, &g_phase_n, 4);
    diag_write(h, L"PhaseLog", 3 /* REG_BINARY */, g_phase_hist, sizeof g_phase_hist);
    ZwFlushKey(h);
    ZwClose(h);
}

/* ---- the ring ------------------------------------------------------------------ */

static void debug_port_out(const char *s)
{
    if (g_debug_port == 0xe9) {
        while (*s)
            VideoPortWritePortUchar((PUCHAR)0xe9, (UCHAR)*s++);
        VideoPortWritePortUchar((PUCHAR)0xe9, '\n');
    } else if (g_debug_port == 0x3f8) {
        const char *p = s;
        for (;;) {
            char ch = *p ? *p++ : '\n';
            ULONG spin = 100000;
            while (!(VideoPortReadPortUchar((PUCHAR)0x3fd) & 0x20) && --spin)
                ;
            VideoPortWritePortUchar((PUCHAR)0x3f8, (UCHAR)ch);
            if (ch == '\n')
                break;
        }
    }
}

/* The phase history of the PREVIOUS boot, kept before this boot's first phase
 * overwrites it: a box that hung (at boot, or mid-game in an SLI/AA bring-up)
 * comes back from its power cycle with Prev* saying where it stopped. */
static void copy_value(HANDLE h, PCWSTR from, PCWSTR to)
{
    UCHAR buf[sizeof(VCR_KEY_VALUE_PARTIAL) + PHASE_SLOTS * 16];
    VCR_KEY_VALUE_PARTIAL *kv = (VCR_KEY_VALUE_PARTIAL *)buf;
    UNICODE_STRING n;
    ULONG got = 0;
    RtlInitUnicodeString(&n, from);
    if (ZwQueryValueKey(h, &n, VCR_KeyValuePartialInformation, kv, sizeof buf, &got) >= 0 &&
        kv->DataLength <= PHASE_SLOTS * 16)
        diag_write(h, to, kv->Type, kv->Data, kv->DataLength);
}

static void keep_previous_boot(void)
{
    HANDLE h;
    if (KeGetCurrentIrql() != VCR_PASSIVE_LEVEL || !(h = diag_open()))
        return;
    copy_value(h, L"PhaseLog", L"PrevPhaseLog");
    copy_value(h, L"PhaseCount", L"PrevPhaseCount");
    copy_value(h, L"LastPhase", L"PrevLastPhase");
    copy_value(h, L"LastPhaseA", L"PrevLastPhaseA");
    copy_value(h, L"LastPhaseMs", L"PrevLastPhaseMs");
    copy_value(h, L"BootCount", L"PrevBootCount");
    ZwFlushKey(h);
    ZwClose(h);
}

void VcrLogCreate(PUNICODE_STRING RegistryPath)
{
    LARGE_INTEGER now;
    ULONG n, i, len;
    ULONG entries = VCR_LOG_ENTRIES_DEFAULT;

    g_t0 = KeQueryInterruptTime();

    /* <service key>\Diag, built from the path handed to DriverEntry (which
     * does not outlive DriverEntry, so copy it) */
    len = RegistryPath ? RegistryPath->Length / 2 : 0;
    if (len + 6 < sizeof g_diag_buf / 2) {
        for (i = 0; i < len; i++)
            g_diag_buf[i] = RegistryPath->Buffer[i];
        g_diag_buf[len++] = L'\\';
        g_diag_buf[len++] = L'D';
        g_diag_buf[len++] = L'i';
        g_diag_buf[len++] = L'a';
        g_diag_buf[len++] = L'g';
        g_diag_buf[len] = 0;
        g_diag_path.Buffer = g_diag_buf;
        g_diag_path.Length = (USHORT)(len * 2);
        g_diag_path.MaximumLength = (USHORT)sizeof g_diag_buf;
    }

    keep_previous_boot();
    g_log_level = VcrDiagGet(L"LogLevel", VCR_LV_DEBUG);
    g_debug_port = VcrDiagGet(L"DebugPort", 0);
    g_dbgprint = VcrDiagGet(L"DbgPrint", 0);
    n = VcrDiagGet(L"LogEntries", entries);
    if (n >= 64 && n <= 16384)
        entries = n;

    g_ring = (vcr_log_ring *)ExAllocatePoolWithTag(VCR_NonPagedPool,
                                                   VCR_LOG_RING_BYTES(entries), VCR_TAG);
    if (!g_ring)
        return;
    KeQuerySystemTime(&now);
    vcr_log_init(g_ring, entries, VcrDiagGet(L"BootCount", 0),
                 VCR_KMD_VERSION_NUM, now.LowPart, (ULONG)now.HighPart);
}

ULONG VcrLog(ULONG level, ULONG code, ULONG a, ULONG b, ULONG c, ULONG d,
             const char *fmt, ...)
{
    char msg[VCR_LOG_MSG_LEN];
    va_list ap;
    ULONG seq;

    if (level > g_log_level)
        return 0;
    va_start(ap, fmt);
    vcr_vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!g_ring)
        return 0;
    seq = vcr_log_put(g_ring, VcrMs(), (vcr_u16)code, VCR_SRC_MINIPORT, (vcr_u8)level,
                      (ULONG)(ULONG_PTR)PsGetCurrentProcessId(), a, b, c, d, msg);
    if (g_dbgprint)
        DbgPrint("vcrmp: [%u] %u %x %x %x %x %s\n", seq, code, a, b, c, d, msg);
    if (g_debug_port) {
        char line[160];
        vcr_snprintf(line, sizeof line, "vcrmp %u c%u %x %x %x %x %s", seq, code,
                     a, b, c, d, msg);
        debug_port_out(line);
    }
    return seq;
}

void VcrLogFromUser(const vcr_log_write_req *r)
{
    char msg[VCR_LOG_MSG_LEN];
    ULONG i;
    if (!g_ring || r->level > g_log_level)
        return;
    for (i = 0; i < VCR_LOG_MSG_LEN - 1 && r->msg[i]; i++)
        msg[i] = r->msg[i];
    msg[i] = 0;
    vcr_log_put(g_ring, VcrMs(), (vcr_u16)r->code,
                (vcr_u8)(r->src ? r->src : VCR_SRC_DISPLAY), (vcr_u8)r->level,
                r->pid ? r->pid : (ULONG)(ULONG_PTR)PsGetCurrentProcessId(),
                r->a, r->b, r->c, r->d, msg);
    if (g_debug_port) {
        char line[160];
        vcr_snprintf(line, sizeof line, "vcrdd c%u %x %x %x %x %s", r->code, r->a, r->b,
                     r->c, r->d, msg);
        debug_port_out(line);
    }
}

ULONG VcrLogRead(ULONG after_seq, vcr_log_read_res *out, ULONG max)
{
    if (!g_ring)
        return 0;
    out->hdr = g_ring->hdr;
    out->last_seq = after_seq;
    out->count = vcr_log_read(g_ring, after_seq, out->e, max, &out->last_seq);
    return out->count;
}

ULONG VcrLogNextSeq(void)
{
    return g_ring ? g_ring->hdr.next_seq : 0;
}

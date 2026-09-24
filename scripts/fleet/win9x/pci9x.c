/* pci9x - READ-ONLY PCI bus-0 configuration-space probe for Windows 9x.
 *
 *   pci9x [outfile] [noio] [force]
 *
 *   outfile  default C:\RETRO_AGENT\PCI9X.TXT (falls back to C:\PCI9X.TXT)
 *   noio     write the header only and exit 0 - proves the EXE loads on the
 *            box without touching any hardware (run this FIRST)
 *   force    allow running on the NT family (it will fault there; the fault
 *            is caught and reported, nothing else happens)
 *
 * Exit code: 10 = a 3dfx (VEN_121A) function answered on bus 0
 *             0 = scan completed, no 3dfx function answered
 *             3 = config mechanism #1 not detected (nothing scanned)
 *             4 = NT family and no 'force'
 *             5 = an exception was caught (logged)
 *             2 = could not create the output file
 *
 * WHAT IT DOES - and it is deliberately the same mechanism Glide 2.x itself
 * uses on Win9x (swlibs/newpci/pcilib/fxvxd.c pciPortInLong9x/OutLong9x;
 * retail glide2x.dll 2.56.00.0459 at 0x1001ffa0..0x1002001a): ring-3
 * configuration mechanism #1, i.e. OUT dword to 0xCF8, IN dword from 0xCFC.
 * fxmemmap.vxd has NO config-space ioctl (FXMEMMAP.ASM servtbl: close,
 * getversion, getappversion, getlinearaddr x2, LDT selectors, MSR get/set,
 * decrementmutex, setpassthroughbase, setaddrperm), so there is no "safer"
 * VxD path to use, and loading that VxD is itself a risk this probe avoids.
 *
 * WHAT IT NEVER DOES
 *   - never writes port 0xCFC (config DATA) -> no config register is written
 *   - never writes a BAR, never sizes a BAR (Glide's fxremap does; we don't)
 *   - never maps or touches device memory, never loads a VxD, never calls
 *     DeviceIoControl, never uses config mechanism #2
 *   - the only port it writes is 0xCF8 (config ADDRESS), and every write of
 *     it is followed by restoring the value 0xCF8 held before, so a ring-0
 *     user of the register that was pre-empted mid-sequence gets it back.
 *
 * Every output line is written through and flushed (FILE_FLAG_WRITE_THROUGH
 * + FlushFileBuffers) so that if the machine dies mid-scan the file on disk
 * shows exactly how far it got - VCACHE lazy-write cannot eat the evidence.
 *
 * No C runtime: Win98's MSVCRT.DLL is not trusted (see fleet9x.c). Imports
 * are KERNEL32 + USER32 (wsprintfA) only. Large buffers are static so the
 * compiler never emits a __chkstk_ms stack probe.
 *
 * Build:
 *   i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -e _start@0 \
 *       -o pci9x.exe pci9x.c -lkernel32 -luser32 -ladvapi32 -s
 */
#include <windows.h>

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

static HANDLE g_out = INVALID_HANDLE_VALUE;
static char   g_line[1024];
static char   g_path[MAX_PATH];
static const char *g_stage = "init";
static unsigned long g_races;        /* reads where 0xCF8 changed under us */
static unsigned long g_ramtop;       /* bytes, rounded up to 1 MB          */

/* one present function, for the overlap report */
static struct { unsigned dev, fn; unsigned long ven_dev, bar[6], rom; } g_fn[64];
static int g_nfn;

static void emit(const char *s)
{
    DWORD n;
    if (g_out == INVALID_HANDLE_VALUE) return;
    WriteFile(g_out, s, lstrlenA(s), &n, NULL);
    WriteFile(g_out, "\r\n", 2, &n, NULL);
    FlushFileBuffers(g_out);
}

static void stage(const char *s)
{
    g_stage = s;
    wsprintfA(g_line, "stage: %s", s);
    emit(g_line);
}

static __inline__ unsigned long io_inl(unsigned short port)
{
    unsigned long v;
    __asm__ __volatile__("inl %w1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* The ONLY port this program ever writes is CFG_ADDR. */
static __inline__ void io_out_cfgaddr(unsigned long v)
{
    __asm__ __volatile__("outl %0, %w1" : : "a"(v), "Nd"((unsigned short)CFG_ADDR));
}

/* Read one config dword on bus 0.  Save 0xCF8, point it at our register,
 * read 0xCFC, confirm 0xCF8 still holds our address (a ring-0 user may have
 * run between our OUT and IN), then put 0xCF8 back the way we found it. */
static unsigned long cfg_rd(unsigned dev, unsigned fn, unsigned reg)
{
    unsigned long addr = 0x80000000UL | ((unsigned long)(dev & 31) << 11) |
                         ((unsigned long)(fn & 7) << 8) | (reg & 0xFC);
    unsigned long save, v = 0xFFFFFFFFUL, chk;
    int tries;

    for (tries = 0; tries < 4; tries++) {
        save = io_inl(CFG_ADDR);
        io_out_cfgaddr(addr);
        v   = io_inl(CFG_DATA);
        chk = io_inl(CFG_ADDR);
        io_out_cfgaddr(save);
        if (chk == addr) return v;
        g_races++;
    }
    return v;
}

static LONG WINAPI on_fault(EXCEPTION_POINTERS *ep)
{
    wsprintfA(g_line,
              "FAULT: exception %08lX at %08lX during stage '%s' - no config "
              "register was written (the probe never writes 0xCFC)",
              (unsigned long)ep->ExceptionRecord->ExceptionCode,
              (unsigned long)ep->ExceptionRecord->ExceptionAddress, g_stage);
    emit(g_line);
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    ExitProcess(5);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int open_out(const char *path)
{
    g_out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    return g_out != INVALID_HANDLE_VALUE;
}

static const char *next_tok(const char *p, char *dst, int max)
{
    int i = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') { p++; while (*p && *p != '"' && i < max - 1) dst[i++] = *p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t' && i < max - 1) dst[i++] = *p++;
    dst[i] = 0;
    return p;
}

/* Is 'a' inside another present function's memory BAR's 16 MB window, or is
 * another memory BAR inside [base, base+16MB)?  Sizes are NOT probed (that
 * needs a BAR write), so this is a proximity report, not a proof. */
static void report_neighbours(unsigned long base, int self)
{
    int i, j, hits = 0;
    for (i = 0; i < g_nfn; i++) {
        if (i == self) continue;
        for (j = 0; j < 6; j++) {
            unsigned long b = g_fn[i].bar[j];
            if (!b || (b & 1)) continue;                 /* unused or I/O BAR */
            b &= ~0xFUL;
            if ((b >= base && b - base < 0x1000000UL) ||
                (base >= b && base - b < 0x1000000UL)) {
                wsprintfA(g_line, "    near/overlap: 00:%02X.%u bar%d=%08lX is within 16 MB of the 3dfx BAR0 %08lX",
                          g_fn[i].dev, g_fn[i].fn, j, g_fn[i].bar[j], base);
                emit(g_line);
                hits++;
            }
        }
    }
    if (!hits) emit("    no other memory BAR within 16 MB of it");
}

static void analyse_3dfx(unsigned dev, unsigned fn, int idx)
{
    unsigned long cmd = cfg_rd(dev, fn, 0x04);
    unsigned long bar0 = cfg_rd(dev, fn, 0x10);
    unsigned long irq = cfg_rd(dev, fn, 0x3C);
    unsigned long init_en = cfg_rd(dev, fn, 0x40);   /* Glide reads this too (sst1init.c) */
    unsigned long base = bar0 & ~0xFUL;

    emit("  >>> 3dfx function found - analysis:");
    wsprintfA(g_line, "    command=%04lX  mem-decode=%s io-decode=%s bus-master=%s",
              cmd & 0xFFFF, (cmd & 2) ? "ON" : "off", (cmd & 1) ? "ON" : "off",
              (cmd & 4) ? "ON" : "off");
    emit(g_line);
    wsprintfA(g_line, "    status=%04lX  (bit13 rcvd-master-abort=%lu bit12 rcvd-target-abort=%lu bit11 sig-target-abort=%lu)",
              cmd >> 16, (cmd >> 29) & 1, (cmd >> 28) & 1, (cmd >> 27) & 1);
    emit(g_line);
    wsprintfA(g_line, "    BAR0 raw=%08lX base=%08lX prefetch=%lu type=%lu", bar0, base,
              (bar0 >> 3) & 1, (bar0 >> 1) & 3);
    emit(g_line);
    if (base == 0)
        emit("    BAR0 VERDICT: UNASSIGNED (0) - nothing (BIOS or Windows) gave this card an address");
    else if (base < g_ramtop)
        emit("    BAR0 VERDICT: BELOW RAM TOP - mapping it would alias system RAM (dangerous)");
    else if (base & 0xFFFFFFUL)
        emit("    BAR0 VERDICT: NOT 16 MB ALIGNED - impossible for a 16 MB BAR, suspect bus/read problem");
    else
        emit("    BAR0 VERDICT: plausibly assigned above RAM");
    if (base) report_neighbours(base, idx);
    wsprintfA(g_line, "    interrupt line=%lu (0xFF/0 = none assigned) pin=%lu (1=INTA)",
              irq & 0xFF, (irq >> 8) & 0xFF);
    emit(g_line);
    wsprintfA(g_line, "    cfg 0x40 initEnable=%08lX", init_en);
    emit(g_line);
    if (fn == 0) {   /* Glide's "single board SLI hack" also looks at fn 1 */
        unsigned long id1 = cfg_rd(dev, 1, 0x00);
        wsprintfA(g_line, "    function 1 id=%08lX%s", id1,
                  (id1 & 0xFFFF) == 0x121A ? "  (single-board SLI second chip)" : "");
        emit(g_line);
    }
}

static int scan(void)
{
    unsigned dev, fn, found = 0, bridges = 0;
    int i, tdfx[8];

    for (dev = 0; dev < 32; dev++) {
        unsigned maxfn = 1;
        for (fn = 0; fn < maxfn; fn++) {
            unsigned long id = cfg_rd(dev, fn, 0x00), id2, cls, hdr;
            if ((id & 0xFFFF) == 0xFFFF) continue;              /* master abort: nothing there */
            if ((id & 0xFFFF) == 0) {                           /* not a legal vendor id */
                wsprintfA(g_line, "00:%02X.%u returned id=%08lX - ANOMALOUS (a device is driving the bus but not a valid header)",
                          dev, fn, id);
                emit(g_line);
                continue;
            }
            id2 = cfg_rd(dev, fn, 0x00);                /* stability re-read */
            cls = cfg_rd(dev, fn, 0x08);
            hdr = cfg_rd(dev, fn, 0x0C);
            if (fn == 0 && (hdr & 0x00800000UL)) maxfn = 8;   /* multifunction */
            if (g_nfn < 64) {
                g_fn[g_nfn].dev = dev; g_fn[g_nfn].fn = fn; g_fn[g_nfn].ven_dev = id;
                for (i = 0; i < 6; i++) g_fn[g_nfn].bar[i] = cfg_rd(dev, fn, 0x10 + 4 * i);
                g_fn[g_nfn].rom = cfg_rd(dev, fn, 0x30);
            }
            wsprintfA(g_line,
                      "00:%02X.%u ven=%04lX dev=%04lX rev=%02lX class=%06lX hdr=%02lX%s",
                      dev, fn, id & 0xFFFF, id >> 16, cls & 0xFF, cls >> 8,
                      (hdr >> 16) & 0xFF, id2 == id ? "" : "  [ID RE-READ DIFFERED]");
            emit(g_line);
            if (g_nfn < 64) {
                unsigned long cmd = cfg_rd(dev, fn, 0x04), irq = cfg_rd(dev, fn, 0x3C);
                wsprintfA(g_line,
                          "    cmd=%04lX sts=%04lX bar0=%08lX bar1=%08lX bar2=%08lX bar3=%08lX bar4=%08lX bar5=%08lX rom=%08lX int=%02lX/%02lX",
                          cmd & 0xFFFF, cmd >> 16, g_fn[g_nfn].bar[0], g_fn[g_nfn].bar[1],
                          g_fn[g_nfn].bar[2], g_fn[g_nfn].bar[3], g_fn[g_nfn].bar[4],
                          g_fn[g_nfn].bar[5], g_fn[g_nfn].rom, irq & 0xFF, (irq >> 8) & 0xFF);
                emit(g_line);
            }
            if ((cls >> 16) == 0x0604) bridges++;
            if ((id & 0xFFFF) == 0x121A) {
                if (found < 8 && g_nfn < 64) tdfx[found++] = (int)g_nfn;
                else found++;
            }
            if (g_nfn < 64) g_nfn++;
        }
    }
    /* analyse after the whole bus is known, so the overlap check sees every
     * other device (the Cirrus at 0F sits after a card at DEV_03) */
    for (i = 0; i < (int)found && i < 8 && tdfx[i] < 64; i++)
        analyse_3dfx(g_fn[tdfx[i]].dev, g_fn[tdfx[i]].fn, tdfx[i]);
    wsprintfA(g_line, "summary: %d function(s) answered on bus 0; config reads that raced a ring-0 user of 0xCF8: %lu",
              g_nfn, g_races);
    emit(g_line);
    if (bridges)
        emit("note: a PCI-PCI bridge is present - buses behind it were NOT scanned");
    if (found) {
        wsprintfA(g_line, "RESULT: 3DFX_PRESENT count=%u (VEN_121A answers config cycles on bus 0)", found);
        emit(g_line);
        return 10;
    }
    emit("RESULT: NO_3DFX_ON_BUS0 - no VEN_121A function answered config cycles on bus 0, devices 0-31");
    return 0;
}

static int run(const char *cmdline)
{
    char tok[MAX_PATH];
    const char *p = cmdline;
    int noio = 0, force = 0;
    DWORD ver = GetVersion();
    MEMORYSTATUS ms;
    unsigned long save, t;

    g_path[0] = 0;
    for (;;) {
        p = next_tok(p, tok, sizeof(tok));
        if (!tok[0]) break;
        if (!lstrcmpiA(tok, "noio")) noio = 1;
        else if (!lstrcmpiA(tok, "force")) force = 1;
        else lstrcpynA(g_path, tok, sizeof(g_path));
    }
    if (!g_path[0]) lstrcpyA(g_path, "C:\\RETRO_AGENT\\PCI9X.TXT");
    if (!open_out(g_path) && !open_out("C:\\PCI9X.TXT")) return 2;

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    g_ramtop = (ms.dwTotalPhys + 0xFFFFFUL) & ~0xFFFFFUL;

    emit("pci9x 1.0 - read-only PCI bus-0 config probe (mechanism #1, ring 3)");
    wsprintfA(g_line, "os: GetVersion=%08lX (%s)  RAM total=%lu KB  ram-top~%08lX",
              (unsigned long)ver, (ver & 0x80000000UL) ? "Win9x" : "NT family",
              (unsigned long)(ms.dwTotalPhys / 1024), g_ramtop);
    emit(g_line);
    if (noio) { emit("noio: header only, no port I/O performed"); return 0; }
    if (!(ver & 0x80000000UL) && !force) {
        emit("refused: NT family - ring-3 port I/O is not permitted there (pass 'force' to see it fault)");
        return 4;
    }

    SetUnhandledExceptionFilter(on_fault);
    stage("before-first-port-io");

    save = io_inl(CFG_ADDR);
    io_out_cfgaddr(0x80000000UL);
    t = io_inl(CFG_ADDR);
    io_out_cfgaddr(save);
    wsprintfA(g_line, "cf8: found=%08lX  write 80000000 read back=%08lX  -> mechanism #1 %s",
              save, t, t == 0x80000000UL ? "PRESENT" : "ABSENT");
    emit(g_line);
    if (t != 0x80000000UL) {
        emit("RESULT: MECH1_ABSENT - not scanning (mechanism #2 is deliberately not attempted)");
        return 3;
    }

    stage("scanning bus 0");
    {
        int rc = scan();
        wsprintfA(g_line, "cf8 at exit=%08lX (was %08lX at start)", io_inl(CFG_ADDR), save);
        emit(g_line);
        stage("done");
        return rc;
    }
}

void __stdcall start(void)
{
    char *c = GetCommandLineA();
    int rc;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; }
    else while (*c && *c != ' ') c++;
    rc = run(c);
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    ExitProcess(rc);
}

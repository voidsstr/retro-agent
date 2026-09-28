/* usb9x - READ-ONLY USB host-controller probe for Windows 9x.
 *
 *   usb9x [outfile]                 default C:\RETRO_AGENT\USB9X.TXT
 *   usb9x watch <secs> [outfile]    default C:\RETRO_AGENT\USBWATCH.TXT - sample
 *       every UHCI controller's USBCMD/USBSTS/PORTSC1/PORTSC2 as fast as
 *       Sleep(1) allows and log each CHANGE with its millisecond offset, to
 *       watch the hub driver reset, enable and address a port (or give up)
 *
 * Written 2026-09-28 for .243 (Compaq Deskpro 2000, Win98 SE): a USB mouse was
 * plugged into the VIA VT83C572 UHCI card and Windows enumerated NOTHING below
 * its root hub - no device, not even "Unknown Device" - while the controller
 * and root hub reported problem 0 and the card's PCI status had bit 14
 * (signaled system error) set. The root hub is a software device, so it can
 * look perfect on a controller that is not running at all. This says what the
 * HARDWARE thinks:
 *
 *   - every UHCI function on bus 0 (class 0C0300): its PCI command/status and
 *     interrupt line, then from its I/O BAR (BAR4): USBCMD, USBSTS (halted,
 *     host-system-error, process-error), USBINTR, the frame number read twice
 *     50 ms apart (a running schedule advances it), and PORTSC1/PORTSC2 -
 *     is a device CONNECTED, and has the port been ENABLED;
 *   - the PIIX/PIIX3 PCI interrupt router's PIRQA-D routing (cfg 0x60-0x63),
 *     the 8259 edge/level control registers (ELCR, 0x4D0/0x4D1 - a PCI IRQ
 *     must be LEVEL) and the interrupt mask registers (0x21/0xA1).
 *
 * It only READS. The only port it writes is 0xCF8 (config ADDRESS), restored
 * after every use, exactly as pci9x does; it never writes 0xCFC, never writes
 * a UHCI register (PORTSC's status bits are write-1-to-clear, so even a
 * read-modify-write would change them - there is none here).
 *
 * No C runtime (see pci9x.c / fleet9x.c). Build:
 *   i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -e _start@0 \
 *       -o usb9x.exe usb9x.c -lkernel32 -luser32 -s
 */
#include <windows.h>

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

static HANDLE g_out = INVALID_HANDLE_VALUE;
static char   g_line[512];
static char   g_path[MAX_PATH];

static void emit(const char *s)
{
    DWORD n;
    if (g_out == INVALID_HANDLE_VALUE) return;
    WriteFile(g_out, s, lstrlenA(s), &n, NULL);
    WriteFile(g_out, "\r\n", 2, &n, NULL);
    FlushFileBuffers(g_out);
}

static __inline__ unsigned long io_inl(unsigned short port)
{
    unsigned long v;
    __asm__ __volatile__("inl %w1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static __inline__ unsigned short io_inw(unsigned short port)
{
    unsigned short v;
    __asm__ __volatile__("inw %w1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static __inline__ unsigned char io_inb(unsigned short port)
{
    unsigned char v;
    __asm__ __volatile__("inb %w1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* The ONLY port this program ever writes. */
static __inline__ void io_out_cfgaddr(unsigned long v)
{
    __asm__ __volatile__("outl %0, %w1" : : "a"(v), "Nd"((unsigned short)CFG_ADDR));
}

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
    }
    return v;
}

static LONG WINAPI on_fault(EXCEPTION_POINTERS *ep)
{
    wsprintfA(g_line, "FAULT: exception %08lX at %08lX - nothing was written",
              (unsigned long)ep->ExceptionRecord->ExceptionCode,
              (unsigned long)ep->ExceptionRecord->ExceptionAddress);
    emit(g_line);
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    ExitProcess(5);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void port_line(const char *name, unsigned short v)
{
    wsprintfA(g_line, "    %s=%04X  connected=%u connect-change=%u enabled=%u enable-change=%u "
              "line-J/K=%u%u resume=%u low-speed=%u reset=%u suspend=%u",
              name, v, v & 1, (v >> 1) & 1, (v >> 2) & 1, (v >> 3) & 1, (v >> 4) & 1, (v >> 5) & 1,
              (v >> 6) & 1, (v >> 8) & 1, (v >> 9) & 1, (v >> 12) & 1);
    emit(g_line);
}

static int uhci(unsigned dev, unsigned fn, unsigned long id)
{
    unsigned long cmdsts = cfg_rd(dev, fn, 0x04), bar4 = cfg_rd(dev, fn, 0x20);
    unsigned long irq = cfg_rd(dev, fn, 0x3C);
    unsigned short base, cmd, sts, intr, fr1, fr2;

    wsprintfA(g_line, "00:%02X.%u UHCI ven=%04lX dev=%04lX  pci-cmd=%04lX pci-sts=%04lX "
              "(sig-system-error=%lu rcvd-master-abort=%lu)  int-line=%lu pin=%lu",
              dev, fn, id & 0xFFFF, id >> 16, cmdsts & 0xFFFF, cmdsts >> 16,
              (cmdsts >> 30) & 1, (cmdsts >> 29) & 1, irq & 0xFF, (irq >> 8) & 0xFF);
    emit(g_line);
    if (!(bar4 & 1) || !(cmdsts & 1)) {
        wsprintfA(g_line, "    BAR4=%08lX - no I/O window decoded, registers not read", bar4);
        emit(g_line);
        return 0;
    }
    base = (unsigned short)(bar4 & 0xFFE0);
    cmd  = io_inw(base + 0x00);
    sts  = io_inw(base + 0x02);
    intr = io_inw(base + 0x04);
    fr1  = io_inw(base + 0x06);
    Sleep(50);
    fr2  = io_inw(base + 0x06);
    wsprintfA(g_line, "    io=%04X  USBCMD=%04X (run=%u) USBSTS=%04X (halted=%u proc-err=%u host-sys-err=%u "
              "resume=%u err-int=%u int=%u)  USBINTR=%04X",
              base, cmd, cmd & 1, sts, (sts >> 5) & 1, (sts >> 4) & 1, (sts >> 3) & 1, (sts >> 2) & 1,
              (sts >> 1) & 1, sts & 1, intr);
    emit(g_line);
    wsprintfA(g_line, "    frame %u -> %u over 50 ms: schedule %s  FLBASEADD=%08lX",
              fr1 & 0x7FF, fr2 & 0x7FF, (fr1 != fr2) ? "RUNNING" : "NOT ADVANCING",
              io_inl(base + 0x08));
    emit(g_line);
    port_line("PORTSC1", io_inw(base + 0x10));
    port_line("PORTSC2", io_inw(base + 0x12));
    return 1;
}

static unsigned short g_io[4];
static int g_nio;

static void find_uhci(void)
{
    unsigned dev, fn;
    g_nio = 0;
    for (dev = 0; dev < 32 && g_nio < 4; dev++)
        for (fn = 0; fn < 8 && g_nio < 4; fn++) {
            unsigned long id = cfg_rd(dev, fn, 0x00), bar4;
            if ((id & 0xFFFF) == 0xFFFF || id == 0) {
                if (fn == 0) break;
                continue;
            }
            if ((cfg_rd(dev, fn, 0x08) >> 8) == 0x0C0300 && ((bar4 = cfg_rd(dev, fn, 0x20)) & 1))
                g_io[g_nio++] = (unsigned short)(bar4 & 0xFFE0);
            if (fn == 0 && !(cfg_rd(dev, 0, 0x0C) & 0x00800000UL))
                break;
        }
}

static int watch(DWORD secs)
{
    unsigned short last[4][4];
    DWORD t0 = GetTickCount(), n = 0, changes = 0;
    int i, k;
    find_uhci();
    wsprintfA(g_line, "watch: %d UHCI controller(s), %lu s", g_nio, secs);
    emit(g_line);
    for (i = 0; i < g_nio; i++)
        for (k = 0; k < 4; k++) last[i][k] = 0xFFFF;
    while (GetTickCount() - t0 < secs * 1000) {
        for (i = 0; i < g_nio; i++) {
            unsigned short v[4];
            v[0] = io_inw(g_io[i] + 0x00);
            v[1] = io_inw(g_io[i] + 0x02);
            v[2] = io_inw(g_io[i] + 0x10);
            v[3] = io_inw(g_io[i] + 0x12);
            if (v[0] != last[i][0] || v[1] != last[i][1] || v[2] != last[i][2] || v[3] != last[i][3]) {
                wsprintfA(g_line, "+%6lu ms io=%04X USBCMD=%04X USBSTS=%04X PORTSC1=%04X PORTSC2=%04X"
                          "  p2: conn=%u en=%u reset=%u low=%u",
                          GetTickCount() - t0, g_io[i], v[0], v[1], v[2], v[3],
                          v[3] & 1, (v[3] >> 2) & 1, (v[3] >> 9) & 1, (v[3] >> 8) & 1);
                emit(g_line);
                for (k = 0; k < 4; k++) last[i][k] = v[k];
                changes++;
            }
        }
        n++;
        Sleep(1);
    }
    wsprintfA(g_line, "watch done: %lu samples, %lu change(s)", n, changes);
    emit(g_line);
    return 0;
}

static int run(const char *args)
{
    unsigned dev, fn;
    unsigned char e0, e1, m0, m1;
    int found = 0;

    if (args && (args[0] == 'w' || args[0] == 'W') && (args[1] == 'a' || args[1] == 'A')
            && (args[5] == ' ' || !args[5])) {
        const char *p = args + 5;
        DWORD secs = 0;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') secs = secs * 10 + (DWORD)(*p++ - '0');
        while (*p == ' ') p++;
        if (!secs || secs > 600) secs = 20;
        lstrcpynA(g_path, *p ? p : "C:\\RETRO_AGENT\\USBWATCH.TXT", sizeof(g_path));
        g_out = CreateFileA(g_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (g_out == INVALID_HANDLE_VALUE) return 2;
        if (!(GetVersion() & 0x80000000UL)) { emit("refused: NT family"); return 4; }
        SetUnhandledExceptionFilter(on_fault);
        return watch(secs);
    }
    lstrcpynA(g_path, (args && *args) ? args : "C:\\RETRO_AGENT\\USB9X.TXT", sizeof(g_path));
    g_out = CreateFileA(g_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (g_out == INVALID_HANDLE_VALUE) return 2;
    emit("usb9x 1.0 - read-only UHCI + PCI interrupt-router probe");
    if (!(GetVersion() & 0x80000000UL)) {
        emit("refused: NT family - ring-3 port I/O is not permitted there");
        return 4;
    }
    SetUnhandledExceptionFilter(on_fault);
    for (dev = 0; dev < 32; dev++) {
        for (fn = 0; fn < 8; fn++) {
            unsigned long id = cfg_rd(dev, fn, 0x00), cls;
            if ((id & 0xFFFF) == 0xFFFF || id == 0) {
                if (fn == 0) break;
                continue;
            }
            cls = cfg_rd(dev, fn, 0x08) >> 8;
            if (cls == 0x0C0300)
                found += uhci(dev, fn, id);
            if (id == 0x70008086UL || id == 0x122E8086UL) {     /* PIIX3 / PIIX ISA bridge */
                unsigned long r = cfg_rd(dev, fn, 0x60);
                wsprintfA(g_line, "00:%02X.%u PIIX router PIRQA=%02lX PIRQB=%02lX PIRQC=%02lX PIRQD=%02lX "
                          "(bit7 = routing disabled; low nibble = IRQ)",
                          dev, fn, r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF, r >> 24);
                emit(g_line);
            }
            if (fn == 0 && !(cfg_rd(dev, 0, 0x0C) & 0x00800000UL))
                break;                              /* single-function device */
        }
    }
    e0 = io_inb(0x4D0); e1 = io_inb(0x4D1);
    m0 = io_inb(0x21);  m1 = io_inb(0xA1);
    wsprintfA(g_line, "ELCR 4D0=%02X 4D1=%02X (bit n = IRQ n / n+8 LEVEL)  IRQ11 %s  |  "
              "IMR 21=%02X A1=%02X  IRQ11 %s",
              e0, e1, (e1 & 0x08) ? "LEVEL" : "EDGE", m0, m1, (m1 & 0x08) ? "MASKED" : "unmasked");
    emit(g_line);
    wsprintfA(g_line, "summary: %d UHCI controller(s) read", found);
    emit(g_line);
    return 0;
}

void __stdcall start(void)
{
    char *c = GetCommandLineA();
    int rc;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; }
    else while (*c && *c != ' ') c++;
    while (*c == ' ') c++;
    rc = run(c);
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    ExitProcess(rc);
}

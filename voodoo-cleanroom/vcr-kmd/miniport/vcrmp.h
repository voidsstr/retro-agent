/*
 * vcrmp.h - vcr-kmd video miniport (vcrmp.sys): private declarations.
 *
 * Built with mingw-w64 against its DDK headers. Those headers give us the
 * videoprt interface but none of ntddk.h (which does not coexist with
 * miniport.h), so the handful of ntoskrnl/hal entry points the flight recorder
 * and the multi-chip code need are declared here. Every one of them is checked
 * against the real XP SP3 export tables at build time (tools/check_imports.py).
 */
#ifndef VCRMP_H
#define VCRMP_H

#include <ntdef.h>
#include <guiddef.h>
#include <devioctl.h>
#include <ddk/miniport.h>
#include <ddk/dderror.h>
#include <ddk/video.h>
#include <stdarg.h>

#include "../include/vcr_types.h"
#include "../include/vcr_regs.h"
#include "../include/vcr_modes.h"
#include "../include/vcr_events.h"
#include "../include/vcr_ioctl.h"
#include "../include/vcr_log.h"
#include "../include/vcr_fmt.h"

#ifndef ERROR_ACCESS_DENIED
#define ERROR_ACCESS_DENIED 5L
#endif

/* ---- ntoskrnl / hal, declared by hand (see header comment) -------------- */
typedef struct {
    ULONG TitleIndex, Type, DataLength;
    UCHAR Data[1];
} VCR_KEY_VALUE_PARTIAL;
#define VCR_KeyValuePartialInformation 2
#define VCR_NonPagedPool               0
#define VCR_DelayedWorkQueue           1
#define VCR_PCIConfiguration           4
#define VCR_REG_OPTION_NON_VOLATILE    0
#define VCR_KEY_ALL_ACCESS             0xF003F
#define VCR_PASSIVE_LEVEL              0

typedef VOID (NTAPI *VCR_WORKER)(PVOID);
typedef struct {
    LIST_ENTRY List;
    VCR_WORKER WorkerRoutine;
    PVOID Parameter;
} VCR_WORK_QUEUE_ITEM;

NTSYSAPI NTSTATUS NTAPI ZwCreateKey(PHANDLE, ULONG, POBJECT_ATTRIBUTES, ULONG,
                                    PUNICODE_STRING, ULONG, PULONG);
NTSYSAPI NTSTATUS NTAPI ZwSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG,
                                      PVOID, ULONG);
NTSYSAPI NTSTATUS NTAPI ZwQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID,
                                        ULONG, PULONG);
NTSYSAPI NTSTATUS NTAPI ZwFlushKey(HANDLE);
NTSYSAPI NTSTATUS NTAPI ZwClose(HANDLE);
NTSYSAPI VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING, PCWSTR);
PVOID NTAPI ExAllocatePoolWithTag(ULONG PoolType, ULONG Bytes, ULONG Tag);
VOID NTAPI ExQueueWorkItem(VCR_WORK_QUEUE_ITEM *Item, ULONG QueueType);
VOID NTAPI KeQuerySystemTime(PLARGE_INTEGER);
ULONGLONG NTAPI KeQueryInterruptTime(VOID);
HANDLE NTAPI PsGetCurrentProcessId(VOID);
PVOID NTAPI PsGetCurrentProcess(VOID);
LONGLONG NTAPI PsGetProcessCreateTimeQuadPart(PVOID Process);
ULONG __cdecl DbgPrint(const char *, ...);
UCHAR NTAPI KeGetCurrentIrql(VOID);
ULONG NTAPI HalGetBusDataByOffset(ULONG BusDataType, ULONG Bus, ULONG Slot,
                                  PVOID Buf, ULONG Offset, ULONG Len);
ULONG NTAPI HalSetBusDataByOffset(ULONG BusDataType, ULONG Bus, ULONG Slot,
                                  PVOID Buf, ULONG Offset, ULONG Len);

/* ---- device extension ------------------------------------------------------- */
#define VCR_MAX_MODES       200
#define VCR_MAX_PROCS       8
#define VCR_MMIO_MAP_LEN    0x400000    /* io + cmd + 2d + 3d register windows */

typedef struct VCR_CHIP {
    ULONG            slot;          /* PCI_SLOT_NUMBER.u.AsULONG on ext->bus */
    PHYSICAL_ADDRESS mmio_phys;     /* memBase0 */
    PHYSICAL_ADDRESS lfb_phys;      /* memBase1 */
    ULONG            io_phys;       /* ioBase */
    PUCHAR           regs;          /* kernel VA of memBase0[0 .. 4 MB) */
} VCR_CHIP;

typedef struct VCR_PROC {
    ULONG     pid;
    PVOID     eprocess;
    LONGLONG  created;
    ULONG     nva;
    PVOID     va[2 + 4 * (VCR_MAX_CHIPS - 1)];
    vcr_glide_map map;              /* what we told this process */
} VCR_PROC;

typedef struct VCR_BOOTSTATE {      /* what the BIOS left, for HwResetHw */
    ULONG saved;
    ULONG vidproccfg, dacmode, pllctrl0, vgainit0, vgainit1, miscinit0, lfbmemcfg,
          desktopstart, stride, screensize;
    UCHAR misc, crtc[0x1c], seq[5], gfx[9], attr[0x15];
} VCR_BOOTSTATE;

typedef struct VCR_EXT {
    ULONG     backend;              /* VCR_HW_* */
    ULONG     vendor, device, subsys, revision;
    ULONG     bus, slot;
    ULONG     nchips;
    VCR_CHIP  chip[VCR_MAX_CHIPS];
    ULONG     mmio_len, lfb_len;
    PUCHAR    io;                   /* mapped I/O BAR (port base) */
    ULONG     io_len;
    PUCHAR    lfb_kernel;           /* only mapped on demand (MAP_VIDEO_MEMORY) */
    ULONG     fb_per_chip;
    ULONG     desktop_offset;       /* where the desktop starts in video memory */
    vcr_hwcaps caps;

    ULONG     nmodes;
    vcr_mode  modes[VCR_MAX_MODES];
    LONG      cur_mode;             /* -1 = VGA / none */
    vcr_modeset cur_set;
    ULONG     cur_stride;

    /* bochs backend */
    volatile USHORT *dispi;         /* BAR2 + 0x500 */
    PUCHAR    bochs_vga;            /* BAR2 + 0x400 (0x3c0 alias) */
    ULONG     bochs_vram;

    VCR_BOOTSTATE boot;
    VCR_PROC  procs[VCR_MAX_PROCS];
    ULONG     allow_poke;
    ULONG     bridge_bus, bridge_slot, bridge_found;
    ULONG     sli_active;

    /* stable-boot timer */
    ULONG     seconds;
    ULONG     boot_marked;
    VCR_WORK_QUEUE_ITEM work;
    ULONG     work_queued;
} VCR_EXT;

/* ---- vcrmp_log.c: flight recorder + registry diagnostics ------------------ */
void    VcrLogCreate(PUNICODE_STRING RegistryPath);
ULONG   VcrLog(ULONG level, ULONG code, ULONG a, ULONG b, ULONG c, ULONG d,
               const char *fmt, ...);
void    VcrLogFromUser(const vcr_log_write_req *r);
ULONG   VcrLogRead(ULONG after_seq, vcr_log_read_res *out, ULONG max);
ULONG   VcrLogNextSeq(void);
/* A boot phase: logged, AND persisted to the registry and flushed, so the
 * last phase reached survives a hang that needs a power cycle. PASSIVE only. */
void    VcrPhase(ULONG code, ULONG a, ULONG b, const char *what);
ULONG   VcrDiagGet(PCWSTR name, ULONG dflt);
void    VcrDiagSet(PCWSTR name, ULONG value, BOOLEAN flush);
ULONG   VcrDiagGetString(PCWSTR name, USHORT *buf, ULONG maxchars);
ULONG   VcrMs(void);
extern  ULONG VcrBootAttempts;

#define VLOG(lv, code, a, b, c, d, ...) VcrLog((lv), (code), (ULONG)(a), (ULONG)(b), (ULONG)(c), (ULONG)(d), __VA_ARGS__)

/* ---- vcrmp_hw.c --------------------------------------------------------------- */
ULONG   VcrRd(VCR_EXT *x, ULONG chip, ULONG off);
void    VcrWr(VCR_EXT *x, ULONG chip, ULONG off, ULONG v);
UCHAR   VcrVgaRd(VCR_EXT *x, ULONG port);
void    VcrVgaWr(VCR_EXT *x, ULONG port, UCHAR v);
UCHAR   VcrVgaIdxRd(VCR_EXT *x, ULONG port, UCHAR idx);
void    VcrVgaIdxWr(VCR_EXT *x, ULONG port, UCHAR idx, UCHAR v);
UCHAR   VcrAttrRd(VCR_EXT *x, UCHAR idx);
ULONG   VcrPciRead(VCR_EXT *x, ULONG slot, ULONG off, ULONG size);
void    VcrPciWrite(VCR_EXT *x, ULONG slot, ULONG off, ULONG v, ULONG size);
ULONG   VcrPciSlot(ULONG dev, ULONG fn);

VP_STATUS VcrHwDiscover(VCR_EXT *x);            /* chips, memory size, caps */
void    VcrHwSaveBootState(VCR_EXT *x);
void    VcrHwSnapshotToLog(VCR_EXT *x, const char *why);
VP_STATUS VcrHwSetMode(VCR_EXT *x, ULONG mode_index);
VP_STATUS VcrHwRestoreMode(VCR_EXT *x);
void    VcrHwResetToVga(VCR_EXT *x);
VP_STATUS VcrHwSetClut(VCR_EXT *x, const VIDEO_CLUT *clut, ULONG len);
void    VcrHwSnapshot(VCR_EXT *x, vcr_snapshot *s);
ULONG   VcrHwWaitIdle(VCR_EXT *x, ULONG chip, ULONG loops);
void    VcrHwPower(VCR_EXT *x, ULONG state);

/* ---- vcrmp_map.c: user mappings for Glide and GDI --------------------------- */
VP_STATUS VcrMapGlide(VCR_EXT *x, vcr_glide_map *m);
VP_STATUS VcrUnmapGlide(VCR_EXT *x, ULONG pid);

#endif /* VCRMP_H */

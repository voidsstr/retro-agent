/*
 * vcrprobe.c - a small, read-mostly kernel probe for golden captures.
 *
 * The vendor driver's CRTC/sequencer/attribute state cannot be read from user
 * mode (XP gives user mode no port I/O, and the VSA-100's MMIO alias of the
 * VGA registers does not read back - measured). This legacy NT driver is
 * loaded and unloaded ON THE FLY through the service manager, next to
 * whatever display driver is installed, and answers three questions:
 *
 *   IOCTL_VCRPROBE_VGA   the whole VGA register file (misc, SEQ, CRTC incl.
 *                        the 3dfx extensions, GFX, ATTR) - the one write it
 *                        makes is the index-then-read sequence itself
 *   IOCTL_VCRPROBE_PCI   up to 256 bytes of PCI config space, any bus/dev/fn
 *                        (the V5's slave chips and its HiNT bridge)
 *   IOCTL_VCRPROBE_MEM   up to 4 KB of physical memory, READ ONLY (a slave
 *                        chip's registers, which no mapping exposes)
 *
 *   sc create vcrprobe type= kernel start= demand binPath= C:\vcr\vcrprobe.sys
 *   sc start vcrprobe  ...  vcrctl probe-vga | probe-pci | probe-mem  ...  sc stop vcrprobe
 */
#include <ddk/ntddk.h>
#include "../include/vcr_probe.h"

static PDEVICE_OBJECT g_dev;

static void vga_dump(vcr_probe_vga *v)
{
    ULONG i;
    UCHAR attr_idx;
    v->misc = READ_PORT_UCHAR((PUCHAR)0x3cc);
    for (i = 0; i < sizeof v->seq; i++) {
        WRITE_PORT_UCHAR((PUCHAR)0x3c4, (UCHAR)i);
        v->seq[i] = READ_PORT_UCHAR((PUCHAR)0x3c5);
    }
    for (i = 0; i < sizeof v->crtc; i++) {
        WRITE_PORT_UCHAR((PUCHAR)0x3d4, (UCHAR)i);
        v->crtc[i] = READ_PORT_UCHAR((PUCHAR)0x3d5);
    }
    for (i = 0; i < sizeof v->gfx; i++) {
        WRITE_PORT_UCHAR((PUCHAR)0x3ce, (UCHAR)i);
        v->gfx[i] = READ_PORT_UCHAR((PUCHAR)0x3cf);
    }
    /* attribute controller: reset the flip-flop, keep PAS (bit 5) set so the
     * screen is not blanked, read, reset again */
    (void)READ_PORT_UCHAR((PUCHAR)0x3da);
    attr_idx = READ_PORT_UCHAR((PUCHAR)0x3c0);
    for (i = 0; i < sizeof v->attr; i++) {
        (void)READ_PORT_UCHAR((PUCHAR)0x3da);
        WRITE_PORT_UCHAR((PUCHAR)0x3c0, (UCHAR)(i | 0x20));
        v->attr[i] = READ_PORT_UCHAR((PUCHAR)0x3c1);
    }
    (void)READ_PORT_UCHAR((PUCHAR)0x3da);
    WRITE_PORT_UCHAR((PUCHAR)0x3c0, (UCHAR)(attr_idx | 0x20));
    (void)READ_PORT_UCHAR((PUCHAR)0x3da);
    v->is1 = READ_PORT_UCHAR((PUCHAR)0x3da);
}

static NTSTATUS NTAPI on_create_close(PDEVICE_OBJECT d, PIRP irp)
{
    (void)d;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI on_ioctl(PDEVICE_OBJECT d, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG in = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0;
    (void)d;

    switch (code) {
    case IOCTL_VCRPROBE_VGA:
        if (out < sizeof(vcr_probe_vga)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        vga_dump((vcr_probe_vga *)buf);
        info = sizeof(vcr_probe_vga);
        st = STATUS_SUCCESS;
        break;
    case IOCTL_VCRPROBE_PCI: {
        vcr_probe_pci req;
        if (in < sizeof req || out < sizeof req) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        req = *(vcr_probe_pci *)buf;
        if (req.len > sizeof req.data || req.offset + req.len > 256) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        req.got = HalGetBusDataByOffset(PCIConfiguration, req.bus,
                                        (req.dev & 0x1f) | ((req.fn & 7) << 5),
                                        req.data, req.offset, req.len);
        *(vcr_probe_pci *)buf = req;
        info = sizeof req;
        st = STATUS_SUCCESS;
        break;
    }
    case IOCTL_VCRPROBE_MEM: {
        vcr_probe_mem req;
        PHYSICAL_ADDRESS pa;
        PUCHAR va;
        ULONG i;
        if (in < sizeof req || out < sizeof req) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        req = *(vcr_probe_mem *)buf;
        if (req.len == 0 || req.len > sizeof req.data || (req.phys & 3) || (req.len & 3) ||
            req.phys < 0x100000) {      /* never RAM below 1 MB; MMIO lives far above */
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        pa.QuadPart = req.phys;
        va = (PUCHAR)MmMapIoSpace(pa, req.len, MmNonCached);
        if (!va) {
            st = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        for (i = 0; i < req.len; i += 4)     /* dword reads: registers want them */
            *(ULONG *)&req.data[i] = READ_REGISTER_ULONG((PULONG)(va + i));
        MmUnmapIoSpace(va, req.len);
        *(vcr_probe_mem *)buf = req;
        info = sizeof req;
        st = STATUS_SUCCESS;
        break;
    }
    }
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static VOID NTAPI on_unload(PDRIVER_OBJECT drv)
{
    UNICODE_STRING link;
    (void)drv;
    RtlInitUnicodeString(&link, L"\\DosDevices\\VcrProbe");
    IoDeleteSymbolicLink(&link);
    if (g_dev)
        IoDeleteDevice(g_dev);
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
    UNICODE_STRING name, link;
    NTSTATUS st;
    (void)path;
    RtlInitUnicodeString(&name, L"\\Device\\VcrProbe");
    RtlInitUnicodeString(&link, L"\\DosDevices\\VcrProbe");
    st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &g_dev);
    if (!NT_SUCCESS(st))
        return st;
    st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(g_dev);
        return st;
    }
    drv->MajorFunction[IRP_MJ_CREATE] = on_create_close;
    drv->MajorFunction[IRP_MJ_CLOSE] = on_create_close;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = on_ioctl;
    drv->DriverUnload = on_unload;
    return STATUS_SUCCESS;
}

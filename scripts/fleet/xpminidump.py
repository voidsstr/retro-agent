#!/usr/bin/env python3
"""Read a Windows XP/2003 32-bit KERNEL minidump (C:\\WINDOWS\\Minidump\\Mini*.dmp)
without WinDbg: bugcheck code + parameters, the faulting address and the
driver it falls in, and the drivers whose code addresses sit on the saved
kernel stack (the usual suspects, innermost first).

    python3 scripts/fleet/xpminidump.py Mini083126-03.dmp [more.dmp ...]

Written 2026-09-26 for .143 (GeForce 6800 + Voodoo5 5500, 25 dumps). It is a
triage aid, not a debugger: "on the stack" means "a return address into this
module is there", which is strong evidence, not proof, of who crashed.
"""
import struct
import sys

BUGCHECKS = {
    0x0A: 'IRQL_NOT_LESS_OR_EQUAL', 0x1E: 'KMODE_EXCEPTION_NOT_HANDLED',
    0x24: 'NTFS_FILE_SYSTEM', 0x50: 'PAGE_FAULT_IN_NONPAGED_AREA',
    0x7E: 'SYSTEM_THREAD_EXCEPTION_NOT_HANDLED', 0x7F: 'UNEXPECTED_KERNEL_MODE_TRAP',
    0x8E: 'KERNEL_MODE_EXCEPTION_NOT_HANDLED', 0xC2: 'BAD_POOL_CALLER',
    0xD1: 'DRIVER_IRQL_NOT_LESS_OR_EQUAL', 0xEA: 'THREAD_STUCK_IN_DEVICE_DRIVER',
    0x100000EA: 'THREAD_STUCK_IN_DEVICE_DRIVER_M', 0x1000007E: 'SYSTEM_THREAD_EXCEPTION_NOT_HANDLED_M',
    0x1000008E: 'KERNEL_MODE_EXCEPTION_NOT_HANDLED_M', 0x19: 'BAD_POOL_HEADER',
    0x4E: 'PFN_LIST_CORRUPT', 0x77: 'KERNEL_STACK_INPAGE_ERROR', 0x7A: 'KERNEL_DATA_INPAGE_ERROR',
    0x9C: 'MACHINE_CHECK_EXCEPTION', 0x124: 'WHEA_UNCORRECTABLE_ERROR', 0xB4: 'VIDEO_DRIVER_INIT_FAILURE',
    0xC5: 'DRIVER_CORRUPTED_EXPOOL', 0xD5: 'DRIVER_PAGE_FAULT_IN_FREED_SPECIAL_POOL',
    0x1A: 'MEMORY_MANAGEMENT', 0x3B: 'SYSTEM_SERVICE_EXCEPTION', 0x10D: 'WDF_VIOLATION',
}


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def parse(path):
    b = open(path, 'rb').read()
    if b[:8] != b'PAGEDUMP':
        return {'file': path, 'error': 'not a 32-bit PAGEDUMP'}
    r = {'file': path, 'build': u32(b, 0x0C), 'code': u32(b, 0x28),
         'params': [u32(b, 0x2C + 4 * i) for i in range(4)]}
    t = 0x1000
    (spb, size, valid, ctx, exc, mm, unl, prcb, proc, thr, cso, csz,
     dlo, dcnt, spo, sps, bdo, topt, tos) = struct.unpack_from('<19I', b, t)
    drivers = []
    if dcnt and dlo:
        stride = 0x4C   # DUMP_DRIVER_ENTRY32: name offset + KLDR_DATA_TABLE_ENTRY32
        for i in range(dcnt):
            e = dlo + i * stride
            noff = u32(b, e)
            base, _ep, size_img = u32(b, e + 4 + 0x18), u32(b, e + 4 + 0x1C), u32(b, e + 4 + 0x20)
            name = '?'
            if noff and noff + 4 <= len(b):
                ln = u32(b, noff)
                name = b[noff + 4:noff + 4 + ln * 2].decode('utf-16-le', 'replace')
            drivers.append((base, base + size_img, name))

    def who(a):
        for lo, hi, n in drivers:
            if lo <= a < hi:
                return f'{n}+0x{a - lo:x}'
        return None

    r['param_modules'] = [who(p) for p in r['params']]
    if exc and exc + 0x10 <= len(b):
        r['exc_code'], r['exc_addr'] = u32(b, exc), u32(b, exc + 0x0C)
        r['exc_module'] = who(r['exc_addr'])
    if ctx and ctx + 0xBC <= len(b):
        r['eip'] = u32(b, ctx + 0xB8)
        r['eip_module'] = who(r['eip'])
    stack = []
    if cso and csz:
        for o in range(cso, min(cso + csz, len(b) - 3), 4):
            m = who(u32(b, o))
            if m:
                n = m.split('+')[0]
                if n.lower() not in ('ntoskrnl.exe', 'hal.dll') and n not in stack:
                    stack.append(n)
    r['stack_drivers'] = stack[:8]
    return r


def main(paths):
    for p in paths:
        r = parse(p)
        if 'error' in r:
            print(f"{p}: {r['error']}")
            continue
        name = BUGCHECKS.get(r['code'], '?')
        params = ', '.join(f'0x{x:08x}' for x in r['params'])
        print(f"{p.split('/')[-1]}: 0x{r['code']:08X} {name} ({params})")
        mods = [m for m in r['param_modules'] if m]
        if mods:
            print(f"   params point into: {', '.join(mods)}")
        if r.get('exc_module') or r.get('eip_module'):
            print(f"   fault: exc 0x{r.get('exc_code', 0):08x} at {r.get('exc_module') or hex(r.get('exc_addr', 0))}; "
                  f"eip {r.get('eip_module') or hex(r.get('eip', 0))}")
        print(f"   drivers on stack: {', '.join(r['stack_drivers']) or '(none besides nt/hal)'}")


if __name__ == '__main__':
    main(sys.argv[1:])

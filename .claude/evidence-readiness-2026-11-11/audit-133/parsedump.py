import struct, sys, datetime
p = sys.argv[1]
b = open(p,'rb').read()
u32 = lambda o: struct.unpack_from('<I', b, o)[0]
print('sig', b[0:8], 'major', hex(u32(8)), 'build', u32(12))
print('DirBase', hex(u32(0x10)), 'PfnDb', hex(u32(0x14)), 'PsLoadedModuleList', hex(u32(0x18)), 'PsActiveProcessHead', hex(u32(0x1c)))
print('machine', hex(u32(0x20)), 'nproc', u32(0x24))
print('BugCheck 0x%08X  P1=0x%08X P2=0x%08X P3=0x%08X P4=0x%08X' % (u32(0x28), u32(0x2c), u32(0x30), u32(0x34), u32(0x38)))
# DUMP_HEADER32 fields: 0x58 KdDebuggerDataBlock? find DumpType & SystemTime
# 32-bit header: 0x0f88 DumpType, 0x0f98 SystemUpTime?, 0xfb8 SystemTime? Use known offsets
for off,name in [(0xf88,'DumpType'),(0xf8c,'MiniDumpFields'),(0xf90,'SecondaryDataState'),(0xf94,'ProductType'),(0xf98,'SuiteMask'),(0xf9c,'WriterStatus'),(0xfa0,'RequiredDumpSpace_lo'),(0xfa4,'RequiredDumpSpace_hi')]:
    print(name, hex(u32(off)))
def ft(o):
    v = struct.unpack_from('<Q', b, o)[0]
    try:
        return datetime.datetime(1601,1,1) + datetime.timedelta(microseconds=v//10)
    except Exception:
        return v
print('SystemUpTime@0xfb0', struct.unpack_from('<Q', b, 0xfb0)[0]/1e7, 's  SystemTime@0xfb8', ft(0xfb8))
print('alt: 0xfc0', ft(0xfc0), ' 0xfa8', ft(0xfa8))
# Triage dump at 0x1000
t = 0x1000
names = ['ServicePackBuild','SizeOfDump','ValidOffset','ContextOffset','ExceptionOffset','MmOffset','UnloadedDriversOffset','PrcbOffset','ProcessOffset','ThreadOffset','CallStackOffset','SizeOfCallStack','DriverListOffset','DriverCount','StringPoolOffset','StringPoolSize','BrokenDriverOffset','TriageOptions','TopOfStack','DataPageAddress','DataPageOffset','DataPageSize','DebuggerDataOffset','DebuggerDataSize','DataBlocksOffset','DataBlocksCount']
tri = {}
for i,n in enumerate(names):
    tri[n] = u32(t + 4*i)
    print('  %-22s 0x%X' % (n, tri[n]))
def dstr(off):
    # DUMP_STRING: ULONG Length (chars) then WCHAR buffer
    n = u32(off)
    return b[off+4: off+4+2*n].decode('utf-16le', errors='replace')
dl = tri['DriverListOffset']; dc = tri['DriverCount']
print('drivers:', dc)
rows = []
for i in range(dc):
    e = dl + i*0x4c
    nameoff = u32(e)
    ldr = e + 4
    base = u32(ldr+0x18); size = u32(ldr+0x20); ts = u32(ldr+0x44)
    try:
        nm = dstr(nameoff)
    except Exception as ex:
        nm = '?%s' % ex
    try:
        tsd = datetime.datetime.utcfromtimestamp(ts).strftime('%Y-%m-%d')
    except Exception:
        tsd = '?'
    rows.append((base, size, nm, ts, tsd))
for base,size,nm,ts,tsd in rows:
    print('  %08X %08X %-20s %08X %s' % (base,size,nm,ts,tsd))
# unloaded drivers
uo = tri['UnloadedDriversOffset']
if uo:
    cnt = u32(uo)
    print('unloaded drivers count', cnt)
    # DUMP_UNLOADED_DRIVERS32: UNICODE_STRING32 Name(8) + WCHAR DriverName[12] (24) + StartAddress(4) + EndAddress(4) = 40
    for i in range(min(cnt, 60)):
        e = uo + 4 + i*40
        nm = b[e+8:e+8+24].decode('utf-16le', errors='replace').split('\x00')[0]
        print('   ', nm, hex(u32(e+32)), hex(u32(e+36)))
# Broken driver
bo = tri['BrokenDriverOffset']
print('BrokenDriverOffset', hex(bo))
# context
co = tri['ContextOffset']
if co:
    # CONTEXT x86: ContextFlags at 0, ... Eip at 0xb8, Esp at 0xc4, Ebp at 0xb4
    print('Context Eip=0x%08X Esp=0x%08X Ebp=0x%08X' % (u32(co+0xb8), u32(co+0xc4), u32(co+0xb4)))
# Find module containing Eip
eip = u32(co+0xb8) if co else 0
for base,size,nm,ts,tsd in rows:
    if base <= eip < base+size:
        print('Eip in', nm, '+0x%X' % (eip-base))
# Look at call stack for addresses in modules
cs = tri['CallStackOffset']; sz = tri['SizeOfCallStack']
hits = {}
for o in range(cs, cs+sz-3, 4):
    v = u32(o)
    for base,size,nm,ts,tsd in rows:
        if base <= v < base+size:
            hits.setdefault(nm, []).append('+0x%X' % (v-base))
print('stack refs per module:')
for k,v in hits.items():
    print('  ', k, len(v), v[:12])

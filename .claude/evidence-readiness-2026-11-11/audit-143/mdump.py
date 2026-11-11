import struct, sys, datetime, collections

BUGS = {0xEA:'THREAD_STUCK_IN_DEVICE_DRIVER',0x8E:'KERNEL_MODE_EXCEPTION_NOT_HANDLED',0x50:'PAGE_FAULT_IN_NONPAGED_AREA',
        0xD1:'DRIVER_IRQL_NOT_LESS_OR_EQUAL',0x0A:'IRQL_NOT_LESS_OR_EQUAL',0x7F:'UNEXPECTED_KERNEL_MODE_TRAP',
        0x1000008E:'KERNEL_MODE_EXCEPTION_NOT_HANDLED_M',0x100000EA:'THREAD_STUCK_IN_DEVICE_DRIVER_M',0x10000050:'PAGE_FAULT_IN_NONPAGED_AREA_M',
        0x1000007E:'SYSTEM_THREAD_EXCEPTION_NOT_HANDLED_M',0x7E:'SYSTEM_THREAD_EXCEPTION_NOT_HANDLED',0xC2:'BAD_POOL_CALLER',
        0x19:'BAD_POOL_HEADER',0xBE:'ATTEMPTED_WRITE_TO_READONLY_MEMORY',0x24:'NTFS_FILE_SYSTEM',0x1000000A:'IRQL_NOT_LESS_OR_EQUAL_M',
        0xE2:'MANUALLY_INITIATED_CRASH',0x9C:'MACHINE_CHECK_EXCEPTION',0x7A:'KERNEL_DATA_INPAGE_ERROR',0x77:'KERNEL_STACK_INPAGE_ERROR',
        0x4E:'PFN_LIST_CORRUPT',0x1A:'MEMORY_MANAGEMENT',0xFC:'ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY',0xC5:'DRIVER_CORRUPTED_EXPOOL',
        0x10000005:'INVALID_PROCESS_ATTACH_ATTEMPT_M'}

def ft(v):
    if not v: return None
    return datetime.datetime(1601,1,1)+datetime.timedelta(microseconds=v//10)

def u32(b,o): return struct.unpack_from('<I',b,o)[0]

def parse(path):
    b=open(path,'rb').read()
    r={'file':path.split('/')[-1],'size':len(b)}
    assert b[:8]==b'PAGEDUMP', b[:8]
    r['build']=u32(b,0x0c)
    code=u32(b,0x28); r['bugcheck']=code; r['name']=BUGS.get(code,'?')
    r['params']=[u32(b,0x2c+4*i) for i in range(4)]
    r['dumptype']=u32(b,0xf88)
    st=struct.unpack_from('<Q',b,0xfc0)[0]; up=struct.unpack_from('<Q',b,0xfb8)[0]
    r['systime']=ft(st); r['uptime_s']=up//10**7 if up else None
    ctx=0x320
    eip=u32(b,ctx+0xb8); esp=u32(b,ctx+0xc4); ebp=u32(b,ctx+0xb4)
    r['eip']=eip; r['esp']=esp
    exc=0x7d0
    r['exc_code']=u32(b,exc); r['exc_addr']=u32(b,exc+0x0c)
    t=0x1000
    T=[u32(b,t+4*i) for i in range(26)]
    names='ServicePackBuild SizeOfDump ValidOffset ContextOffset ExceptionOffset MmOffset UnloadedDriversOffset PrcbOffset ProcessOffset ThreadOffset CallStackOffset SizeOfCallStack DriverListOffset DriverCount StringPoolOffset StringPoolSize BrokenDriverOffset TriageOptions TopOfStack DataPageAddress DataPageOffset DataPageSize DebuggerDataOffset DebuggerDataSize DataBlocksOffset DataBlocksCount'.split()
    TD=dict(zip(names,T)); r['triage']=TD
    # triage context may differ from header context
    if TD['ContextOffset'] and TD['ContextOffset']+0xcc < len(b):
        co=TD['ContextOffset']
        r['t_eip']=u32(b,co+0xb8); r['t_esp']=u32(b,co+0xc4); r['t_ebp']=u32(b,co+0xb4)
    mods=[]
    dl=TD['DriverListOffset']; n=TD['DriverCount']
    for i in range(min(n,400)):
        e=dl+i*0x4c
        if e+0x4c>len(b): break
        noff=u32(b,e)
        L=e+4
        base=u32(b,L+0x18); size=u32(b,L+0x20); ts=u32(b,L+0x44)
        name='?'
        if 0<noff<len(b)-4:
            ln=u32(b,noff)
            if 0<ln<260:
                name=b[noff+4:noff+4+2*ln].decode('utf-16-le','replace')
        mods.append((name,base,size,ts))
    r['mods']=mods
    def which(a):
        for (nm,base,size,ts) in mods:
            if base<=a<base+size: return nm
        return None
    r['eip_mod']=which(r.get('t_eip',eip)); r['hdr_eip_mod']=which(eip)
    r['exc_mod']=which(r['exc_addr'])
    r['param_mods']=[which(p) for p in r['params']]
    # stack scan
    cs=TD['CallStackOffset']; sz=TD['SizeOfCallStack']
    cnt=collections.Counter(); order=[]
    if cs and sz and cs+sz<=len(b):
        for o in range(cs,cs+sz-3,4):
            v=u32(b,o); m=which(v)
            if m:
                cnt[m]+=1
                if not order or order[-1]!=m: order.append(m)
    r['stack_mods']=cnt.most_common(8); r['stack_order']=order[:20]
    # unloaded drivers
    return r

if __name__=='__main__':
    want=set(a.lower() for a in ('nv4_disp.dll','nv4_mini.sys','viaagp.sys','viaagp1.sys','ac97via.sys','3dfxv5d.dll','3dfxv5m.sys','3dfxvs.dll','3dfxvsm.sys','win32k.sys','videoprt.sys','dxg.sys','ntoskrnl.exe','hal.dll','agp440.sys','pci.sys','viaide.sys','d347bus.sys','d347prt.sys','dmfe.sys','ntkrnlpa.exe','ntkrnlmp.exe','halaacpi.dll','halacpi.dll','vcrmp.sys','vcrdd.dll','fxgpio.sys'))
    for p in sys.argv[1:]:
        r=parse(p)
        print('====',r['file'],r['size'],'build',r['build'],'dumptype',r['dumptype'])
        print('  time',r['systime'],'uptime_s',r['uptime_s'])
        print('  bugcheck 0x%X %s params %s'%(r['bugcheck'],r['name'],' '.join('%08x'%x for x in r['params'])),'param_mods',r['param_mods'])
        print('  hdr eip %08x (%s) triage eip %08x (%s) exc %08x addr %08x (%s)'%(r['eip'],r['hdr_eip_mod'],r.get('t_eip',0),r['eip_mod'],r['exc_code'],r['exc_addr'],r['exc_mod']))
        print('  stack modules', r['stack_mods'])
        print('  stack order', r['stack_order'])
        for (nm,base,size,ts) in r['mods']:
            if nm.lower() in want:
                print('    mod %-14s base %08x size %08x ts %s'%(nm,base,size,datetime.datetime.utcfromtimestamp(ts).strftime('%Y-%m-%d') if ts else '-'))

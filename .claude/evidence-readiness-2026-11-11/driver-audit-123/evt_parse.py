import struct, sys, datetime, collections
def parse(path):
    b=open(path,'rb').read()
    recs=[]
    i=0
    n=len(b)
    while i < n-0x38:
        if b[i+4:i+8]==b'LfLe':
            L=struct.unpack_from('<I',b,i)[0]
            if 0x38<=L<=0x10000 and i+L<=n and struct.unpack_from('<I',b,i+L-4)[0]==L:
                (Length,Res,RecNo,TGen,TWr,EvID,EvType,NStr,Cat,RF,Closing,StrOff,SidLen,SidOff,DataLen,DataOff)=struct.unpack_from('<IIIIIIHHHHIIIIII',b,i)
                rec=b[i:i+L]
                # source name at 0x38
                j=0x38
                def wstr(off):
                    e=off
                    while e+1<len(rec) and rec[e:e+2]!=b'\x00\x00': e+=2
                    return rec[off:e].decode('utf-16-le','replace'), e+2
                src,j=wstr(j); comp,j=wstr(j)
                strs=[]; o=StrOff
                for k in range(NStr):
                    if o>=len(rec): break
                    s,o=wstr(o); strs.append(s)
                data=rec[DataOff:DataOff+DataLen] if DataLen else b''
                recs.append(dict(rec=RecNo,t=datetime.datetime.fromtimestamp(TGen,datetime.UTC).replace(tzinfo=None),id=EvID&0xFFFF,fullid=EvID,type=EvType,src=src,strs=strs,data=data))
                i+=L; continue
        i+=4
    recs.sort(key=lambda r:r['rec'])
    return recs
if __name__=='__main__':
    recs=parse(sys.argv[1])
    print(len(recs),'records', recs[0]['t'] if recs else None,'->',recs[-1]['t'] if recs else None)

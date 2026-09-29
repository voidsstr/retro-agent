"""Build a QEMU test disk the way INSTALL does: LBA 32 = the original MBR, LBA0 = cmosmbr
code with the disk's own 0xDA..0xDF stamp and 0x1B8..0x1FF kept."""
import struct, sys
def merge(code, cur):
    assert len(code) == 440 and len(cur) == 512 and cur[510:] == b'\x55\xaa'
    return code[:0xDA] + cur[0xDA:0xE0] + code[0xE0:0x1B8] + cur[0x1B8:]
if __name__ == '__main__':
    code, out = open(sys.argv[1], 'rb').read(), sys.argv[2]
    orig = bytearray(open('test/origmbr.bin', 'rb').read())
    orig[0xDA:0xE0] = bytes([0, 0, 0x80, 0x11, 0x22, 0x33])       # a Win9x-style stamp
    # one active FAT16 partition at LBA 63 (CHS 0/1/1)
    orig[0x1BE:0x1CE] = struct.pack('<BBBBBBBBII', 0x80, 1, 1, 0, 6, 15, 63, 19, 63, 20000)
    img = bytearray(512 * 20480)
    img[0:512] = merge(code, bytes(orig))
    img[32*512:33*512] = orig
    img[63*512:64*512] = open('test/vbr.bin', 'rb').read()
    open(out, 'wb').write(img)

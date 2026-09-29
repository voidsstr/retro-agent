# cmosmbr: 440 bytes of boot code (the installer adds the disk's own
# 0xDA..0xDF stamp, 0x1B8.. signature/partition table and 55AA)
# the CHS sector (cylinder 0, head 0) holding the saved original MBR: LBA 32.
# .243's LBA 1..2 already hold old MBR copies, LBA 3..60 are zero, LBA 61..62 hold data.
SAVESEC = 33

all: cmosmbr.bin cmosmbr_test.bin cmosmbr_rom.bin

# the DOS installer embeds the production bytes (mbrcode.h is generated - never edit it)
installer: CMOSINST.EXE
mbrcode.h: cmosmbr.bin
	python3 -c "import sys;c=open('cmosmbr.bin','rb').read();r=[', '.join('0x%02X'%b for b in c[i:i+12]) for i in range(0,440,12)];sys.stdout.write('/* GENERATED from cmosmbr.bin by the Makefile - do not edit */\\nstatic const unsigned char mbr_code[440] = {\\n    '+',\\n    '.join(r)+'\\n};\\n')" > $@
WATCOM ?= $(HOME)/development/toolchain-dos/watcom
CMOSINST.EXE: cmosinst.c mbrcode.h Makefile
	WATCOM=$(WATCOM) INCLUDE=$(WATCOM)/h PATH=$(WATCOM)/binl64:$$PATH wcl -q -bt=dos -dSAVE_SEC=$(SAVESEC) -ms -0 -os -fe=$@ cmosinst.c

cmosmbr.bin: cmosmbr.S
	as --32 --defsym SAVESEC=$(SAVESEC) --defsym TEST=0 --defsym WANT1B=0x00 --defsym ROMSEG=0xF000 -o cmosmbr.o cmosmbr.S
	ld -m elf_i386 -Ttext 0x600 --oformat binary -o $@ cmosmbr.o
	@test $$(stat -c %s $@) -eq 440 || { echo "size != 440"; exit 1; }
cmosmbr_test.bin: cmosmbr.S
	as --32 --defsym SAVESEC=$(SAVESEC) --defsym TEST=1 --defsym WANT1B=0x5A --defsym ROMSEG=0xF000 -o cmosmbr_t.o cmosmbr.S
	ld -m elf_i386 -Ttext 0x600 --oformat binary -o $@ cmosmbr_t.o
clean:
	rm -f *.o *.bin *.err
# TEST=2: the PRODUCTION logic (ROM check, want 00) reading its ROM strings from
# segment 0x0070 (RAM) instead of F000, so QEMU can exercise the Compaq path.
cmosmbr_rom.bin: cmosmbr.S
	as --32 --defsym SAVESEC=$(SAVESEC) --defsym TEST=2 --defsym WANT1B=0x00 --defsym ROMSEG=0x0070 -o cmosmbr_r.o cmosmbr.S
	ld -m elf_i386 -Ttext 0x600 --oformat binary -o $@ cmosmbr_r.o

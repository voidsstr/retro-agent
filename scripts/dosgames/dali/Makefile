# scripts/dosgames/dali - the real-DOS helpers of the DALI LAN launchers.
# ASKIP/WBOOT: Open Watcom 16-bit (tiny model .COM), IPXCHK/PCINIC: GNU as.
# `make kit` copies the built helpers into provisioning/dali (re-run MD5SUMS).
WATCOM ?= $(HOME)/development/toolchain-dos/watcom
export WATCOM
export PATH := $(WATCOM)/binl64:$(PATH)
export INCLUDE := $(WATCOM)/h

all: ASKIP.COM WBOOT.COM IPXCHK.COM PCINIC.COM

ASKIP.COM: askip.c askipcore.h
	wcl -q -bt=com -mt -os -fe=$@ askip.c && rm -f askip.o askip.obj
WBOOT.COM: wboot.c
	wcl -q -bt=com -mt -os -fe=$@ wboot.c && rm -f wboot.o wboot.obj
%.COM: %.S
	as --32 -o $*.o $< && ld -m elf_i386 -Ttext 0x100 -e 0x100 --oformat binary -o $@ $*.o && rm -f $*.o
IPXCHK.COM: ipxchk.S
PCINIC.COM: pcinic.S

kit: all
	cp ASKIP.COM WBOOT.COM IPXCHK.COM ../../../provisioning/dali/
	cd ../../../provisioning/dali && md5sum ASKIP.COM COPYING.TXT DALI.EXE DHCP.EXE IPXCHK.COM WBOOT.COM > MD5SUMS.txt

.PHONY: all kit

# Makefile for DOSCHAT.EXE — combined DOS retro agent+chat.
# Open Watcom 16-bit cross-compile from Linux, linking the mTCP stack.
#
# Needs:
#   ~/development/toolchain-dos/watcom     Open Watcom v2 snapshot
#   ~/development/toolchain-dos/mtcp-src   mTCP source (github mbbrutman/mTCP)
#     (with lowercase include symlinks — see scripts/dosgames/README.md
#      "wpp on Linux is case-sensitive for quoted includes")
#
# Large model, 8086-safe codegen (mTCP house flags).

WATCOM ?= $(HOME)/development/toolchain-dos/watcom
MTCP   ?= $(HOME)/development/toolchain-dos/mtcp-src/src
export WATCOM
export PATH := $(WATCOM)/binl64:$(PATH)
export INCLUDE := $(WATCOM)/h

MODEL = -ml
OPTS  = -0 $(MODEL) -DCFG_H=\"doschat.cfg\" -oh -ok -os -s -oa -ei -zp2 -zpw \
        -i=$(MTCP)/TCPINC -i=$(MTCP)/INCLUDE -i=. -i=..
TCPOBJS = packet.obj arp.obj eth.obj ip.obj tcp.obj tcpsockm.obj udp.obj \
          utils.obj dns.obj timer.obj trace.obj ipasm.obj

all: doschat.exe

# Every mTCP object bakes in doschat.cfg (socket counts, ring size, feature
# switches), so a cfg edit MUST rebuild the library too — otherwise you get
# a mismatched stack that fails initStack for no visible reason.
$(TCPOBJS): doschat.cfg

%.obj: $(MTCP)/TCPLIB/%.CPP
	wpp $(OPTS) $< -fo=$@

ipasm.obj: $(MTCP)/TCPLIB/IPASM.ASM
	wasm -0 $(MODEL) $< -fo=$@

# TCPLIB filenames are uppercase; map explicitly
packet.obj:   $(MTCP)/TCPLIB/PACKET.CPP
	wpp $(OPTS) $< -fo=$@
arp.obj:      $(MTCP)/TCPLIB/ARP.CPP
	wpp $(OPTS) $< -fo=$@
eth.obj:      $(MTCP)/TCPLIB/ETH.CPP
	wpp $(OPTS) $< -fo=$@
ip.obj:       $(MTCP)/TCPLIB/IP.CPP
	wpp $(OPTS) $< -fo=$@
tcp.obj:      $(MTCP)/TCPLIB/TCP.CPP
	wpp $(OPTS) $< -fo=$@
tcpsockm.obj: $(MTCP)/TCPLIB/TCPSOCKM.CPP
	wpp $(OPTS) $< -fo=$@
udp.obj:      $(MTCP)/TCPLIB/UDP.CPP
	wpp $(OPTS) $< -fo=$@
utils.obj:    $(MTCP)/TCPLIB/UTILS.CPP
	wpp $(OPTS) $< -fo=$@
dns.obj:      $(MTCP)/TCPLIB/DNS.CPP
	wpp $(OPTS) $< -fo=$@
timer.obj:    $(MTCP)/TCPLIB/TIMER.CPP
	wpp $(OPTS) $< -fo=$@
trace.obj:    $(MTCP)/TCPLIB/TRACE.CPP
	wpp $(OPTS) $< -fo=$@

doschat.obj: doschat.cpp doschat.cfg ../shared/frameproto.h \
             ../shared/chatcore.h ../shared/chatcore.c ../shared/chattext.h
	wpp $(OPTS) doschat.cpp -fo=$@

doschat.exe: $(TCPOBJS) doschat.obj
	wlink system dos option quiet name doschat.exe \
	      file {$(TCPOBJS) doschat.obj}
	@echo "Built doschat.exe - $$(stat -c%s doschat.exe) bytes"

clean:
	rm -f *.obj *.exe *.map *.err

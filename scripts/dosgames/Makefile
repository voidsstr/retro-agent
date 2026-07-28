# Makefile for DOSGAME.EXE — Open Watcom cross-compile from Linux.
# Needs the toolchain at ~/development/toolchain-dos/watcom (see README.md).
WATCOM ?= $(HOME)/development/toolchain-dos/watcom
export WATCOM
export PATH := $(WATCOM)/binl64:$(PATH)
export INCLUDE := $(WATCOM)/h

all: dosgame.exe

dosgame.exe: dosgame.c
	wcl -bcl=dos -ml -os -q -wx -fe=dosgame.exe dosgame.c

clean:
	rm -f dosgame.exe *.o *.err

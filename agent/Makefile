# Retro Agent - Cross-compilation Makefile for MinGW-w64
# Targets Win98SE (WINVER=0x0410) and WinXP

CC = i686-w64-mingw32-gcc
WINDRES = i686-w64-mingw32-windres

SRCDIR = src
OBJDIR = obj
TARGET = retro_agent.exe

# Version from highest git tag (e.g. v1.1.0 -> 1.1.0)
VERSION = $(shell git tag -l 'v*' --sort=-v:refname 2>/dev/null | head -1 | sed 's/^v//' || echo "0.0.0")
# Versioned upload: each build goes to its own filename in the retro_agent folder
SMB_BASE = smb://192.168.1.122/files/Utility/Retro%20Automation/retro_agent
SMB_VERSIONED = $(SMB_BASE)/retro_agent_v$(VERSION).exe
# Latest pointer (overwritten on every release for backward compat with old installers)
SMB_LATEST = smb://192.168.1.122/files/Utility/Retro%20Automation/retro_agent.exe
SMB_CREDS = user:password  # EDIT: your SMB share credentials

# compat.c MUST be first - provides CRT stubs for Win98SE
SOURCES = $(SRCDIR)/compat.c \
          $(SRCDIR)/main.c \
          $(SRCDIR)/protocol.c \
          $(SRCDIR)/handlers.c \
          $(SRCDIR)/sysinfo.c \
          $(SRCDIR)/video.c \
          $(SRCDIR)/screen.c \
          $(SRCDIR)/files.c \
          $(SRCDIR)/registry.c \
          $(SRCDIR)/process.c \
          $(SRCDIR)/exec.c \
          $(SRCDIR)/input.c \
          $(SRCDIR)/netshare.c \
          $(SRCDIR)/snapshot.c \
          $(SRCDIR)/service.c \
          $(SRCDIR)/smart.c \
          $(SRCDIR)/gameindex.c \
          $(SRCDIR)/gamesync.c \
          $(SRCDIR)/display.c \
          $(SRCDIR)/audio.c \
          $(SRCDIR)/sysfix.c \
          $(SRCDIR)/licstatus.c \
          $(SRCDIR)/ai.c \
          $(SRCDIR)/chatproxy.c \
          $(SRCDIR)/autoupdate.c \
          $(SRCDIR)/retrowall.c \
          $(SRCDIR)/onboard.c \
          $(SRCDIR)/dosstage.c \
          $(SRCDIR)/watchdog.c \
          $(SRCDIR)/monitor.c \
          $(SRCDIR)/log.c \
          $(SRCDIR)/util.c

OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))

# Icon resource. Without it retro_agent.exe carries NO resource directory, so
# the desktop shortcut the agent places for itself gets Windows' generic
# application icon - and on a fully provisioned fleet desktop two of the
# generic icons were ours rather than a game's. windres is already the MinGW
# toolchain's resource compiler; the .res object links like any other.
RESOBJ = $(OBJDIR)/retro_agent_res.o

# Win98SE compatible flags.
#
# GENUINE-PENTIUM-1 SAFETY (no CMOV): -march=i586 keeps OUR code CMOV-free,
# but two things still pulled i686-only CMOV in:
#   1. MinGW's ANSI stdio (__mingw_printf/vsnprintf in libmingwex) is built
#      for the i686 baseline and uses CMOV (e.g. the "(null)" %s fallback).
#      A genuine Pentium (P5) has no CMOV -> STATUS_ILLEGAL_INSTRUCTION
#      (0xc000001d) on the first printf. -D__USE_MINGW_ANSI_STDIO=0 routes
#      printf/snprintf/etc to the box's own msvcrt.dll instead (P5-safe).
#   2. MinGW compiles __thread to EMULATED TLS (___emutls_get_address), which
#      also has CMOV and sits on the exception-recovery path -> fixed in
#      main.c by using native Win32 TLS (TlsAlloc) instead of __thread.
# Two CMOV-bearing pseudo-relocator helpers (_mark_section_writable,
# __GetPEImageBase) remain in the image but are DEAD: this agent has zero
# runtime pseudo-relocs (LIST == LIST_END) so they are never called. This all
# surfaced on a Compaq Deskpro 2000 (Pentium 1); the fleet's XP boxes are
# i686+ and never hit CMOV.
CFLAGS = -Wall -Wextra -Os -s \
         -DWINVER=0x0410 -D_WIN32_WINNT=0x0410 \
         -DWIN32_LEAN_AND_MEAN \
         -D__USE_MINGW_ANSI_STDIO=0 \
         -DAGENT_VERSION='"$(VERSION)"' \
         -march=i586 -mtune=pentium3 \
         -fno-stack-protector \
         -fno-exceptions \
         -fomit-frame-pointer \
         -DNDEBUG

LDFLAGS = -static -s \
          -Wl,--subsystem,console \
          -Wl,--major-os-version,4 \
          -Wl,--minor-os-version,0 \
          -L$(LIBDIR)

# Patched msvcrt import lib: _strtoui64/_strtoi64 removed (not in Win98SE msvcrt.dll).
# Named libmsvcrt.a and placed in lib/ so -L gives it priority over the system copy.
LIBDIR = lib

# System libraries - order matters for MinGW
# cfgmgr32 loaded dynamically for Win98SE compat (CM_Get_DevNode_Status)
LIBS = -lws2_32 -ladvapi32 -lsetupapi -lgdi32 -luser32 -lkernel32 -lwinmm

.PHONY: all clean

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJECTS) $(RESOBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Built $(TARGET) - $$(stat -c%s $(TARGET) 2>/dev/null || stat -f%z $(TARGET)) bytes"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(RESOBJ): res/retro_agent.rc res/retro_agent.ico | $(OBJDIR)
	$(WINDRES) -I res -i res/retro_agent.rc -O coff -o $@

# Compat object must be linked first to provide stubs before CRT
$(OBJDIR)/compat.o: $(SRCDIR)/compat.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET)

# XP-only build (no compat stubs needed, can use more features)
xp: CFLAGS += -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -DTARGET_XP
xp: all

# Bump patch version, build, and upload to file share
# Usage: make release          (1.0.0 -> 1.0.1)
#        make release BUMP=minor (1.0.1 -> 1.1.0)
#        make release BUMP=major (1.1.0 -> 2.0.0)
BUMP ?= patch
release:
	@CURTAG=$$(git tag -l 'v*' --sort=-v:refname | head -1 || echo "v0.0.0"); \
	MAJOR=$$(echo $$CURTAG | sed 's/^v//' | cut -d. -f1); \
	MINOR=$$(echo $$CURTAG | sed 's/^v//' | cut -d. -f2); \
	PATCH=$$(echo $$CURTAG | sed 's/^v//' | cut -d. -f3); \
	case "$(BUMP)" in \
		major) MAJOR=$$((MAJOR+1)); MINOR=0; PATCH=0;; \
		minor) MINOR=$$((MINOR+1)); PATCH=0;; \
		patch) PATCH=$$((PATCH+1));; \
	esac; \
	NEWTAG="v$$MAJOR.$$MINOR.$$PATCH"; \
	NEWVER="$$MAJOR.$$MINOR.$$PATCH"; \
	echo "Version: $$CURTAG -> $$NEWTAG"; \
	git tag $$NEWTAG; \
	$(MAKE) clean; \
	$(MAKE) VERSION=$$NEWVER; \
	VERSIONED_URL="$(SMB_BASE)/retro_agent_v$$NEWVER.exe"; \
	echo "Uploading versioned: retro_agent/retro_agent_v$$NEWVER.exe"; \
	curl --upload-file $(TARGET) -u $(SMB_CREDS) "$$VERSIONED_URL"; \
	echo ""; \
	echo "Updating latest pointer at root"; \
	curl --upload-file $(TARGET) -u $(SMB_CREDS) "$(SMB_LATEST)"; \
	echo ""; \
	echo "Publishing version sidecar retro_agent.exe.ver = $$NEWVER"; \
	printf '%s\n' "$$NEWVER" > /tmp/retro_agent.ver; \
	curl --upload-file /tmp/retro_agent.ver -u $(SMB_CREDS) "$(SMB_LATEST).ver"; \
	rm -f /tmp/retro_agent.ver; \
	echo ""; \
	echo "Released $$NEWTAG to retro_agent/retro_agent_v$$NEWVER.exe (+ .ver sidecar)"

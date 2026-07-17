# Makefile for retro-infer - dependency-free ML engine for the retro fleet
#
# Build:    make
# Release:  make release            (tag infer-vX.Y.Z, patch bump)
#           make release BUMP=minor|major
#
# Targets Win98SE and up on i586; SSE/3DNow! code lives in dedicated TUs
# compiled with the matching -m flags and is only reached via runtime CPUID
# dispatch (src/kernels.c). Same MinGW toolchain as the agent.

CC = i686-w64-mingw32-gcc
TARGET = retro-infer.exe

# Version from highest infer-* git tag
VERSION = $(shell git tag -l 'infer-v*' --sort=-v:refname 2>/dev/null | head -1 | sed 's/^infer-v//' | grep . || echo "0.0.0")

CFLAGS = -Wall -Wextra -Os -s \
         -DWINVER=0x0410 -D_WIN32_WINNT=0x0410 \
         -DWIN32_LEAN_AND_MEAN \
         -DINFER_VERSION='"$(VERSION)"' \
         -march=i586 -mtune=pentium3 \
         -fno-stack-protector -fno-exceptions -fomit-frame-pointer

LDFLAGS = -lkernel32 -luser32 -lws2_32 -static

OBJDIR = build
BASE_SRCS = src/main.c src/cpuid.c src/tensor.c src/kernels.c src/bench.c \
            src/rim.c src/ops/gemm_scalar.c
SSE_SRCS  = src/ops/gemm_sse.c

BASE_OBJS = $(patsubst src/%.c,$(OBJDIR)/%.o,$(BASE_SRCS))
SSE_OBJS  = $(patsubst src/%.c,$(OBJDIR)/%.sse.o,$(SSE_SRCS))

.PHONY: all clean release

all: $(TARGET)

$(TARGET): $(BASE_OBJS) $(SSE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built $(TARGET) v$(VERSION) - $$(stat -c%s $(TARGET)) bytes"

$(OBJDIR)/%.o: src/%.c src/infer.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# SSE TU: only ever executed behind a runtime cpuid check
$(OBJDIR)/%.sse.o: src/%.c src/infer.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -msse -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET)

BUMP ?= patch
release:
	@CURTAG=$$(git tag -l 'infer-v*' --sort=-v:refname | head -1); \
	[ -n "$$CURTAG" ] || CURTAG="infer-v0.0.0"; \
	CURVER=$$(echo $$CURTAG | sed 's/^infer-v//'); \
	MAJOR=$$(echo $$CURVER | cut -d. -f1); \
	MINOR=$$(echo $$CURVER | cut -d. -f2); \
	PATCH=$$(echo $$CURVER | cut -d. -f3); \
	case "$(BUMP)" in \
		major) MAJOR=$$((MAJOR+1)); MINOR=0; PATCH=0;; \
		minor) MINOR=$$((MINOR+1)); PATCH=0;; \
		patch) PATCH=$$((PATCH+1));; \
	esac; \
	NEWVER="$$MAJOR.$$MINOR.$$PATCH"; \
	echo "Version: $$CURTAG -> infer-v$$NEWVER"; \
	git tag infer-v$$NEWVER; \
	$(MAKE) clean; \
	$(MAKE) VERSION=$$NEWVER

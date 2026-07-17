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

LDFLAGS = -lkernel32 -luser32 -lgdi32 -lws2_32 -static

OBJDIR = build
BASE_SRCS = src/main.c src/cpuid.c src/tensor.c src/kernels.c src/bench.c \
            src/rim.c src/rim_save.c src/json.c src/exec.c src/port.c \
            src/serve.c src/bnn_eval.c src/gpu/glide_mac.c src/gpu/glide_check.c src/gpu/nv_gl.c \
            src/ops/gemm_scalar.c src/ops/nn.c \
            src/train/tutil.c src/train/trees.c src/train/train_nn.c \
            src/train/gbdt.c src/train/forest.c src/train/svm.c src/train/nn_session.c src/train/gb_dist.c
SSE_SRCS  = src/ops/gemm_sse.c
MMX_SRCS  = src/ops/gemm_mmx.c
DNOW_SRCS = src/ops/gemm_3dnow.c

BASE_OBJS = $(patsubst src/%.c,$(OBJDIR)/%.o,$(BASE_SRCS))
SSE_OBJS  = $(patsubst src/%.c,$(OBJDIR)/%.sse.o,$(SSE_SRCS))
MMX_OBJS  = $(patsubst src/%.c,$(OBJDIR)/%.mmx.o,$(MMX_SRCS))
DNOW_OBJS = $(patsubst src/%.c,$(OBJDIR)/%.3dnow.o,$(DNOW_SRCS))

HDRS = src/infer.h src/json.h src/exec.h src/port.h src/ops/nn.h \
       src/train/tutil.h src/train/trees.h src/train/train_nn.h \
       src/train/gbdt.h

.PHONY: all clean release

all: $(TARGET)

$(TARGET): $(BASE_OBJS) $(SSE_OBJS) $(MMX_OBJS) $(DNOW_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built $(TARGET) v$(VERSION) - $$(stat -c%s $(TARGET)) bytes"

$(OBJDIR)/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Vectorized TUs: only ever executed behind a runtime cpuid check
$(OBJDIR)/%.sse.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -msse -c -o $@ $<

$(OBJDIR)/%.mmx.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -mmmx -c -o $@ $<

$(OBJDIR)/%.3dnow.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -m3dnow -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET) retro-infer-host

# Native Linux build for fast local parity testing (no 3DNow! on x86_64)
HOSTCC = gcc
HOST_SRCS = $(BASE_SRCS) $(SSE_SRCS) $(MMX_SRCS)
host:
	$(HOSTCC) -Wall -Wextra -O2 -DRI_NO_3DNOW -DINFER_VERSION='"$(VERSION)-host"' \
	  -o retro-infer-host $(HOST_SRCS) -lm
	@echo "Built retro-infer-host"

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

# Linux Agent - Makefile
# Builds retro_agent_linux for x86_64 (default) and ARM (make arm)

CC ?= gcc
SRCDIR = src
OBJDIR = obj
TARGET = retro_agent_linux

SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/protocol.c \
          $(SRCDIR)/handlers.c \
          $(SRCDIR)/sysinfo.c \
          $(SRCDIR)/screen.c \
          $(SRCDIR)/files.c \
          $(SRCDIR)/process.c \
          $(SRCDIR)/exec.c \
          $(SRCDIR)/package.c \
          $(SRCDIR)/service.c \
          $(SRCDIR)/log.c \
          $(SRCDIR)/util.c \
          $(SRCDIR)/phonehome.c

OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))

CFLAGS = -Wall -Wextra -Os -DNDEBUG
LDFLAGS = -pthread

# Check for X11 via pkg-config
X11_CFLAGS := $(shell pkg-config --cflags x11 2>/dev/null)
X11_LIBS := $(shell pkg-config --libs x11 2>/dev/null)

ifneq ($(X11_LIBS),)
    CFLAGS += -DHAVE_X11 $(X11_CFLAGS)
    LDFLAGS += $(X11_LIBS)
    $(info X11 support: enabled)
else
    $(info X11 support: disabled (framebuffer only))
endif

.PHONY: all clean arm

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "Built $(TARGET) - $$(stat -c%s $(TARGET) 2>/dev/null || stat -f%z $(TARGET)) bytes"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET)

# ARM cross-compile for Raspberry Pi
arm: CC = arm-linux-gnueabihf-gcc
arm: CFLAGS += -march=armv7-a -mfpu=neon-vfpv4
arm: all

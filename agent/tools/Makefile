# Makefile for retro_chat - Claude Code-style chat client for retro PCs
#
# Build:    make
# Release:  make release          (1.0.0 -> 1.0.1)
#           make release BUMP=minor (1.0.1 -> 1.1.0)
#           make release BUMP=major (1.1.0 -> 2.0.0)
#
# Releases are tagged with retro-chat-vX.Y.Z and uploaded to:
#   smb://192.168.1.122/files/Utility/Retro Automation/retro_chat/retro_chat_vX.Y.Z.exe
#
# A "latest pointer" is also written to:
#   smb://192.168.1.122/files/Utility/Retro Automation/retro_chat.exe

CC = i686-w64-mingw32-gcc
TARGET = retro_chat.exe
SOURCE = retro_chat.c

# Version from highest retro-chat-* git tag
VERSION = $(shell cd .. && git tag -l 'retro-chat-v*' --sort=-v:refname 2>/dev/null | head -1 | sed 's/^retro-chat-v//' || echo "0.0.0")

CFLAGS = -Wall -Wextra -Os -s \
         -DWINVER=0x0410 -D_WIN32_WINNT=0x0410 \
         -DWIN32_LEAN_AND_MEAN \
         -DRETRO_CHAT_VERSION='"$(VERSION)"' \
         -march=i586 -mtune=pentium3 \
         -fno-stack-protector

LDFLAGS = -lws2_32 -lkernel32 -luser32 -static

SMB_BASE = smb://192.168.1.122/files/Utility/Retro%20Automation/retro_chat
SMB_LATEST = smb://192.168.1.122/files/Utility/Retro%20Automation/retro_chat.exe
SMB_CREDS = user:password  # EDIT: your SMB share credentials

.PHONY: all clean release

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)
	@echo "Built $(TARGET) v$(VERSION) - $$(stat -c%s $(TARGET) 2>/dev/null || stat -f%z $(TARGET)) bytes"

clean:
	rm -f $(TARGET)

# Bump patch version, build, and upload to versioned share folder
BUMP ?= patch
release:
	@CURTAG=$$(cd .. && git tag -l 'retro-chat-v*' --sort=-v:refname | head -1 || echo "retro-chat-v0.0.0"); \
	CURVER=$$(echo $$CURTAG | sed 's/^retro-chat-v//'); \
	MAJOR=$$(echo $$CURVER | cut -d. -f1); \
	MINOR=$$(echo $$CURVER | cut -d. -f2); \
	PATCH=$$(echo $$CURVER | cut -d. -f3); \
	case "$(BUMP)" in \
		major) MAJOR=$$((MAJOR+1)); MINOR=0; PATCH=0;; \
		minor) MINOR=$$((MINOR+1)); PATCH=0;; \
		patch) PATCH=$$((PATCH+1));; \
	esac; \
	NEWVER="$$MAJOR.$$MINOR.$$PATCH"; \
	NEWTAG="retro-chat-v$$NEWVER"; \
	echo "Version: $$CURTAG -> $$NEWTAG"; \
	(cd .. && git tag $$NEWTAG); \
	$(MAKE) clean; \
	$(MAKE) VERSION=$$NEWVER; \
	VERSIONED_URL="$(SMB_BASE)/retro_chat_v$$NEWVER.exe"; \
	echo "Uploading versioned: retro_chat/retro_chat_v$$NEWVER.exe"; \
	curl --upload-file $(TARGET) -u $(SMB_CREDS) "$$VERSIONED_URL"; \
	echo ""; \
	echo "Updating latest pointer at root"; \
	curl --upload-file $(TARGET) -u $(SMB_CREDS) "$(SMB_LATEST)"; \
	echo ""; \
	echo "Released $$NEWTAG to retro_chat/retro_chat_v$$NEWVER.exe"

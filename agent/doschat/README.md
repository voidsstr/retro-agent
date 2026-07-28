# DOSCHAT — the retro agent AND the retro chat, in one DOS exe

`DOSCHAT.EXE` is a single 16-bit real-mode program that gives a DOS machine
(Win98's DOS 7.1, plain MS-DOS, or DOSBox) everything the Windows fleet boxes
get from `retro_agent.exe` + `retro_chat.exe` together:

- **Agent half** — the same framed-TCP protocol on port 9898 plus the UDP
  discovery broadcast on 9899, so `retro_chat_daemon.py` discovers and claims
  a DOS box exactly like a Windows box. Command subset that makes sense on
  DOS: `PING`, `SYSINFO`, `DIRLIST`, `EXEC`, `UPLOAD`, `DOWNLOAD`, `DELETE`,
  `MKDIR`, `QUIT`, `REBOOT`, plus the **full** chat-proxy surface
  (`PROMPT_*`, `LOG_*`, `STATUS_*`, `PROXY_*`).
- **Chat half** — the retro_chat-style console UI in the same process: prompt
  line, scrollback, `* Working...` spinner, `[subagent status]` line, Up/Down
  history, `:clear` / `:quit`. There's no loopback socket on DOS — the UI
  reads and writes the chat state directly.

## Shared code (this is the point)

| Module | Also used by | What it is |
|---|---|---|
| `agent/shared/frameproto.h` | Windows agent (`src/protocol.h`) | ports, frame limits, `RESP_*` status bytes |
| `agent/shared/chatcore.[ch]` | Windows agent (`src/chatproxy.c`) | prompt slot, log ring (drop-oldest-half), status sequence |
| `agent/shared/chattext.h` | Windows chat (`tools/retro_chat.c`) | control-byte sanitize + word wrap |

The Windows agent keeps only its NT-specific parts (critical section, the
auto/manual-reset events behind the long polls). DOS is single-tasking, so
DOSCHAT calls the same functions with no locking and services long polls from
its cooperative loop with wall-clock deadlines.

`tests/native/test_chatcore.c` compiles the shared engine natively and
`tests/python/test_doschat_shared.py` asserts the sharing (and the DOS memory
limits below) can't silently regress.

## Building

Needs the DOS toolchain (see `scripts/dosgames/README.md` for setup):

```
~/development/toolchain-dos/watcom      Open Watcom v2 snapshot
~/development/toolchain-dos/mtcp-src    mTCP source, github.com/mbbrutman/mTCP
```

```
cd agent/doschat && make          # -> doschat.exe (~88 KB)
```

mTCP is GPLv3; DOSCHAT links it, so the DOS build is distributed under GPLv3.

## Running on a DOS box

```
SET MTCPCFG=C:\DOSCHAT\MTCP.CFG
LH C:\DOSCHAT\NE2000.COM 0x60 <irq> <iobase>     (or your NIC's packet driver)
C:\DOSCHAT\DHCP
C:\DOSCHAT\DOSCHAT.EXE
```

Esc or `:quit` exits. The box then shows up in `chat_status.sh` and can be
prompted from the retro chat like any other fleet machine.

## DOS memory limits (emulator-verified 2026-07-28, do not "optimize" away)

- **mTCP's socket table is one `malloc`, capped at 64 KB.** `sizeof(TcpSocket)`
  scales with `TCP_SOCKET_RING_SIZE`, so raising the ring from the default 4
  cut the usable socket count in half and `initStack` failed with the
  misleading "packet driver?" message. Ring stays at 4; `MAX_CLIENTS` is 7
  (the daemon alone holds three long-poll connections).
- **Every mTCP object bakes in `doschat.cfg`.** The Makefile declares
  `$(TCPOBJS): doschat.cfg` — without it a config edit leaves a stale library
  and `initStack` fails for no visible reason. This cost the most time.
- **DGROUP is 64 KB total and mTCP mallocs from it.** Response and word-wrap
  scratch live on the far heap (`_fmalloc`), not in statics.
- `EXEC` shells out with `system()`, during which the TCP stack is **not**
  serviced; keep commands short.

## Verified end-to-end (DOSBox-X NE2000 + slirp, host port-forward 9898)

`PING`/`SYSINFO` over the real Python client; five concurrent clients; the
whole chat bus (`PROMPT_PUSH`→`POP`, `STATUS_SET`/`WAIT`, `LOG_APPEND`/`READ`,
`LOG_WAIT` timeout path); and a **full round trip** — the production
`retro_chat_daemon.py` discovered the DOS agent, the chat brain answered a
prompt, and the reply rendered wrapped in the DOS UI.

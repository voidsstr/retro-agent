# GoldSrc build 4554: let a non-Valve mod join a ReHLDS server

**One byte per file, in the staged `CounterStrike16` tree's `hw.dll` and
`sw.dll`.** Without it, The Specialists 3.0 cannot join the fleet's TS server
(`specialists-server`, :27017). The client shows:

    Disconnected
    Your .dll [cl_dlls\client.dll] differs from the server's.

and the server logs `Dropped ... Reason: Client sent 'drop'`. The client drops
itself even though its `ts\cl_dlls\client.dll` and the server's are
byte-identical (md5 `68972662…`).

## Cause (measured 2026-11-23)

- **The client.** This engine (No-Steam build 4554, protocol 48), in its
  serverinfo handler at `hw.dll` VA `0x1d28040`, runs `strcmp` on the game
  dir against `cstrike`, `cstrike_beta`, `czero`, `tfc`, `valve`, `dod`,
  `ricochet` and `dmc`. For any other mod it MD5s its own
  `cl_dlls\client.dll` and `memcmp`s that against the 16 bytes from
  `svc_serverinfo`. On a mismatch it prints the message above and disconnects.
  It does not treat an all-zero hash as "no check".
- **The server.** It is **ReHLDS**, because anniversary HLDS refuses
  `ts_i386.so` (see the `game-servers` skill). ReHLDS never hashes
  `client.dll`. `strace -f -e openat` of a ReHLDS instance with a client
  connecting shows no open of any `client.dll`, so the hash it sends cannot
  match. Removing the server's copy changed nothing, which is consistent.
- **Why nothing else broke.** CS 1.6 and HLDM are exempt by name, so they
  never reach the compare. That holds whatever server they join, and their
  servers are anniversary HLDS anyway. A modern Steam client no longer does
  this check, which is why ReHLDS can skip it.

## The edit

The `je` that skips the disconnect when `memcmp` returned 0 becomes a `jmp`
(`0x74` → `0xEB`):

| file | size | original md5 | patched md5 | offset |
|---|---|---|---|---|
| `hw.dll` (OpenGL / D3D) | 1,840,440 | `4903e8e8…` | `bdebc8a3…` | `0x281c2` |
| `sw.dll` (software) | 1,672,504 | `05c1e827…` | `dce2ba09…` | `0x27f71` |

`apply.py` checks size, md5 and the exact 30-odd bytes around each edit. It
refuses anything that is neither the original nor the patched build.

```bash
python3 provisioning/patches/goldsrc4554/apply.py --check
python3 provisioning/patches/goldsrc4554/apply.py --build     # ~/.retro-fleet/patch-out/goldsrc4554
python3 provisioning/patches/goldsrc4554/apply.py --publish   # backs up originals to
                                                              # _patches/CounterStrike16/originals-2026-11-23/
python3 -m pytest -q tests/python/test_patch_goldsrc4554.py
```

## Verified

Verified in the XP build VM on 2026-11-23, using the staged tree plus The
Specialists 3.0 installed there, with `-soft` because the VM's Cirrus has no GL:

- **Before the patch:** the client was refused with the message above.
- **After, TS:** joined `192.168.1.132:27017`. The server logged `entered the
  game`, and the client went through the MOTD and spawned on `ts_lobby`.
- **After, CS 1.6:** CS 1.6 from the same patched engine still joined :27015
  and reached team select.

Game binaries are copyrighted, so the outputs are never committed.

Rejected alternatives:
- Moving the TS server to anniversary HLDS (it refuses the 2007 game DLL).
- Teaching ReHLDS to hash the file (an engine rebuild on the host for one mod).

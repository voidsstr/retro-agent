# usbfix / hubinfo — on-box USB diagnosis tools (compile with the box's own csc.exe)

Two single-file C# tools for diagnosing "Unknown Device (code 43)" USB problems
on fleet Windows boxes (Vista/7+, needs .NET 2.0+ which every Win7 box has).
No dev-host toolchain needed: upload the `.cs` via the agent and compile on the
target, e.g.

```
UPLOAD C:\RETRO_AGENT\usbfix\usbfix.cs
EXEC C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe /nologo /out:C:\RETRO_AGENT\usbfix\usbfix.exe C:\RETRO_AGENT\usbfix\usbfix.cs
```

## usbfix.exe — devnode surgery (SetupDi/CM APIs)

```
usbfix status  "USB\VID_0000&PID_0000"   # list matching devnodes + problem code (includes phantoms)
usbfix remove  "USB\VID_0000&PID_0000"   # uninstall matching devnodes
usbfix restart "USB\VID_2109&PID_2817"   # disable -> 2s -> enable (also PCI\ paths, e.g. EHCI controllers)
usbfix rescan                            # CM_Reenumerate_DevNode from the root (like devmgmt "Scan for hardware changes")
```

The prefix's first path segment picks the enumerator (`USB\...`, `PCI\...`).
Quote the argument — `&` must not reach cmd.exe.

## hubinfo.exe — per-port truth from the hub itself (USB IOCTLs)

`hubinfo [instance-id-substring]` opens each present USB hub interface and
prints self/bus-powered plus each port's `ConnectionStatus` from
`IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX`:

- `DeviceCausedOvercurrent` / `DeviceNotEnoughPower` → power problem, not drivers.
- `DeviceFailedEnumeration` with an all-zero device descriptor → the device
  never answered the USB reset handshake. If this persists after
  `usbfix remove` + hub devnode reinstall + cycling the RMH hubs and the EHCI
  PCI controllers, it is **hardware**: physically power-cycle the hub (a
  self-powered hub keeps its wedged state across PC reboots — unplug its DC
  adapter AND uplink for ~10 s), or plug the devices straight into the PC to
  split hub-vs-device.

Note the usbioctl.h structs are pack(1); `ConnectionStatus` sits at byte 31 of
the output buffer (getting this wrong reads the first endpoint descriptor's
`bLength=7` and misreports).

First used 2026-08-18 on 192.168.1.246 (OptiPlex 790, Win7 32-bit): fleetbook
recipe `usb-code-43-diagnosis-usbfix-hubinfo-on-box-c-tools`, change #35.

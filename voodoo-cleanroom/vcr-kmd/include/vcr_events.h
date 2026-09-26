/*
 * vcr_events.h - every event code the flight recorder can carry.
 *
 * ONE table, X-macro form, so the C side and the Python tools cannot drift:
 * tools/vcrlog.py parses the VCR_EVENT(...) lines of THIS file to name codes
 * (tests/python/test_vcr_kmd_tools.py checks it can).
 *
 * Codes are grouped by hundreds: 1xx miniport lifecycle, 2xx hardware
 * discovery, 3xx mode set, 4xx IOCTL, 5xx display DLL, 6xx HWCEXT/Glide,
 * 7xx SLI/multi-chip, 8xx safety, 9xx harness marks. NEVER renumber a code -
 * old dumps must keep decoding - only append.
 *
 *   VCR_EVENT(name, code, "what a b c d mean")
 */
#ifndef VCR_EVENTS_H
#define VCR_EVENTS_H

#define VCR_EVENT_TABLE \
    VCR_EVENT(VCR_EV_TEXT,            0, "free text") \
    /* 1xx miniport lifecycle */ \
    VCR_EVENT(VCR_EV_DRIVER_ENTRY,  100, "a=VideoPortInitialize status") \
    VCR_EVENT(VCR_EV_FIND_ENTER,    101, "a=boot attempt b=max attempts") \
    VCR_EVENT(VCR_EV_FIND_DONE,     102, "a=status b=backend c=chips d=fb bytes per chip") \
    VCR_EVENT(VCR_EV_INIT_ENTER,    103, "") \
    VCR_EVENT(VCR_EV_INIT_DONE,     104, "a=ok") \
    VCR_EVENT(VCR_EV_RESET_HW,      105, "a=columns b=rows") \
    VCR_EVENT(VCR_EV_POWER_GET,     106, "a=child id b=state") \
    VCR_EVENT(VCR_EV_POWER_SET,     107, "a=child id b=state c=result") \
    VCR_EVENT(VCR_EV_TIMER_STABLE,  108, "a=seconds since init (boot marked good)") \
    VCR_EVENT(VCR_EV_CHILD,         109, "a=child index b=result") \
    /* 2xx hardware discovery */ \
    VCR_EVENT(VCR_EV_PCI_ID,        200, "a=vendor b=device c=subsys d=rev") \
    VCR_EVENT(VCR_EV_ACCESS_RANGE,  201, "a=index b=phys lo c=length d=in io space") \
    VCR_EVENT(VCR_EV_MAP,           202, "a=what b=phys lo c=length d=kernel va") \
    VCR_EVENT(VCR_EV_REG_SNAPSHOT,  203, "a=register offset b=value c=chip") \
    VCR_EVENT(VCR_EV_MEMSIZE,       204, "a=dramInit0 b=dramInit1 c=bytes per chip") \
    VCR_EVENT(VCR_EV_CHIP_FOUND,    205, "a=chip b=slot (bus:dev:fn packed) c=bar0 d=bar1") \
    VCR_EVENT(VCR_EV_BRIDGE_FOUND,  206, "a=bus b=slot c=vendor:device") \
    VCR_EVENT(VCR_EV_MODES_BUILT,   207, "a=valid modes b=table size c=max pixclk kHz") \
    VCR_EVENT(VCR_EV_PCI_DECODE,    208, "a=cfgPciDecode/pciInit0 before b=written c=read back d=chip") \
    VCR_EVENT(VCR_EV_DDC,           209, "a=EDID read ok b=vidSerialParallelPort before c=after d=bytes") \
    VCR_EVENT(VCR_EV_CURSOR,        211, "a=on/flags b=pattern address/width c=hwCurLoc/height d=vidProcCfg") \
    VCR_EVENT(VCR_EV_EDID,          210, "a=pnp id (3x5 bits)|product<<16 b=hmin|hmax<<16 kHz c=vmin|vmax<<16 Hz d=max pixclk kHz") \
    /* 3xx mode set */ \
    VCR_EVENT(VCR_EV_MODESET_BEGIN, 300, "a=width b=height c=bpp d=refresh") \
    VCR_EVENT(VCR_EV_MODESET_PLL,   301, "a=target kHz b=achieved kHz c=pllCtrl0 d=2x") \
    VCR_EVENT(VCR_EV_MODESET_REG,   302, "a=register b=value written c=value read back") \
    VCR_EVENT(VCR_EV_MODESET_DONE,  303, "a=vidProcCfg b=vidScreenSize c=stride d=elapsed us") \
    VCR_EVENT(VCR_EV_MODESET_FAIL,  304, "a=mode index b=reason") \
    VCR_EVENT(VCR_EV_PALETTE,       305, "a=first b=count") \
    VCR_EVENT(VCR_EV_VGA_RESTORE,   306, "a=result") \
    VCR_EVENT(VCR_EV_IDLE_WAIT,     307, "a=status b=loops c=timed out") \
    VCR_EVENT(VCR_EV_ENGINE_RESET,  308, "a=status before b=status after c=idle d=chip") \
    /* 4xx IOCTL */ \
    VCR_EVENT(VCR_EV_IOCTL,         400, "a=ioctl b=in len c=out len d=status") \
    VCR_EVENT(VCR_EV_IOCTL_UNKNOWN, 401, "a=ioctl") \
    VCR_EVENT(VCR_EV_USER_MAP,      402, "a=what b=phys lo c=length d=user va") \
    VCR_EVENT(VCR_EV_USER_UNMAP,    403, "a=user va b=status") \
    VCR_EVENT(VCR_EV_PCI_OP,        404, "a=function b=offset c=value d=write") \
    VCR_EVENT(VCR_EV_POKE,          405, "a=offset b=value c=chip d=write") \
    /* 5xx display DLL */ \
    VCR_EVENT(VCR_EV_DD_ENABLE_PDEV,   500, "a=width b=height c=bpp d=refresh") \
    VCR_EVENT(VCR_EV_DD_COMPLETE_PDEV, 501, "") \
    VCR_EVENT(VCR_EV_DD_ENABLE_SURF,   502, "a=lfb va b=stride c=shadow d=ok") \
    VCR_EVENT(VCR_EV_DD_DISABLE_SURF,  503, "") \
    VCR_EVENT(VCR_EV_DD_DISABLE_PDEV,  504, "") \
    VCR_EVENT(VCR_EV_DD_ASSERT_MODE,   505, "a=enable b=result") \
    VCR_EVENT(VCR_EV_DD_GET_MODES,     506, "a=modes b=bytes") \
    VCR_EVENT(VCR_EV_DD_ESCAPE,        507, "a=escape b=in len c=out len d=result") \
    VCR_EVENT(VCR_EV_DD_FAIL,          508, "a=step b=code") \
    VCR_EVENT(VCR_EV_DD_SET_PALETTE,   509, "a=first b=count") \
    VCR_EVENT(VCR_EV_DD_FLUSH_STATS,   510, "a=flushes b=pixels (K)") \
    /* 6xx HWCEXT (Glide) */ \
    VCR_EVENT(VCR_EV_HWC_REQUEST,   600, "a=which b=pid c=resStatus d=return") \
    VCR_EVENT(VCR_EV_HWC_DEVCONFIG, 601, "a=device b=fbRam c=numChips") \
    VCR_EVENT(VCR_EV_HWC_LINADDR,   602, "a=base0 va b=base1 va c=base1 len") \
    VCR_EVENT(VCR_EV_HWC_SLAVE,     603, "a=chip b=io va c=cmd va d=3d va") \
    VCR_EVENT(VCR_EV_HWC_EXCLUSIVE, 604, "a=set b=pid c=result") \
    VCR_EVENT(VCR_EV_HWC_UNMAP,     605, "a=pid b=views") \
    VCR_EVENT(VCR_EV_HWC_CTXDWORD,  606, "a=user va b=pid") \
    VCR_EVENT(VCR_EV_HWC_SLIAA,     607, "a=chips b=sliEn c=aaEn d=nlines") \
    /* 7xx SLI / multi-chip */ \
    VCR_EVENT(VCR_EV_SLI_STEP,      700, "a=step b=chip c=register/offset d=value") \
    VCR_EVENT(VCR_EV_SLI_DONE,      701, "a=enabled b=chips c=result") \
    VCR_EVENT(VCR_EV_CLOCK_6K,      702, "a=target Hz b=programmed Hz c=24-bit word d=gpio after") \
    /* 8xx safety */ \
    VCR_EVENT(VCR_EV_SAFE_DECLINE,  800, "a=reason (1 disabled, 2 boot loop) b=attempts") \
    VCR_EVENT(VCR_EV_SAFE_MARK_OK,  801, "a=attempts cleared") \
    VCR_EVENT(VCR_EV_REG_FLUSH,     802, "a=ntstatus") \
    VCR_EVENT(VCR_EV_TIMEOUT,       803, "a=what b=loops c=status") \
    /* 9xx harness */ \
    VCR_EVENT(VCR_EV_MARK,          900, "a..d = tool-defined") \

#define VCR_EVENT(name, code, desc) name = code,
enum vcr_event_code { VCR_EVENT_TABLE VCR_EV__END };
#undef VCR_EVENT

#endif /* VCR_EVENTS_H */

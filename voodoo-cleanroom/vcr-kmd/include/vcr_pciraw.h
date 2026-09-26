/*
 * vcr_pciraw.h - PCI configuration mechanism #1 (ports 0xCF8/0xCFC), for the
 * one case the HAL refuses: functions 1-7 of a device whose function-0 header
 * type has no multifunction bit. The V5 6000's slave chips are exactly that
 * (bus 3 dev 0 fn 1-3 behind its HiNT bridge; header type 0x00), so
 * HalGetBusDataByOffset reports them absent - measured on .124 - and 3dfx's
 * own miniport used raw cycles for them too.
 *
 * Interrupts are off across each address/data pair so nothing on this CPU
 * can move 0xCF8 between the two. (The HAL takes its own lock; .124 is a
 * single CPU. On SMP this is not sufficient - see vcrmp_hw.c.)
 * Kernel mode only (port I/O).
 */
#ifndef VCR_PCIRAW_H
#define VCR_PCIRAW_H

#define VCR_PCI_CF8(bus, dev, fn, off) \
    (0x80000000u | ((bus) << 16) | (((dev) & 0x1f) << 11) | (((fn) & 7) << 8) | ((off) & 0xfc))

static __inline unsigned long vcr_irq_save(void)
{
    unsigned long f;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(f) : : "memory");
    return f;
}

static __inline void vcr_irq_restore(unsigned long f)
{
    __asm__ __volatile__("pushl %0; popfl" : : "r"(f) : "memory", "cc");
}

static __inline void vcr_outl(unsigned short port, unsigned long v)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

static __inline unsigned long vcr_inl(unsigned short port)
{
    unsigned long v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static __inline unsigned long vcr_pci_raw_read32(unsigned bus, unsigned dev, unsigned fn,
                                                 unsigned off)
{
    unsigned long f = vcr_irq_save(), v;
    vcr_outl(0xcf8, VCR_PCI_CF8(bus, dev, fn, off));
    v = vcr_inl(0xcfc);
    vcr_irq_restore(f);
    return v;
}

static __inline void vcr_pci_raw_write32(unsigned bus, unsigned dev, unsigned fn,
                                         unsigned off, unsigned long v)
{
    unsigned long f = vcr_irq_save();
    vcr_outl(0xcf8, VCR_PCI_CF8(bus, dev, fn, off));
    vcr_outl(0xcfc, v);
    vcr_irq_restore(f);
}

#endif /* VCR_PCIRAW_H */

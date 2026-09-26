/*
 * vcrmp_map.c - mapping the card into the process that asks (Glide).
 *
 * DrvEscape runs on the calling application's thread, and videoprt calls
 * HwStartIO synchronously on that thread, so VideoPortMapMemory with the
 * USER_MODE flag and the NtCurrentProcess() handle maps into the Glide
 * process itself. Each BAR piece is its OWN view: our Glide validates every
 * address it is given with VirtualQuery (AllocationBase must equal the
 * address), so sub-offsets of one big view would be refused.
 *
 * Processes are keyed by (pid, EPROCESS, create time): a pid alone is
 * recycled, and unmapping "the same pid's" old views inside a NEW process
 * would unmap whatever that process has at those addresses.
 */
#include "vcrmp.h"

#define NT_CURRENT_PROCESS ((PVOID)(LONG_PTR)-1)

static VCR_PROC *proc_find(VCR_EXT *x, BOOLEAN create)
{
    ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId(), i;
    PVOID ep = PsGetCurrentProcess();
    LONGLONG created = PsGetProcessCreateTimeQuadPart(ep);
    VCR_PROC *freeslot = NULL;

    for (i = 0; i < VCR_MAX_PROCS; i++) {
        VCR_PROC *p = &x->procs[i];
        if (p->pid == pid && p->eprocess == ep && p->created == created)
            return p;
        if (p->pid == 0 && !freeslot)
            freeslot = p;
    }
    if (!create)
        return NULL;
    if (!freeslot) {
        /* every slot names a process that never unmapped: it died, and its
         * views died with it. Forget the oldest; never unmap it from here. */
        freeslot = &x->procs[0];
        VLOG(VCR_LV_WARN, VCR_EV_USER_UNMAP, freeslot->pid, 0, 0, 0,
             "slot table full - forgetting pid %u", freeslot->pid);
    }
    VideoPortZeroMemory(freeslot, sizeof *freeslot);
    freeslot->pid = pid;
    freeslot->eprocess = ep;
    freeslot->created = created;
    return freeslot;
}

static ULONG map_user(VCR_EXT *x, VCR_PROC *p, PHYSICAL_ADDRESS pa, ULONG len,
                      BOOLEAN wc, ULONG what)
{
    ULONG inio = VIDEO_MEMORY_SPACE_MEMORY | VIDEO_MEMORY_SPACE_USER_MODE |
                 (wc ? VIDEO_MEMORY_SPACE_P6CACHE : 0);
    PVOID va = NT_CURRENT_PROCESS;
    ULONG l = len;
    VP_STATUS st;

    if (!pa.QuadPart || !len || p->nva >= sizeof p->va / sizeof p->va[0])
        return 0;
    st = VideoPortMapMemory(x, pa, &l, &inio, &va);
    VLOG(st == NO_ERROR ? VCR_LV_INFO : VCR_LV_ERROR, VCR_EV_USER_MAP, what, pa.LowPart,
         l, st == NO_ERROR ? (ULONG)(ULONG_PTR)va : (ULONG)st, "map %u into pid %u: %s", what,
         p->pid, st == NO_ERROR ? "ok" : "FAILED");
    if (st != NO_ERROR)
        return 0;
    p->va[p->nva++] = va;
    return (ULONG)(ULONG_PTR)va;
}

VP_STATUS VcrMapGlide(VCR_EXT *x, vcr_glide_map *m)
{
    VCR_PROC *p = proc_find(x, TRUE);
    ULONG c;
    PHYSICAL_ADDRESS pa;

    if (p->nva) {           /* same live process asking again: same answer */
        *m = p->map;
        return NO_ERROR;
    }
    VideoPortZeroMemory(m, sizeof *m);
    m->nchips = 1;          /* multi-chip is exposed once SLI setup exists */
    m->base0 = map_user(x, p, x->chip[0].mmio_phys, VCR_MB0_SIZE, FALSE, 0);
    m->base1_len = x->lfb_len;
    m->base1 = map_user(x, p, x->chip[0].lfb_phys, x->lfb_len, TRUE, 1);
    for (c = 1; c < m->nchips; c++) {
        static const ULONG piece[4] = { VCR_MB0_IOREGS, VCR_MB0_CMDAGP, VCR_MB0_2D, VCR_MB0_3D };
        ULONG k;
        for (k = 0; k < 4; k++) {
            pa.QuadPart = x->chip[c].mmio_phys.QuadPart + piece[k];
            m->slave[c][k] = map_user(x, p, pa, 0x1000, FALSE, 0x10 * c + k);
        }
    }
    m->status = (m->base0 && m->base1) ? 0 : 1;
    p->map = *m;
    return m->status ? ERROR_NOT_ENOUGH_MEMORY : NO_ERROR;
}

VP_STATUS VcrUnmapGlide(VCR_EXT *x, ULONG pid)
{
    VCR_PROC *p = proc_find(x, FALSE);
    ULONG i, n;
    (void)pid;
    if (!p)
        return NO_ERROR;
    n = p->nva;
    for (i = 0; i < n; i++) {
        VP_STATUS st = VideoPortUnmapMemory(x, p->va[i], NT_CURRENT_PROCESS);
        VLOG(st == NO_ERROR ? VCR_LV_DEBUG : VCR_LV_WARN, VCR_EV_USER_UNMAP,
             (ULONG)(ULONG_PTR)p->va[i], st, 0, 0, "unmap");
    }
    VLOG(VCR_LV_INFO, VCR_EV_HWC_UNMAP, p->pid, n, 0, 0, "released %u views", n);
    VideoPortZeroMemory(p, sizeof *p);
    return NO_ERROR;
}

/*
 * vcrdd_pointer.c - the hardware pointer (monochrome, up to 64x64): GDI's
 * shape and moves become the standard pointer IOCTLs, which the miniport turns
 * into the VSA-100 cursor (miniport/vcrmp_cursor.c). A colour or oversized
 * pointer, a miniport that reports no hardware pointer, and any request while
 * a Glide process owns the chip are declined - GDI then draws the pointer in
 * software, which is always correct.
 */
#include "vcrdd.h"

#define PTR_MAX 64

static void hide(VCR_PDEV *pd)
{
    VcrIoctl(pd->hDriver, IOCTL_VIDEO_DISABLE_POINTER, NULL, 0, NULL, 0, NULL);
}

ULONG APIENTRY DrvSetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask, SURFOBJ *psoColor,
                                  XLATEOBJ *pxlo, LONG xHot, LONG yHot, LONG x, LONG y,
                                  RECTL *prcl, FLONG fl)
{
    VCR_PDEV *pd = (VCR_PDEV *)pso->dhpdev;
    struct {
        VIDEO_POINTER_ATTRIBUTES a;
        UCHAR more[2 * PTR_MAX * (PTR_MAX / 8)];
    } buf;
    ULONG w, h, wb, r;
    (void)pxlo;
    (void)prcl;
    (void)fl;

    if (!pd->hw_pointer || psoColor || !psoMask || pd->exclusive_pid)
        goto decline;
    w = (ULONG)psoMask->sizlBitmap.cx;
    h = (ULONG)psoMask->sizlBitmap.cy / 2;
    if (!w || !h || w > PTR_MAX || h > PTR_MAX)
        goto decline;
    wb = (w + 7) / 8;
    memset(&buf, 0, sizeof buf);
    for (r = 0; r < 2 * h; r++)          /* AND rows, then XOR rows */
        memcpy(buf.a.Pixels + r * wb,
               (PUCHAR)psoMask->pvScan0 + (LONG)r * psoMask->lDelta, wb);
    buf.a.Flags = VIDEO_MODE_MONO_POINTER;
    buf.a.Width = w;
    buf.a.Height = h;
    buf.a.WidthInBytes = wb;
    buf.a.Enable = x != -1;
    buf.a.Column = (SHORT)(x - xHot);
    buf.a.Row = (SHORT)(y - yHot);
    if (VcrIoctl(pd->hDriver, IOCTL_VIDEO_SET_POINTER_ATTR, &buf,
                 FIELD_OFFSET(VIDEO_POINTER_ATTRIBUTES, Pixels) + 2 * h * wb, NULL, 0, NULL))
        goto decline;
    pd->xHot = xHot;
    pd->yHot = yHot;
    pd->ptr_on = x != -1;
    return SPS_ACCEPT_NOEXCLUDE;

decline:
    if (pd->ptr_on)
        hide(pd);
    pd->ptr_on = 0;
    return SPS_DECLINE;
}

VOID APIENTRY DrvMovePointer(SURFOBJ *pso, LONG x, LONG y, RECTL *prcl)
{
    VCR_PDEV *pd = (VCR_PDEV *)pso->dhpdev;
    VIDEO_POINTER_POSITION p;
    (void)prcl;
    if (!pd->hw_pointer || pd->exclusive_pid)
        return;
    if (x == -1) {
        hide(pd);
        pd->ptr_on = 0;
        return;
    }
    p.Column = (SHORT)(x - pd->xHot);
    p.Row = (SHORT)(y - pd->yHot);
    VcrIoctl(pd->hDriver, IOCTL_VIDEO_SET_POINTER_POSITION, &p, sizeof p, NULL, 0, NULL);
    pd->ptr_on = 1;
}

/* DrvEnableSurface: does the miniport have a hardware pointer for us? */
void VcrDdPointerProbe(VCR_PDEV *pd)
{
    VIDEO_POINTER_CAPABILITIES c;
    memset(&c, 0, sizeof c);
    pd->hw_pointer = !VcrIoctl(pd->hDriver, IOCTL_VIDEO_QUERY_POINTER_CAPABILITIES, NULL, 0,
                               &c, sizeof c, NULL) &&
                     (c.Flags & VIDEO_MODE_MONO_POINTER) && c.MaxWidth >= 32 &&
                     c.MaxHeight >= 32;
    VcrDd(VCR_LV_INFO, VCR_EV_DD_ENABLE_SURF, pd->hw_pointer, c.MaxWidth, c.MaxHeight, 2,
          "hardware pointer %s", pd->hw_pointer ? "available" : "not available - GDI draws it");
}

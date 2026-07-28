/*
 * crtshim.c - minimal C-runtime shim for the native fxD3D display driver.
 *
 * The fxD3D portable core (ddi_glue.c, d3dhal_tex.c) allocates with the C
 * library (malloc/calloc/free). A native kernel display driver links
 * -nodefaultlib (no CRT); the DDK provides memory through the graphics engine
 * instead. This file bridges the two by implementing the three CRT allocators
 * over EngAllocMem/EngFreeMem so the portable core links unchanged.
 *
 * Kept deliberately tiny and header-isolated (only <stddef.h>) so the __cdecl
 * CRT names here do not collide with the __stdcall (/Gz) default used for the
 * driver's own entry points.
 *
 * Clean-room: uses only the public DDK graphics-engine memory services.
 */
#include <stddef.h>

#ifdef HAVE_DDK

/* Public DDK graphics-engine memory services (winddi.h), declared locally to
 * avoid pulling <windows.h> (and its CRT prototypes) into this shim. */
extern void * __stdcall EngAllocMem(unsigned long fl, unsigned long cj, unsigned long tag);
extern void   __stdcall EngFreeMem(void *pv);

#define FL_ZERO_MEMORY   0x00000001UL
#define CRTSHIM_TAG      0x33465844UL   /* 'DxF3' */

void * __cdecl malloc(size_t cj)
{
    return EngAllocMem(0, (unsigned long)cj, CRTSHIM_TAG);
}

void * __cdecl calloc(size_t num, size_t sz)
{
    return EngAllocMem(FL_ZERO_MEMORY, (unsigned long)(num * sz), CRTSHIM_TAG);
}

void __cdecl free(void *pv)
{
    if (pv != NULL) {
        EngFreeMem(pv);
    }
}

#endif /* HAVE_DDK */

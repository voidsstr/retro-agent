/*
 * d3dhal_state.c - Direct3D render-state / texture-stage-state -> Glide.
 *
 * The core of the translation. Each D3DRENDERSTATE_* or D3DTSS_* is folded into
 * the device shadow (fxd_device), then fxd_flush() pushes the shadow to the
 * Glide backend. Values are the documented D3D constants (mirrored below when
 * building without the DDK). See the mapping table in
 * docs/3dfx-d3d-hal-design.md sec.5.
 *
 * Clean-room: maps public D3D semantics onto open Glide calls.
 */
#include "fxd3d.h"
#include <string.h>

/* --- documented D3D constant subsets we switch on (values are ABI-fixed) --- */
enum { /* D3DRENDERSTATETYPE */
    D3DRS_ZENABLE=7, D3DRS_FILLMODE=8, D3DRS_SHADEMODE=9,
    D3DRS_ZWRITEENABLE=14, D3DRS_ALPHATESTENABLE=15,
    D3DRS_SRCBLEND=19, D3DRS_DESTBLEND=20, D3DRS_CULLMODE=22,
    D3DRS_ZFUNC=23, D3DRS_ALPHAREF=24, D3DRS_ALPHAFUNC=25,
    D3DRS_DITHERENABLE=26, D3DRS_ALPHABLENDENABLE=27, D3DRS_FOGENABLE=28,
    D3DRS_SPECULARENABLE=29, D3DRS_FOGCOLOR=34, D3DRS_FOGTABLEMODE=35,
    D3DRS_FOGDENSITY=38, D3DRS_COLORKEYENABLE=41, D3DRS_TEXTUREFACTOR=60
};
/* Under HAVE_DDK these come from the DX7 DDK's d3dtypes.h; define them only for
 * the DDK-less host build (unit tests). Guarding avoids C2371 redefinitions. */
#ifndef HAVE_DDK
enum { /* D3DCMPFUNC */
    D3DCMP_NEVER=1,D3DCMP_LESS=2,D3DCMP_EQUAL=3,D3DCMP_LESSEQUAL=4,
    D3DCMP_GREATER=5,D3DCMP_NOTEQUAL=6,D3DCMP_GREATEREQUAL=7,D3DCMP_ALWAYS=8
};
enum { /* D3DBLEND */
    D3DBLEND_ZERO=1,D3DBLEND_ONE=2,D3DBLEND_SRCCOLOR=3,D3DBLEND_INVSRCCOLOR=4,
    D3DBLEND_SRCALPHA=5,D3DBLEND_INVSRCALPHA=6,D3DBLEND_DESTALPHA=7,
    D3DBLEND_INVDESTALPHA=8,D3DBLEND_DESTCOLOR=9,D3DBLEND_INVDESTCOLOR=10
};
enum { D3DCULL_NONE=1, D3DCULL_CW=2, D3DCULL_CCW=3 };
enum { D3DSHADE_FLAT=1, D3DSHADE_GOURAUD=2 };
enum { /* D3DTEXTURESTAGESTATETYPE (subset) */
    D3DTSS_COLOROP=1, D3DTSS_ALPHAOP=4, D3DTSS_MAGFILTER=16,
    D3DTSS_MINFILTER=17, D3DTSS_MIPFILTER=18, D3DTSS_ADDRESSU=13, D3DTSS_ADDRESSV=14,
    D3DTSS_TEXTURELODBIAS=19
};
enum { D3DTADDRESS_WRAP=1, D3DTADDRESS_CLAMP=3 };
enum { D3DTFN_POINT=1, D3DTFN_LINEAR=2 };
#else
/* The DX7 DDK spells the LOD-bias texture-stage state D3DTSS_MIPMAPLODBIAS;
 * fxD3D uses the DX6-era name. Map it for the driver build. */
#  define D3DTSS_TEXTURELODBIAS D3DTSS_MIPMAPLODBIAS
#endif /* !HAVE_DDK */

/* --- helpers: D3D enum -> Glide enum -------------------------------------- */

static int cmp2gr(DWORD f){
    switch(f){
        case D3DCMP_NEVER:        return GR_CMP_NEVER;
        case D3DCMP_LESS:         return GR_CMP_LESS;
        case D3DCMP_EQUAL:        return GR_CMP_EQUAL;
        case D3DCMP_LESSEQUAL:    return GR_CMP_LEQUAL;
        case D3DCMP_GREATER:      return GR_CMP_GREATER;
        case D3DCMP_NOTEQUAL:     return GR_CMP_NOTEQUAL;
        case D3DCMP_GREATEREQUAL: return GR_CMP_GEQUAL;
        default:                  return GR_CMP_ALWAYS;
    }
}
static int blend2gr(DWORD b){
    switch(b){
        case D3DBLEND_ZERO:         return GR_BLEND_ZERO;
        case D3DBLEND_ONE:          return GR_BLEND_ONE;
        case D3DBLEND_SRCALPHA:     return GR_BLEND_SRC_ALPHA;
        case D3DBLEND_INVSRCALPHA:  return GR_BLEND_ONE_MINUS_SRC_ALPHA;
        case D3DBLEND_SRCCOLOR:     return GR_BLEND_SRC_COLOR;
        case D3DBLEND_INVSRCCOLOR:  return GR_BLEND_ONE_MINUS_SRC_COLOR;
        case D3DBLEND_DESTALPHA:    return GR_BLEND_DST_ALPHA;
        case D3DBLEND_INVDESTALPHA: return GR_BLEND_ONE_MINUS_DST_ALPHA;
        default:                    return GR_BLEND_ONE; /* nearest fit       */
    }
}

/* --- render state --------------------------------------------------------- */

int fxd_set_renderstate(fxd_device *d, DWORD s, DWORD v){
    switch(s){
        case D3DRS_ZENABLE:        d->z_enable=(v!=0); break;
        case D3DRS_ZWRITEENABLE:   d->z_write=(v!=0);  break;
        case D3DRS_ZFUNC:          d->z_func=cmp2gr(v); break;
        case D3DRS_ALPHABLENDENABLE:d->blend_enable=(v!=0); break;
        case D3DRS_SRCBLEND:       d->src_blend=blend2gr(v); break;
        case D3DRS_DESTBLEND:      d->dst_blend=blend2gr(v); break;
        case D3DRS_ALPHATESTENABLE:d->atest_enable=(v!=0); break;
        case D3DRS_ALPHAFUNC:      d->atest_func=cmp2gr(v); break;
        case D3DRS_ALPHAREF:       d->atest_ref=(v&0xFF); break;
        case D3DRS_FOGENABLE:      d->fog_enable=(v!=0); break;
        case D3DRS_FOGCOLOR:       d->fog_color=(v&0xFFFFFF); break;
        case D3DRS_FOGDENSITY:     { float f; memcpy(&f,&v,sizeof(f)); d->fog_density=f; } break;
        case D3DRS_CULLMODE:
            d->cull=(v==D3DCULL_NONE)?GB_CULL_NONE:
                    (v==D3DCULL_CW)?GB_CULL_CW:GB_CULL_CCW; break;
        case D3DRS_SHADEMODE:      d->shade=(v==D3DSHADE_FLAT)?GB_SHADE_FLAT:GB_SHADE_GOURAUD; break;
        case D3DRS_DITHERENABLE:   d->dither=(v!=0); break;
        case D3DRS_COLORKEYENABLE: d->ckey_enable=(v!=0); break;
        case D3DRS_TEXTUREFACTOR:  d->tex_factor=v; break;
        case D3DRS_FILLMODE: case D3DRS_SPECULARENABLE:
        case D3DRS_FOGTABLEMODE:   break; /* handled at draw/flush            */
        default: return 0; /* unrecognized state - runtime tolerates          */
    }
    d->dirty=1;
    return 1;
}

int fxd_set_tss(fxd_device *d, DWORD stage, DWORD s, DWORD v){
    if(stage!=0) return 1; /* v1: single TMU stage; stage1 folded later       */
    switch(s){
        case D3DTSS_COLOROP: d->color_op=(int)v; break;
        case D3DTSS_ALPHAOP: d->alpha_op=(int)v; break;
        case D3DTSS_MAGFILTER: d->mag_filter=(v==D3DTFN_LINEAR); break;
        case D3DTSS_MINFILTER: d->min_filter=(v==D3DTFN_LINEAR); break;
        case D3DTSS_MIPFILTER: break;
        case D3DTSS_ADDRESSU: d->addr_u=(v==D3DTADDRESS_CLAMP); break;
        case D3DTSS_ADDRESSV: d->addr_v=(v==D3DTADDRESS_CLAMP); break;
        case D3DTSS_TEXTURELODBIAS: break;
        default: return 0;
    }
    d->dirty=1;
    return 1;
}

/* --- flush shadow -> backend ---------------------------------------------- */

void fxd_flush(fxd_device *d){
    if(!d->dirty) return;
    gb_set_shade(d->shade);
    gb_set_depth(d->z_enable, d->z_func, d->z_write);
    gb_set_blend(d->blend_enable, d->src_blend, d->dst_blend);
    gb_set_alphatest(d->atest_enable, d->atest_func, (unsigned char)d->atest_ref);
    gb_set_fog(d->fog_enable, d->fog_color, d->fog_density>0?d->fog_density:3.0f);
    gb_set_cull(d->cull);
    gb_set_dither(d->dither);
    gb_set_chromakey(d->ckey_enable, d->ckey);
    gb_set_texfactor(d->tex_factor);
    if(d->bound_tex) gb_set_texfilter(d->mag_filter, d->addr_u, d->addr_v);
    d->dirty=0;
}

void fxd_device_init(fxd_device *d, int w, int h, int depth){
    /* D3D default render state -> shadow */
    d->z_enable=1; d->z_func=GR_CMP_LEQUAL; d->z_write=1;
    d->blend_enable=0; d->src_blend=GR_BLEND_ONE; d->dst_blend=GR_BLEND_ZERO;
    d->atest_enable=0; d->atest_func=GR_CMP_ALWAYS; d->atest_ref=0;
    d->fog_enable=0; d->fog_color=0x808080; d->fog_density=3.0f;
    d->cull=GB_CULL_CCW; d->shade=GB_SHADE_GOURAUD; d->dither=1;
    d->ckey_enable=0; d->ckey=0; d->tex_factor=0xFFFFFFFF;
    d->color_op=0; d->alpha_op=0; d->min_filter=1; d->mag_filter=1;
    d->addr_u=0; d->addr_v=0; d->bound_tex=0;
    d->width=w; d->height=h; d->depth=depth; d->dirty=1;
}

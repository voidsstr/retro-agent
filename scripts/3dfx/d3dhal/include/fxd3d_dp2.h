/*
 * fxd3d_dp2.h - REAL DirectX 7 DrawPrimitives2 stream shapes (M4a).
 *
 * Fixed-width mirrors of the documented DP2 command stream: the 4-byte
 * D3DHAL_DP2COMMAND header, the D3DDP2OP_* opcode values, the per-op operand
 * records and the FVF vertex-format bits. All fx-prefixed and defined
 * UNCONDITIONALLY (they never clash with the DDK's D3DHAL_DP2 / D3DDP2OP
 * names), so host gcc, mingw and VC6 /Zp8 all compile the identical layout;
 * the compile-time asserts below pin every size. The stream itself has NO
 * alignment guarantee (commands and operands are packed back-to-back), so
 * consumers must read it via memcpy/byte composition, never by casting.
 *
 * ENDIANNESS: the byte-composed rd_u16/rd_u32/rd_f32 helpers are endian-
 * portable; the ONE native-endian read is the fxdp2_cmd overlay (memcpy of
 * BYTE,BYTE,WORD - the WORD is native-endian), which is correct on
 * little-endian x86 - the only target this driver runs on.
 *
 * Clean-room: shapes and values are the public Microsoft DX7 DDK contract
 * (d3dhal.h / d3dtypes.h); see docs/3dfx-d3d-hal-design.md.
 */
#ifndef FXD3D_DP2_H
#define FXD3D_DP2_H

#include "fxd3d.h"
#include <stddef.h>   /* offsetof (layout pins below) */

/* ---- the real DP2 command header (D3DHAL_DP2COMMAND, 4 bytes) ------------ */

typedef struct {
    BYTE bCommand;      /* D3DDP2OP_* opcode                                  */
    BYTE bReserved;
    WORD wCount;        /* wPrimitiveCount / wStateCount (union in the DDI)   */
} fxdp2_cmd;

/* ---- D3DDP2OP_* opcodes (complete documented enum) ----------------------- */

enum {
    FXDP2OP_POINTS               = 1,
    FXDP2OP_INDEXEDLINELIST      = 2,
    FXDP2OP_INDEXEDTRIANGLELIST  = 3,
    FXDP2OP_RENDERSTATE          = 8,
    FXDP2OP_LINELIST             = 15,
    FXDP2OP_LINESTRIP            = 16,
    FXDP2OP_INDEXEDLINESTRIP     = 17,
    FXDP2OP_TRIANGLELIST         = 18,
    FXDP2OP_TRIANGLESTRIP        = 19,
    FXDP2OP_INDEXEDTRIANGLESTRIP = 20,
    FXDP2OP_TRIANGLEFAN          = 21,
    FXDP2OP_INDEXEDTRIANGLEFAN   = 22,
    FXDP2OP_TRIANGLEFAN_IMM      = 23,
    FXDP2OP_LINELIST_IMM         = 24,
    FXDP2OP_TEXTURESTAGESTATE    = 25,
    FXDP2OP_INDEXEDTRIANGLELIST2 = 26,
    FXDP2OP_INDEXEDLINELIST2     = 27,
    FXDP2OP_VIEWPORTINFO         = 28,
    FXDP2OP_WINFO                = 29,
    FXDP2OP_SETPALETTE           = 30,
    FXDP2OP_UPDATEPALETTE        = 31,
    FXDP2OP_ZRANGE               = 32,
    FXDP2OP_SETMATERIAL          = 33,
    FXDP2OP_SETLIGHT             = 34,
    FXDP2OP_CREATELIGHT          = 35,
    FXDP2OP_SETTRANSFORM         = 36,
    FXDP2OP_EXT                  = 37,
    FXDP2OP_TEXBLT               = 38,
    FXDP2OP_STATESET             = 39,
    FXDP2OP_SETPRIORITY          = 40,
    FXDP2OP_SETRENDERTARGET      = 41,
    FXDP2OP_CLEAR                = 42,
    FXDP2OP_SETTEXLOD            = 43,
    FXDP2OP_SETCLIPPLANE         = 44
};

/* ---- per-op operand records (exact DDI shapes, naturally packed) ---------- */

typedef struct { DWORD dwState; DWORD dwValue; }             fxdp2_renderstate; /* 8 */
typedef struct { WORD wStage; WORD wTSState; DWORD dwValue; } fxdp2_tss;        /* 8 */
/* {WORD wVStart}: LINELIST/TRIANGLELIST/STRIP/FAN operand AND the
 * DP2STARTVERTEX prefix of the indexed "2"/strip/fan ops. */
typedef struct { WORD wVStart; }                             fxdp2_vstart;      /* 2 */
typedef struct { WORD wCount; WORD wVStart; }                fxdp2_points;      /* 4 */
typedef struct { WORD wV1, wV2, wV3, wFlags; }               fxdp2_indexedtri;  /* 8 */
typedef struct { WORD wV1, wV2, wV3; }                       fxdp2_indexedtri2; /* 6 */
typedef struct { DWORD dwPaletteHandle; WORD wStartIndex; WORD wNumEntries; }
                                                             fxdp2_updatepalette; /* 8 */

/* compile-time layout pins: host gcc, mingw and VC6 /Zp8 must all agree */
typedef char fxdp2_sz_cmd [(sizeof(fxdp2_cmd)==4)?1:-1];
typedef char fxdp2_sz_rs  [(sizeof(fxdp2_renderstate)==8)?1:-1];
typedef char fxdp2_sz_tss [(sizeof(fxdp2_tss)==8)?1:-1];
typedef char fxdp2_sz_vs  [(sizeof(fxdp2_vstart)==2)?1:-1];
typedef char fxdp2_sz_pts [(sizeof(fxdp2_points)==4)?1:-1];
typedef char fxdp2_sz_it  [(sizeof(fxdp2_indexedtri)==8)?1:-1];
typedef char fxdp2_sz_it2 [(sizeof(fxdp2_indexedtri2)==6)?1:-1];
typedef char fxdp2_sz_upal[(sizeof(fxdp2_updatepalette)==8)?1:-1];
typedef char fxdp2_sz_tlv [(sizeof(fxd_tlvertex)==32)?1:-1];
/* fxd_tlvertex is passed BY STRUCT to fxd_draw, so pin the field offsets too
 * (not just the total size) - makes the gcc/mingw/VC6-/Zp8 layout equivalence
 * self-checking where it matters (the 4x float prefix + the packed DWORDs). */
typedef char fxdp2_off_col[(offsetof(fxd_tlvertex,color)==16)?1:-1];
typedef char fxdp2_off_tu [(offsetof(fxd_tlvertex,tu)==24)?1:-1];

/* ---- render-state mirror bound (D3DHAL_MAX_RSTATES) ---------------------- *
 * The runtime's lpdwRStates array is exactly D3DHAL_MAX_RSTATES DWORDs =
 * D3DRENDERSTATE_WRAPBIAS(128) + 128 = 256 - the size the DDK reference
 * rasterizer allocates AND the bound it enforces (primfns.cpp:
 * "if (dwState >= D3DHAL_MAX_RSTATES) ..."). Render-state indices >= 256 are
 * OVERRIDE tokens (D3DSTATE_OVERRIDE_BIAS = 256), control markers - NOT array
 * slots. Mirroring them would be an out-of-bounds store into a caller array
 * whose length we are never told. Bound every mirror write by this.
 * Clean-room: DDK d3dhal.h:55 / d3dtypes.h:1245,1024. */
#define FXD_MAX_RSTATES 256u

/* ---- FVF vertex-format bits (D3DFVF_*) ----------------------------------- */

#define FXDP2FVF_RESERVED0      0x001u  /* execute-buffer only; rejected      */
#define FXDP2FVF_POSITION_MASK  0x00Eu
#define FXDP2FVF_XYZ            0x002u  /* 3 floats (untransformed; rejected) */
#define FXDP2FVF_XYZRHW         0x004u  /* 4 floats - the TL path we require  */
#define FXDP2FVF_NORMAL         0x010u  /* 3 floats (invalid with RHW)        */
#define FXDP2FVF_RESERVED1      0x020u  /* 1 DWORD placeholder (LVERTEX)      */
#define FXDP2FVF_DIFFUSE        0x040u  /* D3DCOLOR                           */
#define FXDP2FVF_SPECULAR       0x080u  /* D3DCOLOR                           */
#define FXDP2FVF_TEXCOUNT_MASK  0xF00u
#define FXDP2FVF_TEXCOUNT_SHIFT 8
#define FXDP2FVF_RESERVED2      0xF000u /* must be 0                          */
/* texcoord-set size codes at bit 16+2i: 0->2, 1->3, 2->4, 3->1 floats        */

/* ---- translator error codes ---------------------------------------------- */

#define FXD_DP2E_UNKNOWN_OP (-1) /* unrecognized bCommand -> COMMAND_UNPARSED */
#define FXD_DP2E_MALFORMED  (-2) /* truncated stream / vertex ref out of range*/
#define FXD_DP2E_BADFVF     (-3) /* dwVertexType we cannot fetch (need XYZRHW)*/

/* Optional in-driver "parse unknown command" hook. Mirrors the DDK's
 * PFND3DPARSEUNKNOWNCOMMAND contract (obtained via GetDriverInfo /
 * GUID_D3DParseUnknownCommandCallback): when the translator meets an opcode it
 * does not implement, it calls this so the RUNTIME parses/skips that one
 * command and hands back where to resume - instead of aborting the whole batch.
 *   ctx      - opaque pass-through (the driver's adapter state)
 *   cmds     - the command-buffer base
 *   off      - byte offset (within cmds) of the unknown DP2COMMAND header
 *   cmd_len  - total command-buffer length
 *   next_off - out: byte offset of the NEXT command to resume at
 * Return 0 on success (with *next_off in (off, cmd_len]); non-zero if the
 * runtime cannot parse it either (the walk then stops with FXD_DP2E_UNKNOWN_OP,
 * i.e. D3DERR_COMMAND_UNPARSED at *err_off). */
typedef int (*fxd_parse_unknown_fn)(void *ctx, const void *cmds, DWORD off,
                                    DWORD cmd_len, DWORD *next_off);

/* Execute a REAL DX7 DP2 command stream against the fxD3D core.
 *   cmds/cmd_len - command-buffer bytes (surface bits + dwCommandOffset)
 *   verts        - vertex 0 (user pointer or VB surface bits, + dwVertexOffset)
 *   vert_count   - NUMBER OF VERTICES available (dwVertexLength semantics)
 *   fvf          - dwVertexType FVF code (XYZRHW required)
 *   rstates      - lpdwRStates mirror array (>= FXD_MAX_RSTATES=256 DWORDs), or 0
 *   err_off      - out: byte offset (within cmds) of the offending
 *                  DP2COMMAND when a negative code is returned
 *   parse/ctx    - optional unknown-command hook (see above); pass 0 to make an
 *                  unknown opcode stop the walk (FXD_DP2E_UNKNOWN_OP)
 * Returns #commands executed (>=0), or a negative FXD_DP2E_* code. */
int fxd_dp2_execute_real_cb(fxd_device *dev, const void *cmds, DWORD cmd_len,
                            const void *verts, DWORD vert_count, DWORD fvf,
                            DWORD *rstates, DWORD *err_off,
                            fxd_parse_unknown_fn parse, void *parse_ctx);

/* Back-compat wrapper: no unknown-command hook (unknown opcode -> stop). */
int fxd_dp2_execute_real(fxd_device *dev, const void *cmds, DWORD cmd_len,
                         const void *verts, DWORD vert_count, DWORD fvf,
                         DWORD *rstates, DWORD *err_off);

/* Byte stride of one FVF vertex under the same XYZRHW-required rule the
 * walk applies; 0 for a code the walk would reject (FXD_DP2E_BADFVF). The
 * DDI caller uses it to clamp dwVertexOffset + dwVertexLength*stride to the
 * vertex surface's byte size BEFORE executing (review #4 - vfetch bounds
 * indices against vert_count, not against the buffer's byte size). */
DWORD fxd_dp2_fvf_stride(DWORD fvf);

#endif /* FXD3D_DP2_H */

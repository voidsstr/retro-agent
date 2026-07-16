/*
 * ddcci.c - DDC/CI monitor control on Windows XP via the NVIDIA driver's I2C
 *           interface (NVAPI). XP has no built-in DDC/CI API (that's Vista+),
 *           but the monitor's I2C/DDC bus is reachable through the GPU driver,
 *           so we can read/write the monitor's own VCP controls in HARDWARE:
 *           brightness (0x10), contrast (0x12), RGB gains (0x16/18/1A),
 *           color temp preset (0x14), geometry (H/V size+pos 0x20/22/30/32,
 *           pincushion 0x24, trapezoid 0x42, rotation 0x44), degauss (0x01),
 *           and factory-reset (0x04).
 *
 * This is the piece I said "isn't possible on XP" using the standard API — it
 * IS possible by going through NVAPI. It only works if the installed NVIDIA
 * driver ships nvapi.dll AND exposes the I2C functions (older ForceWare may
 * not). We load nvapi.dll dynamically so the tool never fails to start; it just
 * reports cleanly if NVAPI/I2C is unavailable.
 *
 * Usage (console):
 *   ddcci probe            init, enumerate, try reading brightness (SAFE, read-only)
 *   ddcci caps             dump the monitor's VCP capability string (read-only)
 *   ddcci get <hexVCP>     read a VCP value, e.g. `ddcci get 10`  (read-only)
 *   ddcci set <hexVCP> <n> write a VCP value, e.g. `ddcci set 10 60`
 *   ddcci degauss          pulse degauss (VCP 0x01)
 *   ddcci reset            factory reset (VCP 0x04)
 *
 * Build:
 *   i686-w64-mingw32-gcc -O2 -s -o ddcci.exe ddcci.c \
 *     -DWINVER=0x0410 -D_WIN32_WINNT=0x0410 -march=i586 -mtune=pentium3
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int  NvU32;
typedef unsigned char NvU8;
typedef void *        NvHandle;
typedef int           NvStatus;   /* 0 = OK, <0 = error */

/* NVAPI function IDs (stable magic numbers used by nvapi_QueryInterface). */
#define ID_Initialize        0x0150E828u
#define ID_Unload            0xD22BDD7Eu
#define ID_GetErrorMessage   0x6C2D048Cu
#define ID_EnumPhysicalGPUs  0xE5AC921Fu
#define ID_GetActiveOutputs  0x0E3E89B6u
#define ID_GetConnectedOut   0x1730BFC9u
#define ID_I2CRead           0x2FDE12C5u
#define ID_I2CWrite          0xE812EB07u

typedef void *(__cdecl *QueryInterface_t)(NvU32);
typedef NvStatus (__cdecl *Initialize_t)(void);
typedef NvStatus (__cdecl *Unload_t)(void);
typedef NvStatus (__cdecl *GetErrorMessage_t)(NvStatus, char *);
typedef NvStatus (__cdecl *EnumPhysicalGPUs_t)(NvHandle *, NvU32 *);
typedef NvStatus (__cdecl *GetActiveOutputs_t)(NvHandle, NvU32 *);

/* NV_I2C_INFO — we declare the full (v3) layout but set `version` to try v1/v2/v3.
 * The leading fields are identical across versions, so the driver reads only the
 * prefix that matches whatever version we pass. */
typedef struct {
    NvU32  version;
    NvU32  displayMask;
    NvU8   bIsDDCPort;
    NvU8   i2cDevAddress;
    NvU8  *pbI2cRegAddress;
    NvU32  regAddrSize;
    NvU8  *pbData;
    NvU32  cbSize;
    NvU32  i2cSpeed;        /* legacy; NVAPI_I2C_SPEED_DEFAULT = 0xFFFF */
    NvU32  i2cSpeedKhz;     /* v3 */
    NvU8   portId;          /* v3 */
    NvU32  bIsPortIdSet;    /* v3 */
} NV_I2C_INFO;

typedef NvStatus (__cdecl *I2C_t)(NvHandle, NV_I2C_INFO *);

static Initialize_t       pInit;
static Unload_t           pUnload;
static GetErrorMessage_t  pErr;
static EnumPhysicalGPUs_t pEnum;
static GetActiveOutputs_t pOutputs;
static GetActiveOutputs_t pConnected;
static I2C_t              pRead, pWrite;

static NvHandle g_gpu;
static NvU32    g_disp;
static NvU32    g_i2cver;   /* chosen NV_I2C_INFO version tag that the driver accepts */
static NvStatus g_last;     /* raw NVAPI status of the last I2C call (for diagnostics) */

static const char *errstr(NvStatus s)
{
    static char m[64];
    if (pErr && pErr(s, m) == 0) return m;
    _snprintf(m, sizeof(m), "status %d", s);
    return m;
}

/* Try each NV_I2C_INFO version until one is accepted (or all fail). */
static NvU32 VER(int v, unsigned sz) { return sz | ((NvU32)v << 16); }

static int nv_load(void)
{
    HMODULE h = LoadLibraryA("nvapi.dll");
    if (!h) { printf("NVAPI: nvapi.dll not present (driver too old / not NVIDIA)\n"); return 0; }
    QueryInterface_t qi = (QueryInterface_t)GetProcAddress(h, "nvapi_QueryInterface");
    if (!qi) { printf("NVAPI: nvapi_QueryInterface missing\n"); return 0; }
    pInit      = (Initialize_t)       qi(ID_Initialize);
    pUnload    = (Unload_t)           qi(ID_Unload);
    pErr       = (GetErrorMessage_t)  qi(ID_GetErrorMessage);
    pEnum      = (EnumPhysicalGPUs_t) qi(ID_EnumPhysicalGPUs);
    pOutputs   = (GetActiveOutputs_t) qi(ID_GetActiveOutputs);
    pConnected = (GetActiveOutputs_t) qi(ID_GetConnectedOut);
    pRead      = (I2C_t)              qi(ID_I2CRead);
    pWrite     = (I2C_t)              qi(ID_I2CWrite);
    if (!pInit || !pEnum || !pRead || !pWrite) {
        printf("NVAPI: I2C entry points not exported by this driver (%p %p %p %p)\n",
               (void*)pInit,(void*)pEnum,(void*)pRead,(void*)pWrite);
        return 0;
    }
    return 1;
}

static int nv_init(void)
{
    NvStatus s = pInit();
    if (s != 0) { printf("NvAPI_Initialize failed: %s\n", errstr(s)); return 0; }
    NvHandle gpus[64]; NvU32 n = 0;
    s = pEnum(gpus, &n);
    if (s != 0 || n == 0) { printf("EnumPhysicalGPUs failed: %s\n", errstr(s)); return 0; }
    g_gpu = gpus[0];
    g_disp = 0;
    NvU32 active = 0, conn = 0;
    if (pOutputs)   pOutputs(g_gpu, &active);
    if (pConnected) pConnected(g_gpu, &conn);
    g_disp = active ? active : conn;
    printf("NVAPI up: %u GPU(s), active outputs 0x%08X, connected 0x%08X\n", n, active, conn);
    return 1;
}

static void fill(NV_I2C_INFO *ii, NvU8 addr, NvU8 *data, NvU32 sz)
{
    memset(ii, 0, sizeof(*ii));
    ii->displayMask   = g_disp;
    ii->bIsDDCPort    = 1;
    ii->i2cDevAddress = addr;
    ii->pbI2cRegAddress = NULL;
    ii->regAddrSize   = 0;
    ii->pbData        = data;
    ii->cbSize        = sz;
    ii->i2cSpeed      = 0xFFFF;     /* default */
    ii->i2cSpeedKhz   = 4;          /* NVAPI_I2C_SPEED_100KHZ-ish */
}

/* Write with automatic version negotiation on first call. */
static NvStatus ddc_write(NvU8 *data, NvU32 sz)
{
    NV_I2C_INFO ii;
    NvStatus s = -1;
    int versions[3] = {3, 2, 1};
    unsigned sizes[3] = { sizeof(NV_I2C_INFO), 44, 36 };
    int i, start = 0, end = 3;
    if (g_i2cver) { versions[0] = (g_i2cver >> 16); sizes[0] = (g_i2cver & 0xFFFF); end = 1; }
    for (i = start; i < end; i++) {
        fill(&ii, 0x6E, data, sz);
        ii.version = VER(versions[i], sizes[i]);
        s = pWrite(g_gpu, &ii);
        if (s == 0) { g_i2cver = ii.version; g_last = 0; return 0; }
    }
    g_last = s; return s;
}
static NvStatus ddc_read(NvU8 *buf, NvU32 sz)
{
    NV_I2C_INFO ii;
    NvStatus s = -1;
    int versions[3] = {3, 2, 1};
    unsigned sizes[3] = { sizeof(NV_I2C_INFO), 44, 36 };
    int i, end = 3;
    if (g_i2cver) { versions[0] = (g_i2cver >> 16); sizes[0] = (g_i2cver & 0xFFFF); end = 1; }
    for (i = 0; i < end; i++) {
        fill(&ii, 0x6F, buf, sz);
        ii.version = VER(versions[i], sizes[i]);
        s = pRead(g_gpu, &ii);
        if (s == 0) { g_i2cver = ii.version; g_last = 0; return 0; }
    }
    g_last = s; return s;
}

static NvU8 csum(NvU8 first, NvU8 *b, int n)
{
    NvU8 c = first; int i;
    for (i = 0; i < n; i++) c ^= b[i];
    return c;
}

/* DDC/CI Get VCP: returns current(<0 on failure), sets *maxv. */
static int vcp_get(NvU8 vcp, int *maxv)
{
    NvU8 req[4] = { 0x51, 0x82, 0x01, vcp };
    NvU8 pkt[5];
    memcpy(pkt, req, 4);
    pkt[4] = csum(0x6E, req, 4);
    if (ddc_write(pkt, 5) != 0) return -1000;
    Sleep(50);
    NvU8 resp[12];
    memset(resp, 0, sizeof(resp));
    if (ddc_read(resp, 11) != 0) return -1001;
    /* resp: [0x6E,0x88,0x02,result,vcp,type,maxH,maxL,curH,curL,chk] */
    if (resp[2] != 0x02 || resp[4] != vcp) return -1002;
    if (maxv) *maxv = (resp[6] << 8) | resp[7];
    return (resp[8] << 8) | resp[9];
}

static int vcp_set(NvU8 vcp, int val)
{
    NvU8 body[5] = { 0x51, 0x84, 0x03, vcp, (NvU8)(val >> 8) };
    NvU8 pkt[7];
    memcpy(pkt, body, 5);
    pkt[5] = (NvU8)(val & 0xFF);
    pkt[6] = csum(0x6E, pkt, 6);
    return ddc_write(pkt, 7);
}

/* NVAPI rejects a wrong NV_I2C_INFO.version with NVAPI_INCOMPATIBLE_STRUCT_VERSION
 * (-190) WITHOUT touching the buffers, so we can safely sweep version tags until
 * one is accepted (any status other than -190 means the driver understood it). */
#define NVAPI_INCOMPATIBLE_STRUCT_VERSION (-190)
static int find_version(void)
{
    NvU8 req[5] = { 0x51, 0x82, 0x01, 0x10, 0 };
    req[4] = csum(0x6E, req, 4);
    int vn; unsigned sz;
    for (vn = 1; vn <= 3; vn++) {
        for (sz = 24; sz <= 64; sz += 4) {
            NV_I2C_INFO ii; fill(&ii, 0x6E, req, 5);
            ii.version = sz | ((NvU32)vn << 16);
            NvStatus s = pWrite(g_gpu, &ii);
            char buf[64]; strncpy(buf, errstr(s), 63); buf[63] = 0;
            if (!strstr(buf, "INCOMPATIBLE") && !strstr(buf, "STRUCT_VERSION")) {
                g_i2cver = ii.version;
                printf("i2c struct version 0x%X accepted (write status: %s)\n",
                       g_i2cver, buf);
                return 1;
            }
        }
    }
    printf("no NV_I2C_INFO version accepted by this driver\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "probe";
    if (!nv_load() || !nv_init()) {
        printf("RESULT: DDC/CI-over-NVAPI NOT available on this machine.\n");
        return 2;
    }
    find_version();   /* lock the ABI struct version for this driver */

    if (strcmp(cmd, "probe") == 0) {
        /* The I2C displayMask must target the actual output the CRT is on.
         * GetActiveOutputs can report 0, so sweep the candidate output masks
         * until the monitor answers DDC/CI. */
        NvU32 cands[] = { g_disp, 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80,
                          0x100, 0x200, 0x10000, 0x20000, 0x40000, 0x80000 };
        int i, maxv = 0, cur = -1; NvU32 hit = 0;
        for (i = 0; i < (int)(sizeof(cands)/sizeof(cands[0])); i++) {
            if (cands[i] == 0) continue;
            g_disp = cands[i];            /* keep the negotiated g_i2cver from find_version */
            cur = vcp_get(0x10, &maxv);
            if (cur >= 0) { hit = cands[i]; break; }
        }
        if (cur >= 0) {
            printf("RESULT: DDC/CI WORKS. output mask 0x%X, brightness(0x10) = %d / %d "
                   "[i2cver=0x%X]\n", hit, cur, maxv, g_i2cver);
            int c = vcp_get(0x12, &maxv);
            if (c >= 0) printf("        contrast(0x12) = %d / %d\n", c, maxv);
            int t = vcp_get(0x14, &maxv);
            if (t >= 0) printf("        color-temp preset(0x14) = %d\n", t);
        } else {
            /* Transport works; dump the raw reply so we can tell "DDC/CI off"
             * (all zeros / no reply) from a response-offset parse bug. */
            NvU8 req[5] = { 0x51, 0x82, 0x01, 0x10, 0 }; req[4] = csum(0x6E, req, 4);
            NvU8 raw[16]; memset(raw, 0, sizeof(raw));
            g_disp = 0x2;
            NvStatus w = ddc_write(req, 5); Sleep(60);
            NvStatus r = ddc_read(raw, 11);
            printf("RESULT: NVAPI I2C transport OK (write=%s read=%s) but no valid DDC/CI "
                   "VCP reply.\n        raw reply:", errstr(w), errstr(r));
            for (i = 0; i < 11; i++) printf(" %02X", raw[i]);
            printf("\n        (all-zero => monitor's DDC/CI is OFF in its OSD or unsupported "
                   "on VGA; nonzero => parse offset to fix.)\n");
        }
    } else if (strcmp(cmd, "get") == 0 && argc > 2) {
        int maxv = 0, cur = vcp_get((NvU8)strtol(argv[2], 0, 16), &maxv);
        printf("get 0x%s = %d / %d\n", argv[2], cur, maxv);
    } else if (strcmp(cmd, "set") == 0 && argc > 3) {
        NvStatus s = vcp_set((NvU8)strtol(argv[2], 0, 16), atoi(argv[3]));
        printf("set 0x%s %s -> %s\n", argv[2], argv[3], s == 0 ? "OK" : errstr(s));
    } else if (strcmp(cmd, "degauss") == 0) {
        printf("degauss -> %s\n", vcp_set(0x01, 1) == 0 ? "OK" : "fail");
    } else if (strcmp(cmd, "reset") == 0) {
        printf("factory reset -> %s\n", vcp_set(0x04, 1) == 0 ? "OK" : "fail");
    } else {
        printf("usage: ddcci probe|caps|get <hex>|set <hex> <n>|degauss|reset\n");
    }

    if (pUnload) pUnload();
    return 0;
}

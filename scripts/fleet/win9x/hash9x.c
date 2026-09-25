/* hash9x - MD5 and size of each file named on the command line, one line per
 * file, to C:\RETRO_AGENT\HASH9X.TXT:
 *
 *     <md5 hex>  <bytes>  <path>          or      ERROR <GetLastError>  <path>
 *
 * Why it exists (.243, 2026-09-24): the agent's DOWNLOAD reads a whole file
 * into one heap buffer, so proving that a 20-80 MB game pak arrived intact on
 * a 127 MB Win98 box means shipping the bytes back through that buffer. This
 * reads 64 KB at a time and sends back only the hash. It is what showed that
 * agent 1.84.2's resume-after-a-failed-read wrote Hexen II's paks correctly.
 *
 * No C runtime (see README.md): -nostdlib, static buffers, own memset/memcpy. */
#include <windows.h>

void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

typedef struct { DWORD s[4]; DWORD lo, hi; unsigned char buf[64]; } md5_t;

#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define STEP(f, a, b, c, d, x, t, s) \
    (a) += f((b), (c), (d)) + (x) + (t); (a) = ((a) << (s)) | ((a) >> (32 - (s))); (a) += (b);

static void md5_block(md5_t *m, const unsigned char *p)
{
    DWORD a = m->s[0], b = m->s[1], c = m->s[2], d = m->s[3], x[16];
    int i;
    for (i = 0; i < 16; i++)
        x[i] = (DWORD)p[i * 4] | ((DWORD)p[i * 4 + 1] << 8) | ((DWORD)p[i * 4 + 2] << 16) | ((DWORD)p[i * 4 + 3] << 24);
    STEP(F, a, b, c, d, x[0], 0xd76aa478, 7)   STEP(F, d, a, b, c, x[1], 0xe8c7b756, 12)
    STEP(F, c, d, a, b, x[2], 0x242070db, 17)  STEP(F, b, c, d, a, x[3], 0xc1bdceee, 22)
    STEP(F, a, b, c, d, x[4], 0xf57c0faf, 7)   STEP(F, d, a, b, c, x[5], 0x4787c62a, 12)
    STEP(F, c, d, a, b, x[6], 0xa8304613, 17)  STEP(F, b, c, d, a, x[7], 0xfd469501, 22)
    STEP(F, a, b, c, d, x[8], 0x698098d8, 7)   STEP(F, d, a, b, c, x[9], 0x8b44f7af, 12)
    STEP(F, c, d, a, b, x[10], 0xffff5bb1, 17) STEP(F, b, c, d, a, x[11], 0x895cd7be, 22)
    STEP(F, a, b, c, d, x[12], 0x6b901122, 7)  STEP(F, d, a, b, c, x[13], 0xfd987193, 12)
    STEP(F, c, d, a, b, x[14], 0xa679438e, 17) STEP(F, b, c, d, a, x[15], 0x49b40821, 22)
    STEP(G, a, b, c, d, x[1], 0xf61e2562, 5)   STEP(G, d, a, b, c, x[6], 0xc040b340, 9)
    STEP(G, c, d, a, b, x[11], 0x265e5a51, 14) STEP(G, b, c, d, a, x[0], 0xe9b6c7aa, 20)
    STEP(G, a, b, c, d, x[5], 0xd62f105d, 5)   STEP(G, d, a, b, c, x[10], 0x02441453, 9)
    STEP(G, c, d, a, b, x[15], 0xd8a1e681, 14) STEP(G, b, c, d, a, x[4], 0xe7d3fbc8, 20)
    STEP(G, a, b, c, d, x[9], 0x21e1cde6, 5)   STEP(G, d, a, b, c, x[14], 0xc33707d6, 9)
    STEP(G, c, d, a, b, x[3], 0xf4d50d87, 14)  STEP(G, b, c, d, a, x[8], 0x455a14ed, 20)
    STEP(G, a, b, c, d, x[13], 0xa9e3e905, 5)  STEP(G, d, a, b, c, x[2], 0xfcefa3f8, 9)
    STEP(G, c, d, a, b, x[7], 0x676f02d9, 14)  STEP(G, b, c, d, a, x[12], 0x8d2a4c8a, 20)
    STEP(H, a, b, c, d, x[5], 0xfffa3942, 4)   STEP(H, d, a, b, c, x[8], 0x8771f681, 11)
    STEP(H, c, d, a, b, x[11], 0x6d9d6122, 16) STEP(H, b, c, d, a, x[14], 0xfde5380c, 23)
    STEP(H, a, b, c, d, x[1], 0xa4beea44, 4)   STEP(H, d, a, b, c, x[4], 0x4bdecfa9, 11)
    STEP(H, c, d, a, b, x[7], 0xf6bb4b60, 16)  STEP(H, b, c, d, a, x[10], 0xbebfbc70, 23)
    STEP(H, a, b, c, d, x[13], 0x289b7ec6, 4)  STEP(H, d, a, b, c, x[0], 0xeaa127fa, 11)
    STEP(H, c, d, a, b, x[3], 0xd4ef3085, 16)  STEP(H, b, c, d, a, x[6], 0x04881d05, 23)
    STEP(H, a, b, c, d, x[9], 0xd9d4d039, 4)   STEP(H, d, a, b, c, x[12], 0xe6db99e5, 11)
    STEP(H, c, d, a, b, x[15], 0x1fa27cf8, 16) STEP(H, b, c, d, a, x[2], 0xc4ac5665, 23)
    STEP(I, a, b, c, d, x[0], 0xf4292244, 6)   STEP(I, d, a, b, c, x[7], 0x432aff97, 10)
    STEP(I, c, d, a, b, x[14], 0xab9423a7, 15) STEP(I, b, c, d, a, x[5], 0xfc93a039, 21)
    STEP(I, a, b, c, d, x[12], 0x655b59c3, 6)  STEP(I, d, a, b, c, x[3], 0x8f0ccc92, 10)
    STEP(I, c, d, a, b, x[10], 0xffeff47d, 15) STEP(I, b, c, d, a, x[1], 0x85845dd1, 21)
    STEP(I, a, b, c, d, x[8], 0x6fa87e4f, 6)   STEP(I, d, a, b, c, x[15], 0xfe2ce6e0, 10)
    STEP(I, c, d, a, b, x[6], 0xa3014314, 15)  STEP(I, b, c, d, a, x[13], 0x4e0811a1, 21)
    STEP(I, a, b, c, d, x[4], 0xf7537e82, 6)   STEP(I, d, a, b, c, x[11], 0xbd3af235, 10)
    STEP(I, c, d, a, b, x[2], 0x2ad7d2bb, 15)  STEP(I, b, c, d, a, x[9], 0xeb86d391, 21)
    m->s[0] += a; m->s[1] += b; m->s[2] += c; m->s[3] += d;
}

static void md5_init(md5_t *m)
{
    m->s[0] = 0x67452301; m->s[1] = 0xefcdab89; m->s[2] = 0x98badcfe; m->s[3] = 0x10325476;
    m->lo = m->hi = 0;
}

static void md5_update(md5_t *m, const unsigned char *p, DWORD n)
{
    DWORD used = m->lo & 63;
    if ((m->lo += n) < n) m->hi++;
    if (used) {
        DWORD take = 64 - used;
        if (take > n) take = n;
        memcpy(m->buf + used, p, take);
        p += take; n -= take;
        if (used + take < 64) return;
        md5_block(m, m->buf);
    }
    while (n >= 64) { md5_block(m, p); p += 64; n -= 64; }
    memcpy(m->buf, p, n);
}

static void md5_final(md5_t *m, unsigned char out[16])
{
    static const unsigned char pad[64] = { 0x80 };
    unsigned char len[8];
    DWORD lo = m->lo << 3, hi = (m->hi << 3) | (m->lo >> 29), used = m->lo & 63;
    int i;
    for (i = 0; i < 4; i++) { len[i] = (unsigned char)(lo >> (8 * i)); len[4 + i] = (unsigned char)(hi >> (8 * i)); }
    md5_update(m, pad, used < 56 ? 56 - used : 120 - used);
    md5_update(m, len, 8);
    for (i = 0; i < 16; i++) out[i] = (unsigned char)(m->s[i / 4] >> (8 * (i % 4)));
}

static HANDLE out;
static unsigned char data[65536];
static char line[600], path[MAX_PATH];

static void w(const char *s) { DWORD n; WriteFile(out, s, lstrlenA(s), &n, NULL); WriteFile(out, "\r\n", 2, &n, NULL); }

static void hash_one(const char *p)
{
    static const char hex[] = "0123456789abcdef";
    md5_t m;
    unsigned char dig[16];
    char h[33];
    DWORD rd, total = 0;
    int i;
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { wsprintfA(line, "ERROR %lu  open  %s", GetLastError(), p); w(line); return; }
    md5_init(&m);
    for (;;) {
        if (!ReadFile(f, data, sizeof(data), &rd, NULL)) {
            wsprintfA(line, "ERROR %lu  read at %lu  %s", GetLastError(), total, p); w(line);
            CloseHandle(f); return;
        }
        if (!rd) break;
        md5_update(&m, data, rd);
        total += rd;
    }
    CloseHandle(f);
    md5_final(&m, dig);
    for (i = 0; i < 16; i++) { h[i * 2] = hex[dig[i] >> 4]; h[i * 2 + 1] = hex[dig[i] & 15]; }
    h[32] = 0;
    wsprintfA(line, "%s  %lu  %s", h, total, p);
    w(line);
}

void __stdcall start(void)
{
    const char *c = GetCommandLineA();
    CreateDirectoryA("C:\\RETRO_AGENT", NULL);
    out = CreateFileA("C:\\RETRO_AGENT\\HASH9X.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) ExitProcess(3);
    /* skip the program's own name */
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; }
    else while (*c && *c != ' ') c++;
    for (;;) {
        int n = 0;
        while (*c == ' ') c++;
        if (!*c) break;
        if (*c == '"') { c++; while (*c && *c != '"' && n < MAX_PATH - 1) path[n++] = *c++; if (*c) c++; }
        else while (*c && *c != ' ' && n < MAX_PATH - 1) path[n++] = *c++;
        path[n] = 0;
        hash_one(path);
    }
    w("--end--");
    CloseHandle(out);
    ExitProcess(0);
}

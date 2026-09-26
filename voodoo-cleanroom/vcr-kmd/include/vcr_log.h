/*
 * vcr_log.h - the vcr-kmd flight recorder.
 *
 * ONE ring, owned by the miniport, in non-paged memory. Everything the driver
 * pair does that matters goes in it: the miniport writes directly, the display
 * DLL writes through IOCTL_VCR_LOG_WRITE, user-mode tools read it through
 * IOCTL_VCR_LOG_READ (reached via DrvEscape VCR_ESC_LOG_READ).
 *
 * It is built to be found WITHOUT the driver: after a bugcheck the ring is in
 * the kernel memory dump, and `tools/vcrdump.py` finds it by scanning for the
 * 16-byte header magic - no symbols, no WinDbg. So:
 *   - the header starts with a fixed 16-byte magic and records its own
 *     geometry (entry size, count) so the parser needs no build-time constants;
 *   - every entry is a fixed 128 bytes and carries its own sequence number,
 *     written LAST, so a record torn by the crash reads as seq 0 (empty) or as
 *     an older sequence, never as a plausible new one;
 *   - text is always NUL-terminated inside the entry.
 *
 * Pure C with no OS dependency: the same code runs in the miniport, in the
 * host tests (tests/native/test_vcr_kmd_log.c) and is mirrored by the Python
 * parser, which reads the geometry from the header.
 */
#ifndef VCR_LOG_H
#define VCR_LOG_H

#include "vcr_types.h"

#define VCR_LOG_MAGIC           "VCRKMD-FLIGHTREC"   /* 16 bytes, no NUL */
#define VCR_LOG_MAGIC_LEN       16
#define VCR_LOG_VERSION         1
#define VCR_LOG_ENTRY_SIZE      128
#define VCR_LOG_MSG_LEN         96
#define VCR_LOG_ENTRIES_DEFAULT 1024              /* 128 KB */

/* who wrote the entry */
#define VCR_SRC_MINIPORT        1
#define VCR_SRC_DISPLAY         2
#define VCR_SRC_ESCAPE          3   /* display DLL, on behalf of a user app */
#define VCR_SRC_TOOL            4   /* a user-mode harness (vcrctl mark ...) */

/* level */
#define VCR_LV_ERROR            0
#define VCR_LV_WARN             1
#define VCR_LV_INFO             2
#define VCR_LV_DEBUG            3
#define VCR_LV_TRACE            4

typedef struct vcr_log_entry {
    vcr_u32 seq;        /* 1-based, monotonically increasing; 0 = never written */
    vcr_u32 ms;         /* milliseconds since the ring was created */
    vcr_u16 code;       /* VCR_EV_* (vcr_events.h) */
    vcr_u8  src;        /* VCR_SRC_* */
    vcr_u8  level;      /* VCR_LV_* */
    vcr_u32 pid;        /* process the work was done for (0 = system) */
    vcr_u32 a, b, c, d; /* event arguments */
    char    msg[VCR_LOG_MSG_LEN];   /* NUL-terminated free text */
} vcr_log_entry;

typedef struct vcr_log_header {
    char    magic[VCR_LOG_MAGIC_LEN];
    vcr_u32 version;
    vcr_u32 header_size;    /* sizeof(vcr_log_header) */
    vcr_u32 entry_size;     /* VCR_LOG_ENTRY_SIZE */
    vcr_u32 nentries;
    vcr_u32 next_seq;       /* last sequence handed out */
    vcr_u32 boot_count;     /* VcrBootCount at driver load (registry) */
    vcr_u32 drv_version;    /* VCR_KMD_VERSION_NUM */
    vcr_u32 created_lo;     /* KeQuerySystemTime at creation, 100 ns units */
    vcr_u32 created_hi;
    vcr_u32 dropped;        /* writes refused (ring not ready / bad args) */
    vcr_u32 reserved[6];
} vcr_log_header;           /* 80 bytes */

typedef struct vcr_log_ring {
    vcr_log_header hdr;
    vcr_log_entry  e[1];    /* hdr.nentries of them */
} vcr_log_ring;

#define VCR_LOG_RING_BYTES(n)   (sizeof(vcr_log_header) + (n) * sizeof(vcr_log_entry))

/* Atomic increment hook: the kernel build maps it onto InterlockedIncrement. */
#ifndef VCR_LOG_NEXT_SEQ
#define VCR_LOG_NEXT_SEQ(p)     (++*(p))
#endif

void    vcr_log_init(vcr_log_ring *r, vcr_u32 nentries, vcr_u32 boot_count,
                     vcr_u32 drv_version, vcr_u32 created_lo, vcr_u32 created_hi);
vcr_u32 vcr_log_put(vcr_log_ring *r, vcr_u32 ms, vcr_u16 code, vcr_u8 src,
                    vcr_u8 level, vcr_u32 pid, vcr_u32 a, vcr_u32 b, vcr_u32 c,
                    vcr_u32 d, const char *msg);
/* Copy entries with seq > after_seq, oldest first, into out[max]. Returns the
 * count; *last_seq receives the newest sequence copied (unchanged if none). */
vcr_u32 vcr_log_read(const vcr_log_ring *r, vcr_u32 after_seq,
                     vcr_log_entry *out, vcr_u32 max, vcr_u32 *last_seq);

#endif /* VCR_LOG_H */

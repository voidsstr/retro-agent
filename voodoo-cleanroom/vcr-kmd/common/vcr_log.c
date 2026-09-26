/*
 * vcr_log.c - flight-recorder ring (see include/vcr_log.h for the contract).
 * No OS calls: the caller supplies the time and, in the kernel, the atomic
 * sequence increment via VCR_LOG_NEXT_SEQ.
 */
#include "../include/vcr_log.h"

VCR_STATIC_ASSERT(entry_size, sizeof(vcr_log_entry) == VCR_LOG_ENTRY_SIZE);
VCR_STATIC_ASSERT(header_size, sizeof(vcr_log_header) == 80);

static void vcr_zero(void *p, vcr_u32 n)
{
    volatile vcr_u8 *b = (volatile vcr_u8 *)p;
    while (n--)
        *b++ = 0;
}

void vcr_log_init(vcr_log_ring *r, vcr_u32 nentries, vcr_u32 boot_count,
                  vcr_u32 drv_version, vcr_u32 created_lo, vcr_u32 created_hi)
{
    vcr_u32 i;
    vcr_zero(r, (vcr_u32)VCR_LOG_RING_BYTES(nentries));
    r->hdr.version = VCR_LOG_VERSION;
    r->hdr.header_size = sizeof(vcr_log_header);
    r->hdr.entry_size = sizeof(vcr_log_entry);
    r->hdr.nentries = nentries;
    r->hdr.boot_count = boot_count;
    r->hdr.drv_version = drv_version;
    r->hdr.created_lo = created_lo;
    r->hdr.created_hi = created_hi;
    /* the magic goes in last: a half-initialised ring is not "found" */
    for (i = 0; i < VCR_LOG_MAGIC_LEN; i++)
        r->hdr.magic[i] = VCR_LOG_MAGIC[i];
}

vcr_u32 vcr_log_put(vcr_log_ring *r, vcr_u32 ms, vcr_u16 code, vcr_u8 src,
                    vcr_u8 level, vcr_u32 pid, vcr_u32 a, vcr_u32 b, vcr_u32 c,
                    vcr_u32 d, const char *msg)
{
    vcr_u32 seq, i;
    volatile vcr_log_entry *e;

    if (!r || r->hdr.nentries == 0)
        return 0;
    seq = VCR_LOG_NEXT_SEQ(&r->hdr.next_seq);
    e = &r->e[(seq - 1) % r->hdr.nentries];
    e->seq = 0;                 /* torn-record guard: invalid until complete */
    e->ms = ms;
    e->code = code;
    e->src = src;
    e->level = level;
    e->pid = pid;
    e->a = a;
    e->b = b;
    e->c = c;
    e->d = d;
    i = 0;
    if (msg)
        for (; i < VCR_LOG_MSG_LEN - 1 && msg[i]; i++)
            e->msg[i] = msg[i];
    for (; i < VCR_LOG_MSG_LEN; i++)
        e->msg[i] = 0;
    e->seq = seq;               /* publish */
    return seq;
}

vcr_u32 vcr_log_read(const vcr_log_ring *r, vcr_u32 after_seq,
                     vcr_log_entry *out, vcr_u32 max, vcr_u32 *last_seq)
{
    vcr_u32 newest, oldest, s, n = 0;

    if (!r || r->hdr.nentries == 0 || !out || max == 0)
        return 0;
    newest = r->hdr.next_seq;
    oldest = newest > r->hdr.nentries ? newest - r->hdr.nentries + 1 : 1;
    if (after_seq + 1 > oldest)
        oldest = after_seq + 1;
    for (s = oldest; s <= newest && n < max; s++) {
        const vcr_log_entry *e = &r->e[(s - 1) % r->hdr.nentries];
        if (e->seq != s)        /* overwritten meanwhile, or still being written */
            continue;
        out[n++] = *e;
        if (last_seq)
            *last_seq = s;
    }
    return n;
}

/* test_vcr_kmd_clutread.c - TRUE-SOURCE test of
 * voodoo-cleanroom/vcr-kmd/include/vcr_clutread.h: the miniport's READ-ONLY
 * CLUT read (IOCTL_VCR_REG kind VCR_REG_CLUT, vcrmp.c reg_op ->
 * vcrmp_hw.c VcrHwClutRead), 2026-09-28.
 *
 * The defect it answers: `vcrctl fbshot` of Warcraft II's 640x480x8 menu on
 * .124 came out greyscale - the CLUT read was refused. Not by DirectDraw's
 * exclusive mode: VCR_ESC_REG goes straight to the miniport, whose reg_op
 * refused every WRITE without Diag\AllowPoke, and a CLUT read begins by
 * writing dacAddr. The gate below reproduces that refusal (the old path) and
 * lets the read-only kind through.
 *
 * Against a simulated DAC (dacAddr/dacData, 512 entries) it pins: the entry
 * read is the entry; dacData is NEVER written; dacAddr is put back; dropped
 * dacAddr writes are retried (the pair drops writes - xf86-video-tdfx, Glide);
 * a writer that moves dacAddr between our index and our data read is seen and
 * retried, never averaged in; every loop is bounded and a failure leaves the
 * caller's value untouched.
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_ioctl.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_regs.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_clutread.h"

typedef struct dac {
    vcr_u32 addr;
    vcr_u32 clut[512];
    int drop_addr;              /* dacAddr writes still to drop (-1 = all) */
    int move_on_data;           /* data reads after which a stranger moves dacAddr (-1 = all) */
    vcr_u32 stranger_addr;
    int data_writes, addr_writes, reads;
} dac;

static vcr_u32 d_rd(void *ctx, vcr_u32 off)
{
    dac *d = (dac *)ctx;
    d->reads++;
    if (off == VCR_CLUT_DACADDR)
        return d->addr | 0xfe00u;       /* the upper bits are not the index */
    if (off == VCR_CLUT_DACDATA) {
        vcr_u32 v = d->clut[d->addr & 0x1ff] | 0xff000000u;
        if (d->move_on_data) {
            if (d->move_on_data > 0)
                d->move_on_data--;
            d->addr = d->stranger_addr;         /* Glide's gamma loop, say */
        }
        return v;
    }
    return 0xdeadbeefu;
}

static void d_wr(void *ctx, vcr_u32 off, vcr_u32 v)
{
    dac *d = (dac *)ctx;
    if (off == VCR_CLUT_DACADDR) {
        d->addr_writes++;
        if (d->drop_addr) {
            if (d->drop_addr > 0)
                d->drop_addr--;
            return;
        }
        d->addr = v & 0x1ff;
    } else if (off == VCR_CLUT_DACDATA) {
        d->data_writes++;
    }
}

static void dac_init(dac *d, vcr_dac_io *io)
{
    int i;
    memset(d, 0, sizeof *d);
    for (i = 0; i < 512; i++)
        d->clut[i] = (vcr_u32)(i * 0x010307u) & 0xffffffu;
    d->addr = 0x123;
    io->ctx = d;
    io->rd = d_rd;
    io->wr = d_wr;
}

TEST(an_entry_is_read_and_the_dac_left_as_it_was)
{
    dac d;
    vcr_dac_io io;
    vcr_u32 v = 0;
    dac_init(&d, &io);
    CHECK_EQ_I(vcr_clut_read_entry(&io, 7, &v), VCR_CLUT_OK);
    CHECK_EQ_U(v, d.clut[7]);
    CHECK_EQ_U(d.addr, 0x123);                  /* put back */
    CHECK_EQ_I(d.data_writes, 0);               /* dacData is never written */
    CHECK_EQ_I(vcr_clut_read_entry(&io, 300, &v), VCR_CLUT_OK);
    CHECK_EQ_U(v, d.clut[300]);                 /* bank 1 */
    CHECK_EQ_U(d.addr, 0x123);
    CHECK((v & 0xff000000u) == 0, "only the 24 colour bits");
}

TEST(dropped_address_writes_are_retried_and_bounded)
{
    dac d;
    vcr_dac_io io;
    vcr_u32 v = 0xabcdef;
    dac_init(&d, &io);
    d.drop_addr = 3;
    CHECK_EQ_I(vcr_clut_read_entry(&io, 9, &v), VCR_CLUT_OK);
    CHECK_EQ_U(v, d.clut[9]);
    CHECK_EQ_U(d.addr, 0x123);
    /* a pair that never takes a write: E_ADDR, value untouched, bounded */
    dac_init(&d, &io);
    d.drop_addr = -1;
    v = 0xabcdef;
    CHECK((vcr_clut_read_entry(&io, 9, &v) & VCR_CLUT_E_ADDR) != 0, "reported");
    CHECK_EQ_U(v, 0xabcdef);
    CHECK(d.addr_writes <= 2 * VCR_CLUT_TRIES + 1, "bounded");
    CHECK_EQ_I(d.data_writes, 0);
}

TEST(a_writer_that_moves_the_index_is_seen_not_averaged_in)
{
    dac d;
    vcr_dac_io io;
    vcr_u32 v = 0;
    dac_init(&d, &io);
    d.move_on_data = 2;                 /* twice, someone moves dacAddr under us */
    d.stranger_addr = 0x40;
    CHECK_EQ_I(vcr_clut_read_entry(&io, 200, &v), VCR_CLUT_OK);
    CHECK_EQ_U(v, d.clut[200]);
    CHECK_EQ_U(d.addr, 0x123);
    /* someone who never stops: refused, value untouched */
    dac_init(&d, &io);
    d.move_on_data = -1;
    d.stranger_addr = 0x40;
    v = 0x111111;
    CHECK((vcr_clut_read_entry(&io, 200, &v) & VCR_CLUT_E_ADDR) != 0, "refused");
    CHECK_EQ_U(v, 0x111111);
    CHECK(d.reads < 10 * VCR_CLUT_TRIES * VCR_CLUT_TRIES, "bounded");
}

TEST(past_the_two_banks_nothing_is_touched)
{
    dac d;
    vcr_dac_io io;
    vcr_u32 v = 5;
    dac_init(&d, &io);
    CHECK_EQ_I(vcr_clut_read_entry(&io, 512, &v), VCR_CLUT_E_INDEX);
    CHECK_EQ_I(d.reads + d.addr_writes + d.data_writes, 0);
    CHECK_EQ_U(v, 5);
}

TEST(the_gate_the_warcraft_read_hit_and_the_one_that_replaces_it)
{
    /* the old path: a dacAddr WRITE through VCR_REG_MMIO32 - refused without
     * AllowPoke (the greyscale Warcraft II capture), allowed with it */
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_MMIO32, 1, 0, VCR_R_DACADDR, 0, 1, 1), VCR_REGOP_DENIED);
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_MMIO32, 1, 0, VCR_R_DACADDR, 1, 1, 1), VCR_REGOP_OK);
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_MMIO32, 0, 0, VCR_R_DACDATA, 0, 1, 1), VCR_REGOP_OK);
    /* the read-only kind: no AllowPoke */
    CHECK_EQ_U(VCR_REGOP_KIND_CLUT, VCR_REG_CLUT);
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_CLUT, 0, 0, 255, 0, 1, 1), VCR_REGOP_OK);
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_CLUT, 0, 0, 511, 0, 1, 1), VCR_REGOP_OK);
    /* ...never a write, even with AllowPoke; chip 0; 0..511; a Voodoo */
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_CLUT, 1, 0, 7, 1, 1, 1), VCR_REGOP_INVALID);
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_CLUT, 0, 1, 7, 0, 1, 1), VCR_REGOP_INVALID);
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_CLUT, 0, 0, 512, 0, 1, 1), VCR_REGOP_INVALID);
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_CLUT, 0, 0, 7, 0, 1, 0), VCR_REGOP_INVALID);
    /* Diag\ClutRead absent or 0 - the default since the 2026-09-28 integration */
    CHECK_EQ_I(vcr_regop_gate(VCR_REG_CLUT, 0, 0, 7, 1, 0, 1), VCR_REGOP_DENIED);
    CHECK_EQ_U(VCR_CLUT_DACADDR, VCR_R_DACADDR);
    CHECK_EQ_U(VCR_CLUT_DACDATA, VCR_R_DACDATA);
}

MUNIT_MAIN("vcr-kmd read-only CLUT read (include/vcr_clutread.h)", {
    RUN(an_entry_is_read_and_the_dac_left_as_it_was);
    RUN(dropped_address_writes_are_retried_and_bounded);
    RUN(a_writer_that_moves_the_index_is_seen_not_averaged_in);
    RUN(past_the_two_banks_nothing_is_touched);
    RUN(the_gate_the_warcraft_read_hit_and_the_one_that_replaces_it);
})

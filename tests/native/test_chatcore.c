/* test_chatcore.c — TRUE-SOURCE test: compiles the REAL shared chat-proxy
 * state engine (agent/shared/chatcore.c) natively and exercises it. This is
 * the state machine behind PROMPT_PUSH/POP, LOG_APPEND/READ and STATUS_SET
 * on BOTH the Windows agent (chatproxy.c wraps it in locks/events) and the
 * DOS combined agent+chat. If someone changes the single-slot prompt
 * semantics, the drop-oldest-half log policy, or the status sequencing,
 * these fail at test time instead of on the fleet.
 */
#include "munit.h"
#include <string.h>

#include "../../agent/shared/chatcore.c"

TEST(prompt_single_slot_push_pop) {
    chatcore_t c;
    char out[64];
    chatcore_init(&c, 4096);
    CHECK(chatcore_prompt_push(&c, "hello fleet") == 0, "push must succeed");
    CHECK(chatcore_prompt_push(&c, "second") == 0,
          "second push overwrites the single slot");
    CHECK(chatcore_prompt_pop(&c, out, sizeof(out)) == 1, "pop returns prompt");
    CHECK(strcmp(out, "second") == 0, "slot holds the LATEST prompt");
    CHECK(chatcore_prompt_pop(&c, out, sizeof(out)) == 0,
          "slot is emptied by pop");
    chatcore_free(&c);
}

TEST(prompt_rejects_empty_and_oversize) {
    chatcore_t c;
    char big[CHATCORE_PROMPT_MAX + 8];
    chatcore_init(&c, 4096);
    CHECK(chatcore_prompt_push(&c, "") == -1, "empty prompt rejected");
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(chatcore_prompt_push(&c, big) == -1, "oversize prompt rejected");
    chatcore_free(&c);
}

TEST(log_append_and_drop_oldest_half_when_full) {
    chatcore_t c;
    unsigned long i;
    chatcore_init(&c, 1024);          /* small cap to exercise the ring */
    for (i = 0; i < 100; i++)
        CHECK(chatcore_log_append(&c, "0123456789", 10) == 10,
              "append within cap succeeds fully");
    /* 1000 bytes in a 1024 cap: no drop yet */
    CHECK_EQ_U(c.log_size, 1000);
    /* 30 more crosses the cap: oldest half dropped first (keep 512) */
    chatcore_log_append(&c, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);
    CHECK_EQ_U(c.log_size, 542);      /* 512 kept + 30 new */
    CHECK(memcmp(c.log + c.log_size - 30, "AAAAAAAAAA", 10) == 0,
          "new data is at the tail after the drop");
    chatcore_free(&c);
}

TEST(status_seq_bumps_on_every_set_and_clear) {
    chatcore_t c;
    unsigned long s0;
    chatcore_init(&c, 4096);
    s0 = c.status_seq;
    chatcore_status_set(&c, "EXEC dir C:\\");
    CHECK(c.status_seq == s0 + 1, "set bumps seq");
    chatcore_status_set(&c, "EXEC dir C:\\");
    CHECK(c.status_seq == s0 + 2, "identical value still bumps seq");
    CHECK(strcmp(c.status, "EXEC dir C:\\") == 0, "status stored");
    chatcore_log_clear(&c);
    CHECK(c.status_seq == s0 + 3, "clear bumps seq");
    CHECK(c.status[0] == '\0' && c.log_size == 0 && !c.prompt_pending,
          "clear resets log, prompt and status");
    chatcore_free(&c);
}

/* ---- absolute log offsets (agent 1.85.0) -------------------------------
 * The ring used to be addressed by BUFFER offset, so a drop moved the bytes
 * under every reader: the total shrank, retro_chat reset to 0 and reprinted up
 * to 128 KB, and the DOS UI (offset now past the end) printed nothing. */

TEST(log_base_advances_by_exactly_what_a_drop_discards) {
    chatcore_t c;
    unsigned long i;
    chatcore_init(&c, 1024);
    for (i = 0; i < 100; i++) chatcore_log_append(&c, "0123456789", 10);
    CHECK_EQ_U(c.log_base, 0);
    CHECK_EQ_U(chatcore_log_end(&c), 1000);
    chatcore_log_append(&c, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);
    CHECK_EQ_U(c.log_size, 542);            /* buffer: 512 kept + 30 */
    CHECK_EQ_U(c.log_base, 488);            /* 1000 - 512 dropped */
    CHECK_EQ_U(chatcore_log_end(&c), 1030); /* absolute end never shrinks */
    CHECK(chatcore_log_end(&c) > 1000,
          "old buggy total (log_size = 542) was BELOW a caught-up reader (1000)");
    chatcore_free(&c);
}

TEST(reader_offsets_are_resolved_against_the_ring) {
    chatcore_t c;
    unsigned long i, idx, avail;
    int past;
    chatcore_init(&c, 1024);
    for (i = 0; i < 103; i++) chatcore_log_append(&c, "0123456789", 10);
    /* 1030 written, 1024 cap: one drop, base 1000-512 = 488 ... (as above) */
    CHECK(c.log_base > 0, "a drop happened");

    /* caught-up reader: nothing new */
    idx = chatcore_log_window(&c, chatcore_log_end(&c), &avail, &past);
    CHECK(avail == 0 && !past && idx == c.log_size, "caught up");

    /* reader part-way: gets exactly the unread tail */
    idx = chatcore_log_window(&c, chatcore_log_end(&c) - 7, &avail, &past);
    CHECK(avail == 7 && !past && idx == c.log_size - 7, "unread tail");

    /* reader BEHIND the base: its bytes were dropped, it skips ahead */
    idx = chatcore_log_window(&c, 5, &avail, &past);
    CHECK(idx == 0 && avail == c.log_size && !past, "behind base -> base");

    /* reader PAST the end: the log was cleared or the agent restarted */
    idx = chatcore_log_window(&c, chatcore_log_end(&c) + 1, &avail, &past);
    CHECK(avail == 0 && past == 1, "past end -> reset signal");
    chatcore_free(&c);
}

TEST(clear_restarts_the_absolute_numbering) {
    chatcore_t c;
    unsigned long i, avail;
    int past;
    chatcore_init(&c, 1024);
    for (i = 0; i < 200; i++) chatcore_log_append(&c, "0123456789", 10);
    CHECK(c.log_base > 0, "dropped");
    chatcore_log_clear(&c);
    CHECK_EQ_U(c.log_base, 0);              /* old clients count from 0 again */
    CHECK_EQ_U(chatcore_log_end(&c), 0);
    chatcore_log_window(&c, 2000, &avail, &past);
    CHECK(past == 1, "a pre-clear reader is past the end");
    chatcore_free(&c);
}

/* ---- prompt in flight (agent 1.85.0) ------------------------------------
 * A prompt popped into a dead connection used to vanish. take keeps it in
 * flight until that connection's next command; a drop first puts it back. */

TEST(take_ack_requeue) {
    chatcore_t c;
    char out[64];
    chatcore_init(&c, 4096);
    chatcore_prompt_push(&c, "hello");
    CHECK(chatcore_prompt_take(&c, out, sizeof(out), 42) == 1, "taken");
    CHECK(strcmp(out, "hello") == 0, "text");
    CHECK(!c.prompt_pending && c.prompt_inflight, "in flight, not pending");
    CHECK(chatcore_prompt_take(&c, out, sizeof(out), 43) == 0,
          "nobody else can take an in-flight prompt");
    CHECK(chatcore_prompt_requeue(&c, 43) == 0, "only the owner requeues");
    CHECK(chatcore_prompt_requeue(&c, 42) == 1, "owner died: put back");
    CHECK(c.prompt_pending && !c.prompt_inflight, "pending again");
    CHECK(chatcore_prompt_take(&c, out, sizeof(out), 43) == 1 &&
          strcmp(out, "hello") == 0, "re-delivered intact");
    chatcore_prompt_ack(&c, 42);            /* stale owner: no effect */
    CHECK(c.prompt_inflight, "ack by a non-owner is ignored");
    chatcore_prompt_ack(&c, 43);
    CHECK(!c.prompt_inflight && !c.prompt_pending, "acknowledged: done");
    CHECK(chatcore_prompt_requeue(&c, 43) == 0, "nothing to requeue after ack");
    chatcore_free(&c);
}

TEST(a_newer_push_supersedes_an_inflight_prompt) {
    chatcore_t c;
    char out[64];
    chatcore_init(&c, 4096);
    chatcore_prompt_push(&c, "old");
    chatcore_prompt_take(&c, out, sizeof(out), 7);
    chatcore_prompt_push(&c, "new");
    CHECK(chatcore_prompt_requeue(&c, 7) == 0, "requeue must not clobber");
    CHECK(c.prompt_pending && strcmp(c.prompt, "new") == 0, "newer wins");
    chatcore_log_clear(&c);
    CHECK(!c.prompt_pending && !c.prompt_inflight, "clear drops both");
    chatcore_free(&c);
}

/* ---- LOG_APPEND2 dedupe (agent 1.85.0) ---- */

TEST(append_once_drops_a_resend) {
    chatcore_t c;
    int dup = -1;
    unsigned long id;
    chatcore_init(&c, 4096);
    CHECK_EQ_U(chatcore_log_append_once(&c, 5, "abc", 3, &dup), 3);
    CHECK_EQ_I(dup, 0);
    CHECK_EQ_U(chatcore_log_append_once(&c, 5, "abc", 3, &dup), 0);
    CHECK_EQ_I(dup, 1);
    CHECK_EQ_U(c.log_size, 3);              /* the resend appended nothing */
    CHECK_EQ_U(chatcore_log_append_once(&c, 0, "z", 1, &dup), 1);
    CHECK_EQ_I(dup, 0);                     /* id 0 is a real id, not "empty" */
    /* the memory is CHATCORE_DEDUP_IDS deep */
    for (id = 100; id < 100 + CHATCORE_DEDUP_IDS; id++)
        chatcore_log_append_once(&c, id, "x", 1, &dup);
    chatcore_log_append_once(&c, 5, "abc", 3, &dup);
    CHECK_EQ_I(dup, 0);                     /* 5 has aged out */
    chatcore_free(&c);
}

/* ---- PROMPT_PUSH reply (agent 1.85.0) ---- */

TEST(push_reply_always_starts_ok) {
    char r[64];
    chatcore_push_reply(r, sizeof(r), 0, 1, 0);
    CHECK(strcmp(r, "OK") == 0, "plain");
    chatcore_push_reply(r, sizeof(r), 1, 1, 0);
    CHECK(strcmp(r, "OK replaced") == 0, "replaced");
    chatcore_push_reply(r, sizeof(r), 0, 0, 125999);
    CHECK(strcmp(r, "OK no-listener 125s") == 0, "no listener, whole seconds");
    chatcore_push_reply(r, sizeof(r), 1, 0, 0);
    CHECK(strcmp(r, "OK replaced no-listener 0s") == 0, "both");
    chatcore_push_reply(r, 5, 1, 0, 0);
    CHECK(strcmp(r, "OK r") == 0, "bounded by the buffer");
}

MUNIT_MAIN("chatcore shared chat-proxy state engine (true-source)", {
    RUN(prompt_single_slot_push_pop);
    RUN(prompt_rejects_empty_and_oversize);
    RUN(log_append_and_drop_oldest_half_when_full);
    RUN(status_seq_bumps_on_every_set_and_clear);
    RUN(log_base_advances_by_exactly_what_a_drop_discards);
    RUN(reader_offsets_are_resolved_against_the_ring);
    RUN(clear_restarts_the_absolute_numbering);
    RUN(take_ack_requeue);
    RUN(a_newer_push_supersedes_an_inflight_prompt);
    RUN(append_once_drops_a_resend);
    RUN(push_reply_always_starts_ok);
})

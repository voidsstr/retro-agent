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

MUNIT_MAIN("chatcore shared chat-proxy state engine (true-source)", {
    RUN(prompt_single_slot_push_pop);
    RUN(prompt_rejects_empty_and_oversize);
    RUN(log_append_and_drop_oldest_half_when_full);
    RUN(status_seq_bumps_on_every_set_and_clear);
})

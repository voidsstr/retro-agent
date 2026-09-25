/* test_protocol_stall.c — TRUE-SOURCE test: compiles the REAL
 * agent/src/protocol.c against the fake Winsock in stubs/netfake_env.h.
 *
 * Fix 3 (agent 1.85.0): no recv or send may wait forever once a frame has
 * started. On Win9x SO_RCVTIMEO is off (it crashes Win98 Winsock) and ONE
 * thread serves every client, so a peer that vanished mid-frame left recv()
 * blocked for good and froze the whole agent. The fake counts every recv/send
 * that real Winsock would never return from (fake_blocked_forever); the old
 * protocol.c scores 1 on each stall case below, the fixed one 0.
 *
 * Fix 9: a small reply goes out as ONE send (the old frame_send made two:
 * header, then payload), and a large one is chunked so no single blocking
 * send can outlive the stall limit.
 */
#include "munit.h"
#include "stubs/netfake_env.h"

void log_msg(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

/* protocol.c prints transfer progress; keep the test output readable */
#define printf(...) ((void)0)
#include "../../agent/src/protocol.c"
#undef printf

enum { S = 3 };

static void feed_header(unsigned long len)
{
    unsigned char h[4];
    h[0] = (unsigned char)(len & 0xFF);
    h[1] = (unsigned char)((len >> 8) & 0xFF);
    h[2] = (unsigned char)((len >> 16) & 0xFF);
    h[3] = (unsigned char)((len >> 24) & 0xFF);
    fake_sock_feed(S, h, 4);
}

TEST(a_peer_that_vanishes_mid_frame_cannot_freeze_recv) {
    char *buf = NULL;
    DWORD len = 0, t0;
    fake_sock_reset();
    feed_header(100);
    fake_sock_feed(S, "only ten b", 10);       /* ...and then silence */
    t0 = fake_now;
    CHECK_EQ_I(frame_recv(S, &buf, &len), -1);
    CHECK_EQ_I(fake_blocked_forever, 0);       /* the old recv_exact: 1 */
    CHECK_EQ_U(fake_now - t0, RECV_STALL_MS);  /* gave up after the limit */
}

TEST(a_stall_inside_the_header_is_bounded_too) {
    char *buf = NULL;
    DWORD len = 0;
    fake_sock_reset();
    fake_sock_feed(S, "\x10\x00", 2);           /* half a header */
    CHECK_EQ_I(frame_recv(S, &buf, &len), -1);
    CHECK_EQ_I(fake_blocked_forever, 0);
}

TEST(a_stall_inside_a_large_frame_is_bounded) {
    char *buf = NULL;
    DWORD len = 0;
    static char chunk[70000];
    fake_sock_reset();
    feed_header(200000);                       /* the >64 KB progress path */
    memset(chunk, 'z', sizeof(chunk));
    fake_sock_feed(S, chunk, sizeof(chunk));
    CHECK_EQ_I(frame_recv(S, &buf, &len), -1);
    CHECK_EQ_I(fake_blocked_forever, 0);
}

TEST(the_idle_wait_for_the_next_command_is_not_cut_off) {
    char *buf = NULL;
    DWORD len = 0, t0;
    fake_sock_reset();
    t0 = fake_now;
    frame_recv(S, &buf, &len);                 /* nothing sent at all */
    /* A connection may sit between commands for minutes: the FIRST byte is
     * waited for with a plain recv (the multiplex loop only calls in once
     * select() has seen it; threaded NT has SO_RCVTIMEO). */
    CHECK_EQ_U(fake_now - t0, 0);
    CHECK_EQ_I(fake_blocked_forever, 1);
}

TEST(an_upload_payload_must_start_promptly) {
    char *buf = NULL;
    DWORD len = 0, t0;
    fake_sock_reset();
    t0 = fake_now;
    CHECK_EQ_I(frame_recv_timed(S, &buf, &len, FRAME_FOLLOW_MS), -1);
    CHECK_EQ_I(fake_blocked_forever, 0);
    CHECK_EQ_U(fake_now - t0, FRAME_FOLLOW_MS);
}

TEST(a_slow_but_alive_transfer_is_never_cut_off) {
    char *buf = NULL;
    DWORD len = 0, t0;
    static char payload[150000];
    int i;
    fake_sock_reset();
    for (i = 0; i < (int)sizeof(payload); i++) payload[i] = (char)('a' + i % 26);
    feed_header(sizeof(payload));
    fake_sock_feed(S, payload, sizeof(payload));
    fake_socks[S].in_trickle = 1500;           /* one packet per recv */
    fake_socks[S].in_gap_ms = 20000;           /* 20 s apart: slow, alive */
    t0 = fake_now;
    CHECK_EQ_I(frame_recv(S, &buf, &len), 0);
    CHECK_EQ_U(len, sizeof(payload));
    CHECK(buf && memcmp(buf, payload, sizeof(payload)) == 0, "intact");
    CHECK(fake_now - t0 >= 100ul * 20000ul,
          "took over half an hour in total and was still accepted");
    CHECK_EQ_I(fake_blocked_forever, 0);
    free(buf);
}

TEST(a_peer_that_stops_reading_fails_the_send_instead_of_blocking) {
    DWORD t0;
    fake_sock_reset();
    fake_socks[S].unwritable = 1;
    t0 = fake_now;
    CHECK_EQ_I(send_text_response(S, "PONG"), -1);
    CHECK_EQ_I(fake_blocked_forever, 0);       /* send() never called */
    CHECK_EQ_U(fake_now - t0, SEND_STALL_MS);
}

TEST(a_small_reply_is_one_send) {
    fake_sock_reset();
    CHECK_EQ_I(send_text_response(S, "PONG"), 0);
    CHECK_EQ_I(fake_socks[S].send_calls, 1);   /* the old frame_send: 2 */
    CHECK_EQ_I(fake_socks[S].out_len, 9);
    CHECK(memcmp(fake_socks[S].out, "\x05\x00\x00\x00\x00PONG", 9) == 0,
          "[len=5][RESP_OK_TEXT]PONG");
    fake_sock_reset();
    CHECK_EQ_I(send_error_response(S, "bad"), 0);
    CHECK(fake_socks[S].send_calls == 1 && fake_socks[S].out[4] == 0xFF,
          "errors too");
    fake_sock_reset();
    CHECK_EQ_I(frame_send(S, "SHUTDOWN", 8), 0);
    CHECK_EQ_I(fake_socks[S].send_calls, 1);
    CHECK(memcmp(fake_socks[S].out, "\x08\x00\x00\x00SHUTDOWN", 12) == 0,
          "public frame_send coalesces too");
}

TEST(a_large_reply_is_chunked_and_intact) {
    static char big[300000];
    int i;
    fake_sock_reset();
    for (i = 0; i < (int)sizeof(big); i++) big[i] = (char)i;
    CHECK_EQ_I(send_binary_response(S, big, sizeof(big)), 0);
    CHECK(fake_socks[S].max_send <= SEND_CHUNK, "no single send > one chunk");
    CHECK_EQ_I(fake_socks[S].out_len, 4 + 1 + (int)sizeof(big));
    CHECK(fake_socks[S].out[4] == 0x01 &&
          memcmp(fake_socks[S].out + 5, big, sizeof(big)) == 0, "intact");
    fake_sock_reset();
    CHECK_EQ_I(frame_send(S, big, sizeof(big)), 0);   /* > COALESCE_MAX path */
    CHECK_EQ_I(fake_socks[S].out_len, 4 + (int)sizeof(big));
    CHECK(fake_socks[S].max_send <= SEND_CHUNK, "chunked");
}

TEST(round_trip_through_the_fake) {
    char *buf = NULL;
    DWORD len = 0;
    fake_sock_reset();
    feed_header(5);
    fake_sock_feed(S, "hello", 5);
    CHECK_EQ_I(frame_recv(S, &buf, &len), 0);
    CHECK(len == 5 && memcmp(buf, "hello", 5) == 0 && buf[5] == 0, "framed");
    free(buf);
}

MUNIT_MAIN("protocol.c frame I/O cannot wait forever (true-source)", {
    RUN(a_peer_that_vanishes_mid_frame_cannot_freeze_recv);
    RUN(a_stall_inside_the_header_is_bounded_too);
    RUN(a_stall_inside_a_large_frame_is_bounded);
    RUN(the_idle_wait_for_the_next_command_is_not_cut_off);
    RUN(an_upload_payload_must_start_promptly);
    RUN(a_slow_but_alive_transfer_is_never_cut_off);
    RUN(a_peer_that_stops_reading_fails_the_send_instead_of_blocking);
    RUN(a_small_reply_is_one_send);
    RUN(a_large_reply_is_chunked_and_intact);
    RUN(round_trip_through_the_fake);
})

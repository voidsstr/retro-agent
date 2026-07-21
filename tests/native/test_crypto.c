/* test_crypto.c — TRUE-SOURCE test: compiles the REAL agent crypto source
 * (agent/src/crypto.c) natively against a stub <windows.h> and exercises the
 * public API. If someone changes the keystream derivation, the enable-gate, or
 * the XOR loop, these fail at test time instead of silently on the fleet.
 *
 * Contract (crypto.c header): this is transport scrambling, NOT a security
 * boundary — so the tests assert its documented, deterministic behaviour, not
 * cryptographic strength:
 *   - keystream[i] = secret[i % slen] ^ (i*0x5A + 0x3C), 256 bytes
 *   - XOR is an involution: applying crypto_xor twice restores the plaintext
 *   - disabled (no crypto_enable) => crypto_xor is a no-op
 *   - empty secret => never enabled-effective (key_len stays 0 => no-op)
 */
#include "munit.h"
#include <string.h>

/* pull in the real agent source (its <windows.h> resolves to our empty stub) */
#include "../../agent/src/crypto.c"

TEST(xor_is_an_involution) {
    crypto_init("retro-agent-secret");
    crypto_enable();
    char plain[64];
    for (int i = 0; i < 64; i++) plain[i] = (char)(i * 7 + 3);
    char work[64];
    memcpy(work, plain, 64);

    crypto_xor(work, 64);
    CHECK(memcmp(work, plain, 64) != 0, "one XOR pass must change the data");
    crypto_xor(work, 64);
    CHECK(memcmp(work, plain, 64) == 0, "two XOR passes must restore plaintext");
}

TEST(keystream_matches_documented_formula) {
    crypto_init("abc");            /* slen = 3 */
    crypto_enable();
    char zero[300];
    memset(zero, 0, sizeof(zero));
    crypto_xor(zero, 300);         /* 0 ^ key[i] == key[i]; key wraps mod 256 */
    const char *sec = "abc";
    for (int i = 0; i < 300; i++) {
        int k = i % 256;           /* keystream index wraps at 256 */
        unsigned char expect = (unsigned char)(sec[k % 3] ^ (k * 0x5A + 0x3C));
        CHECK_EQ_U((unsigned char)zero[i], expect);
    }
}

/* NOTE: crypto.c holds PROCESS-GLOBAL state (g_encryption_enabled is sticky —
 * there is no crypto_disable — and crypto_init does not clear the key). So the
 * "disabled" branch can only be observed from the fresh process state; this test
 * is intentionally RUN FIRST, before any crypto_enable(). */
TEST(disabled_is_noop_from_fresh_state) {
    crypto_init("secret");          /* derives a key but does NOT enable */
    char data[16];
    for (int i = 0; i < 16; i++) data[i] = (char)i;
    char copy[16]; memcpy(copy, data, 16);
    crypto_xor(data, 16);           /* enabled flag still 0 -> guard returns early */
    CHECK(memcmp(data, copy, 16) == 0, "disabled crypto_xor must not touch data");
}

TEST(different_secrets_produce_different_keystreams) {
    char a[32]; memset(a, 0, 32);
    crypto_init("alpha"); crypto_enable(); crypto_xor(a, 32);
    char b[32]; memset(b, 0, 32);
    crypto_init("bravo"); crypto_enable(); crypto_xor(b, 32);
    CHECK(memcmp(a, b, 32) != 0, "distinct secrets must yield distinct keystreams");
}

MUNIT_MAIN("agent crypto_xor (true-source)", {
    RUN(disabled_is_noop_from_fresh_state);   /* must run first (sticky enable) */
    RUN(xor_is_an_involution);
    RUN(keystream_matches_documented_formula);
    RUN(different_secrets_produce_different_keystreams);
})

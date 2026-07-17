/*
 * crypto.c - Optional lightweight transport scrambling for the LAN link.
 *
 * A keystream XOR keyed from the shared AUTH secret, negotiated via the
 * AUTH_ENC handshake. Its ONLY purpose is to keep management commands from
 * appearing as plaintext in a casual packet capture on the local network.
 *
 * This is deliberately NOT presented as a security boundary and must not be
 * relied on as one: a stream XOR is trivially breakable. The actual trust
 * boundary for this tool is (1) network isolation (run it on a trusted/
 * isolated LAN or behind a firewall) and (2) the AUTH shared secret. For any
 * exposure beyond a trusted LAN, tunnel the connection over TLS/SSH instead
 * of relying on this layer.
 */

#include <windows.h>
#include <string.h>

static unsigned char g_xor_key[256];
static int g_xor_key_len = 0;
static int g_encryption_enabled = 0;

void crypto_init(const char *secret)
{
    /* Derive XOR key from shared secret via simple hash expansion */
    int slen = (int)strlen(secret);
    int i;

    if (slen <= 0) return;

    for (i = 0; i < 256; i++) {
        g_xor_key[i] = (unsigned char)(secret[i % slen] ^ (i * 0x5A + 0x3C));
    }
    g_xor_key_len = 256;
}

void crypto_enable(void)
{
    g_encryption_enabled = 1;
}

int crypto_is_enabled(void)
{
    return g_encryption_enabled;
}

void crypto_xor(char *data, int len)
{
    int i;
    if (!g_encryption_enabled || g_xor_key_len == 0) return;

    for (i = 0; i < len; i++) {
        data[i] ^= g_xor_key[i % g_xor_key_len];
    }
}

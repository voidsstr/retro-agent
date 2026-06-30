/* Portable CLI wrapper around the XPConfirmationIDKeygen generate() core.
 * Builds on Linux/GCC. Algorithm code is verbatim from main.c (core.inc). */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define assert(x) /*nothing*/

/* MSVC intrinsic used by __umul128: 32x32->64 unsigned multiply */
static inline uint64_t __emulu(unsigned a, unsigned b) {
    return (uint64_t)a * (uint64_t)b;
}

#include "core.inc"

static const char *errmsg(int e) {
    switch (e) {
        case 1: return "installation ID too short";
        case 2: return "installation ID too large";
        case 3: return "invalid character (digits/spaces/dashes only)";
        case 4: return "invalid check digit (mistyped group)";
        case 5: return "unknown/unsupported version";
        case 6: return "unlucky input, cannot generate (extremely rare)";
        default: return "unknown error";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <installation-id>\n"
            "  45 digits, spaces/dashes ignored.\n", argv[0]);
        return 2;
    }
    /* Join all args so the ID can be passed as separate space-separated groups */
    char iid[512]; iid[0] = 0;
    for (int i = 1; i < argc; i++) {
        strncat(iid, argv[i], sizeof(iid) - strlen(iid) - 2);
        strncat(iid, " ", sizeof(iid) - strlen(iid) - 2);
    }
    char cid[49];
    int err = generate(iid, cid);
    if (err) {
        fprintf(stderr, "Error: %s\n", errmsg(err));
        return 1;
    }
    printf("%s\n", cid);
    return 0;
}

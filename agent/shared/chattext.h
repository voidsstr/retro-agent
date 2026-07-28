/*
 * chattext.h - Chat text presentation helpers (SHARED, pure logic)
 *
 * Extracted verbatim from agent/tools/retro_chat.c (v0.14.0) so the DOS
 * combined agent+chat renders responses identically. Static inline-style
 * header: include from exactly one presentation module per binary.
 *
 * Users: agent/tools/retro_chat.c (Win32 console), agent/doschat (DOS).
 */

#ifndef CHATTEXT_H
#define CHATTEXT_H

/* Sanitize a chunk of log content into a print-safe buffer.
 *
 * Strips:
 *   - \x01 (USER_MARKER from old proxy versions — defensive)
 *   - other ASCII control bytes except \n, \r, \t
 *   - high-bit bytes that retro consoles may render as garbage
 *
 * Returns the number of bytes written to `out` (always <= len).
 */
static unsigned long chat_sanitize_chunk(const char *in, unsigned long len,
                                         char *out)
{
    unsigned long i, j = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            out[j++] = (char)c;
        } else if (c < 32) {
            continue;
        } else if (c < 127) {
            out[j++] = (char)c;
        } else {
            continue;
        }
    }
    return j;
}

/* Word-wrap text to a maximum column width.
 *
 * Wraps at word boundaries (spaces). Existing newlines are preserved.
 * Words longer than the max are hard-wrapped (split mid-word).
 *
 * Assumes the cursor is at column 0 when this output starts. The output
 * buffer must be at least 2 * len + 16 bytes to accommodate inserted
 * newlines.
 *
 * Returns the number of bytes written to `out`.
 */
static unsigned long chat_wrap_text(const char *in, unsigned long len,
                                    char *out, int max_col)
{
    unsigned long i = 0, j = 0;
    int col = 0;

    if (max_col < 10) max_col = 10;

    while (i < len) {
        unsigned char c = (unsigned char)in[i];
        unsigned long word_start, word_len;

        if (c == '\n') {
            out[j++] = '\n';
            col = 0;
            i++;
            continue;
        }
        if (c == '\r') {
            i++;
            continue;
        }
        if (c == ' ') {
            if (col > 0 && col < max_col) {
                out[j++] = ' ';
                col++;
            }
            i++;
            continue;
        }

        word_start = i;
        while (i < len) {
            unsigned char w = (unsigned char)in[i];
            if (w == ' ' || w == '\n' || w == '\r') break;
            i++;
        }
        word_len = i - word_start;
        if (word_len == 0) continue;

        if (col + (int)word_len > max_col && col > 0) {
            out[j++] = '\n';
            col = 0;
        }

        if ((int)word_len > max_col) {
            unsigned long k;
            for (k = 0; k < word_len; k++) {
                if (col >= max_col) {
                    out[j++] = '\n';
                    col = 0;
                }
                out[j++] = in[word_start + k];
                col++;
            }
        } else {
            unsigned long k;
            for (k = 0; k < word_len; k++) {
                out[j++] = in[word_start + k];
            }
            col += (int)word_len;
        }
    }

    return j;
}

#endif /* CHATTEXT_H */

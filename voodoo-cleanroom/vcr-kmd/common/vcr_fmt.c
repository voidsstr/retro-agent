/*
 * vcr_fmt.c - see include/vcr_fmt.h. Integer-only formatter shared by the
 * miniport, the display DLL and the host tests.
 */
#include "../include/vcr_fmt.h"

typedef struct {
    char   *buf;
    vcr_u32 size;
    vcr_u32 len;    /* characters produced (may exceed size - 1) */
} vcr_out;

static void put(vcr_out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void put_num(vcr_out *o, vcr_u32 v, int neg, unsigned base, int upper,
                    int width, int zero, int left)
{
    char tmp[12];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0, pad;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    pad = width - n - (neg ? 1 : 0);
    if (!left && !zero)
        while (pad-- > 0)
            put(o, ' ');
    if (neg)
        put(o, '-');
    if (!left && zero)
        while (pad-- > 0)
            put(o, '0');
    while (n)
        put(o, tmp[--n]);
    if (left)
        while (pad-- > 0)
            put(o, ' ');
}

int vcr_vsnprintf(char *buf, vcr_u32 size, const char *fmt, va_list ap)
{
    vcr_out o;
    o.buf = buf;
    o.size = size;
    o.len = 0;

    while (fmt && *fmt) {
        int width = 0, zero = 0, left = 0;
        char c = *fmt++;
        if (c != '%') {
            put(&o, c);
            continue;
        }
        for (;; fmt++) {
            if (*fmt == '0')
                zero = 1;
            else if (*fmt == '-')
                left = 1;
            else
                break;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h')
            fmt++;
        c = *fmt ? *fmt++ : 0;
        switch (c) {
        case 'd':
        case 'i': {
            vcr_i32 v = va_arg(ap, vcr_i32);
            if (v < 0)
                put_num(&o, (vcr_u32)0 - (vcr_u32)v, 1, 10, 0, width, zero, left);
            else
                put_num(&o, (vcr_u32)v, 0, 10, 0, width, zero, left);
            break;
        }
        case 'u':
            put_num(&o, va_arg(ap, vcr_u32), 0, 10, 0, width, zero, left);
            break;
        case 'x':
        case 'X':
            put_num(&o, va_arg(ap, vcr_u32), 0, 16, c == 'X', width, zero, left);
            break;
        case 'p':
            put_num(&o, (vcr_u32)(unsigned long)va_arg(ap, void *), 0, 16, 0,
                    8, 1, 0);
            break;
        case 'c':
            put(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0;
            if (!s)
                s = "(null)";
            while (s[n])
                n++;
            if (!left)
                while (width-- > n)
                    put(&o, ' ');
            while (*s)
                put(&o, *s++);
            if (left)
                while (width-- > n)
                    put(&o, ' ');
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case 0:
            break;
        default:        /* unknown conversion: show it rather than eat it */
            put(&o, '%');
            put(&o, c);
            break;
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}

int vcr_snprintf(char *buf, vcr_u32 size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vcr_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/*
 * vcr_fmt.h - a tiny printf for the driver pair.
 *
 * The display DLL may import only win32k.sys, which exports no vsnprintf, and
 * the miniport's log calls run at raised IRQL where ntoskrnl's _vsnprintf is
 * not safe for every conversion. So both use this: integer conversions only,
 * no floating point (kernel code must not touch the FPU), no allocation.
 *
 *   %d %i %u %x %X %p %c %s %%, flags '0' '-', a field width, and the size
 *   prefixes 'l' / 'h' (accepted and ignored: every integer here is 32-bit).
 * The output is always NUL-terminated and truncated to the buffer.
 */
#ifndef VCR_FMT_H
#define VCR_FMT_H

#include <stdarg.h>
#include "vcr_types.h"

int vcr_vsnprintf(char *buf, vcr_u32 size, const char *fmt, va_list ap);
int vcr_snprintf(char *buf, vcr_u32 size, const char *fmt, ...);

#endif /* VCR_FMT_H */

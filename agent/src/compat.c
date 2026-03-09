/*
 * compat.c - Win98SE CRT compatibility stubs
 *
 * MinGW-w64 CRT uses EncodePointer/DecodePointer which are only available
 * on WinXP SP2+. We provide no-op stubs so the binary runs on Win98SE.
 * These must be linked before the CRT to override the imports.
 */

#include <windows.h>

/*
 * On Win98, these functions don't exist. The CRT calls them for security
 * hardening (pointer encoding). We stub them as identity functions.
 *
 * We need to suppress the dllimport attribute from the Windows headers
 * since we're providing our own local definitions.
 */

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"

PVOID WINAPI EncodePointer(PVOID ptr)
{
    return ptr;
}

PVOID WINAPI DecodePointer(PVOID ptr)
{
    return ptr;
}

PVOID WINAPI EncodeSystemPointer(PVOID ptr)
{
    return ptr;
}

PVOID WINAPI DecodeSystemPointer(PVOID ptr)
{
    return ptr;
}

#pragma GCC diagnostic pop
#endif

/*
 * _strtoui64 / _strtoi64 are not in Win98SE's msvcrt.dll.
 * MinGW's CRT startup pulls them in via the import library.
 * We provide implementations here AND use an inline asm trick
 * to make them available as import-replacements.
 */
static unsigned __int64 my_strtoui64(const char *nptr, char **endptr, int base)
{
    unsigned __int64 result = 0;
    const char *p = nptr;
    int digit;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p == '+') p++;

    if (base == 0) {
        if (*p == '0') {
            p++;
            if (*p == 'x' || *p == 'X') { base = 16; p++; }
            else base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    while (*p) {
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * (unsigned __int64)base + (unsigned __int64)digit;
        p++;
    }

    if (endptr) *endptr = (char *)p;
    return result;
}

static __int64 my_strtoi64(const char *nptr, char **endptr, int base)
{
    const char *p = nptr;
    int neg = 0;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    {
        unsigned __int64 val = my_strtoui64(p, endptr, base);
        return neg ? -(__int64)val : (__int64)val;
    }
}

unsigned __int64 __cdecl _strtoui64(const char *nptr, char **endptr, int base)
{
    return my_strtoui64(nptr, endptr, base);
}

__int64 __cdecl _strtoi64(const char *nptr, char **endptr, int base)
{
    return my_strtoi64(nptr, endptr, base);
}

unsigned long long __cdecl strtoull(const char *nptr, char **endptr, int base)
{
    return (unsigned long long)my_strtoui64(nptr, endptr, base);
}

long long __cdecl strtoll(const char *nptr, char **endptr, int base)
{
    return (long long)my_strtoi64(nptr, endptr, base);
}

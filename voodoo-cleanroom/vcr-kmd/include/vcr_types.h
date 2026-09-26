/*
 * vcr_types.h - fixed-width types for code shared by the kernel driver pair,
 * the Win32 tools and the host tests. No OS headers: <stdint.h> is not
 * available in every one of those builds, so spell the widths out (ILP32 on
 * the target; the host tests are LP64 and use the same `unsigned int`).
 */
#ifndef VCR_TYPES_H
#define VCR_TYPES_H

typedef unsigned char  vcr_u8;
typedef unsigned short vcr_u16;
typedef unsigned int   vcr_u32;
typedef signed int     vcr_i32;

#define VCR_STATIC_ASSERT(name, cond) typedef char vcr_sa_##name[(cond) ? 1 : -1]

#endif /* VCR_TYPES_H */

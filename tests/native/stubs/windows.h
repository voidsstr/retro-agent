/* Minimal stub: crypto.c includes <windows.h> but its crypto_init/crypto_xor
 * use no Win32 API (only strlen). This empty header lets the REAL agent source
 * compile natively for a true-source regression test. */
#ifndef STUB_WINDOWS_H
#define STUB_WINDOWS_H
#endif

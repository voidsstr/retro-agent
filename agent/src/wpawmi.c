/*
 * wpawmi.c - Win32_WindowsProductActivation, read through WMI. See wpawmi.h.
 *
 * Read-only by construction: this file calls ExecQuery and IWbemClassObject::Get
 * and nothing else. It never invokes a method of the class (ActivateOffline,
 * SetProductKey, ...) - changing activation is the operator's call.
 */

#include "wpawmi.h"
#include "util.h"
#include "../shared/clockguard.h"
#include <wbemcli.h>
#include <string.h>
#include <stdio.h>

/* Local GUIDs: linking libwbemuuid for two constants is not worth it. */
static const GUID WPA_CLSID_WbemLocator =
    { 0x4590f811, 0x1d3a, 0x11d0, { 0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 } };
static const GUID WPA_IID_IWbemLocator =
    { 0xdc12a687, 0x737f, 0x11cf, { 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 } };

typedef HRESULT (WINAPI *wpa_coinit_t)(LPVOID);
typedef void    (WINAPI *wpa_couninit_t)(void);
typedef HRESULT (WINAPI *wpa_cocreate_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef HRESULT (WINAPI *wpa_blanket_t)(IUnknown *, DWORD, DWORD, OLECHAR *, DWORD,
                                        DWORD, RPC_AUTH_IDENTITY_HANDLE, DWORD);
typedef BSTR    (WINAPI *wpa_sysalloc_t)(const OLECHAR *);
typedef void    (WINAPI *wpa_sysfree_t)(BSTR);
typedef HRESULT (WINAPI *wpa_varclear_t)(VARIANTARG *);

int wpa_logon_os(void)
{
    OSVERSIONINFOA v;
    memset(&v, 0, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    if (!GetVersionExA(&v))
        return 0;
    /* GetVersionEx's 6.2 shim (see hostpolicy.h) only ever over-reports
     * Windows 8+, so an honest 5.1/5.2 answer here is trustworthy. */
    return clockguard_wpa_os(v.dwPlatformId == VER_PLATFORM_WIN32_NT,
                             v.dwMajorVersion, v.dwMinorVersion);
}

static void wpa_why(char *why, size_t cch, const char *what, HRESULT hr)
{
    if (!why || !cch)
        return;
    _snprintf(why, cch - 1, "%s (0x%08lX)", what, (unsigned long)hr);
    why[cch - 1] = 0;
}

static int wpa_get_u32(IWbemClassObject *obj, const wchar_t *name,
                       wpa_varclear_t varclear, DWORD *out)
{
    VARIANT v;
    HRESULT hr;
    int ok = 0;
    memset(&v, 0, sizeof(v));     /* VT_EMPTY; VariantInit would be a static oleaut32 import */
    hr = obj->lpVtbl->Get(obj, name, 0, &v, NULL, NULL);
    if (SUCCEEDED(hr)) {
        /* WMI hands a uint32 back as VT_I4 */
        if (V_VT(&v) == VT_I4)       { *out = (DWORD)V_I4(&v);  ok = 1; }
        else if (V_VT(&v) == VT_UI4) { *out = (DWORD)V_UI4(&v); ok = 1; }
        else if (V_VT(&v) == VT_BOOL){ *out = V_BOOL(&v) ? 1 : 0; ok = 1; }
    }
    if (varclear)
        varclear(&v);
    return ok;
}

int wpa_query(DWORD *required, DWORD *grace_days, char *why, size_t why_cch)
{
    HMODULE ole, olea;
    wpa_coinit_t    coinit;
    wpa_couninit_t  couninit;
    wpa_cocreate_t  cocreate;
    wpa_blanket_t   blanket;
    wpa_sysalloc_t  sysalloc;
    wpa_sysfree_t   sysfree;
    wpa_varclear_t  varclear;
    IWbemLocator *loc = NULL;
    IWbemServices *svc = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *obj = NULL;
    BSTR ns = NULL, lang = NULL, query = NULL;
    HRESULT hr, hr_init;
    ULONG n = 0;
    int ok = 0;

    if (why && why_cch)
        why[0] = 0;
    ole  = LoadLibraryA("ole32.dll");
    olea = LoadLibraryA("oleaut32.dll");
    if (!ole || !olea) {
        wpa_why(why, why_cch, "ole32/oleaut32 not loadable", 0);
        goto out_libs;
    }
    coinit   = (wpa_coinit_t)(void *)GetProcAddress(ole, "CoInitialize");
    couninit = (wpa_couninit_t)(void *)GetProcAddress(ole, "CoUninitialize");
    cocreate = (wpa_cocreate_t)(void *)GetProcAddress(ole, "CoCreateInstance");
    blanket  = (wpa_blanket_t)(void *)GetProcAddress(ole, "CoSetProxyBlanket");
    sysalloc = (wpa_sysalloc_t)(void *)GetProcAddress(olea, "SysAllocString");
    sysfree  = (wpa_sysfree_t)(void *)GetProcAddress(olea, "SysFreeString");
    varclear = (wpa_varclear_t)(void *)GetProcAddress(olea, "VariantClear");
    if (!coinit || !couninit || !cocreate || !sysalloc || !sysfree) {
        wpa_why(why, why_cch, "COM entry points missing", 0);
        goto out_libs;
    }

    hr_init = coinit(NULL);        /* S_FALSE = already initialised: still balance it */
    hr = cocreate(&WPA_CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                  &WPA_IID_IWbemLocator, (LPVOID *)&loc);
    if (FAILED(hr) || !loc) {
        wpa_why(why, why_cch, "no WMI locator", hr);
        goto out_com;
    }
    ns = sysalloc(L"ROOT\\CIMV2");
    hr = loc->lpVtbl->ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &svc);
    if (FAILED(hr) || !svc) {
        wpa_why(why, why_cch, "WMI ConnectServer failed", hr);
        goto out_com;
    }
    /* The activation provider needs impersonation, which is not COM's default. */
    if (blanket)
        blanket((IUnknown *)svc, 10 /* RPC_C_AUTHN_WINNT */, 0 /* RPC_C_AUTHZ_NONE */,
                NULL, 3 /* RPC_C_AUTHN_LEVEL_CALL */, 3 /* RPC_C_IMP_LEVEL_IMPERSONATE */,
                NULL, 0 /* EOAC_NONE */);
    lang  = sysalloc(L"WQL");
    query = sysalloc(L"SELECT ActivationRequired, RemainingGracePeriod FROM Win32_WindowsProductActivation");
    hr = svc->lpVtbl->ExecQuery(svc, lang, query,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                NULL, &en);
    if (FAILED(hr) || !en) {
        wpa_why(why, why_cch, "Win32_WindowsProductActivation query failed", hr);
        goto out_com;
    }
    hr = en->lpVtbl->Next(en, 30000, 1, &obj, &n);
    if (FAILED(hr) || n != 1 || !obj) {
        wpa_why(why, why_cch, "Win32_WindowsProductActivation returned no instance", hr);
        goto out_com;
    }
    if (!wpa_get_u32(obj, L"ActivationRequired", varclear, required) ||
        !wpa_get_u32(obj, L"RemainingGracePeriod", varclear, grace_days)) {
        wpa_why(why, why_cch, "activation properties unreadable", 0);
        goto out_com;
    }
    ok = 1;

out_com:
    if (obj)   obj->lpVtbl->Release(obj);
    if (en)    en->lpVtbl->Release(en);
    if (svc)   svc->lpVtbl->Release(svc);
    if (loc)   loc->lpVtbl->Release(loc);
    if (query) sysfree(query);
    if (lang)  sysfree(lang);
    if (ns)    sysfree(ns);
    if (SUCCEEDED(hr_init))
        couninit();
out_libs:
    /* ole32/oleaut32 stay loaded: the process already maps them via other
     * modules, and FreeLibrary on COM DLLs mid-process is asking for trouble. */
    return ok;
}

/*
 * ipxnt.c - IPX/SPX on Windows 2000/XP/2003: NWLink (MS_NWIPX) through INetCfg
 * (agent 1.97.0). Orchestration: ipxsetup.c; decisions: agent/shared/ipxplan.h.
 *
 * The snetcfg sequence (Microsoft's DDK sample), in a worker thread under a
 * watchdog:
 *   CoInitializeEx -> CoCreateInstance(CLSID_CNetCfg, IID_INetCfg)
 *   -> QI INetCfgLock -> AcquireWriteLock (S_FALSE = another program holds it:
 *      say who, retry at the next start, never loop)
 *   -> Initialize -> FindComponent(L"MS_NWIPX"): there already = nothing to do
 *   -> QueryNetCfgClass(GUID_DEVCLASS_NETTRANS, IID_INetCfgClassSetup)
 *   -> Install(L"MS_NWIPX", OBO_USER, NSF_POSTSYSINSTALL) -> Apply, or Cancel
 *   -> Uninitialize -> ReleaseWriteLock -> CoTaskMemFree(holder) -> Release
 * with SetupAPI's non-interactive mode on around Install, so a "Files Needed"
 * prompt cannot park the install on a console nobody is watching (netnwlnk.inf
 * has no CopyFiles, so none is expected).
 *
 * NO NEW STATIC IMPORT: ole32 is LoadLibrary'd and every COM entry point and
 * SetupSetNonInteractiveMode are GetProcAddress'd. The GUIDs are spelled out
 * (ipxplan.h) - no libuuid, no initguid. Win9x never reaches this file (it has
 * no NWLink INF and no INetCfg), but the EXE must still LOAD there:
 * tests/python/test_agent_win9x_imports.py checks the built import table.
 *
 * THERE IS NO REBOOT PATH HERE, deliberately: NETCFG_S_REBOOT is reported as
 * reboot_required and safe-reboot.py (PXE hold + activation check) owns any
 * reboot of an NT box. The NIC is never restarted and the frame type is left
 * on Auto (it is only READ, from NwlnkIpx\Parameters\Adapters\*\PktType).
 */
#define COBJMACROS
#include "ipxsetup.h"
#include "log.h"
#include "ntdyn.h"

#include <objbase.h>
#include <netcfgx.h>
#include <string.h>
#include <stdio.h>

/* GetProcAddress returns FARPROC; the casts below are the point (ntdyn.c). */
#pragma GCC diagnostic ignored "-Wcast-function-type"

static const ipx_guid_t g_clsid_cnetcfg  = IPX_CLSID_CNETCFG;
static const ipx_guid_t g_iid_inetcfg    = IPX_IID_INETCFG;
static const ipx_guid_t g_iid_lock       = IPX_IID_INETCFGLOCK;
static const ipx_guid_t g_iid_classsetup = IPX_IID_INETCFGCLASSSETUP;
static const ipx_guid_t g_guid_nettrans  = IPX_GUID_DEVCLASS_NETTRANS;
typedef char ipxnt_chk_guid[sizeof(ipx_guid_t) == sizeof(GUID) ? 1 : -1];

#define IPXNT_LOCK_WAIT_MS 10000

typedef HRESULT (WINAPI *pCoInitializeEx_t)(LPVOID, DWORD);
typedef HRESULT (WINAPI *pCoInitialize_t)(LPVOID);
typedef HRESULT (WINAPI *pCoCreateInstance_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef void    (WINAPI *pCoUninitialize_t)(void);
typedef void    (WINAPI *pCoTaskMemFree_t)(LPVOID);
typedef BOOL    (WINAPI *pSetupSetNonInteractiveMode_t)(BOOL);

/* ---- observe (registry + SCM only - never COM) --------------------------- */

static int ipxnt_exists(const char *path)
{
    return GetFileAttributesA(path) != 0xFFFFFFFF;
}

void ipxnt_observe(ipxnt_info_t *x)
{
    HKEY h, k;
    DWORD i;
    char win[MAX_PATH], sys[MAX_PATH], path[MAX_PATH + 64];

    memset(x, 0, sizeof(*x));
    x->service_running = -1;

    /* the component: a NetCfg NetTrans instance whose ComponentId is ms_nwipx
     * (IPX_NT_NETTRANS_KEY - Control\Network, keyed by GUID) */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, IPX_NT_NETTRANS_KEY, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        for (i = 0; !x->component_present; i++) {
            char sub[64], id[64];
            DWORD n = sizeof(sub), type = 0, sz = sizeof(id) - 1;
            if (RegEnumKeyExA(h, i, sub, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (RegOpenKeyExA(h, sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
                continue;
            memset(id, 0, sizeof(id));
            if (RegQueryValueExA(k, "ComponentId", NULL, &type, (LPBYTE)id, &sz) == ERROR_SUCCESS
                    && type == REG_SZ && _stricmp(id, IPX_NT_COMPONENT) == 0) {
                x->component_present = 1;
                lstrcpynA(x->comp_key, sub, sizeof(x->comp_key));
            }
            RegCloseKey(k);
        }
        RegCloseKey(h);
    }

    /* the driver service, and whether it is running */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\" IPX_NT_SERVICE,
                      0, KEY_READ, &h) == ERROR_SUCCESS) {
        x->service_present = 1;
        RegCloseKey(h);
    }
    if (ntdyn_scm_available()) {
        SC_HANDLE scm = ntdyn_OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
        if (scm) {
            SC_HANDLE svc = ntdyn_OpenServiceA(scm, IPX_NT_SERVICE, SERVICE_QUERY_STATUS);
            if (svc) {
                SERVICE_STATUS st;
                memset(&st, 0, sizeof(st));
                if (ntdyn_QueryServiceStatus(svc, &st))
                    x->service_running = st.dwCurrentState == SERVICE_RUNNING;
                ntdyn_CloseServiceHandle(svc);
            } else {
                x->service_running = 0;     /* no such service: certainly not running */
            }
            ntdyn_CloseServiceHandle(scm);
        }
    }

    /* the in-box payload (R3: on every imaged box; netnwlnk.inf has no CopyFiles) */
    win[0] = sys[0] = 0;
    GetWindowsDirectoryA(win, sizeof(win));
    GetSystemDirectoryA(sys, sizeof(sys));
    _snprintf(path, sizeof(path) - 1, "%s\\inf\\netnwlnk.inf", win);
    path[sizeof(path) - 1] = 0;
    if (ipxnt_exists(path)) {
        _snprintf(path, sizeof(path) - 1, "%s\\drivers\\nwlnkipx.sys", sys);
        path[sizeof(path) - 1] = 0;
        x->files_present = ipxnt_exists(path);
        if (!x->files_present) {
            _snprintf(path, sizeof(path) - 1, "%s\\Driver Cache\\i386\\driver.cab", win);
            path[sizeof(path) - 1] = 0;
            x->files_present = ipxnt_exists(path);
        }
    }

    /* the frame type per adapter - READ ONLY (left on Auto by decision) */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Services\\" IPX_NT_SERVICE "\\Parameters\\Adapters",
                      0, KEY_READ, &h) == ERROR_SUCCESS) {
        for (i = 0; x->nadapters < 4; i++) {
            char sub[80], pkt[64];
            DWORD n = sizeof(sub), type = 0, sz = sizeof(pkt) - 2;
            if (RegEnumKeyExA(h, i, sub, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            lstrcpynA(x->ad[x->nadapters].name, sub, sizeof(x->ad[0].name));
            x->ad[x->nadapters].frame = IPX_FRAME_AUTO;
            if (RegOpenKeyExA(h, sub, 0, KEY_READ, &k) == ERROR_SUCCESS) {
                memset(pkt, 0, sizeof(pkt));
                if (RegQueryValueExA(k, "PktType", NULL, &type, (LPBYTE)pkt, &sz) == ERROR_SUCCESS
                        && (type == REG_MULTI_SZ || type == REG_SZ))
                    x->ad[x->nadapters].frame = ipx_frame_from_nt(pkt);
                RegCloseKey(k);
            }
            x->nadapters++;
        }
        RegCloseKey(h);
    }
}

/* ---- install (COM, in a worker under a watchdog) ------------------------- */

typedef struct {
    ipxnt_result_t r;
    volatile LONG  done;
} ipxnt_job_t;

static void ipxnt_fail(ipxnt_job_t *j, const char *step, HRESULT hr)
{
    j->r.result = IPXNT_FAILED;
    j->r.hr = (long)hr;
    lstrcpynA(j->r.step, step, sizeof(j->r.step));
}

static DWORD WINAPI ipxnt_worker(LPVOID arg)
{
    ipxnt_job_t *j = (ipxnt_job_t *)arg;
    HMODULE ole, sapi;
    pCoInitializeEx_t coinitex = NULL;
    pCoInitialize_t coinit = NULL;
    pCoCreateInstance_t cocreate = NULL;
    pCoUninitialize_t couninit = NULL;
    pCoTaskMemFree_t taskfree = NULL;
    pSetupSetNonInteractiveMode_t nonint = NULL;
    INetCfg *nc = NULL;
    INetCfgLock *lk = NULL;
    INetCfgClassSetup *cs = NULL;
    INetCfgComponent *comp = NULL;
    LPWSTR holder = NULL;
    OBO_TOKEN obo;
    HRESULT hr;
    int com_up = 0, locked = 0, inited = 0;

    ole = LoadLibraryA("ole32.dll");
    if (!ole) {
        j->r.result = IPXNT_NO_COM;
        lstrcpynA(j->r.step, "LoadLibrary(ole32.dll)", sizeof(j->r.step));
        goto done;
    }
    coinitex = (pCoInitializeEx_t)GetProcAddress(ole, "CoInitializeEx");
    coinit   = (pCoInitialize_t)GetProcAddress(ole, "CoInitialize");
    cocreate = (pCoCreateInstance_t)GetProcAddress(ole, "CoCreateInstance");
    couninit = (pCoUninitialize_t)GetProcAddress(ole, "CoUninitialize");
    taskfree = (pCoTaskMemFree_t)GetProcAddress(ole, "CoTaskMemFree");
    if ((!coinitex && !coinit) || !cocreate || !couninit || !taskfree) {
        j->r.result = IPXNT_NO_COM;
        lstrcpynA(j->r.step, "ole32 exports", sizeof(j->r.step));
        goto done;
    }
    sapi = GetModuleHandleA("setupapi.dll");
    if (!sapi)
        sapi = LoadLibraryA("setupapi.dll");
    if (sapi)
        nonint = (pSetupSetNonInteractiveMode_t)GetProcAddress(sapi, "SetupSetNonInteractiveMode");

    hr = coinitex ? coinitex(NULL, COINIT_APARTMENTTHREADED) : coinit(NULL);
    if (FAILED(hr)) {
        ipxnt_fail(j, "CoInitializeEx", hr);
        goto done;
    }
    com_up = 1;

    hr = cocreate((REFCLSID)&g_clsid_cnetcfg, NULL, CLSCTX_INPROC_SERVER,
                  (REFIID)&g_iid_inetcfg, (void **)&nc);
    if (FAILED(hr) || !nc) {
        j->r.result = IPXNT_NO_COM;
        j->r.hr = (long)hr;
        lstrcpynA(j->r.step, "CoCreateInstance(CNetCfg)", sizeof(j->r.step));
        goto done;
    }
    hr = INetCfg_QueryInterface(nc, (REFIID)&g_iid_lock, (void **)&lk);
    if (FAILED(hr) || !lk) {
        ipxnt_fail(j, "QueryInterface(INetCfgLock)", hr);
        goto done;
    }
    hr = INetCfgLock_AcquireWriteLock(lk, IPXNT_LOCK_WAIT_MS, L"RetroAgent IPXSETUP", &holder);
    if (hr == S_FALSE) {
        j->r.result = IPXNT_BUSY;
        j->r.hr = (long)hr;
        lstrcpynA(j->r.step, "AcquireWriteLock", sizeof(j->r.step));
        if (holder)
            WideCharToMultiByte(CP_ACP, 0, holder, -1, j->r.holder, sizeof(j->r.holder) - 1,
                                NULL, NULL);
        goto done;
    }
    if (FAILED(hr)) {
        ipxnt_fail(j, "AcquireWriteLock", hr);
        goto done;
    }
    locked = 1;
    hr = INetCfg_Initialize(nc, NULL);
    if (FAILED(hr)) {
        ipxnt_fail(j, "INetCfg::Initialize", hr);
        goto done;
    }
    inited = 1;

    /* IDEMPOTENT: an installed component is left exactly as it is. */
    hr = INetCfg_FindComponent(nc, L"MS_NWIPX", &comp);
    if (hr == S_OK) {
        j->r.result = IPXNT_ALREADY;
        goto done;
    }
    if (comp) {
        INetCfgComponent_Release(comp);
        comp = NULL;
    }

    hr = INetCfg_QueryNetCfgClass(nc, (const GUID *)&g_guid_nettrans,
                                  (REFIID)&g_iid_classsetup, (void **)&cs);
    if (FAILED(hr) || !cs) {
        ipxnt_fail(j, "QueryNetCfgClass(NetTrans)", hr);
        goto done;
    }
    memset(&obo, 0, sizeof(obo));
    obo.Type = OBO_USER;
    {
        BOOL was = FALSE;
        int install_reboot = 0;
        if (nonint)
            was = nonint(TRUE);
        hr = INetCfgClassSetup_Install(cs, L"MS_NWIPX", &obo, IPX_NSF_POSTSYSINSTALL, 0,
                                       NULL, NULL, &comp);
        if (SUCCEEDED(hr)) {
            /* Install can itself answer NETCFG_S_REBOOT; Apply then says S_OK.
             * Keep it - the reboot is still owed (snetcfg does the same). */
            install_reboot = hr == NETCFG_S_REBOOT;
            hr = INetCfg_Apply(nc);
            if (FAILED(hr))
                ipxnt_fail(j, "INetCfg::Apply", hr);
            else if (install_reboot)
                hr = NETCFG_S_REBOOT;
        } else {
            ipxnt_fail(j, "INetCfgClassSetup::Install(MS_NWIPX)", hr);
            INetCfg_Cancel(nc);
        }
        if (nonint)
            nonint(was);
    }
    if (SUCCEEDED(hr))
        j->r.result = hr == NETCFG_S_REBOOT ? IPXNT_REBOOT : IPXNT_INSTALLED;
    j->r.hr = (long)hr;

done:
    if (comp)
        INetCfgComponent_Release(comp);
    if (cs)
        INetCfgClassSetup_Release(cs);
    if (inited)
        INetCfg_Uninitialize(nc);
    if (locked)
        INetCfgLock_ReleaseWriteLock(lk);
    if (holder && taskfree)
        taskfree(holder);
    if (lk)
        INetCfgLock_Release(lk);
    if (nc)
        INetCfg_Release(nc);
    if (com_up && couninit)
        couninit();
    /* ole32 stays loaded: a COM server may still hold a reference to it. */
    InterlockedExchange(&j->done, 1);
    return 0;
}

int ipxnt_install(ipxnt_result_t *r, DWORD watchdog_ms)
{
    ipxnt_job_t *j;
    HANDLE th;
    DWORD tid = 0;

    memset(r, 0, sizeof(*r));
    j = (ipxnt_job_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*j));
    if (!j) {
        r->result = IPXNT_FAILED;
        lstrcpynA(r->step, "out of memory", sizeof(r->step));
        return r->result;
    }
    j->r.result = IPXNT_FAILED;
    th = CreateThread(NULL, 0, ipxnt_worker, j, 0, &tid);
    if (!th) {
        r->result = IPXNT_FAILED;
        _snprintf(r->step, sizeof(r->step) - 1, "CreateThread error %lu",
                  (unsigned long)GetLastError());
        HeapFree(GetProcessHeap(), 0, j);
        return r->result;
    }
    if (WaitForSingleObject(th, watchdog_ms) != WAIT_OBJECT_0) {
        /* Still inside INetCfg: leak the job and the thread (as gs_force_install
         * does) - freeing either under it would crash the agent when it returns. */
        CloseHandle(th);
        r->result = IPXNT_HUNG;
        lstrcpynA(r->step, "INetCfg did not return", sizeof(r->step));
        return r->result;
    }
    CloseHandle(th);
    *r = j->r;
    HeapFree(GetProcessHeap(), 0, j);
    return r->result;
}

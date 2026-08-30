/*
 * ntdyn.c - runtime resolution of the Win32 entry points Windows 9x lacks.
 *
 * See ntdyn.h for why this file exists. In short: a static import the loader
 * cannot resolve kills the process at LOAD time, silently, and Win98SE's
 * advapi32.dll exports no Service Control Manager at all.
 */

#include "ntdyn.h"
#include "log.h"

#include <cfgmgr32.h>

/* GetProcAddress returns FARPROC; casting it to a specific prototype is
 * unavoidable and exactly what every other dynamic-load site here does. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"

/* ================================================================
 * Service Control Manager
 * ================================================================ */

typedef SC_HANDLE (WINAPI *pfn_OpenSCManagerA_t)(LPCSTR, LPCSTR, DWORD);
typedef SC_HANDLE (WINAPI *pfn_OpenServiceA_t)(SC_HANDLE, LPCSTR, DWORD);
typedef BOOL      (WINAPI *pfn_CloseServiceHandle_t)(SC_HANDLE);
typedef BOOL      (WINAPI *pfn_QueryServiceStatus_t)(SC_HANDLE, LPSERVICE_STATUS);
typedef BOOL      (WINAPI *pfn_ControlService_t)(SC_HANDLE, DWORD, LPSERVICE_STATUS);
typedef BOOL      (WINAPI *pfn_ChangeServiceConfigA_t)(
    SC_HANDLE, DWORD, DWORD, DWORD, LPCSTR, LPCSTR, LPDWORD,
    LPCSTR, LPCSTR, LPCSTR, LPCSTR);

static struct {
    int                        loaded;
    pfn_OpenSCManagerA_t       pOpenSCManager;
    pfn_OpenServiceA_t         pOpenService;
    pfn_CloseServiceHandle_t   pCloseServiceHandle;
    pfn_QueryServiceStatus_t   pQueryServiceStatus;
    pfn_ControlService_t       pControlService;
    pfn_ChangeServiceConfigA_t pChangeServiceConfig;
} g_scm = {0, NULL, NULL, NULL, NULL, NULL, NULL};

static void scm_load(void)
{
    HMODULE h;

    if (g_scm.loaded)
        return;
    g_scm.loaded = 1;

    h = GetModuleHandleA("advapi32.dll");
    if (!h)
        h = LoadLibraryA("advapi32.dll");
    if (!h) {
        log_msg(LOG_MAIN, "ntdyn: advapi32.dll not loadable - no SCM");
        return;
    }

    g_scm.pOpenSCManager      = (pfn_OpenSCManagerA_t)
        GetProcAddress(h, "OpenSCManagerA");
    g_scm.pOpenService        = (pfn_OpenServiceA_t)
        GetProcAddress(h, "OpenServiceA");
    g_scm.pCloseServiceHandle = (pfn_CloseServiceHandle_t)
        GetProcAddress(h, "CloseServiceHandle");
    g_scm.pQueryServiceStatus = (pfn_QueryServiceStatus_t)
        GetProcAddress(h, "QueryServiceStatus");
    g_scm.pControlService     = (pfn_ControlService_t)
        GetProcAddress(h, "ControlService");
    g_scm.pChangeServiceConfig = (pfn_ChangeServiceConfigA_t)
        GetProcAddress(h, "ChangeServiceConfigA");

    if (!g_scm.pOpenSCManager)
        log_msg(LOG_MAIN, "ntdyn: this Windows has no Service Control Manager "
                          "(expected on 9x/ME) - service work will be skipped");
}

int ntdyn_scm_available(void)
{
    scm_load();
    /* Every SCM caller here opens the manager, opens a service, queries it and
     * closes the handles, so "available" means the whole set resolved. A
     * partial set would leave a NULL pointer to be called later, which is the
     * failure mode this module exists to remove. */
    return g_scm.pOpenSCManager && g_scm.pOpenService
        && g_scm.pCloseServiceHandle && g_scm.pQueryServiceStatus
        && g_scm.pControlService && g_scm.pChangeServiceConfig;
}

SC_HANDLE ntdyn_OpenSCManagerA(LPCSTR machine, LPCSTR database, DWORD access)
{
    scm_load();
    if (!g_scm.pOpenSCManager)
        return NULL;
    return g_scm.pOpenSCManager(machine, database, access);
}

SC_HANDLE ntdyn_OpenServiceA(SC_HANDLE scm, LPCSTR name, DWORD access)
{
    scm_load();
    if (!g_scm.pOpenService)
        return NULL;
    return g_scm.pOpenService(scm, name, access);
}

BOOL ntdyn_CloseServiceHandle(SC_HANDLE h)
{
    scm_load();
    if (!g_scm.pCloseServiceHandle)
        return FALSE;
    return g_scm.pCloseServiceHandle(h);
}

BOOL ntdyn_QueryServiceStatus(SC_HANDLE svc, LPSERVICE_STATUS status)
{
    scm_load();
    if (!g_scm.pQueryServiceStatus)
        return FALSE;
    return g_scm.pQueryServiceStatus(svc, status);
}

BOOL ntdyn_ControlService(SC_HANDLE svc, DWORD control, LPSERVICE_STATUS status)
{
    scm_load();
    if (!g_scm.pControlService)
        return FALSE;
    return g_scm.pControlService(svc, control, status);
}

BOOL ntdyn_ChangeServiceConfigA(SC_HANDLE svc, DWORD type, DWORD start,
                                DWORD error_control, LPCSTR path,
                                LPCSTR load_order_group, LPDWORD tag_id,
                                LPCSTR dependencies, LPCSTR start_name,
                                LPCSTR password, LPCSTR display_name)
{
    scm_load();
    if (!g_scm.pChangeServiceConfig)
        return FALSE;
    return g_scm.pChangeServiceConfig(svc, type, start, error_control, path,
                                      load_order_group, tag_id, dependencies,
                                      start_name, password, display_name);
}

/* ================================================================
 * Configuration Manager
 * ================================================================ */

typedef DWORD (WINAPI *pfn_CM_Get_DevNode_Status_t)(
    PULONG pulStatus, PULONG pulProblemNumber, DWORD dnDevInst, ULONG ulFlags);

static pfn_CM_Get_DevNode_Status_t g_cm_status = NULL;
static int g_cm_loaded = 0;

static void cm_load(void)
{
    HMODULE hmod;

    if (g_cm_loaded)
        return;
    g_cm_loaded = 1;

    /* cfgmgr32 first: that is where Win98SE keeps it. On NT both DLLs export
     * it (setupapi forwards), so trying 9x's home first costs nothing. */
    hmod = LoadLibraryA("cfgmgr32.dll");
    if (hmod) {
        g_cm_status = (pfn_CM_Get_DevNode_Status_t)
            GetProcAddress(hmod, "CM_Get_DevNode_Status");
        if (g_cm_status) {
            log_msg(LOG_VIDEO, "CM_Get_DevNode_Status loaded from cfgmgr32.dll");
            return;
        }
    }

    hmod = LoadLibraryA("setupapi.dll");
    if (hmod) {
        g_cm_status = (pfn_CM_Get_DevNode_Status_t)
            GetProcAddress(hmod, "CM_Get_DevNode_Status");
        if (g_cm_status) {
            log_msg(LOG_VIDEO, "CM_Get_DevNode_Status loaded from setupapi.dll");
            return;
        }
    }

    log_msg(LOG_VIDEO, "CM_Get_DevNode_Status not available");
}

int ntdyn_cm_available(void)
{
    cm_load();
    return g_cm_status != NULL;
}

DWORD ntdyn_CM_Get_DevNode_Status(PULONG status, PULONG problem,
                                  DWORD devinst, ULONG flags)
{
    cm_load();
    if (!g_cm_status)
        return CR_FAILURE;
    return g_cm_status(status, problem, devinst, flags);
}

#pragma GCC diagnostic pop

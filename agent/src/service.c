/*
 * service.c - Auto-login configuration and Windows service management
 *
 * Agent commands:
 *   AUTOLOGIN enable <username> [password]  - Configure auto-login
 *   AUTOLOGIN disable                       - Remove auto-login
 *   AUTOLOGIN status                        - Query current config
 *   SERVICE install                         - Install as NT service
 *   SERVICE remove                          - Remove NT service
 *   SERVICE status                          - Query service state
 *
 * Also provides the NT service entry point (try_service_start) so the
 * same binary works as both a console app and a Windows service.
 */

#include "handlers.h"
#include <winsvc.h>
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

#define LOG_SVC "SVC"

#define SVC_NAME    "RetroAgent"
#define SVC_DISPLAY "Retro Agent Remote Management"

/* Run key for console-mode auto-start */
#define RUN_KEY   "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE "RetroAgent"

/* Externs from main.c */
extern char g_secret[256];
extern char g_logfile[256];

/* Service mode flag - set when running as an NT service */
int g_service_mode = 0;


/* ================================================================
 * Dynamic NT Service API
 * These functions don't exist in Win98's advapi32.dll.
 * We load them at runtime to stay compatible with Win9x.
 * ================================================================ */

typedef SC_HANDLE (WINAPI *pfn_OpenSCManagerA)(LPCSTR, LPCSTR, DWORD);
typedef SC_HANDLE (WINAPI *pfn_CreateServiceA)(
    SC_HANDLE, LPCSTR, LPCSTR, DWORD, DWORD, DWORD, DWORD,
    LPCSTR, LPCSTR, LPDWORD, LPCSTR, LPCSTR, LPCSTR);
typedef SC_HANDLE (WINAPI *pfn_OpenServiceA)(SC_HANDLE, LPCSTR, DWORD);
typedef BOOL (WINAPI *pfn_DeleteService)(SC_HANDLE);
typedef BOOL (WINAPI *pfn_ControlService)(SC_HANDLE, DWORD, LPSERVICE_STATUS);
typedef BOOL (WINAPI *pfn_CloseServiceHandle)(SC_HANDLE);
typedef BOOL (WINAPI *pfn_QueryServiceStatus)(SC_HANDLE, LPSERVICE_STATUS);
typedef BOOL (WINAPI *pfn_StartServiceCtrlDispatcherA)(
    const SERVICE_TABLE_ENTRYA *);
typedef SERVICE_STATUS_HANDLE (WINAPI *pfn_RegisterServiceCtrlHandlerA)(
    LPCSTR, LPHANDLER_FUNCTION);
typedef BOOL (WINAPI *pfn_SetServiceStatus)(
    SERVICE_STATUS_HANDLE, LPSERVICE_STATUS);
typedef BOOL (WINAPI *pfn_ChangeServiceConfig2A)(
    SC_HANDLE, DWORD, LPVOID);

static struct {
    int loaded;
    pfn_OpenSCManagerA              pOpenSCManager;
    pfn_CreateServiceA              pCreateService;
    pfn_OpenServiceA                pOpenService;
    pfn_DeleteService               pDeleteService;
    pfn_ControlService              pControlService;
    pfn_CloseServiceHandle          pCloseServiceHandle;
    pfn_QueryServiceStatus          pQueryServiceStatus;
    pfn_StartServiceCtrlDispatcherA pStartSCDispatcher;
    pfn_RegisterServiceCtrlHandlerA pRegisterSCHandler;
    pfn_SetServiceStatus            pSetServiceStatus;
    pfn_ChangeServiceConfig2A       pChangeServiceConfig2;
} svc = {0};

/* Suppress -Wcast-function-type for GetProcAddress casts (unavoidable) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"

static int load_svc_api(void)
{
    HMODULE h;
    if (svc.loaded) return svc.pOpenSCManager != NULL;
    svc.loaded = 1;

    h = GetModuleHandleA("advapi32.dll");
    if (!h) h = LoadLibraryA("advapi32.dll");
    if (!h) return 0;

    svc.pOpenSCManager       = (pfn_OpenSCManagerA)
        GetProcAddress(h, "OpenSCManagerA");
    svc.pCreateService       = (pfn_CreateServiceA)
        GetProcAddress(h, "CreateServiceA");
    svc.pOpenService         = (pfn_OpenServiceA)
        GetProcAddress(h, "OpenServiceA");
    svc.pDeleteService       = (pfn_DeleteService)
        GetProcAddress(h, "DeleteService");
    svc.pControlService      = (pfn_ControlService)
        GetProcAddress(h, "ControlService");
    svc.pCloseServiceHandle  = (pfn_CloseServiceHandle)
        GetProcAddress(h, "CloseServiceHandle");
    svc.pQueryServiceStatus  = (pfn_QueryServiceStatus)
        GetProcAddress(h, "QueryServiceStatus");
    svc.pStartSCDispatcher   = (pfn_StartServiceCtrlDispatcherA)
        GetProcAddress(h, "StartServiceCtrlDispatcherA");
    svc.pRegisterSCHandler   = (pfn_RegisterServiceCtrlHandlerA)
        GetProcAddress(h, "RegisterServiceCtrlHandlerA");
    svc.pSetServiceStatus    = (pfn_SetServiceStatus)
        GetProcAddress(h, "SetServiceStatus");
    svc.pChangeServiceConfig2 = (pfn_ChangeServiceConfig2A)
        GetProcAddress(h, "ChangeServiceConfig2A");

    return svc.pOpenSCManager != NULL;
}

#pragma GCC diagnostic pop


/* ================================================================
 * Helpers
 * ================================================================ */

static int is_nt(void)
{
    OSVERSIONINFOA osvi;
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    return (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT);
}

static const char *svc_state_str(DWORD state)
{
    switch (state) {
    case SERVICE_STOPPED:          return "stopped";
    case SERVICE_START_PENDING:    return "start_pending";
    case SERVICE_STOP_PENDING:     return "stop_pending";
    case SERVICE_RUNNING:          return "running";
    case SERVICE_CONTINUE_PENDING: return "continue_pending";
    case SERVICE_PAUSE_PENDING:    return "pause_pending";
    case SERVICE_PAUSED:           return "paused";
    default:                       return "unknown";
    }
}


/* ================================================================
 * AUTOLOGIN command
 * ================================================================ */

static void autologin_status(json_t *j)
{
    json_kv_str(j, "action", "status");

    if (is_nt()) {
        HKEY hkey;
        char val[256];
        DWORD vsize, vtype;

        json_kv_str(j, "platform", "nt");

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                0, KEY_READ, &hkey) == ERROR_SUCCESS) {

            vsize = sizeof(val);
            if (RegQueryValueExA(hkey, "AutoAdminLogon", NULL, &vtype,
                    (BYTE *)val, &vsize) == ERROR_SUCCESS) {
                json_kv_str(j, "auto_admin_logon", val);
                json_kv_bool(j, "enabled", strcmp(val, "1") == 0);
            } else {
                json_kv_bool(j, "enabled", 0);
            }

            vsize = sizeof(val);
            if (RegQueryValueExA(hkey, "DefaultUserName", NULL, &vtype,
                    (BYTE *)val, &vsize) == ERROR_SUCCESS)
                json_kv_str(j, "default_username", val);

            vsize = sizeof(val);
            if (RegQueryValueExA(hkey, "DefaultDomainName", NULL, &vtype,
                    (BYTE *)val, &vsize) == ERROR_SUCCESS)
                json_kv_str(j, "default_domain", val);

            /* Don't expose password - just report whether one is set */
            vsize = sizeof(val);
            json_kv_bool(j, "password_set",
                RegQueryValueExA(hkey, "DefaultPassword", NULL, &vtype,
                    (BYTE *)val, &vsize) == ERROR_SUCCESS && vsize > 1);

            RegCloseKey(hkey);
        }
    } else {
        HKEY hkey;
        char val[256];
        DWORD vsize, vtype, dval, dsize;

        json_kv_str(j, "platform", "win9x");

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "Network\\Logon", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {

            vsize = sizeof(val);
            if (RegQueryValueExA(hkey, "username", NULL, &vtype,
                    (BYTE *)val, &vsize) == ERROR_SUCCESS)
                json_kv_str(j, "username", val);

            dsize = sizeof(dval);
            if (RegQueryValueExA(hkey, "MustBeValidated", NULL, &vtype,
                    (BYTE *)&dval, &dsize) == ERROR_SUCCESS)
                json_kv_uint(j, "must_be_validated", dval);

            vsize = sizeof(val);
            if (RegQueryValueExA(hkey, "PrimaryProvider", NULL, &vtype,
                    (BYTE *)val, &vsize) == ERROR_SUCCESS) {
                json_kv_str(j, "primary_provider", val);
                json_kv_bool(j, "enabled", val[0] == '\0');
            } else {
                json_kv_bool(j, "enabled", 0);
            }

            RegCloseKey(hkey);
        }
    }
}

static void autologin_enable(json_t *j, const char *username,
                             const char *password)
{
    json_kv_str(j, "action", "enable");
    json_kv_str(j, "username", username);

    if (is_nt()) {
        HKEY hkey;
        LONG rc;
        char compname[256];
        DWORD cn_size = sizeof(compname);

        json_kv_str(j, "platform", "nt");

        rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
            0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL);

        if (rc == ERROR_SUCCESS) {
            RegSetValueExA(hkey, "AutoAdminLogon", 0, REG_SZ,
                (const BYTE *)"1", 2);
            RegSetValueExA(hkey, "DefaultUserName", 0, REG_SZ,
                (const BYTE *)username, (DWORD)strlen(username) + 1);
            RegSetValueExA(hkey, "DefaultPassword", 0, REG_SZ,
                (const BYTE *)password, (DWORD)strlen(password) + 1);

            GetComputerNameA(compname, &cn_size);
            RegSetValueExA(hkey, "DefaultDomainName", 0, REG_SZ,
                (const BYTE *)compname, (DWORD)strlen(compname) + 1);

            RegCloseKey(hkey);
            json_kv_bool(j, "success", 1);
            log_msg(LOG_SVC, "AUTOLOGIN: NT auto-login enabled for '%s'",
                    username);
        } else {
            json_kv_bool(j, "success", 0);
            json_kv_str(j, "error", "Failed to open Winlogon registry key");
            log_msg(LOG_SVC, "AUTOLOGIN: RegCreateKeyEx failed: %ld", rc);
        }
    } else {
        /* Win9x: bypass network login by setting Windows Logon as primary */
        HKEY hkey;
        LONG rc;
        DWORD zero = 0;

        json_kv_str(j, "platform", "win9x");

        rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
            "Network\\Logon", 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL);

        if (rc == ERROR_SUCCESS) {
            /* Empty PrimaryProvider = Windows Logon = no login dialog */
            RegSetValueExA(hkey, "PrimaryProvider", 0, REG_SZ,
                (const BYTE *)"", 1);
            RegSetValueExA(hkey, "username", 0, REG_SZ,
                (const BYTE *)username, (DWORD)strlen(username) + 1);
            RegSetValueExA(hkey, "MustBeValidated", 0, REG_DWORD,
                (const BYTE *)&zero, sizeof(zero));

            RegCloseKey(hkey);

            /* Delete .PWL password cache files to avoid stale prompts */
            {
                char windir[MAX_PATH];
                char pattern[MAX_PATH];
                WIN32_FIND_DATAA fd;
                HANDLE hFind;
                int deleted = 0;

                GetWindowsDirectoryA(windir, sizeof(windir));
                _snprintf(pattern, sizeof(pattern), "%s\\*.PWL", windir);

                hFind = FindFirstFileA(pattern, &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        char filepath[MAX_PATH];
                        _snprintf(filepath, sizeof(filepath),
                                  "%s\\%s", windir, fd.cFileName);
                        if (DeleteFileA(filepath)) {
                            log_msg(LOG_SVC, "Deleted PWL: %s", filepath);
                            deleted++;
                        }
                    } while (FindNextFileA(hFind, &fd));
                    FindClose(hFind);
                }
                json_kv_int(j, "pwl_files_deleted", deleted);
            }

            json_kv_bool(j, "success", 1);
            log_msg(LOG_SVC, "AUTOLOGIN: Win9x auto-login enabled for '%s'",
                    username);
        } else {
            json_kv_bool(j, "success", 0);
            json_kv_str(j, "error",
                         "Failed to open Network\\Logon registry key");
        }
    }
}

static void autologin_disable(json_t *j)
{
    json_kv_str(j, "action", "disable");

    if (is_nt()) {
        HKEY hkey;

        json_kv_str(j, "platform", "nt");

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                0, KEY_WRITE, &hkey) == ERROR_SUCCESS) {
            RegSetValueExA(hkey, "AutoAdminLogon", 0, REG_SZ,
                (const BYTE *)"0", 2);
            RegDeleteValueA(hkey, "DefaultPassword");
            RegCloseKey(hkey);
            json_kv_bool(j, "success", 1);
            log_msg(LOG_SVC, "AUTOLOGIN: NT auto-login disabled");
        } else {
            json_kv_bool(j, "success", 0);
        }
    } else {
        HKEY hkey;
        DWORD one = 1;

        json_kv_str(j, "platform", "win9x");

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "Network\\Logon", 0, KEY_WRITE, &hkey) == ERROR_SUCCESS) {
            RegSetValueExA(hkey, "MustBeValidated", 0, REG_DWORD,
                (const BYTE *)&one, sizeof(one));
            RegCloseKey(hkey);
            json_kv_bool(j, "success", 1);
            json_kv_str(j, "note",
                "MustBeValidated set to 1. Use Network control panel "
                "to change Primary Network Logon if needed.");
            log_msg(LOG_SVC, "AUTOLOGIN: Win9x validation re-enabled");
        } else {
            json_kv_bool(j, "success", 0);
        }
    }
}

void handle_autologin(SOCKET sock, const char *args)
{
    char action[16];
    json_t j;
    char *result;

    if (!args || !args[0]) {
        send_error_response(sock,
            "AUTOLOGIN requires: enable <user> [pass] | disable | status");
        return;
    }

    if (sscanf(args, "%15s", action) < 1) {
        send_error_response(sock, "AUTOLOGIN requires an action");
        return;
    }

    json_init(&j);
    json_object_start(&j);

    if (_stricmp(action, "status") == 0) {
        autologin_status(&j);
    }
    else if (_stricmp(action, "enable") == 0) {
        char username[128] = "";
        const char *password = "";
        const char *p;
        int i;

        /* Parse: "enable <username> [password with spaces]" */
        p = str_skip_spaces(args + 6);
        if (!p || !*p) {
            json_free(&j);
            send_error_response(sock,
                "AUTOLOGIN enable requires a username");
            return;
        }

        /* First word = username */
        for (i = 0; *p && *p != ' ' && i < (int)sizeof(username) - 1; i++)
            username[i] = *p++;
        username[i] = '\0';

        /* Rest = password (may contain spaces) */
        if (*p == ' ')
            password = str_skip_spaces(p + 1);
        if (!password) password = "";

        autologin_enable(&j, username, password);
    }
    else if (_stricmp(action, "disable") == 0) {
        autologin_disable(&j);
    }
    else {
        json_free(&j);
        send_error_response(sock,
            "Unknown action. Use: enable <user> [pass] | disable | status");
        return;
    }

    json_object_end(&j);
    result = json_finish(&j);
    if (result) {
        send_text_response(sock, result);
        HeapFree(GetProcessHeap(), 0, result);
    } else {
        send_error_response(sock, "Out of memory");
    }
}


/* ================================================================
 * SERVICE command
 * ================================================================ */

static void service_install(json_t *j)
{
    SC_HANDLE hSCM, hSvc;
    char exe_path[MAX_PATH];
    char image_path[1024];

    json_kv_str(j, "action", "install");

    if (!is_nt()) {
        json_kv_bool(j, "success", 0);
        json_kv_str(j, "error",
            "Windows services not supported on Win9x. "
            "Use auto-login + Run key for auto-start.");
        return;
    }

    if (!load_svc_api()) {
        json_kv_bool(j, "success", 0);
        json_kv_str(j, "error", "Service API not available");
        return;
    }

    /* Build ImagePath from current exe + args */
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    _snprintf(image_path, sizeof(image_path), "\"%s\"", exe_path);

    /* Preserve non-default args in service ImagePath */
    if (strcmp(g_secret, "retro-agent-secret") != 0) {
        char tmp[300];
        _snprintf(tmp, sizeof(tmp), " -s \"%s\"", g_secret);
        strncat(image_path, tmp, sizeof(image_path) - strlen(image_path) - 1);
    }
    if (g_logfile[0]) {
        char tmp[300];
        _snprintf(tmp, sizeof(tmp), " -l \"%s\"", g_logfile);
        strncat(image_path, tmp, sizeof(image_path) - strlen(image_path) - 1);
    }

    log_msg(LOG_SVC, "SERVICE install: ImagePath=%s", image_path);

    hSCM = svc.pOpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCM) {
        char err[128];
        _snprintf(err, sizeof(err), "OpenSCManager failed: %lu",
                  (unsigned long)GetLastError());
        json_kv_bool(j, "success", 0);
        json_kv_str(j, "error", err);
        return;
    }

    hSvc = svc.pCreateService(
        hSCM,
        SVC_NAME,
        SVC_DISPLAY,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        image_path,
        NULL, NULL, NULL, NULL, NULL);

    if (!hSvc) {
        DWORD gle = GetLastError();
        if (gle == ERROR_SERVICE_EXISTS) {
            json_kv_bool(j, "success", 1);
            json_kv_str(j, "note", "Service already exists");
            log_msg(LOG_SVC, "SERVICE install: already exists");
        } else {
            char err[128];
            _snprintf(err, sizeof(err), "CreateService failed: %lu",
                      (unsigned long)gle);
            json_kv_bool(j, "success", 0);
            json_kv_str(j, "error", err);
        }
        svc.pCloseServiceHandle(hSCM);
        return;
    }

    /* Set service description */
    if (svc.pChangeServiceConfig2) {
        SERVICE_DESCRIPTIONA desc;
        desc.lpDescription = (LPSTR)"Remote management agent for retro PCs";
        svc.pChangeServiceConfig2(hSvc, SERVICE_CONFIG_DESCRIPTION, &desc);
    }

    svc.pCloseServiceHandle(hSvc);
    svc.pCloseServiceHandle(hSCM);

    /* Remove Run key entry (service handles auto-start now) */
    {
        HKEY hkey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, RUN_KEY, 0, KEY_WRITE,
                          &hkey) == ERROR_SUCCESS) {
            if (RegDeleteValueA(hkey, RUN_VALUE) == ERROR_SUCCESS) {
                json_kv_bool(j, "run_key_removed", 1);
                log_msg(LOG_SVC, "Removed Run key: %s\\%s", RUN_KEY,
                        RUN_VALUE);
            }
            RegCloseKey(hkey);
        }
    }

    json_kv_bool(j, "success", 1);
    json_kv_str(j, "service_name", SVC_NAME);
    json_kv_str(j, "image_path", image_path);
    json_kv_str(j, "start_type", "auto");
    json_kv_str(j, "note",
        "Service installed. It will start automatically on next boot. "
        "Use 'net start RetroAgent' to start immediately, or reboot.");
    log_msg(LOG_SVC, "SERVICE install: success");
}

static void service_remove(json_t *j)
{
    SC_HANDLE hSCM, hSvc;
    SERVICE_STATUS status;
    char exe_path[MAX_PATH];

    json_kv_str(j, "action", "remove");

    if (!is_nt()) {
        json_kv_bool(j, "success", 0);
        json_kv_str(j, "error", "Windows services not supported on Win9x");
        return;
    }

    if (!load_svc_api()) {
        json_kv_bool(j, "success", 0);
        json_kv_str(j, "error", "Service API not available");
        return;
    }

    hSCM = svc.pOpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        json_kv_bool(j, "success", 0);
        json_kv_str(j, "error", "OpenSCManager failed");
        return;
    }

    hSvc = svc.pOpenService(hSCM, SVC_NAME,
                            SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!hSvc) {
        DWORD gle = GetLastError();
        if (gle == ERROR_SERVICE_DOES_NOT_EXIST) {
            json_kv_bool(j, "success", 1);
            json_kv_str(j, "note", "Service was not installed");
        } else {
            char err[128];
            _snprintf(err, sizeof(err), "OpenService failed: %lu",
                      (unsigned long)gle);
            json_kv_bool(j, "success", 0);
            json_kv_str(j, "error", err);
        }
        svc.pCloseServiceHandle(hSCM);
        return;
    }

    /* Stop the service if running */
    if (svc.pQueryServiceStatus(hSvc, &status) &&
        status.dwCurrentState != SERVICE_STOPPED) {
        svc.pControlService(hSvc, SERVICE_CONTROL_STOP, &status);
        log_msg(LOG_SVC, "SERVICE remove: sent stop signal");
        /* Brief wait for stop */
        Sleep(1000);
    }

    if (svc.pDeleteService(hSvc)) {
        json_kv_bool(j, "success", 1);
        log_msg(LOG_SVC, "SERVICE remove: deleted");
    } else {
        char err[128];
        _snprintf(err, sizeof(err), "DeleteService failed: %lu",
                  (unsigned long)GetLastError());
        json_kv_bool(j, "success", 0);
        json_kv_str(j, "error", err);
    }

    svc.pCloseServiceHandle(hSvc);
    svc.pCloseServiceHandle(hSCM);

    /* Restore Run key so agent starts on next login */
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    {
        HKEY hkey;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, RUN_KEY, 0, NULL, 0,
                            KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS) {
            RegSetValueExA(hkey, RUN_VALUE, 0, REG_SZ,
                (const BYTE *)exe_path, (DWORD)strlen(exe_path) + 1);
            RegCloseKey(hkey);
            json_kv_bool(j, "run_key_restored", 1);
            log_msg(LOG_SVC, "Restored Run key: %s", exe_path);
        }
    }
}

static void service_status_query(json_t *j)
{
    SC_HANDLE hSCM, hSvc;
    SERVICE_STATUS status;

    json_kv_str(j, "action", "status");
    json_kv_bool(j, "running_as_service", g_service_mode);

    if (!is_nt()) {
        json_kv_str(j, "platform", "win9x");
        json_kv_bool(j, "service_supported", 0);
        json_kv_str(j, "note",
            "Win9x does not support NT services. "
            "Use auto-login + Run key for auto-start.");
        return;
    }

    json_kv_str(j, "platform", "nt");
    json_kv_bool(j, "service_supported", 1);

    if (!load_svc_api()) {
        json_kv_str(j, "error", "Service API not available");
        return;
    }

    hSCM = svc.pOpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM) {
        json_kv_str(j, "error", "Cannot connect to SCM");
        return;
    }

    hSvc = svc.pOpenService(hSCM, SVC_NAME, SERVICE_QUERY_STATUS);
    if (!hSvc) {
        json_kv_bool(j, "installed", 0);
        svc.pCloseServiceHandle(hSCM);
        return;
    }

    json_kv_bool(j, "installed", 1);
    json_kv_str(j, "service_name", SVC_NAME);

    if (svc.pQueryServiceStatus(hSvc, &status)) {
        json_kv_str(j, "state", svc_state_str(status.dwCurrentState));
        json_kv_uint(j, "pid", status.dwServiceSpecificExitCode);
    }

    svc.pCloseServiceHandle(hSvc);
    svc.pCloseServiceHandle(hSCM);
}

void handle_service(SOCKET sock, const char *args)
{
    char action[16];
    json_t j;
    char *result;

    if (!args || !args[0]) {
        send_error_response(sock,
            "SERVICE requires: install | remove | status");
        return;
    }

    if (sscanf(args, "%15s", action) < 1) {
        send_error_response(sock, "SERVICE requires an action");
        return;
    }

    json_init(&j);
    json_object_start(&j);

    if (_stricmp(action, "install") == 0)
        service_install(&j);
    else if (_stricmp(action, "remove") == 0)
        service_remove(&j);
    else if (_stricmp(action, "status") == 0)
        service_status_query(&j);
    else {
        json_free(&j);
        send_error_response(sock,
            "Unknown action. Use: install | remove | status");
        return;
    }

    json_object_end(&j);
    result = json_finish(&j);
    if (result) {
        send_text_response(sock, result);
        HeapFree(GetProcessHeap(), 0, result);
    } else {
        send_error_response(sock, "Out of memory");
    }
}


/* ================================================================
 * NT Service infrastructure
 * Allows the same binary to run as a service or console app.
 * ================================================================ */

static SERVICE_STATUS_HANDLE g_svc_handle = 0;
static SERVICE_STATUS        g_svc_status;

static void WINAPI svc_ctrl_handler(DWORD control)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_svc_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_svc_status.dwWaitHint = 5000;
        if (svc.pSetServiceStatus)
            svc.pSetServiceStatus(g_svc_handle, &g_svc_status);
        g_running = 0;
        break;
    case SERVICE_CONTROL_INTERROGATE:
        if (svc.pSetServiceStatus)
            svc.pSetServiceStatus(g_svc_handle, &g_svc_status);
        break;
    }
}

static void WINAPI svc_main(DWORD argc, LPSTR *argv)
{
    (void)argc;
    (void)argv;

    if (!svc.pRegisterSCHandler || !svc.pSetServiceStatus)
        return;

    g_svc_handle = svc.pRegisterSCHandler(SVC_NAME, svc_ctrl_handler);
    if (!g_svc_handle)
        return;

    /* Report start pending */
    g_svc_status.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwCurrentState     = SERVICE_START_PENDING;
    g_svc_status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                      SERVICE_ACCEPT_SHUTDOWN;
    g_svc_status.dwWin32ExitCode    = 0;
    g_svc_status.dwCheckPoint       = 1;
    g_svc_status.dwWaitHint         = 10000;
    svc.pSetServiceStatus(g_svc_handle, &g_svc_status);

    g_service_mode = 1;

    /* Run the agent - this blocks until g_running becomes 0 */
    agent_run();

    /* Report stopped */
    g_svc_status.dwCurrentState = SERVICE_STOPPED;
    g_svc_status.dwCheckPoint   = 0;
    g_svc_status.dwWaitHint     = 0;
    svc.pSetServiceStatus(g_svc_handle, &g_svc_status);
}

/*
 * try_service_start - Attempt to run as an NT service.
 *
 * Dynamically loads StartServiceCtrlDispatcherA and calls it.
 * If we're being started by the SCM, this blocks until the service stops.
 * If we're not a service (or on Win9x), returns 0 immediately.
 *
 * Returns: 1 = ran as service (now shutting down), 0 = not a service
 */
int try_service_start(void)
{
    SERVICE_TABLE_ENTRYA table[2];

    if (!load_svc_api() || !svc.pStartSCDispatcher)
        return 0;

    table[0].lpServiceName = (LPSTR)SVC_NAME;
    table[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTIONA)svc_main;
    table[1].lpServiceName = NULL;
    table[1].lpServiceProc = NULL;

    /*
     * StartServiceCtrlDispatcher blocks if we're a service process.
     * If we're a normal console app, it fails with
     * ERROR_FAILED_SERVICE_CONTROLLER_CONNECT and returns FALSE.
     */
    if (svc.pStartSCDispatcher(table))
        return 1;

    /* Not running as a service - fall through to console mode */
    return 0;
}

/*
 * service_report_running - Tell SCM we're fully initialized.
 * Called from agent_run() after sockets are bound and listening.
 * No-op if not in service mode.
 */
void service_report_running(void)
{
    if (!g_service_mode || !g_svc_handle || !svc.pSetServiceStatus)
        return;

    g_svc_status.dwCurrentState     = SERVICE_RUNNING;
    g_svc_status.dwCheckPoint       = 0;
    g_svc_status.dwWaitHint         = 0;
    svc.pSetServiceStatus(g_svc_handle, &g_svc_status);

    log_msg(LOG_SVC, "Service status: RUNNING");
}

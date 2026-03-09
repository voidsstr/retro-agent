/*
 * sysinfo.c - System information gathering
 * CPU (GetSystemInfo), RAM (GlobalMemoryStatus), OS (GetVersionExA),
 * drives (GetDriveTypeA, GetDiskFreeSpaceA), uptime
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

static void add_cpu_info(json_t *j)
{
    SYSTEM_INFO si;
    char arch[32];

    GetSystemInfo(&si);

    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_INTEL: safe_strncpy(arch, "x86", sizeof(arch)); break;
    default: _snprintf(arch, sizeof(arch), "unknown(%u)", si.wProcessorArchitecture); break;
    }

    json_key(j, "cpu");
    json_object_start(j);
    json_kv_str(j, "architecture", arch);
    json_kv_uint(j, "processors", si.dwNumberOfProcessors);
    json_kv_uint(j, "processor_type", si.dwProcessorType);
    json_kv_uint(j, "processor_level", si.wProcessorLevel);
    json_kv_uint(j, "processor_revision", si.wProcessorRevision);
    json_object_end(j);
}

static void add_memory_info(json_t *j)
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);

    json_key(j, "memory");
    json_object_start(j);
    json_kv_uint(j, "total_mb", (DWORD)(ms.dwTotalPhys / (1024 * 1024)));
    json_kv_uint(j, "avail_mb", (DWORD)(ms.dwAvailPhys / (1024 * 1024)));
    json_kv_uint(j, "load_percent", ms.dwMemoryLoad);
    json_kv_uint(j, "total_page_mb", (DWORD)(ms.dwTotalPageFile / (1024 * 1024)));
    json_kv_uint(j, "avail_page_mb", (DWORD)(ms.dwAvailPageFile / (1024 * 1024)));
    json_object_end(j);
}

static void add_os_info(json_t *j)
{
    OSVERSIONINFOA osvi;
    char ver_str[128];
    const char *product = "Unknown";

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);

    if (osvi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) {
        if (osvi.dwMinorVersion == 0) product = "Windows 95";
        else if (osvi.dwMinorVersion == 10) product = "Windows 98";
        else if (osvi.dwMinorVersion == 90) product = "Windows Me";
    } else if (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT) {
        if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 0) product = "Windows 2000";
        else if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 1) product = "Windows XP";
        else if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 2) product = "Windows Server 2003";
    }

    _snprintf(ver_str, sizeof(ver_str), "%lu.%lu.%lu",
              osvi.dwMajorVersion, osvi.dwMinorVersion,
              osvi.dwBuildNumber & 0xFFFF);

    json_key(j, "os");
    json_object_start(j);
    json_kv_str(j, "product", product);
    json_kv_str(j, "version", ver_str);
    json_kv_uint(j, "platform_id", osvi.dwPlatformId);
    json_kv_str(j, "csd_version", osvi.szCSDVersion);
    json_object_end(j);
}

static void add_drive_info(json_t *j)
{
    char drive_letter;
    char root[4] = "A:\\";

    json_key(j, "drives");
    json_array_start(j);

    for (drive_letter = 'A'; drive_letter <= 'Z'; drive_letter++) {
        UINT dtype;
        root[0] = drive_letter;
        dtype = GetDriveTypeA(root);

        if (dtype == DRIVE_FIXED || dtype == DRIVE_REMOVABLE ||
            dtype == DRIVE_CDROM || dtype == DRIVE_REMOTE) {
            DWORD spc, bps, fc, tc;
            const char *type_str;

            json_object_start(j);
            json_kv_str(j, "root", root);

            switch (dtype) {
            case DRIVE_REMOVABLE: type_str = "removable"; break;
            case DRIVE_FIXED:     type_str = "fixed"; break;
            case DRIVE_CDROM:     type_str = "cdrom"; break;
            case DRIVE_REMOTE:    type_str = "network"; break;
            default:              type_str = "unknown"; break;
            }
            json_kv_str(j, "type", type_str);

            if (GetDiskFreeSpaceA(root, &spc, &bps, &fc, &tc)) {
                /* Calculate sizes in MB to avoid overflow on 32-bit */
                DWORD bytes_per_cluster = spc * bps;
                DWORD free_mb  = (DWORD)(((__int64)fc * bytes_per_cluster) / (1024 * 1024));
                DWORD total_mb = (DWORD)(((__int64)tc * bytes_per_cluster) / (1024 * 1024));
                json_kv_uint(j, "free_mb", free_mb);
                json_kv_uint(j, "total_mb", total_mb);
            }

            json_object_end(j);
        }
    }

    json_array_end(j);
}

void handle_sysinfo(SOCKET sock)
{
    json_t j;
    char hostname[256];
    char *result;

    json_init(&j);
    json_object_start(&j);

    {
        DWORD hn_size = sizeof(hostname);
        GetComputerNameA(hostname, &hn_size);
    }
    json_kv_str(&j, "hostname", hostname);
#ifdef AGENT_VERSION
    json_kv_str(&j, "agent_version", AGENT_VERSION);
#endif
    json_kv_uint(&j, "uptime_seconds", GetTickCount() / 1000);

    add_cpu_info(&j);
    add_memory_info(&j);
    add_os_info(&j);
    add_drive_info(&j);

    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

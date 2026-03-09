/*
 * sysinfo.c - System information from /proc and uname.
 * Parses /proc/cpuinfo, /proc/meminfo, /proc/uptime, /etc/os-release, statvfs.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <mntent.h>

static void read_line_value(const char *path, const char *key, char *out, int outsize)
{
    FILE *f = fopen(path, "r");
    char line[512];
    out[0] = '\0';

    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            char *val = strchr(line, ':');
            if (!val) val = strchr(line, '=');
            if (val) {
                val++;
                while (*val == ' ' || *val == '\t' || *val == '"') val++;
                safe_strncpy(out, val, outsize);
                /* Trim trailing whitespace and quotes */
                int len = (int)strlen(out);
                while (len > 0 && (out[len-1] == '\n' || out[len-1] == '\r' ||
                       out[len-1] == '"' || out[len-1] == ' '))
                    out[--len] = '\0';
            }
            break;
        }
    }
    fclose(f);
}

static void add_cpu_info(json_t *j)
{
    char model[256] = "unknown";
    char cores_str[32] = "1";
    struct utsname uts;

    read_line_value("/proc/cpuinfo", "model name", model, sizeof(model));
    read_line_value("/proc/cpuinfo", "cpu cores", cores_str, sizeof(cores_str));

    uname(&uts);

    json_key(j, "cpu");
    json_object_start(j);
    json_kv_str(j, "architecture", uts.machine);
    json_kv_str(j, "model", model);
    json_kv_int(j, "cores", atoi(cores_str));
    json_kv_int(j, "processors", (int)sysconf(_SC_NPROCESSORS_ONLN));
    json_object_end(j);
}

static void add_memory_info(json_t *j)
{
    char total_str[64] = "0", avail_str[64] = "0", free_str[64] = "0";
    unsigned long total_kb, avail_kb, free_kb;

    read_line_value("/proc/meminfo", "MemTotal", total_str, sizeof(total_str));
    read_line_value("/proc/meminfo", "MemAvailable", avail_str, sizeof(avail_str));
    read_line_value("/proc/meminfo", "MemFree", free_str, sizeof(free_str));

    total_kb = strtoul(total_str, NULL, 10);
    avail_kb = strtoul(avail_str, NULL, 10);
    free_kb = strtoul(free_str, NULL, 10);
    if (avail_kb == 0) avail_kb = free_kb;

    json_key(j, "memory");
    json_object_start(j);
    json_kv_uint(j, "total_mb", total_kb / 1024);
    json_kv_uint(j, "avail_mb", avail_kb / 1024);
    if (total_kb > 0)
        json_kv_uint(j, "load_percent", ((total_kb - avail_kb) * 100) / total_kb);
    else
        json_kv_uint(j, "load_percent", 0);
    json_object_end(j);
}

static void add_os_info(json_t *j)
{
    struct utsname uts;
    char pretty_name[256] = "Linux";
    char version_id[64] = "";

    uname(&uts);
    read_line_value("/etc/os-release", "PRETTY_NAME", pretty_name, sizeof(pretty_name));
    read_line_value("/etc/os-release", "VERSION_ID", version_id, sizeof(version_id));

    json_key(j, "os");
    json_object_start(j);
    json_kv_str(j, "product", pretty_name);
    json_kv_str(j, "version", uts.release);
    json_kv_str(j, "version_id", version_id);
    json_kv_str(j, "sysname", uts.sysname);
    json_kv_str(j, "machine", uts.machine);
    json_object_end(j);
}

static void add_drive_info(json_t *j)
{
    FILE *mtab;
    struct mntent *mnt;

    json_key(j, "drives");
    json_array_start(j);

    mtab = setmntent("/etc/mtab", "r");
    if (mtab) {
        while ((mnt = getmntent(mtab)) != NULL) {
            struct statvfs vfs;
            const char *type;

            /* Skip virtual/pseudo filesystems */
            if (strncmp(mnt->mnt_fsname, "/dev/", 5) != 0)
                continue;

            if (statvfs(mnt->mnt_dir, &vfs) != 0)
                continue;

            /* Determine type */
            if (strcmp(mnt->mnt_type, "ext4") == 0 || strcmp(mnt->mnt_type, "ext3") == 0 ||
                strcmp(mnt->mnt_type, "xfs") == 0 || strcmp(mnt->mnt_type, "btrfs") == 0)
                type = "fixed";
            else if (strcmp(mnt->mnt_type, "vfat") == 0 || strcmp(mnt->mnt_type, "exfat") == 0)
                type = "removable";
            else if (strcmp(mnt->mnt_type, "nfs") == 0 || strcmp(mnt->mnt_type, "cifs") == 0)
                type = "network";
            else if (strcmp(mnt->mnt_type, "iso9660") == 0)
                type = "cdrom";
            else
                type = mnt->mnt_type;

            json_object_start(j);
            json_kv_str(j, "root", mnt->mnt_dir);
            json_kv_str(j, "device", mnt->mnt_fsname);
            json_kv_str(j, "type", type);
            json_kv_str(j, "fstype", mnt->mnt_type);
            {
                unsigned long total_mb = (unsigned long)((vfs.f_blocks * vfs.f_frsize) / (1024 * 1024));
                unsigned long free_mb  = (unsigned long)((vfs.f_bavail * vfs.f_frsize) / (1024 * 1024));
                json_kv_uint(j, "total_mb", total_mb);
                json_kv_uint(j, "free_mb", free_mb);
            }
            json_object_end(j);
        }
        endmntent(mtab);
    }

    json_array_end(j);
}

void handle_sysinfo(SOCKET sock)
{
    json_t j;
    char hostname[256];
    char *result;
    double uptime_sec = 0;

    json_init(&j);
    json_object_start(&j);

    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';
    json_kv_str(&j, "hostname", hostname);

    /* Uptime from /proc/uptime */
    {
        FILE *f = fopen("/proc/uptime", "r");
        if (f) {
            fscanf(f, "%lf", &uptime_sec);
            fclose(f);
        }
    }
    json_kv_uint(&j, "uptime_seconds", (unsigned long)uptime_sec);

    add_cpu_info(&j);
    add_memory_info(&j);
    add_os_info(&j);
    add_drive_info(&j);

    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

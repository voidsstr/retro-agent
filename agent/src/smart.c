/*
 * smart.c - SMART disk health information
 * Reads SMART data via DeviceIoControl (SMART_RCV_DRIVE_DATA).
 * Falls back to IOCTL_STORAGE_QUERY_PROPERTY for model/serial.
 * Win98SE compatible: structures defined manually.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

#define LOG_SMART "SMART"

/* SMART IOCTL definitions - defined manually for Win98SE compat */
#ifndef SMART_RCV_DRIVE_DATA
#define SMART_RCV_DRIVE_DATA        0x7C088
#endif
#ifndef SMART_GET_VERSION
#define SMART_GET_VERSION           0x74080
#endif

#define ID_CMD                      0xEC    /* IDENTIFY DEVICE */
#define SMART_CMD                   0xB0    /* SMART command */
#define READ_ATTRIBUTES             0xD0    /* SMART READ DATA subcommand */

/* SMART attribute IDs of interest */
#define ATTR_REALLOCATED_SECTORS    0x05
#define ATTR_POWER_ON_HOURS         0x09
#define ATTR_TEMPERATURE            0xC2

#pragma pack(push, 1)

/* GETVERSIONINPARAMS - returned by SMART_GET_VERSION */
typedef struct {
    BYTE  bVersion;
    BYTE  bRevision;
    BYTE  bReserved;
    BYTE  bIDEDeviceMap;
    DWORD fCapabilities;
    DWORD dwReserved[4];
} GETVERSIONINPARAMS_98;

/* IDEREGS - IDE register set */
typedef struct {
    BYTE  bFeaturesReg;
    BYTE  bSectorCountReg;
    BYTE  bSectorNumberReg;
    BYTE  bCylLowReg;
    BYTE  bCylHighReg;
    BYTE  bDriveHeadReg;
    BYTE  bCommandReg;
    BYTE  bReserved;
} IDEREGS_98;

/* SENDCMDINPARAMS - input for SMART_RCV_DRIVE_DATA */
typedef struct {
    DWORD       cBufferSize;
    IDEREGS_98  irDriveRegs;
    BYTE        bDriveNumber;
    BYTE        bReserved[3];
    DWORD       dwReserved[4];
    BYTE        bBuffer[1];
} SENDCMDINPARAMS_98;

/* DRIVERSTATUS - returned in output */
typedef struct {
    BYTE  bDriverError;
    BYTE  bIDEError;
    BYTE  bReserved[2];
    DWORD dwReserved[2];
} DRIVERSTATUS_98;

/* SENDCMDOUTPARAMS - output from SMART_RCV_DRIVE_DATA */
typedef struct {
    DWORD           cBufferSize;
    DRIVERSTATUS_98 DriverStatus;
    BYTE            bBuffer[512];
} SENDCMDOUTPARAMS_98;

/* SMART attribute entry (12 bytes each in SMART data) */
typedef struct {
    BYTE  bAttrID;
    WORD  wFlags;
    BYTE  bCurrent;
    BYTE  bWorst;
    BYTE  bRawValue[6];
    BYTE  bReserved;
} SMART_ATTRIBUTE;

/* IDENTIFY DEVICE data - 512 bytes, relevant fields */
typedef struct {
    WORD  wGenConfig;           /* 0 */
    WORD  wNumCyls;             /* 1 */
    WORD  wReserved;            /* 2 */
    WORD  wNumHeads;            /* 3 */
    WORD  wReserved2[2];        /* 4-5 */
    WORD  wSectorsPerTrack;     /* 6 */
    WORD  wVendorUnique[3];     /* 7-9 */
    WORD  wSerialNumber[10];    /* 10-19 */
    WORD  wBufferType;          /* 20 */
    WORD  wBufferSize;          /* 21 */
    WORD  wECCSize;             /* 22 */
    WORD  wFirmwareRevision[4]; /* 23-26 */
    WORD  wModelNumber[20];     /* 27-46 */
    WORD  wReserved3[209];      /* 47-255 */
} IDENTIFY_DATA;

/* IOCTL_STORAGE_QUERY_PROPERTY structures */
#ifndef IOCTL_STORAGE_QUERY_PROPERTY
#define IOCTL_STORAGE_QUERY_PROPERTY 0x2D1400
#endif

/* Use the values directly instead of enum to avoid conflicts with winioctl.h */
#define PROPERTY_STANDARD_QUERY_98  0
#define STORAGE_DEVICE_PROPERTY_98  0

typedef struct {
    DWORD PropertyId;
    DWORD QueryType;
    BYTE  AdditionalParameters[1];
} STORAGE_PROPERTY_QUERY_98;

typedef struct {
    DWORD Version;
    DWORD Size;
    BYTE  DeviceType;
    BYTE  DeviceTypeModifier;
    BYTE  RemovableMedia;
    BYTE  CommandQueueing;
    DWORD VendorIdOffset;
    DWORD ProductIdOffset;
    DWORD ProductRevisionOffset;
    DWORD SerialNumberOffset;
    DWORD BusType;
    DWORD RawPropertiesLength;
    BYTE  RawDeviceProperties[1];
} STORAGE_DEVICE_DESCRIPTOR_98;

#pragma pack(pop)

/*
 * Trim trailing spaces from ATA string and copy to destination.
 * ATA strings have bytes swapped within each word.
 */
static void ata_string_copy(char *dst, const WORD *src, int word_count)
{
    int i, len;
    char *p = dst;

    for (i = 0; i < word_count; i++) {
        *p++ = (char)(src[i] >> 8);
        *p++ = (char)(src[i] & 0xFF);
    }
    *p = '\0';

    /* Trim trailing spaces */
    len = (int)strlen(dst);
    while (len > 0 && dst[len - 1] == ' ')
        dst[--len] = '\0';
}

/*
 * Try to read IDENTIFY DEVICE data via SMART_RCV_DRIVE_DATA.
 * Returns 1 on success, 0 on failure.
 */
static int smart_identify(HANDLE hDrive, int drive_num, IDENTIFY_DATA *id_out)
{
    SENDCMDINPARAMS_98 cmd_in;
    SENDCMDOUTPARAMS_98 cmd_out;
    DWORD bytes_returned = 0;

    ZeroMemory(&cmd_in, sizeof(cmd_in));
    ZeroMemory(&cmd_out, sizeof(cmd_out));

    cmd_in.cBufferSize = 512;
    cmd_in.irDriveRegs.bDriveHeadReg = (BYTE)(0xA0 | ((drive_num & 1) << 4));
    cmd_in.irDriveRegs.bCommandReg = ID_CMD;
    cmd_in.bDriveNumber = (BYTE)drive_num;

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
                         &cmd_in, sizeof(cmd_in) - 1,
                         &cmd_out, sizeof(cmd_out),
                         &bytes_returned, NULL)) {
        log_msg(LOG_SMART, "IDENTIFY drive %d failed, err=%lu",
                drive_num, (unsigned long)GetLastError());
        return 0;
    }

    if (cmd_out.DriverStatus.bDriverError != 0) {
        log_msg(LOG_SMART, "IDENTIFY drive %d driver error=%d",
                drive_num, cmd_out.DriverStatus.bDriverError);
        return 0;
    }

    memcpy(id_out, cmd_out.bBuffer, sizeof(IDENTIFY_DATA));
    return 1;
}

/*
 * Read SMART attributes via SMART_RCV_DRIVE_DATA with READ_ATTRIBUTES.
 * Returns 1 on success, 0 on failure.
 * attr_buf receives 512 bytes of SMART attribute data.
 */
static int smart_read_attributes(HANDLE hDrive, int drive_num, BYTE *attr_buf)
{
    SENDCMDINPARAMS_98 cmd_in;
    SENDCMDOUTPARAMS_98 cmd_out;
    DWORD bytes_returned = 0;

    ZeroMemory(&cmd_in, sizeof(cmd_in));
    ZeroMemory(&cmd_out, sizeof(cmd_out));

    cmd_in.cBufferSize = 512;
    cmd_in.irDriveRegs.bFeaturesReg = READ_ATTRIBUTES;
    cmd_in.irDriveRegs.bCylLowReg = 0x4F;
    cmd_in.irDriveRegs.bCylHighReg = 0xC2;
    cmd_in.irDriveRegs.bDriveHeadReg = (BYTE)(0xA0 | ((drive_num & 1) << 4));
    cmd_in.irDriveRegs.bCommandReg = SMART_CMD;
    cmd_in.bDriveNumber = (BYTE)drive_num;

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
                         &cmd_in, sizeof(cmd_in) - 1,
                         &cmd_out, sizeof(cmd_out),
                         &bytes_returned, NULL)) {
        log_msg(LOG_SMART, "SMART READ_ATTRIBUTES drive %d failed, err=%lu",
                drive_num, (unsigned long)GetLastError());
        return 0;
    }

    if (cmd_out.DriverStatus.bDriverError != 0) {
        log_msg(LOG_SMART, "SMART READ_ATTRIBUTES drive %d driver error=%d",
                drive_num, cmd_out.DriverStatus.bDriverError);
        return 0;
    }

    memcpy(attr_buf, cmd_out.bBuffer, 512);
    return 1;
}

/*
 * Extract a specific SMART attribute raw value (32-bit).
 * SMART data starts with a 2-byte revision, then 30 attribute entries of 12 bytes each.
 * Returns -1 if not found.
 */
static int smart_get_attribute(const BYTE *attr_buf, BYTE attr_id,
                               DWORD *raw_out, BYTE *current_out, BYTE *worst_out)
{
    int i;
    const BYTE *p;

    /* Skip 2-byte revision header */
    p = attr_buf + 2;

    for (i = 0; i < 30; i++, p += 12) {
        SMART_ATTRIBUTE *attr = (SMART_ATTRIBUTE *)p;
        if (attr->bAttrID == attr_id) {
            if (raw_out) {
                *raw_out = (DWORD)attr->bRawValue[0]
                         | ((DWORD)attr->bRawValue[1] << 8)
                         | ((DWORD)attr->bRawValue[2] << 16)
                         | ((DWORD)attr->bRawValue[3] << 24);
            }
            if (current_out) *current_out = attr->bCurrent;
            if (worst_out) *worst_out = attr->bWorst;
            return 1;
        }
    }

    return 0;
}

/*
 * Fallback: try IOCTL_STORAGE_QUERY_PROPERTY for model and serial.
 * Returns 1 on success.
 */
static int storage_query_property(HANDLE hDrive, char *model, int model_size,
                                  char *serial, int serial_size)
{
    STORAGE_PROPERTY_QUERY_98 query;
    BYTE out_buf[1024];
    DWORD bytes_returned = 0;
    STORAGE_DEVICE_DESCRIPTOR_98 *desc;

    model[0] = '\0';
    serial[0] = '\0';

    ZeroMemory(&query, sizeof(query));
    query.PropertyId = STORAGE_DEVICE_PROPERTY_98;
    query.QueryType = PROPERTY_STANDARD_QUERY_98;

    ZeroMemory(out_buf, sizeof(out_buf));

    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query),
                         out_buf, sizeof(out_buf),
                         &bytes_returned, NULL)) {
        return 0;
    }

    desc = (STORAGE_DEVICE_DESCRIPTOR_98 *)out_buf;

    if (desc->ProductIdOffset && desc->ProductIdOffset < bytes_returned) {
        safe_strncpy(model, (const char *)out_buf + desc->ProductIdOffset,
                     model_size);
        /* Trim trailing spaces */
        {
            int len = (int)strlen(model);
            while (len > 0 && model[len - 1] == ' ')
                model[--len] = '\0';
        }
    }

    if (desc->SerialNumberOffset && desc->SerialNumberOffset < bytes_returned) {
        safe_strncpy(serial, (const char *)out_buf + desc->SerialNumberOffset,
                     serial_size);
        {
            int len = (int)strlen(serial);
            while (len > 0 && serial[len - 1] == ' ')
                serial[--len] = '\0';
        }
    }

    return 1;
}

void handle_smartinfo(SOCKET sock)
{
    json_t j;
    char *result;
    int drive_num;
    int drive_count = 0;

    json_init(&j);
    json_array_start(&j);

    for (drive_num = 0; drive_num < 10; drive_num++) {
        HANDLE hDrive;
        char path[32];
        IDENTIFY_DATA id_data;
        BYTE attr_buf[512];
        char model[64], serial[32], firmware[16];
        int has_identify = 0;
        int has_smart = 0;

        _snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", drive_num);
        hDrive = CreateFileA(path,
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);

        if (hDrive == INVALID_HANDLE_VALUE) {
            /* Try read-only access (non-admin) */
            hDrive = CreateFileA(path,
                                 GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
            if (hDrive == INVALID_HANDLE_VALUE)
                continue;
        }

        log_msg(LOG_SMART, "Opened %s", path);

        model[0] = serial[0] = firmware[0] = '\0';

        /* Try IDENTIFY DEVICE */
        ZeroMemory(&id_data, sizeof(id_data));
        if (smart_identify(hDrive, drive_num, &id_data)) {
            has_identify = 1;
            ata_string_copy(model, id_data.wModelNumber, 20);
            ata_string_copy(serial, id_data.wSerialNumber, 10);
            ata_string_copy(firmware, id_data.wFirmwareRevision, 4);
            log_msg(LOG_SMART, "Drive %d: model=\"%s\" serial=\"%s\" fw=\"%s\"",
                    drive_num, model, serial, firmware);
        }

        /* Try SMART READ ATTRIBUTES */
        ZeroMemory(attr_buf, sizeof(attr_buf));
        if (smart_read_attributes(hDrive, drive_num, attr_buf)) {
            has_smart = 1;
        }

        /* Fallback: if no IDENTIFY, try STORAGE_QUERY_PROPERTY */
        if (!has_identify) {
            if (storage_query_property(hDrive, model, sizeof(model),
                                       serial, sizeof(serial))) {
                log_msg(LOG_SMART, "Drive %d (StorageQuery): model=\"%s\" serial=\"%s\"",
                        drive_num, model, serial);
            }
        }

        CloseHandle(hDrive);

        /* Only emit JSON if we got something useful */
        if (model[0] || serial[0] || has_smart) {
            json_object_start(&j);
            json_kv_int(&j, "drive_index", drive_num);
            json_kv_str(&j, "path", path);
            json_kv_str(&j, "model", model);
            json_kv_str(&j, "serial", serial);
            json_kv_str(&j, "firmware", firmware);

            if (has_smart) {
                DWORD raw;
                BYTE current, worst;

                json_key(&j, "smart");
                json_object_start(&j);

                /* Temperature (0xC2) */
                if (smart_get_attribute(attr_buf, ATTR_TEMPERATURE,
                                        &raw, &current, &worst)) {
                    json_key(&j, "temperature");
                    json_object_start(&j);
                    json_kv_uint(&j, "celsius", raw & 0xFF);
                    json_kv_uint(&j, "current", current);
                    json_kv_uint(&j, "worst", worst);
                    json_object_end(&j);
                }

                /* Power-on hours (0x09) */
                if (smart_get_attribute(attr_buf, ATTR_POWER_ON_HOURS,
                                        &raw, &current, &worst)) {
                    json_key(&j, "power_on_hours");
                    json_object_start(&j);
                    json_kv_uint(&j, "hours", raw);
                    json_kv_uint(&j, "current", current);
                    json_kv_uint(&j, "worst", worst);
                    json_object_end(&j);
                }

                /* Reallocated sectors (0x05) */
                if (smart_get_attribute(attr_buf, ATTR_REALLOCATED_SECTORS,
                                        &raw, &current, &worst)) {
                    json_key(&j, "reallocated_sectors");
                    json_object_start(&j);
                    json_kv_uint(&j, "count", raw);
                    json_kv_uint(&j, "current", current);
                    json_kv_uint(&j, "worst", worst);
                    json_object_end(&j);
                }

                json_object_end(&j);
            } else {
                json_kv_str(&j, "smart", "unavailable");
            }

            json_object_end(&j);
            drive_count++;
        }
    }

    json_array_end(&j);

    if (drive_count == 0) {
        json_free(&j);
        send_text_response(sock, "[]");
        return;
    }

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

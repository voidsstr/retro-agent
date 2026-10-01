/*
 * ipxsetup.h - the agent-internal interface between ipxsetup.c (orchestration,
 * probe, command, startup thread), ipxnt.c (NWLink through INetCfg on NT 5.x)
 * and ipx9x.c (the Windows 98 SE registry template + VxD payload).
 *
 * The decisions live in agent/shared/ipxplan.h (Win32-free, natively tested).
 * util.h's json_t is an anonymous-struct typedef and cannot be forward-declared,
 * so the HWPROFILE emitter is declared here rather than in handlers.h.
 */
#ifndef IPXSETUP_H
#define IPXSETUP_H

#include <winsock2.h>
#include <windows.h>
#include "util.h"
#include "../shared/ipxplan.h"

#define LOG_IPX "IPXSETUP"

/* ---- ipxsetup.c ---------------------------------------------------------- */

/* The live answer: does Winsock open an IPX socket, and what address does it
 * get? catalog_ipx: the Winsock 2 catalog lists an AF_IPX provider (1/0),
 * -1 when WSAEnumProtocolsA could not be asked. */
typedef struct {
    int  winsock_ipx;
    int  catalog_ipx;
    char net[12];               /* "00000000" */
    char node[16];              /* "0013d4a4a413" */
} ipx_probe_t;
void ipx_probe(ipx_probe_t *p);

/* HWPROFILE's "ipx" object (hwprofile.c). Read-only and cheap. */
void ipxsetup_emit_hwprofile(json_t *j);

/* Called once on the main thread before any IPX thread or command runs. */
void ipxsetup_init(void);

/* ---- ipxnt.c (NT 5.x) ---------------------------------------------------- */

typedef struct {
    int  component_present;     /* a NetTrans instance with ComponentId ms_nwipx */
    char comp_key[40];          /* its Control\Network\{4D36E975-...}\{GUID} instance */
    int  service_present;       /* Services\NwlnkIpx exists */
    int  service_running;       /* 1 running, 0 not, -1 unknown (no SCM answer) */
    int  files_present;         /* netnwlnk.inf + (nwlnkipx.sys or driver.cab) */
    int  nadapters;             /* NwlnkIpx\Parameters\Adapters\* */
    struct {
        char name[80];
        int  frame;             /* IPX_FRAME_* from PktType */
    } ad[4];
} ipxnt_info_t;

void ipxnt_observe(ipxnt_info_t *x);

enum {
    IPXNT_INSTALLED = 0,        /* Install + Apply returned S_OK */
    IPXNT_REBOOT,               /* ...NETCFG_S_REBOOT: reported, never acted on */
    IPXNT_ALREADY,              /* FindComponent found MS_NWIPX: nothing installed */
    IPXNT_BUSY,                 /* another program holds the network-config write lock */
    IPXNT_FAILED,
    IPXNT_NO_COM,               /* ole32 / netcfgx could not be loaded */
    IPXNT_HUNG                  /* the worker never returned: leaked, reported until restart */
};

typedef struct {
    int  result;                /* IPXNT_* */
    long hr;                    /* the failing HRESULT */
    char step[48];              /* which call failed */
    char holder[128];           /* who holds the write lock (BUSY) */
} ipxnt_result_t;

/* Install MS_NWIPX in a worker thread, waiting up to watchdog_ms. Returns
 * r->result. On IPXNT_HUNG the worker and its job are deliberately leaked:
 * they are still inside INetCfg. */
int ipxnt_install(ipxnt_result_t *r, DWORD watchdog_ms);

/* ---- ipx9x.c (Windows 98 SE) --------------------------------------------- */

typedef struct {
    int  nic_count;             /* TCP/IP-bound physical adapters */
    char nic[200];              /* the one adapter's Enum path ("" unless exactly one) */
    char nic_desc[96];
    char cls[8];                /* the NWLINK NetTrans index in use, "" none */
    char inst[8];               /* the NWLINK devnode instance, "" none */
    int  devnode_present;       /* Enum\Network\NWLINK\<inst> with a class key */
    int  bound;                 /* the adapter's Bindings names NWLINK\<inst> */
    int  stack_loaded;          /* a live devnode NETWORK\NWLINK\<inst>, problem 0 */
    int  files_present;         /* NWLINK.VXD + WSIPX.VXD in SYSTEM */
    int  frame;                 /* IPX_FRAME_* of the class key's Frame_Type */
} ipx9x_info_t;

void ipx9x_observe(ipx9x_info_t *x);

typedef struct {
    int  ok;
    int  values_written;
    int  keys_created;
    int  files_copied;
    char msg[400];
} ipx9x_result_t;

/* The whole 9x install: one adapter, render, check, ADDITIVE preflight, stage
 * the payload, write the undo file, keys, values (binding LAST), RegFlushKey,
 * read everything back. Writes nothing unless every check before passed.
 * catalog_has_ipx: skip the queued WSCInstallProvider. Returns r->ok. */
int ipx9x_install(ipx9x_result_t *r, int catalog_has_ipx);

#endif /* IPXSETUP_H */

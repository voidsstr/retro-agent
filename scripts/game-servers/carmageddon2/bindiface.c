/*
 * bindiface.so - LD_PRELOAD shim for the Carmageddon 2 host container.
 *
 * WHY: IPXWrapper sends every IPX broadcast as a UDP datagram to the subnet
 * broadcast address of the interface it is using (192.168.1.255). This dev
 * host is multi-homed ON THAT SAME SUBNET: wired enp129s0 (.132, .196) and
 * Wi-Fi wlp128s20f3 (.129). Linux picks the egress for 192.168.1.255 from
 * its local broadcast routes, and `ip route get 192.168.1.255` answers
 * "dev wlp128s20f3 src 192.168.1.129" - so without this shim every
 * "is anyone hosting?" reply left over Wi-Fi from the wrong address, and
 * every fleet broadcast arrived twice (once per NIC).
 *
 * WHAT: when a socket is bound to UDP port $C2_BIND_PORT (IPXWrapper's
 * 54792), set SO_BINDTODEVICE=$C2_BIND_IFACE first. Sends then leave that
 * NIC and only that NIC's datagrams are received. Nothing else is touched.
 *
 * WHERE IT MUST LOAD: in Wine 8 the AFD bind ioctl is executed by
 * WINESERVER (64-bit), not by the 32-bit game process - so this is built
 * 64-bit. The 32-bit processes print a harmless "wrong ELF class" line.
 *
 * Every bind it touches is logged - to $C2_BIND_LOG when set, else stderr -
 * so the pin is PROVEN rather than implied. (wineserver is started by
 * wineboot, whose stderr is usually thrown away; hence the file.)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netinet/in.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    static int (*real_bind)(int, const struct sockaddr *, socklen_t);
    if (!real_bind)
        real_bind = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "bind");

    const char *ifname = getenv("C2_BIND_IFACE");
    const char *portstr = getenv("C2_BIND_PORT");
    /* Parsed by hand: atoi() on a new toolchain binds __isoc23_strtol
     * (GLIBC_2.38), which the bookworm container's glibc 2.36 lacks. */
    int port = 0;
    for (const char *p = portstr ? portstr : "54792"; *p >= '0' && *p <= '9'; p++)
        port = port * 10 + (*p - '0');

    if (ifname && *ifname && addr && addr->sa_family == AF_INET &&
        len >= (socklen_t)sizeof(struct sockaddr_in) &&
        ntohs(((const struct sockaddr_in *)addr)->sin_port) == port) {
        int rc = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1);
        const char *logpath = getenv("C2_BIND_LOG");
        FILE *out = logpath ? fopen(logpath, "a") : NULL;
        fprintf(out ? out : stderr, "%ld [bindiface] fd %d port %d -> SO_BINDTODEVICE %s: %s\n",
                (long)time(NULL), fd, port, ifname, rc == 0 ? "OK" : "FAILED");
        if (out)
            fclose(out);
    }
    return real_bind(fd, addr, len);
}

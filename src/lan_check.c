/*
 * lan_check.c
 * --------------------------------------------
 * Linux-only utility functions for discovering the default gateway,
 * associated network interface, and basic connectivity testing.
 *
 * Author: Aidan Bradley
 * Date:   9/27/25
 */

#include "lan_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/select.h>

#define MAX_LINE 512

/* -------------------------------------------------------------------------- */
/**
 * @brief Convert a gateway string from /proc/net/route (hex, little-endian)
 *        into dotted-quad notation.
 *
 * Example input: "0102A8C0" -> "192.168.2.1"
 *
 * @param hexstr  Gateway string read from /proc/net/route.
 * @param out     Output buffer for dotted-quad IP.
 * @param outlen  Size of output buffer.
 * @return 0 on success, -1 on failure.
 */
static int hexgw_to_dotted(const char *hexstr, char *out, size_t outlen)
{
    unsigned long gw = 0;
    if (sscanf(hexstr, "%lx", &gw) != 1)
        return -1;
    unsigned char b0 = (gw) & 0xFF;
    unsigned char b1 = (gw >> 8) & 0xFF;
    unsigned char b2 = (gw >> 16) & 0xFF;
    unsigned char b3 = (gw >> 24) & 0xFF;
    snprintf(out, outlen, "%u.%u.%u.%u", b0, b1, b2, b3);
    return 0;
}

/* -------------------------------------------------------------------------- */
/**
 * @brief Parse the default gateway from /proc/net/route.
 *
 * Opens and reads the Linux kernel routing table to discover the default
 * route (destination == 00000000). Extracts both the interface name and
 * gateway IP (converted to dotted-quad).
 *
 * @param iface_out  Output buffer for interface name.
 * @param iflen      Size of iface_out.
 * @param gw_out     Output buffer for gateway dotted IP.
 * @param gwlen      Size of gw_out.
 * @return 0 on success, -1 if no default route is found.
 */
static int parse_default_gateway(char *iface_out, size_t iflen,
                                 char *gw_out, size_t gwlen)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return -1;

    char line[MAX_LINE];
    /* Skip header */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }

    while (fgets(line, sizeof(line), f)) {
        char iface[IFNAMSIZ];
        char destination[32], gateway[32];
        if (sscanf(line, "%s %31s %31s", iface, destination, gateway) != 3)
            continue;

        if (strcmp(destination, "00000000") == 0) {
            if (hexgw_to_dotted(gateway, gw_out, gwlen) != 0) {
                fclose(f);
                return -1;
            }
            strncpy(iface_out, iface, iflen - 1);
            iface_out[iflen - 1] = '\0';
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}

/* -------------------------------------------------------------------------- */
/**
 * @brief Get the IPv4 address assigned to a given interface.
 *
 * Iterates through system interfaces (via getifaddrs) and returns the
 * first IPv4 address found for the requested interface.
 *
 * @param ifname    Interface name (e.g., "eth0").
 * @param addr_out  Buffer to hold dotted-quad IP.
 * @param outlen    Size of addr_out.
 * @return 0 on success, -1 if no address is found.
 */
static int get_iface_ipv4(const char *ifname, char *addr_out, size_t outlen)
{
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return -1;

    int rc = -1;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name, ifname) == 0) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            if (inet_ntop(AF_INET, &sa->sin_addr, addr_out, outlen) != NULL) {
                rc = 0;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    return rc;
}

/* -------------------------------------------------------------------------- */
/**
 * @brief Attempt a non-blocking TCP connection with a timeout.
 *
 * Used as a low-privilege alternative to ICMP ping for reachability
 * testing. This checks whether the default gateway responds to TCP
 * connection attempts on common ports.
 *
 * @param ipstr       Dotted-quad IP of target.
 * @param port        TCP port to connect to.
 * @param timeout_ms  Timeout in milliseconds.
 * @return 0 on success, -1 on failure or timeout.
 */
static int connect_with_timeout(const char *ipstr, int port, int timeout_ms)
{
    int s = -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ipstr, &addr.sin_addr) != 1)
        return -1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) { close(s); return -1; }
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) { close(s); return -1; }

    int ret = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) { close(s); return 0; }

    if (errno != EINPROGRESS) { close(s); return -1; }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ret = select(s + 1, NULL, &wfds, NULL, &tv);
    if (ret <= 0) { close(s); return -1; }

    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len) == -1) {
        close(s);
        return -1;
    }
    close(s);
    return soerr == 0 ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/**
 * @brief Check whether the gateway responds on common service ports.
 *
 * Ports tested: 53 (DNS), 80 (HTTP), 443 (HTTPS). These ports are chosen
 * because they are usually open and listening, which increases the
 * likelihood of a successful connection attempt.
 *
 * @param gw_ip  Dotted-quad IP of the gateway.
 * @return 1 if reachable, 0 otherwise.
 */
static int gateway_is_reachable(const char *gw_ip)
{
    const int ports[] = {53, 80, 443};
    const int timeout_ms = 600;
    for (size_t i = 0; i < sizeof(ports)/sizeof(ports[0]); ++i) {
        if (connect_with_timeout(gw_ip, ports[i], timeout_ms) == 0)
            return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/**
 * @brief Main entry point for LAN checking.
 *
 * Discovers the default gateway, the associated interface, and the local
 * IP address for that interface. Performs a basic reachability test by
 * attempting TCP connections to the gateway. Results are returned in a
 * lan_info_t struct.
 *
 * @param info  Pointer to a lan_info_t struct that will be populated.
 * @return 0 on success, -1 if no default route is found.
 */
int check_LAN(lan_info_t *info)
{
    if (!info) return -1;
    memset(info, 0, sizeof(*info));

    if (parse_default_gateway(info->ifname, sizeof(info->ifname),
                              info->gateway, sizeof(info->gateway)) != 0) {
        return -1;
    }

    if (get_iface_ipv4(info->ifname, info->local_addr,
                       sizeof(info->local_addr)) != 0) {
        strncpy(info->local_addr, "0.0.0.0", sizeof(info->local_addr));
    }

    info->reachable = gateway_is_reachable(info->gateway);
    return 0;
}

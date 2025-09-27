/*
 * check_wlan.c
 * --------------------------------------------
 * Linux-only utility functions for verifying Internet reachability
 * by testing access to well-known public DNS servers.
 *
 * Author: Aidan Bradley
 * Date:   9/27/25
 */

#include "wlan_check.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/select.h>

/* -------------------------------------------------------------------------- */
/**
 * @brief Attempt a non-blocking TCP connection to a given server.
 *
 * Used to test reachability of public DNS servers (Google, Cloudflare, etc.).
 * DNS typically runs on UDP/53, but we use TCP/53 here to avoid raw socket
 * requirements and to maintain consistency with the LAN connectivity check.
 *
 * @param ipstr       Dotted-quad IP of server.
 * @param port        TCP port (typically 53).
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
 * @brief Check if at least one public DNS server is reachable.
 *
 * Common servers tested:
 *  - Google:    8.8.8.8, 8.8.4.4
 *  - Cloudflare:1.1.1.1, 1.0.0.1
 *
 * @return 1 if Internet access is confirmed (DNS server reachable), 0 otherwise.
 */
int check_public_dns(void)
{
    const char *servers[] = {
        "8.8.8.8",   /* Google DNS */
        "8.8.4.4",   /* Google DNS */
        "1.1.1.1",   /* Cloudflare DNS */
        "1.0.0.1"    /* Cloudflare DNS */
    };
    const int port = 53;
    const int timeout_ms = 1000;

    for (size_t i = 0; i < sizeof(servers)/sizeof(servers[0]); ++i) {
        if (connect_with_timeout(servers[i], port, timeout_ms) == 0)
            return 1; /* success */
    }

    return 0; /* all attempts failed */
}

/*
 * lan_check.h
 * --------------------------------------------
 * Public header for LAN checking utilities.
 *
 * Provides declarations for discovering the system default gateway,
 * associated interface, and basic connectivity testing.
 *
 * This header is paired with lan_check.c.
 */

#ifndef LAN_CHECK_H
#define LAN_CHECK_H

#include <net/if.h>
#include <netinet/in.h>

/* -------------------------------------------------------------------------- */
/**
 * @struct lan_info_t
 * @brief  Holds details about the LAN default route and connectivity.
 *
 * Members:
 *  - ifname:     Interface name with default route (e.g., "eth0").
 *  - gateway:    Default gateway IP address (dotted-quad string).
 *  - local_addr: IPv4 address assigned to that interface.
 *  - reachable:  Flag (1 = gateway reachable via TCP, 0 = not reachable).
 */
typedef struct {
    char ifname[IFNAMSIZ];
    char gateway[INET_ADDRSTRLEN];
    char local_addr[INET_ADDRSTRLEN];
    int reachable;
} lan_info_t;

/* -------------------------------------------------------------------------- */
/**
 * @brief Discover default gateway, interface, and perform basic reachability test.
 *
 * @param info  Pointer to a lan_info_t struct to be populated.
 * @return 0 on success, -1 if no default gateway is found.
 */
int check_LAN(lan_info_t *info);

#endif /* LAN_CHECK_H */

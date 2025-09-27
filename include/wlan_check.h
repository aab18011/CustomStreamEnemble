/*
 * check_wlan.h
 * --------------------------------------------
 * Public header for Internet connectivity checking utilities.
 *
 * Provides a function to test whether the machine can reach public
 * DNS servers, indicating that Internet access is available.
 *
 * This header is paired with check_wlan.c.
 */

#ifndef CHECK_WLAN_H
#define CHECK_WLAN_H

/* -------------------------------------------------------------------------- */
/**
 * @brief Test Internet connectivity by checking reachability of public DNS servers.
 *
 * Attempts to establish TCP connections to a set of well-known public DNS
 * servers (Google, Cloudflare). If at least one server is reachable, the
 * Internet is considered available.
 *
 * @return 1 if Internet access is confirmed, 0 otherwise.
 */
int check_public_dns(void);

#endif /* CHECK_WLAN_H */

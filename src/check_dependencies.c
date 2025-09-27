/*
 * check_dependencies.c
 * --------------------------------------------
 * Utilities for checking existence of applications and kernel modules.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/* -------------------------------------------------------------------------- */
/**
 * @brief Check whether a user-space application is installed.
 *
 * Uses the `which` command to determine whether the application is in PATH.
 *
 * @param app_name Name of the application (e.g., "ffmpeg").
 * @return 1 if installed (found in PATH), 0 otherwise.
 */
int check_application(const char *app_name)
{
    if (!app_name) return 0;

    char command[256];
    snprintf(command, sizeof(command), "which %s > /dev/null 2>&1", app_name);
    int ret = system(command);
    return (ret == 0);
}

/* -------------------------------------------------------------------------- */
/**
 * @brief Check whether a kernel module is loaded.
 *
 * Parses /proc/modules to search for the given module name. This does not
 * guarantee the module is usable, only that it is currently loaded.
 *
 * @param module_name Name of the module to search for (e.g., "v4l2loopback").
 * @return 1 if the module is loaded, 0 otherwise.
 */
int check_kernel_module(const char *module_name)
{
    if (!module_name) return 0;

    FILE *fp = fopen("/proc/modules", "r");
    if (!fp) return 0;

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, module_name)) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* -------------------------------------------------------------------------- */
/**
 * @brief Check a group of related kernel modules.
 *
 * Useful for cases like V4L2 where multiple related modules may be needed.
 *
 * @param modules Array of module names.
 * @param count   Number of modules in array.
 * @param results Output array of flags (1 if found, 0 if not).
 */
void check_multiple_modules(const char *modules[], int count, int *results)
{
    for (int i = 0; i < count; i++) {
        results[i] = check_kernel_module(modules[i]);
    }
}


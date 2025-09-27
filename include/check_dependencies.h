/*
 * check_dependencies.h
 * --------------------------------------------
 * Public header for dependency checking utilities.
 *
 * Provides declarations for functions that verify whether specific
 * applications and kernel modules are present on the system.
 *
 * This header is paired with check_dependencies.c.
 */

#ifndef CHECK_DEPENDENCIES_H
#define CHECK_DEPENDENCIES_H

/* -------------------------------------------------------------------------- */
/**
 * @brief Check whether a user-space application is installed.
 *
 * Uses the `which` command to see if the application exists in PATH.
 *
 * @param app_name Name of the application (e.g., "ffmpeg").
 * @return 1 if installed, 0 otherwise.
 */
int check_application(const char *app_name);

/* -------------------------------------------------------------------------- */
/**
 * @brief Check whether a kernel module is currently loaded.
 *
 * Searches /proc/modules for the given module name.
 *
 * @param module_name Name of the module (e.g., "v4l2loopback").
 * @return 1 if the module is loaded, 0 otherwise.
 */
int check_kernel_module(const char *module_name);

/* -------------------------------------------------------------------------- */
/**
 * @brief Check multiple kernel modules at once.
 *
 * @param modules Array of module names to check.
 * @param count   Number of modules in array.
 * @param results Output array of flags (1 if found, 0 if not).
 */
void check_multiple_modules(const char *modules[], int count, int *results);


#endif /* CHECK_DEPENDENCIES_H */

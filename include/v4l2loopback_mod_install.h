/*
 * v4l2loopback_manager.h
 * --------------------------------------------
 * Header file for the V4L2loopback Automated Installation and Management Utility.
 * Provides interface definitions, constants, and function prototypes for managing
 * the v4l2loopback kernel module lifecycle including Git repository management,
 * DKMS integration, intelligent device allocation, and Secure Boot compatibility.
 *
 * This utility automates the complete installation and update workflow for the
 * modified v4l2loopback kernel module that supports more than 8 virtual video
 * devices (up to 16), with automatic conflict avoidance for existing hardware
 * video devices.
 *
 * Key capabilities:
 * - Automated repository cloning and version tracking
 * - DKMS-based installation for kernel update resilience
 * - Dynamic device number allocation (starting from /dev/video10)
 * - Secure Boot MOK key management and diagnostics
 * - Memory-safe string operations throughout
 * - Idempotent operation for safe repeated execution
 *
 * Author: Aidan A. Bradley
 * Date:   10/01/2025
 */

#ifndef V4L2LOOPBACK_MANAGER_H
#define V4L2LOOPBACK_MANAGER_H

#include <stddef.h>

// ============================================================================
// CONFIGURATION CONSTANTS
// ============================================================================

/**
 * Repository configuration
 * REPO_DIR: Local directory where v4l2loopback source will be cloned
 * REPO_URL: GitHub repository URL for the modified v4l2loopback source
 */
#define REPO_DIR "/home/user/Documents/v4l2loopback"
#define REPO_URL "https://github.com/aab18011/v4l2loopback.git"

/**
 * Module identification
 * MODULE_NAME: Kernel module name for DKMS operations
 */
#define MODULE_NAME "v4l2loopback"

/**
 * Buffer size limits
 * MAX_CMD_LEN: Maximum length for shell command construction
 * MAX_VERSION_LEN: Maximum length for git version strings
 * MAX_OUTPUT_LEN: Maximum length for command output capture
 */
#define MAX_CMD_LEN 4096
#define MAX_VERSION_LEN 128
#define MAX_OUTPUT_LEN 8192

// ============================================================================
// UTILITY FUNCTIONS - STRING OPERATIONS
// ============================================================================

/**
 * safe_strncpy - Bounds-checked string copy with null termination
 * @dest: Destination buffer
 * @src: Source string
 * @dest_size: Total size of destination buffer
 * 
 * Copies source string to destination with guaranteed null termination,
 * preventing buffer overflows. Always leaves room for null terminator.
 * 
 * Return: 0 on success, -1 if parameters are invalid
 */
static int safe_strncpy(char *dest, const char *src, size_t dest_size);

/**
 * safe_strncat - Bounds-checked string concatenation
 * @dest: Destination buffer (must contain valid null-terminated string)
 * @src: Source string to append
 * @dest_size: Total size of destination buffer
 * 
 * Appends source string to destination with overflow protection and
 * guaranteed null termination. Checks current length before appending.
 * 
 * Return: 0 on success, -1 if would overflow or parameters invalid
 */
static int safe_strncat(char *dest, const char *src, size_t dest_size);

// ============================================================================
// UTILITY FUNCTIONS - COMMAND EXECUTION
// ============================================================================

/**
 * exec_cmd - Execute command and capture output with exit code
 * @cmd: Shell command string to execute
 * @output: Buffer to store stdout (may be NULL to discard)
 * @output_size: Size of output buffer
 * @exit_code: Pointer to store command exit code (may be NULL)
 * 
 * Executes a shell command using popen() and captures stdout into the
 * provided buffer. Handles proper buffer management and exit code extraction.
 * This is the core command execution primitive used throughout the utility.
 * 
 * Return: 0 on successful execution, -1 on popen failure
 */
static int exec_cmd(const char *cmd, char *output, size_t output_size, 
                    int *exit_code);

/**
 * get_username - Get current effective user's username
 * @username: Buffer to store username
 * @size: Size of username buffer
 * 
 * Retrieves the username associated with the current effective UID using
 * getpwuid(). Used for git operations that should preserve proper ownership.
 * 
 * Return: 0 on success, -1 on failure
 */
static int get_username(char *username, size_t size);

/**
 * dir_exists - Check if directory exists
 * @path: Path to check
 * 
 * Return: 1 if path exists and is a directory, 0 otherwise
 */
static int dir_exists(const char *path);

/**
 * get_kernel_version - Get current running kernel version
 * @version: Buffer to store kernel version string
 * @size: Size of version buffer
 * 
 * Retrieves the kernel release string using uname(). This is used to verify
 * DKMS installation against the currently running kernel.
 * 
 * Return: 0 on success, -1 on failure
 */
static int get_kernel_version(char *version, size_t size);

// ============================================================================
// GIT REPOSITORY MANAGEMENT
// ============================================================================

/**
 * git_cmd - Execute git command as specific user in repository
 * @user: Username to execute git command as (via sudo -u)
 * @repo_dir: Repository directory path
 * @git_args: Git command arguments (after 'git')
 * @output: Buffer to store command output (may be NULL)
 * @output_size: Size of output buffer
 * 
 * Executes a git command within the repository directory as the specified
 * user. Automatically trims trailing newlines from output. Stderr is
 * redirected to /dev/null to suppress noise.
 * 
 * Return: Git command exit code, or -1 on execution failure
 */
static int git_cmd(const char *user, const char *repo_dir, 
                   const char *git_args, char *output, size_t output_size);

/**
 * clone_repo - Clone git repository as specific user
 * @user: Username to own the cloned repository
 * @repo_dir: Destination directory for clone
 * @repo_url: Git repository URL to clone from
 * 
 * Clones the v4l2loopback repository from GitHub. Uses sudo -u to ensure
 * proper file ownership.
 * 
 * Return: 0 on success, non-zero on failure
 */
static int clone_repo(const char *user, const char *repo_dir, 
                      const char *repo_url);

/**
 * get_version - Get git version string for DKMS naming
 * @user: Username for git operations
 * @repo_dir: Repository directory path
 * @version: Buffer to store version string
 * @size: Size of version buffer
 * 
 * Attempts to generate a version string using git describe. Falls back to
 * commit hash or "snapshot" if tags are unavailable. This version string
 * is used for DKMS module versioning.
 * 
 * Return: 0 on success, -1 if all methods fail (still provides fallback)
 */
static int get_version(const char *user, const char *repo_dir, 
                       char *version, size_t size);

// ============================================================================
// DKMS MODULE MANAGEMENT
// ============================================================================

/**
 * is_module_installed - Check if module is installed for current kernel
 * @version: Module version string to check
 * @kernel: Kernel version string to check against
 * 
 * Queries DKMS status to determine if the specified v4l2loopback version
 * is installed for the given kernel version.
 * 
 * Return: 1 if installed, 0 if not installed
 */
static int is_module_installed(const char *version, const char *kernel);

/**
 * cleanup_broken_dkms - Remove corrupted DKMS entries
 * 
 * Scans for v4l2loopback DKMS entries that have missing or incomplete
 * source directories (no dkms.conf file). Removes these broken entries
 * to prevent DKMS operation failures.
 * 
 * This cleanup is essential before attempting installation to ensure
 * DKMS is in a consistent state.
 */
static void cleanup_broken_dkms(void);

/**
 * remove_old_versions - Remove obsolete DKMS module versions
 * @current_version: Version to preserve (not remove)
 * 
 * Removes all v4l2loopback DKMS entries except the specified current
 * version. This frees disk space and prevents confusion from multiple
 * installed versions.
 */
static void remove_old_versions(const char *current_version);

/**
 * install_module - Install v4l2loopback via DKMS
 * @user: Username for source operations
 * @repo_dir: Source repository directory
 * @version: Version string for DKMS naming
 * 
 * Performs complete DKMS installation workflow:
 * 1. Removes any existing installation of the same version
 * 2. Copies source files to /usr/src/v4l2loopback-<version>
 * 3. Adds module to DKMS tree (dkms add)
 * 4. Builds module against current kernel (dkms build)
 * 5. Installs module with signing if required (dkms install)
 * 
 * On Secure Boot systems, DKMS will automatically sign the module using
 * the MOK key if available.
 * 
 * Return: 0 on success, -1 on any step failure
 */
static int install_module(const char *user, const char *repo_dir, 
                          const char *version);

// ============================================================================
// MODULE LOADING AND DEVICE MANAGEMENT
// ============================================================================

/**
 * is_module_loaded - Check if v4l2loopback is currently loaded
 * 
 * Queries lsmod to determine if the v4l2loopback kernel module is
 * currently loaded in memory.
 * 
 * Return: 1 if loaded, 0 if not loaded
 */
static int is_module_loaded(void);

/**
 * get_available_video_numbers - Find available video device numbers
 * @numbers: Array to store available device numbers
 * @count: Desired number of devices
 * @actual_count: Output parameter for number of devices found
 * 
 * Scans /dev starting from video10 to find available (non-existent) video
 * device numbers. This prevents conflicts with existing hardware like
 * webcams which typically occupy video0-9.
 * 
 * The function continues scanning up to video255 (V4L2 subsystem limit)
 * and stops when it finds the requested count or reaches the limit.
 */
static void get_available_video_numbers(int *numbers, int count, 
                                       int *actual_count);

/**
 * load_module - Load v4l2loopback kernel module with dynamic device allocation
 * 
 * Loads the v4l2loopback kernel module with intelligent configuration:
 * 1. Checks if module is already loaded and unloads if necessary
 * 2. Scans for available video device numbers starting from video10
 * 3. Constructs module parameters with device list and labels
 * 4. Loads module with exclusive_caps=1 for application compatibility
 * 5. Detects and provides diagnostics for Secure Boot key issues
 * 
 * On successful load, prints the list of created device paths.
 * On Secure Boot key rejection, provides detailed troubleshooting steps.
 * 
 * Return: 0 on success, -1 on failure
 */
static int load_module(void);

// ============================================================================
// MAIN PROGRAM
// ============================================================================

/**
 * main - Entry point for v4l2loopback manager
 * 
 * Orchestrates the complete v4l2loopback management workflow:
 * 1. Validates root privileges
 * 2. Determines current username and kernel version
 * 3. Clones repository if not present
 * 4. Fetches latest changes and compares commits
 * 5. Determines if installation/update is needed
 * 6. Performs DKMS cleanup and installation if needed
 * 7. Loads module with dynamic device allocation
 * 
 * The program is idempotent - it can be run multiple times safely and will
 * only perform work when updates are available or the module is not loaded.
 * 
 * Must be run as root (via sudo) for kernel module operations.
 * 
 * Return: 0 on success, 1 on error
 */
int main(void);

#endif /* V4L2LOOPBACK_MANAGER_H */
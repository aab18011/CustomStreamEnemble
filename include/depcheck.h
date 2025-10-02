/*
 * depcheck.h
 *
 * Author: Aidan A. Bradley
 * Date: 09/30/2025
 * 
 * Header file for Linux Dependency Checker
 * 
 * This header provides the public API for the dependency checking system.
 * It can be included in other projects that need to verify dependencies
 * programmatically rather than as a standalone tool.
 * 
 * Usage:
 *   #include "depcheck.h"
 *   
 *   // Check if a single program exists
 *   Dependency dep;
 *   if (find_dependency("ffmpeg", &dep, detect_distro())) {
 *       printf("Found at: %s\n", dep.path);
 *   }
 *   
 *   // Check from JSON
 *   const char *json = "{\"dependencies\": [\"gcc\", \"make\"]}";
 *   int result = check_dependencies_from_json(json);
 * 
 * Thread Safety: NOT thread-safe (uses strtok, system(), getenv())
 * Re-entrancy: NOT re-entrant (modifies global environment)
 */

#ifndef DEPCHECK_H
#define DEPCHECK_H

/*
 * Standard includes needed by API consumers
 */
#include <stdio.h>
#include <stdlib.h>

/*
 * Version information
 * Semantic versioning: MAJOR.MINOR.PATCH
 * - MAJOR: Incompatible API changes
 * - MINOR: Add functionality (backwards compatible)
 * - PATCH: Bug fixes (backwards compatible)
 */
#define DEPCHECK_VERSION_MAJOR 1
#define DEPCHECK_VERSION_MINOR 0
#define DEPCHECK_VERSION_PATCH 0
#define DEPCHECK_VERSION_STRING "1.0.0"

/*
 * Constants
 * These define limits for the dependency checker
 * 
 * MAX_PATH: Maximum length of a file path
 *   - Linux PATH_MAX is typically 4096
 *   - We define our own for portability (PATH_MAX not guaranteed)
 * 
 * MAX_CMD: Maximum length of a shell command
 *   - Needs to be larger than MAX_PATH to accommodate arguments
 *   - Used for constructing package manager queries
 * 
 * MAX_DEP_NAME: Maximum length of a dependency name
 *   - Most package names are < 50 chars
 *   - 256 provides comfortable headroom
 */
#define MAX_PATH 4096
#define MAX_CMD 8192
#define MAX_DEP_NAME 256

/*
 * DistroType
 * 
 * Enumeration of supported Linux distribution families.
 * Each family shares package management tools and system organization.
 * 
 * Purpose:
 * - Determines which package manager to query
 * - Influences search paths (distro-specific layouts)
 * - Affects how results are interpreted
 * 
 * Market Coverage (approximate):
 * - DEBIAN: 35-40% (Ubuntu, Debian, Mint, Pop!_OS, Elementary)
 * - REDHAT: 20-25% (RHEL, CentOS, Fedora, Rocky, AlmaLinux)
 * - ARCH: 10-15% (Arch, Manjaro, EndeavourOS, Garuda)
 * - SUSE: 5-8% (openSUSE, SLES)
 * - ALPINE: 3-5% (Alpine, popular in containers)
 * - Others: <5% combined
 * 
 * Total: ~75%+ of Linux installations covered
 */
typedef enum {
    DISTRO_UNKNOWN = 0,   /* Unrecognized distribution - use fallback methods */
    DISTRO_DEBIAN,        /* Debian family: dpkg, apt */
    DISTRO_REDHAT,        /* Red Hat family: rpm, yum, dnf */
    DISTRO_ARCH,          /* Arch family: pacman */
    DISTRO_SUSE,          /* SUSE family: zypper, rpm */
    DISTRO_ALPINE,        /* Alpine: apk */
    DISTRO_GENTOO,        /* Gentoo: emerge, equery */
    DISTRO_VOID,          /* Void Linux: xbps */
    DISTRO_SLACKWARE      /* Slackware: traditional package files */
} DistroType;

/*
 * Dependency
 * 
 * Structure containing information about a checked dependency.
 * 
 * Fields:
 * - name: The program name being searched for (e.g., "ffmpeg")
 * - path: Full path where the program was found (e.g., "/usr/bin/ffmpeg")
 *         Empty string if not found
 * - found: Boolean flag (0/1) indicating if file exists anywhere
 * - executable: Boolean flag (0/1) indicating if file is executable and working
 * 
 * State combinations:
 * 1. found=0, executable=0: Not found anywhere
 * 2. found=1, executable=0: Found but not executable (permission issue, corruption)
 * 3. found=1, executable=1: Found and working (success!)
 * 
 * Note: found=0, executable=1 should never occur
 * 
 * Memory: 4352 bytes per structure (stack allocated, safe)
 */
typedef struct {
    char name[MAX_DEP_NAME];   /* Program name (null-terminated) */
    char path[MAX_PATH];       /* Full path to executable (null-terminated) */
    int found;                 /* Boolean: file exists */
    int executable;            /* Boolean: file is executable and working */
} Dependency;

/*
 * ============================================================================
 * CORE API FUNCTIONS
 * ============================================================================
 * These are the main functions for checking dependencies.
 */

/*
 * detect_distro()
 * 
 * Automatically detects the Linux distribution family.
 * 
 * Returns: DistroType enum value
 *          DISTRO_UNKNOWN if detection fails
 * 
 * Detection method:
 * 1. Reads /etc/os-release (modern systemd standard)
 * 2. Falls back to legacy release files for older systems
 * 
 * Thread safety: Safe (read-only operations)
 * Performance: Fast (<1ms typically), results can be cached
 * 
 * Example:
 *   DistroType distro = detect_distro();
 *   printf("Running on: %s\n", distro_name(distro));
 */
DistroType detect_distro(void);

/*
 * find_dependency()
 * 
 * Searches for a program using multiple strategies.
 * This is the core function for locating dependencies.
 * 
 * Parameters:
 * - prog_name: Name of program to find (e.g., "ffmpeg")
 * - dep: Pointer to Dependency structure to fill with results
 * - distro: Distribution type (from detect_distro())
 * 
 * Returns:
 * - 1: Program found and executable
 * - 0: Program not found or not executable
 * 
 * Side effects:
 * - Fills dep structure with results
 * - May execute shell commands (command -v, which)
 * - May read environment variables (PATH)
 * 
 * Search strategy (in order):
 * 1. command -v (POSIX standard, fastest)
 * 2. which command (common but not POSIX)
 * 3. PATH environment variable (manual search)
 * 4. Standard binary directories (/usr/bin, /bin, etc.)
 * 5. Distribution-specific paths
 * 6. Library directories (last resort)
 * 
 * Thread safety: NOT thread-safe (uses getenv, strtok)
 * Performance: Varies (fast if in PATH, slower for filesystem search)
 * 
 * Example:
 *   Dependency dep;
 *   DistroType distro = detect_distro();
 *   if (find_dependency("python3", &dep, distro)) {
 *       printf("Python3 found at: %s\n", dep.path);
 *   } else {
 *       fprintf(stderr, "Python3 not found\n");
 *   }
 */
int find_dependency(const char *prog_name, Dependency *dep, DistroType distro);

/*
 * check_dependencies_from_json()
 * 
 * Parses JSON and checks multiple dependencies.
 * This is the high-level API for checking a list of dependencies.
 * 
 * Parameters:
 * - json_str: Null-terminated JSON string
 * 
 * Expected JSON format:
 *   {
 *     "dependencies": ["program1", "program2", "program3"]
 *   }
 * 
 * Returns:
 * -  0: All dependencies satisfied
 * -  1: Some dependencies missing
 * - -1: Error (invalid JSON, missing dependencies array)
 * 
 * Side effects:
 * - Prints progress to stdout
 * - Prints warnings to stderr
 * - May execute shell commands
 * - Allocates memory internally (freed before return)
 * 
 * Output format:
 *   Detected distribution: Debian-based
 *   Checking 3 dependencies...
 *   
 *   [1/3] ffmpeg: ✓ FOUND at /usr/bin/ffmpeg
 *   [2/3] gcc: ✓ FOUND at /usr/bin/gcc
 *   [3/3] python3: ✓ FOUND at /usr/bin/python3
 *   
 *   ========================================
 *   Summary: 3/3 dependencies satisfied
 *   ========================================
 * 
 * Thread safety: NOT thread-safe
 * Memory: Allocates temporary buffers (all freed before return)
 * 
 * Example:
 *   const char *json = "{\"dependencies\": [\"git\", \"make\"]}";
 *   int result = check_dependencies_from_json(json);
 *   if (result == 0) {
 *       printf("All dependencies satisfied!\n");
 *   }
 */
int check_dependencies_from_json(const char *json_str);

/*
 * ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================
 * Helper functions for more granular control.
 */

/*
 * distro_name()
 * 
 * Converts DistroType enum to human-readable string.
 * 
 * Parameters:
 * - distro: DistroType enum value
 * 
 * Returns: Constant string (do not free)
 * 
 * Possible return values:
 * - "Debian-based"
 * - "RedHat-based"
 * - "Arch-based"
 * - "SUSE-based"
 * - "Alpine"
 * - "Gentoo"
 * - "Void"
 * - "Slackware"
 * - "Unknown"
 * 
 * Thread safety: Safe (returns constant strings)
 * 
 * Example:
 *   DistroType d = detect_distro();
 *   printf("Distribution: %s\n", distro_name(d));
 */
const char* distro_name(DistroType distro);

/*
 * check_package_installed()
 * 
 * Queries package manager to check if a package is installed.
 * Useful for diagnosing "installed but not in PATH" situations.
 * 
 * Parameters:
 * - package_name: Name of package to check
 * - distro: Distribution type (determines which package manager to query)
 * 
 * Returns:
 * - 1: Package is installed according to package manager
 * - 0: Package not found or package manager query failed
 * 
 * Package managers queried by distro:
 * - DEBIAN: dpkg, dpkg-query
 * - REDHAT: rpm, dnf, yum
 * - ARCH: pacman
 * - SUSE: rpm, zypper
 * - ALPINE: apk
 * - GENTOO: equery, qlist
 * - VOID: xbps-query
 * - SLACKWARE: /var/log/packages
 * 
 * Side effects:
 * - Executes package manager commands
 * - May be slow (package database queries)
 * 
 * Thread safety: NOT thread-safe (uses system())
 * Performance: Slow (100-500ms typically)
 * 
 * Note: A package being installed doesn't guarantee the executable
 *       is in a findable location. Use find_dependency() first.
 * 
 * Example:
 *   if (check_package_installed("ffmpeg", DISTRO_DEBIAN)) {
 *       printf("Package installed but executable not found\n");
 *       printf("May need to configure PATH or reinstall\n");
 *   }
 */
int check_package_installed(const char *package_name, DistroType distro);

/*
 * check_sandbox_restrictions()
 * 
 * Detects if running in a restricted/sandboxed environment.
 * 
 * Returns:
 * - 1: Running in restricted environment (Docker, Snap, chroot, etc.)
 * - 0: Running in normal environment
 * 
 * Detection heuristics:
 * - Cannot access /usr/bin or /bin
 * - PATH variable missing or unusually short
 * - Multiple restrictions detected
 * 
 * Use case:
 * - Warn users that results may be limited
 * - Adjust search strategy for containers
 * - Provide troubleshooting guidance
 * 
 * Thread safety: Safe (read-only checks)
 * Performance: Fast (<1ms)
 * 
 * Example:
 *   if (check_sandbox_restrictions()) {
 *       fprintf(stderr, "WARNING: Limited environment detected\n");
 *       fprintf(stderr, "Some dependencies may not be visible\n");
 *   }
 */
int check_sandbox_restrictions(void);

/*
 * is_executable()
 * 
 * Checks if a file exists and has execute permissions.
 * 
 * Parameters:
 * - path: Full path to file to check
 * 
 * Returns:
 * - 1: File exists, is regular file or symlink, and is executable
 * - 0: File doesn't exist, is not executable, or is not a regular file
 * 
 * Checks performed:
 * 1. File exists (stat succeeds)
 * 2. Is regular file or symlink (not directory, device, etc.)
 * 3. Has execute permission for user, group, or other
 * 
 * Thread safety: Safe (read-only operations)
 * Performance: Fast (single stat syscall)
 * 
 * Note: This only checks permissions, not if the program actually works.
 *       Use verify_executable() for deeper validation.
 * 
 * Example:
 *   if (is_executable("/usr/bin/python3")) {
 *       printf("Python3 is executable\n");
 *   }
 */
int is_executable(const char *path);

/*
 * verify_executable()
 * 
 * Verifies a program actually runs (not just has execute permission).
 * 
 * Parameters:
 * - path: Full path to program
 * - prog_name: Name of program (for context)
 * 
 * Returns:
 * - 1: Program runs successfully
 * - 0: Program doesn't run or isn't executable
 * 
 * Verification strategy:
 * - Tries common flags: --version, -v, -version, version
 * - If any produces output, considers program working
 * - Falls back to permission check if all version checks fail
 * 
 * Why this matters:
 * - File might be executable but corrupted
 * - Missing shared libraries
 * - Wrong architecture (ARM binary on x86)
 * - Script with missing interpreter
 * 
 * Thread safety: NOT thread-safe (uses popen)
 * Performance: Moderate (executes program, ~10-100ms)
 * 
 * Example:
 *   if (verify_executable("/usr/bin/ffmpeg", "ffmpeg")) {
 *       printf("FFmpeg is working\n");
 *   } else {
 *       printf("FFmpeg found but not working\n");
 *   }
 */
int verify_executable(const char *path, const char *prog_name);

/*
 * ============================================================================
 * LOWER-LEVEL SEARCH FUNCTIONS
 * ============================================================================
 * These are used internally but exposed for advanced use cases.
 */

/*
 * search_directory()
 * 
 * Searches for a program in a specific directory.
 * 
 * Parameters:
 * - dir: Directory path to search
 * - prog_name: Program name to look for
 * - result_path: Buffer to store full path if found (must be MAX_PATH size)
 * 
 * Returns:
 * - 1: Program found in directory
 * - 0: Program not found or directory doesn't exist
 * 
 * Side effects:
 * - Fills result_path with full path if found
 * - Resolves symlinks to canonical path
 * 
 * Thread safety: Safe (read-only operations)
 * Performance: Fast (single access syscall)
 * 
 * Example:
 *   char path[MAX_PATH];
 *   if (search_directory("/usr/local/bin", "myapp", path)) {
 *       printf("Found at: %s\n", path);
 *   }
 */
int search_directory(const char *dir, const char *prog_name, char *result_path);

/*
 * check_path_env()
 * 
 * Searches all directories in PATH environment variable.
 * 
 * Parameters:
 * - prog_name: Program name to find
 * - result_path: Buffer to store full path if found (must be MAX_PATH size)
 * 
 * Returns:
 * - 1: Program found in PATH
 * - 0: Program not found or PATH not set
 * 
 * Thread safety: NOT thread-safe (uses strtok, getenv)
 * Performance: Moderate (searches multiple directories)
 * 
 * Example:
 *   char path[MAX_PATH];
 *   if (check_path_env("node", path)) {
 *       printf("Node.js at: %s\n", path);
 *   }
 */
int check_path_env(const char *prog_name, char *result_path);

/*
 * check_which()
 * 
 * Uses 'which' command to find program.
 * 
 * Parameters:
 * - prog_name: Program name to find
 * - result_path: Buffer to store full path if found (must be MAX_PATH size)
 * 
 * Returns:
 * - 1: which found the program
 * - 0: which didn't find program or which command unavailable
 * 
 * Thread safety: NOT thread-safe (uses popen)
 * Performance: Moderate (executes external command)
 * 
 * Note: 'which' is not POSIX standard. Use check_command() for better portability.
 * 
 * Example:
 *   char path[MAX_PATH];
 *   if (check_which("gcc", path)) {
 *       printf("GCC at: %s\n", path);
 *   }
 */
int check_which(const char *prog_name, char *result_path);

/*
 * check_command()
 * 
 * Uses 'command -v' to find program (POSIX standard).
 * 
 * Parameters:
 * - prog_name: Program name to find
 * - result_path: Buffer to store full path if found (must be MAX_PATH size)
 * 
 * Returns:
 * - 1: command found the program
 * - 0: command didn't find program
 * 
 * Thread safety: NOT thread-safe (uses popen)
 * Performance: Fast (shell built-in)
 * 
 * Advantages over check_which():
 * - POSIX standard (more portable)
 * - Shell built-in (faster)
 * - Always available (doesn't require external tool)
 * 
 * Example:
 *   char path[MAX_PATH];
 *   if (check_command("python3", path)) {
 *       printf("Python3 at: %s\n", path);
 *   }
 */
int check_command(const char *prog_name, char *result_path);

/*
 * ============================================================================
 * ERROR CODES AND STATUS
 * ============================================================================
 */

/*
 * Return code definitions for check_dependencies_from_json()
 * These match Unix conventions for easy shell scripting.
 */
#define DEPCHECK_SUCCESS 0           /* All dependencies satisfied */
#define DEPCHECK_MISSING_DEPS 1      /* Some dependencies not found */
#define DEPCHECK_ERROR -1            /* Fatal error (invalid JSON, etc.) */

/*
 * ============================================================================
 * USAGE NOTES
 * ============================================================================
 * 
 * Compilation:
 *   gcc -o depcheck depcheck.c -lcjson -Wall -Wextra
 * 
 * Linking:
 *   Requires cJSON library: apt-get install libcjson-dev
 * 
 * Common patterns:
 * 
 * 1. Check single dependency:
 *    DistroType distro = detect_distro();
 *    Dependency dep;
 *    if (find_dependency("ffmpeg", &dep, distro)) {
 *        printf("FFmpeg: %s\n", dep.path);
 *    }
 * 
 * 2. Check list from JSON:
 *    const char *json = load_json_file("deps.json");
 *    int result = check_dependencies_from_json(json);
 *    return result;
 * 
 * 3. Custom search:
 *    char path[MAX_PATH];
 *    if (check_command("myapp", path)) {
 *        printf("Found: %s\n", path);
 *    } else if (search_directory("/opt/myapp/bin", "myapp", path)) {
 *        printf("Found in custom location: %s\n", path);
 *    }
 * 
 * Thread Safety Summary:
 * - detect_distro(): Thread-safe
 * - distro_name(): Thread-safe
 * - is_executable(): Thread-safe
 * - check_sandbox_restrictions(): Thread-safe
 * - All other functions: NOT thread-safe
 * 
 * For multi-threaded use, serialize calls with mutexes or use
 * separate processes.
 */

#endif /* DEPCHECK_H */
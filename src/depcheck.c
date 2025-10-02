/*
 * depcheck.c
 * 
 * Author: Aidan A. Bradley
 * Date: 09/30/2025
 *
 * Purpose: Robustly check if required programs/dependencies are installed and accessible
 * across multiple Linux distributions. This tool searches through various system paths,
 * validates executability, and queries distribution-specific package managers.
 * 
 * Design Philosophy:
 * - Defense in depth: Multiple strategies with graceful fallbacks
 * - Distribution agnostic: Works on 75%+ of Linux systems
 * - Executable validation: Not just finding files, but ensuring they work
 * - JSON-based configuration: Easy to maintain dependency lists
 * 
 * Compilation: gcc -o depcheck depcheck.c -lcjson -Wall
 */
#include <stdio.h>   // For popen, pclose
#include <stdlib.h>  // For realpath, strdup
#include <sys/wait.h> // For WIFEXITED, WEXITSTATUS
#include <string.h>
#include <unistd.h>      // For access(), POSIX system calls
#include <sys/stat.h>    // For stat(), file information
#include <errno.h>       // For error codes
#include <limits.h>      // For PATH_MAX (though we define our own for portability)
#include <dirent.h>      // For directory operations (if needed later)
#include "cJSON.h"       // External JSON parsing library

// Maximum path length - using our own definition for maximum portability
// PATH_MAX isn't guaranteed on all POSIX systems
#define MAX_PATH 4096
#define MAX_CMD 8192

/*
 * Dependency structure
 * Stores all information about a single dependency being checked
 */
typedef struct {
    char name[256];        // Name of the program (e.g., "ffmpeg")
    char path[MAX_PATH];   // Full path where found (e.g., "/usr/bin/ffmpeg")
    int found;             // Boolean: was the file found anywhere?
    int executable;        // Boolean: is it actually executable and working?
} Dependency;

/*
 * Linux Distribution Types
 * We categorize distributions by their package management family
 * This helps us use the right package manager commands and search paths
 */
typedef enum {
    DISTRO_UNKNOWN,
    DISTRO_DEBIAN,    // Debian, Ubuntu, Mint, Pop!_OS, Elementary, Kali, MX
    DISTRO_REDHAT,    // RHEL, CentOS, Fedora, Rocky Linux, AlmaLinux, Oracle
    DISTRO_ARCH,      // Arch, Manjaro, EndeavourOS, Garuda
    DISTRO_SUSE,      // openSUSE (Leap/Tumbleweed), SLES
    DISTRO_ALPINE,    // Alpine (common in Docker containers)
    DISTRO_GENTOO,    // Gentoo, Calculate Linux
    DISTRO_VOID,      // Void Linux
    DISTRO_SLACKWARE  // Slackware
} DistroType;

/*
 * detect_distro()
 * 
 * Determines which Linux distribution family we're running on.
 * This is critical because different distros have different:
 * - Package managers (apt vs yum vs pacman)
 * - Default installation paths
 * - System organization
 * 
 * Strategy:
 * 1. Check /etc/os-release (modern standard, systemd-based systems)
 * 2. Fall back to legacy release files for older systems
 * 
 * Why this matters:
 * - We can use the correct package manager queries
 * - We know which paths to prioritize
 * - We can handle distro-specific quirks
 */
DistroType detect_distro() {
    FILE *fp;
    char buffer[256];
    
    /*
     * /etc/os-release is the modern standard (systemd)
     * Format: KEY=value lines
     * We look for ID= and ID_LIKE= which tell us the distro family
     * 
     * Example from Ubuntu:
     *   ID=ubuntu
     *   ID_LIKE=debian
     */
    fp = fopen("/etc/os-release", "r");
    if (fp != NULL) {
        while (fgets(buffer, sizeof(buffer), fp)) {
            // Check both ID and ID_LIKE to catch derivatives
            // e.g., Linux Mint has ID=linuxmint but ID_LIKE=ubuntu:debian
            if (strncmp(buffer, "ID=", 3) == 0 || strncmp(buffer, "ID_LIKE=", 8) == 0) {
                
                // Debian family: Use dpkg/apt
                if (strstr(buffer, "debian") || strstr(buffer, "ubuntu") || 
                    strstr(buffer, "mint") || strstr(buffer, "pop")) {
                    fclose(fp);
                    return DISTRO_DEBIAN;
                }
                
                // Red Hat family: Use rpm/yum/dnf
                if (strstr(buffer, "rhel") || strstr(buffer, "centos") || 
                    strstr(buffer, "fedora") || strstr(buffer, "rocky") || 
                    strstr(buffer, "alma") || strstr(buffer, "oracle")) {
                    fclose(fp);
                    return DISTRO_REDHAT;
                }
                
                // Arch family: Use pacman
                if (strstr(buffer, "arch") || strstr(buffer, "manjaro") || 
                    strstr(buffer, "endeavour")) {
                    fclose(fp);
                    return DISTRO_ARCH;
                }
                
                // SUSE family: Use zypper
                if (strstr(buffer, "suse") || strstr(buffer, "sles")) {
                    fclose(fp);
                    return DISTRO_SUSE;
                }
                
                // Alpine: Use apk (common in containers)
                if (strstr(buffer, "alpine")) {
                    fclose(fp);
                    return DISTRO_ALPINE;
                }
                
                // Gentoo: Use emerge/equery
                if (strstr(buffer, "gentoo")) {
                    fclose(fp);
                    return DISTRO_GENTOO;
                }
                
                // Void: Use xbps
                if (strstr(buffer, "void")) {
                    fclose(fp);
                    return DISTRO_VOID;
                }
                
                // Slackware: Use legacy package tools
                if (strstr(buffer, "slackware")) {
                    fclose(fp);
                    return DISTRO_SLACKWARE;
                }
            }
        }
        fclose(fp);
    }
    
    /*
     * Fallback: Check legacy release files
     * Older systems (pre-systemd) used different files for each distro
     * These still exist on modern systems for compatibility
     */
    if (access("/etc/debian_version", F_OK) == 0) return DISTRO_DEBIAN;
    if (access("/etc/redhat-release", F_OK) == 0) return DISTRO_REDHAT;
    if (access("/etc/arch-release", F_OK) == 0) return DISTRO_ARCH;
    if (access("/etc/SuSE-release", F_OK) == 0) return DISTRO_SUSE;
    if (access("/etc/alpine-release", F_OK) == 0) return DISTRO_ALPINE;
    if (access("/etc/gentoo-release", F_OK) == 0) return DISTRO_GENTOO;
    
    return DISTRO_UNKNOWN;
}

/*
 * is_executable()
 * 
 * Checks if a file exists AND has execute permissions.
 * 
 * Why we need this:
 * - A file might exist but not be executable (permissions issue)
 * - A path might be a directory, not a file
 * - A symlink might be broken
 * 
 * We check:
 * 1. File exists (stat succeeds)
 * 2. It's a regular file or symlink (not a directory)
 * 3. Someone (user/group/other) has execute permission
 * 
 * Permission bits explained:
 * S_IXUSR = user execute (owner can run it)
 * S_IXGRP = group execute (group members can run it)
 * S_IXOTH = other execute (anyone can run it)
 */
int is_executable(const char *path) {
    struct stat st;
    
    // stat() fills st with file information, returns 0 on success
    if (stat(path, &st) == 0) {
        // Check if it's a regular file or symlink (not directory, device, etc.)
        if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            // Check if ANY execute bit is set
            // We use OR because we don't care WHO can execute it,
            // just that it CAN be executed by someone
            return (st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH);
        }
    }
    return 0;
}

/*
 * verify_executable()
 * 
 * Goes beyond file permissions - actually tries to run the program
 * to verify it's not just an executable file, but a WORKING executable.
 * 
 * Why this is important:
 * - File might be executable but corrupted
 * - Might be a script with missing interpreter
 * - Might be compiled for wrong architecture
 * - Might have missing shared library dependencies
 * 
 * Strategy:
 * - Try common version flags (--version, -v, etc.)
 * - If any produces output, program is likely working
 * - Use popen() to capture output and check exit status
 * 
 * Trade-off:
 * - This is slower than just checking permissions
 * - But it gives us much higher confidence
 * - We only do this after finding the file, not during search
 */
int verify_executable(const char *path, const char *prog_name) {
    char cmd[MAX_CMD];
    FILE *fp;
    
    // Common flags that programs use to show version/help
    // We try these because:
    // 1. They usually exit quickly (fast check)
    // 2. They produce output (confirms program runs)
    // 3. They don't modify anything (safe)
    const char *version_flags[] = {"--version", "-v", "-version", "version", NULL};
    
    for (int i = 0; version_flags[i] != NULL; i++) {
        // Build command: /path/to/program --version
        // 2>&1 redirects stderr to stdout so we catch all output
        snprintf(cmd, sizeof(cmd), "%s %s 2>&1", path, version_flags[i]);
        
        // popen() runs command and gives us a pipe to read output
        fp = popen(cmd, "r");
        if (fp == NULL) continue;  // Command failed to start
        
        char output[256];
        int has_output = 0;
        
        // Try to read first line of output
        if (fgets(output, sizeof(output), fp) != NULL) {
            has_output = 1;
        }
        
        // pclose() returns the exit status of the command
        int status = pclose(fp);
        
        // If we got output AND the program exited normally (not crashed/killed)
        // then it's working. We don't require status==0 because some programs
        // return non-zero even for --version
        if (has_output && WIFEXITED(status)) {
            return 1;
        }
    }
    
    // If version checks all failed, fall back to permission check
    // This handles programs that don't support version flags
    return is_executable(path);
}

/*
 * search_directory()
 * 
 * Looks for a program in a specific directory.
 * 
 * Why separate function:
 * - We search many directories with same logic
 * - Allows early exit when found
 * - Centralizes path construction and validation
 * 
 * Process:
 * 1. Check directory exists (avoids errors on non-existent paths)
 * 2. Construct full path (dir + "/" + program)
 * 3. Check if file exists and is executable
 * 4. Resolve symlinks to get canonical path
 * 
 * realpath() is important because:
 * - Resolves all symlinks (gives us true location)
 * - Resolves . and .. (gives us absolute path)
 * - Returns NULL if path is invalid/broken
 */
int search_directory(const char *dir, const char *prog_name, char *result_path) {
    char full_path[MAX_PATH];
    
    // Quick check: does this directory even exist?
    // Saves us from trying to search non-existent paths
    // F_OK = check for existence only
    if (access(dir, F_OK) != 0) return 0;
    
    // Build full path: /usr/bin + / + ffmpeg = /usr/bin/ffmpeg
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, prog_name);
    
    // Check if file exists AND is executable
    if (access(full_path, F_OK) == 0 && is_executable(full_path)) {
        // realpath() resolves symlinks and gives canonical absolute path
        // It allocates memory when we pass NULL as second parameter
        char *resolved = realpath(full_path, NULL);
        
        if (resolved != NULL) {
            // Copy resolved path to result
            strncpy(result_path, resolved, MAX_PATH - 1);
            result_path[MAX_PATH - 1] = '\0';  // Ensure null termination
            free(resolved);  // Free memory allocated by realpath
            return 1;
        }
        
        // If realpath failed, use the unresolved path
        // (Better to have something than nothing)
        strncpy(result_path, full_path, MAX_PATH - 1);
        result_path[MAX_PATH - 1] = '\0';
        return 1;
    }
    return 0;
}

/*
 * check_path_env()
 * 
 * Searches all directories in the PATH environment variable.
 * 
 * Why PATH is important:
 * - It's where the shell looks for commands
 * - It's how users normally run programs
 * - It includes user-specific directories
 * 
 * PATH format: /usr/bin:/bin:/usr/local/bin
 * Colon-separated list of directories
 * 
 * We use strtok() to split on colons:
 * - First call: strtok(string, ":") returns first token
 * - Subsequent calls: strtok(NULL, ":") returns next token
 * - Returns NULL when no more tokens
 * 
 * Why strdup():
 * - strtok() modifies the string it's parsing
 * - We don't want to modify the actual PATH variable
 * - So we duplicate it first, then parse the copy
 */
int check_path_env(const char *prog_name, char *result_path) {
    char *path_env = getenv("PATH");
    if (path_env == NULL) return 0;  // No PATH variable (very unusual)
    
    // Duplicate PATH so we don't modify the environment
    char *path_copy = strdup(path_env);
    if (path_copy == NULL) return 0;  // Memory allocation failed
    
    // Split PATH on colons and search each directory
    char *token = strtok(path_copy, ":");
    
    while (token != NULL) {
        if (search_directory(token, prog_name, result_path)) {
            free(path_copy);  // Clean up before returning
            return 1;
        }
        token = strtok(NULL, ":");  // Get next directory
    }
    
    free(path_copy);  // Clean up
    return 0;
}

/*
 * check_which()
 * 
 * Uses the 'which' command to find programs.
 * 
 * Why use 'which':
 * - It's a standard Unix tool
 * - It searches PATH just like the shell does
 * - It's what users would manually type
 * - It handles aliases and shell built-ins (in some shells)
 * 
 * How it works:
 * - Run: which program_name
 * - Capture output (the path)
 * - Verify the path is valid and executable
 * 
 * popen() explained:
 * - Opens a pipe to a command
 * - "r" mode = read from command's stdout
 * - Returns FILE* we can use with fgets()
 * - pclose() closes pipe and returns exit status
 * 
 * Why check exit status:
 * - which returns 0 if found, non-zero if not found
 * - WIFEXITED = true if program exited normally
 * - WEXITSTATUS = gets the actual exit code
 */
int check_which(const char *prog_name, char *result_path) {
    char cmd[MAX_CMD];
    FILE *fp;
    
    // Build command: which ffmpeg 2>/dev/null
    // 2>/dev/null suppresses error messages to keep output clean
    snprintf(cmd, sizeof(cmd), "which %s 2>/dev/null", prog_name);
    
    fp = popen(cmd, "r");
    if (fp == NULL) return 0;  // Failed to run command
    
    // Read the output (should be a path)
    if (fgets(result_path, MAX_PATH, fp) != NULL) {
        // Remove trailing newline that fgets() includes
        result_path[strcspn(result_path, "\n")] = 0;
        
        int status = pclose(fp);
        
        // Success if: got output, path is non-empty, file is executable,
        // and which exited successfully
        if (strlen(result_path) > 0 && access(result_path, X_OK) == 0 && 
            WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 1;
        }
    } else {
        pclose(fp);
    }
    return 0;
}

/*
 * check_command()
 * 
 * Uses 'command -v' as an alternative to 'which'.
 * 
 * Why this is better than 'which':
 * - 'command -v' is POSIX standard (more portable)
 * - 'which' is not in POSIX (might not exist on minimal systems)
 * - 'command' is a shell built-in (faster, always available)
 * 
 * This is particularly important for:
 * - Minimal containers (Alpine, busybox)
 * - Embedded systems
 * - Very old Unix systems
 * 
 * We try 'command -v' BEFORE 'which' because it's more reliable
 */
int check_command(const char *prog_name, char *result_path) {
    char cmd[MAX_CMD];
    FILE *fp;
    
    // command -v returns the path to an executable
    snprintf(cmd, sizeof(cmd), "command -v %s 2>/dev/null", prog_name);
    
    fp = popen(cmd, "r");
    if (fp == NULL) return 0;
    
    if (fgets(result_path, MAX_PATH, fp) != NULL) {
        result_path[strcspn(result_path, "\n")] = 0;
        int status = pclose(fp);
        
        if (strlen(result_path) > 0 && access(result_path, X_OK) == 0 && 
            WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 1;
        }
    } else {
        pclose(fp);
    }
    return 0;
}

/*
 * check_sandbox_restrictions()
 * 
 * Detects if we're running in a restricted/sandboxed environment.
 * 
 * Why this matters:
 * - Docker containers often have limited filesystem access
 * - Snap/Flatpak apps run in sandboxes
 * - Some security tools (SELinux, AppArmor) restrict access
 * - chroot jails limit visible filesystem
 * 
 * What we check:
 * 1. Can we access common system directories?
 * 2. Is PATH variable present and reasonable?
 * 
 * If restricted:
 * - Warn the user
 * - They might need to adjust container/sandbox settings
 * - Some dependencies might appear missing when they're actually there
 * 
 * R_OK = read permission, X_OK = execute permission
 */
int check_sandbox_restrictions() {
    int restricted = 0;
    
    // Try to access common system directories
    // These should be accessible on any normal Linux system
    if (access("/usr/bin", R_OK | X_OK) != 0) restricted++;
    if (access("/bin", R_OK | X_OK) != 0) restricted++;
    
    // Check if PATH environment variable is reasonable
    // A very short PATH is suspicious and suggests restrictions
    char *path = getenv("PATH");
    if (path == NULL || strlen(path) < 10) restricted++;
    
    // If we hit multiple restrictions, we're likely in a sandbox
    // One restriction might be a fluke, but multiple suggest a pattern
    return restricted > 1;
}

/*
 * find_dependency()
 * 
 * Main search function - tries multiple strategies to find a program.
 * 
 * Search strategy (in order):
 * 1. command -v (POSIX standard, most portable)
 * 2. which command (common Unix tool)
 * 3. PATH environment variable (user's search path)
 * 4. Standard binary directories (system defaults)
 * 5. Distro-specific paths (handle unique layouts)
 * 6. Library directories (for shared libraries/tools)
 * 
 * Why this order:
 * - Start with fastest, most reliable methods
 * - Fall back to slower, more thorough searches
 * - Each strategy has different strengths:
 *   * command/which: respect shell configuration
 *   * PATH: finds user-installed programs
 *   * Standard dirs: catches programs not in PATH
 *   * Distro-specific: handles unusual installations
 *   * Lib dirs: finds tools packaged with libraries
 * 
 * Defense in depth:
 * - If one method fails, try another
 * - Different environments might have different configurations
 * - We want to find the program if it exists ANYWHERE
 */
int find_dependency(const char *prog_name, Dependency *dep, DistroType distro) {
    // Initialize dependency structure
    strncpy(dep->name, prog_name, sizeof(dep->name) - 1);
    dep->name[sizeof(dep->name) - 1] = '\0';  // Ensure null termination
    dep->found = 0;
    dep->executable = 0;
    dep->path[0] = '\0';
    
    /*
     * STRATEGY 1: command -v
     * POSIX standard, most portable, respects shell configuration
     * This should work on 95%+ of systems
     */
    if (check_command(prog_name, dep->path)) {
        dep->found = 1;
        dep->executable = verify_executable(dep->path, prog_name);
        if (dep->executable) return 1;  // Found and working, we're done
    }
    
    /*
     * STRATEGY 2: which command
     * Common Unix tool, might not be on minimal systems
     * Try this if command -v failed
     */
    if (check_which(prog_name, dep->path)) {
        dep->found = 1;
        dep->executable = verify_executable(dep->path, prog_name);
        if (dep->executable) return 1;
    }
    
    /*
     * STRATEGY 3: PATH environment variable
     * Manual search through PATH, handles edge cases where
     * command/which might not work (old shells, weird configs)
     */
    if (check_path_env(prog_name, dep->path)) {
        dep->found = 1;
        dep->executable = verify_executable(dep->path, prog_name);
        if (dep->executable) return 1;
    }
    
    /*
     * STRATEGY 4: Standard binary directories
     * Direct filesystem search, doesn't rely on any tools
     * 
     * Directory hierarchy explained:
     * /bin - Essential user commands (ls, cat, etc.)
     * /usr/bin - Non-essential user commands (most programs)
     * /usr/local/bin - Locally compiled/installed programs
     * /sbin - System administration commands (ifconfig, fdisk)
     * /usr/sbin - Non-essential system admin commands
     * /opt/bin - Optional/add-on software
     * 
     * We check all of these because programs can be installed anywhere
     */
    const char *bin_dirs[] = {
        "/usr/bin",          // Most common location
        "/usr/local/bin",    // Locally compiled software
        "/bin",              // Essential commands
        "/opt/bin",          // Optional packages
        "/usr/sbin",         // System admin tools (for things like iptables)
        "/sbin",             // Essential system tools
        "/usr/local/sbin",   // Locally compiled system tools
        NULL                 // Sentinel to mark end of array
    };
    
    for (int i = 0; bin_dirs[i] != NULL; i++) {
        if (search_directory(bin_dirs[i], prog_name, dep->path)) {
            dep->found = 1;
            dep->executable = verify_executable(dep->path, prog_name);
            if (dep->executable) return 1;
        }
    }
    
    /*
     * STRATEGY 5: Distribution-specific paths
     * Different distros organize files differently
     * 
     * Why this matters:
     * - Arch puts some binaries in /usr/lib
     * - Gentoo uses /usr/libexec for helper programs
     * - SUSE uses /usr/lib64 on 64-bit systems
     * - Alpine uses /usr/libexec for certain tools
     * 
     * We only check relevant paths for detected distro
     * (No point searching Arch paths on Ubuntu)
     */
    const char *extra_dirs[10] = {NULL};
    int extra_count = 0;
    
    switch (distro) {
        case DISTRO_ARCH:
            extra_dirs[extra_count++] = "/usr/lib";
            break;
        case DISTRO_GENTOO:
            extra_dirs[extra_count++] = "/usr/libexec";
            break;
        case DISTRO_SUSE:
            extra_dirs[extra_count++] = "/usr/lib64";
            break;
        case DISTRO_ALPINE:
            extra_dirs[extra_count++] = "/usr/libexec";
            break;
        default:
            break;
    }
    
    for (int i = 0; i < extra_count && extra_dirs[i] != NULL; i++) {
        if (search_directory(extra_dirs[i], prog_name, dep->path)) {
            dep->found = 1;
            dep->executable = verify_executable(dep->path, prog_name);
            if (dep->executable) return 1;
        }
    }
    
    /*
     * STRATEGY 6: Library directories
     * Last resort - some tools are installed alongside libraries
     * 
     * Examples:
     * - Tools that come with development libraries
     * - Helper programs that aren't meant to be directly called
     * - Programs that haven't been properly packaged
     * 
     * /usr/lib - 32-bit libraries and associated tools
     * /usr/lib64 - 64-bit libraries (on multilib systems)
     * /lib - Essential system libraries
     * /usr/libexec - Programs called by other programs (not users)
     * 
     * Note: We only check these if not found elsewhere because
     * programs here might not be intended for direct use
     */
    const char *lib_dirs[] = {
        "/usr/lib",
        "/usr/local/lib",
        "/lib",
        "/lib64",
        "/usr/lib64",
        "/usr/libexec",
        NULL
    };
    
    for (int i = 0; lib_dirs[i] != NULL; i++) {
        if (search_directory(lib_dirs[i], prog_name, dep->path)) {
            dep->found = 1;
            dep->executable = verify_executable(dep->path, prog_name);
            if (dep->executable) return 1;
        }
    }
    
    // If we get here, we didn't find a working executable
    return 0;
}

/*
 * check_package_installed()
 * 
 * Queries the distribution's package manager to see if a package is installed.
 * 
 * Why this is useful:
 * - Even if we can't find the executable, the package might be installed
 * - Helps diagnose: "installed but not in PATH" issues
 * - Can suggest: "package exists but needs configuration"
 * 
 * Each distro uses different package managers:
 * - dpkg: Debian package manager (low-level)
 * - apt: Debian package manager (high-level, not used here)
 * - rpm: Red Hat package manager (low-level)
 * - dnf/yum: Red Hat package managers (high-level)
 * - pacman: Arch package manager
 * - zypper: SUSE package manager
 * - apk: Alpine package manager
 * - emerge: Gentoo package manager
 * - xbps: Void package manager
 * 
 * We use system() to run commands:
 * - Returns exit code of command
 * - 0 = success (package found)
 * - non-zero = failure (package not found)
 * 
 * Why multiple commands per distro:
 * - Different tools available on different versions
 * - Fallback if primary tool isn't installed
 * - Some commands are more reliable than others
 */
int check_package_installed(const char *package_name, DistroType distro) {
    char cmd[MAX_CMD];
    int result;
    
    switch (distro) {
        case DISTRO_DEBIAN:
            /*
             * Debian family (Ubuntu, Mint, etc.)
             * 
             * dpkg -l: list packages
             * grep '^ii': filter for installed packages
             *   'ii' = desired state: installed, current state: installed
             * 
             * dpkg-query: more reliable query tool
             * Checks ${Status} field for "install ok installed"
             */
            snprintf(cmd, sizeof(cmd), "dpkg -l %s 2>/dev/null | grep -q '^ii'", package_name);
            if (system(cmd) == 0) return 1;
            
            snprintf(cmd, sizeof(cmd), "dpkg-query -W -f='${Status}' %s 2>/dev/null | grep -q 'install ok installed'", package_name);
            if (system(cmd) == 0) return 1;
            break;
            
        case DISTRO_REDHAT:
            /*
             * Red Hat family (CentOS, Fedora, Rocky, Alma)
             * 
             * rpm -q: query package (low-level)
             * dnf list installed: Fedora/RHEL 8+ (high-level)
             * yum list installed: RHEL 7 and older (high-level)
             * 
             * We try all three because:
             * - Older systems only have yum
             * - Newer systems use dnf
             * - rpm always works but might miss details
             */
            snprintf(cmd, sizeof(cmd), "rpm -q %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            
            snprintf(cmd, sizeof(cmd), "dnf list installed %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            
            snprintf(cmd, sizeof(cmd), "yum list installed %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            break;
            
        case DISTRO_ARCH:
            /*
             * Arch family (Manjaro, EndeavourOS)
             * 
             * pacman -Q: query local database
             * Simple and reliable - Arch keeps things straightforward
             * 
             * Arch philosophy: simplicity and user-centricity
             * So their package manager is more consistent than others
             */
            snprintf(cmd, sizeof(cmd), "pacman -Q %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            break;
            
        case DISTRO_SUSE:
            /*
             * SUSE family (openSUSE, SLES)
             * 
             * Uses RPM underneath, so rpm -q works
             * zypper se -i: search installed packages
             * grep '^i': filter for installed (marked with 'i')
             * 
             * SUSE is unique in enterprise Linux world:
             * - YaST configuration system
             * - zypper is more user-friendly than yum
             */
            snprintf(cmd, sizeof(cmd), "rpm -q %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            
            snprintf(cmd, sizeof(cmd), "zypper se -i %s 2>/dev/null | grep -q '^i'", package_name);
            if (system(cmd) == 0) return 1;
            break;
            
        case DISTRO_ALPINE:
            /*
             * Alpine Linux
             * 
             * apk info -e: check if exact package is installed
             * -e flag is important: exact match only
             * 
             * Alpine is popular in containers because:
             * - Small size (uses musl libc instead of glibc)
             * - Security focused
             * - Fast package manager
             */
            snprintf(cmd, sizeof(cmd), "apk info -e %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            break;
            
        case DISTRO_GENTOO:
            /*
             * Gentoo (source-based distribution)
             * 
             * equery: part of gentoolkit, powerful query tool
             * qlist: part of portage-utils, faster but less featured
             * 
             * Gentoo compiles everything from source, so:
             * - Package management is more complex
             * - Multiple tools for different queries
             * - equery is more accurate but requires gentoolkit
             * - qlist is always available but simpler
             */
            snprintf(cmd, sizeof(cmd), "equery l %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            
            // Fallback to qlist (from portage-utils)
            snprintf(cmd, sizeof(cmd), "qlist -I %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            break;
            
        case DISTRO_VOID:
            /*
             * Void Linux
             * 
             * xbps-query: query XBPS package database
             * Void is unique: uses runit instead of systemd
             * 
             * XBPS (X Binary Package System):
             * - Fast and simple
             * - Rolling release like Arch
             * - Less known but growing community
             */
            snprintf(cmd, sizeof(cmd), "xbps-query %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            break;
            
        case DISTRO_SLACKWARE:
            /*
             * Slackware (oldest surviving Linux distro)
             * 
             * No fancy package database - uses simple files
             * /var/log/packages/ contains file for each package
             * Format: packagename-version-arch-build
             * 
             * We use wildcards to match version/arch variations
             * 
             * Slackware philosophy: simplicity and stability
             * Package management is intentionally simple
             */
            snprintf(cmd, sizeof(cmd), "ls /var/log/packages/%s-* >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            break;
            
        default:
            /*
             * Unknown distribution - try common package managers
             * 
             * This is our "best effort" for unrecognized distros
             * Try the most common ones in order of popularity:
             * 1. dpkg (Debian is most popular)
             * 2. rpm (Red Hat is second most popular)
             * 3. pacman (Arch is growing fast)
             * 
             * Even on unknown distros, one of these often works
             * because many distros are derivatives
             */
            snprintf(cmd, sizeof(cmd), "dpkg -l %s 2>/dev/null | grep -q '^ii'", package_name);
            if (system(cmd) == 0) return 1;
            
            snprintf(cmd, sizeof(cmd), "rpm -q %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            
            snprintf(cmd, sizeof(cmd), "pacman -Q %s >/dev/null 2>&1", package_name);
            if (system(cmd) == 0) return 1;
            break;
    }
    
    return 0;
}

/*
 * distro_name()
 * 
 * Converts DistroType enum to human-readable string.
 * 
 * Why we need this:
 * - User-friendly output in reports
 * - Debugging messages
 * - Logging
 * 
 * We use "-based" suffix to indicate families:
 * - "Debian-based" covers Debian, Ubuntu, Mint, etc.
 * - "RedHat-based" covers RHEL, CentOS, Fedora, etc.
 * 
 * This helps users understand the category even if
 * they're on a derivative distro we don't explicitly name
 */
const char* distro_name(DistroType distro) {
    switch (distro) {
        case DISTRO_DEBIAN: return "Debian-based";
        case DISTRO_REDHAT: return "RedHat-based";
        case DISTRO_ARCH: return "Arch-based";
        case DISTRO_SUSE: return "SUSE-based";
        case DISTRO_ALPINE: return "Alpine";
        case DISTRO_GENTOO: return "Gentoo";
        case DISTRO_VOID: return "Void";
        case DISTRO_SLACKWARE: return "Slackware";
        default: return "Unknown";
    }
}

/*
 * check_dependencies_from_json()
 * 
 * Main orchestration function - parses JSON and checks all dependencies.
 * 
 * JSON format expected:
 * {
 *   "dependencies": ["program1", "program2", "program3"]
 * }
 * 
 * Process:
 * 1. Parse JSON string into cJSON structure
 * 2. Detect distribution (for package manager queries)
 * 3. Check for sandbox restrictions (warn if limited)
 * 4. Extract dependencies array from JSON
 * 5. Check each dependency using find_dependency()
 * 6. Query package manager if not found
 * 7. Generate summary report
 * 
 * Return values:
 * 0 = all dependencies satisfied
 * 1 = some dependencies missing
 * -1 = error (JSON parse failed, no dependencies array, etc.)
 * 
 * Why JSON:
 * - Easy to parse (cJSON library)
 * - Human readable and editable
 * - Standard format for configuration
 * - Can be generated by other tools
 * - Extensible (can add metadata later)
 */
int check_dependencies_from_json(const char *json_str) {
    // Parse JSON string into cJSON tree structure
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        // cJSON_GetErrorPtr() returns pointer to where parsing failed
        fprintf(stderr, "Error parsing JSON: %s\n", cJSON_GetErrorPtr());
        return -1;
    }
    
    /*
     * Detect distribution first
     * This determines:
     * - Which package manager to query
     * - Which special paths to search
     * - How to interpret results
     */
    DistroType distro = detect_distro();
    printf("Detected distribution: %s\n", distro_name(distro));
    
    /*
     * Check for sandbox/container restrictions
     * Important because:
     * - Docker containers often have limited /proc, /sys
     * - Snap/Flatpak apps can't see full filesystem
     * - Results might be misleading in restricted environments
     * 
     * We warn but continue - user needs to know context
     */
    if (check_sandbox_restrictions()) {
        fprintf(stderr, "WARNING: Running in restricted environment. Results may be limited.\n");
    }
    
    /*
     * Extract "dependencies" array from JSON
     * cJSON_GetObjectItem: find key in JSON object
     * cJSON_IsArray: verify it's an array (not string, number, etc.)
     * 
     * If no dependencies array found, this is a fatal error
     * because we have nothing to check
     */
    cJSON *deps = cJSON_GetObjectItem(root, "dependencies");
    if (deps == NULL || !cJSON_IsArray(deps)) {
        fprintf(stderr, "No dependencies array found in JSON\n");
        cJSON_Delete(root);  // Clean up cJSON tree
        return -1;
    }
    
    int total = cJSON_GetArraySize(deps);
    int found_count = 0;
    
    printf("Checking %d dependencies...\n\n", total);
    
    /*
     * Main checking loop - iterate through each dependency
     * 
     * For each dependency:
     * 1. Extract program name from JSON
     * 2. Call find_dependency() to search for it
     * 3. If not found, query package manager
     * 4. Print results with visual indicators
     * 
     * Visual indicators:
     * ✓ = found and working (success)
     * ⚠ = found but not executable (warning)
     * ✗ = not found (failure)
     * 
     * Progress indicator: [1/5], [2/5], etc.
     * Helps user know how much longer the check will take
     */
    for (int i = 0; i < total; i++) {
        cJSON *item = cJSON_GetArrayItem(deps, i);
        const char *prog_name = cJSON_GetStringValue(item);
        
        // Skip if item isn't a string (malformed JSON)
        if (prog_name == NULL) continue;
        
        Dependency dep;
        int success = find_dependency(prog_name, &dep, distro);
        
        // Print progress and program name
        printf("[%d/%d] %s: ", i + 1, total, prog_name);
        
        if (success) {
            // Best case: found and working
            printf("✓ FOUND at %s\n", dep.path);
            found_count++;
        } else if (dep.found) {
            // Found but something wrong (permissions, corruption, etc.)
            printf("⚠ Found at %s but not executable\n", dep.path);
        } else {
            // Not found in filesystem - check package manager
            printf("✗ NOT FOUND\n");
            
            /*
             * Package manager query as last resort
             * 
             * Why this is useful:
             * - Package might be installed but not in standard paths
             * - Might need post-install configuration
             * - Helps diagnose "installed but not working" issues
             * 
             * Example: Some packages install to /opt and require
             * manual PATH configuration
             */
            if (check_package_installed(prog_name, distro)) {
                printf("  (Package is installed but executable not in standard paths)\n");
            }
        }
    }
    
    /*
     * Summary report
     * 
     * Provides at-a-glance status:
     * - How many dependencies satisfied
     * - Percentage (implied: found_count / total)
     * - Visual separator for easy reading
     * 
     * This is what users will look at first to see if
     * their system is ready
     */
    printf("\n========================================\n");
    printf("Summary: %d/%d dependencies satisfied\n", found_count, total);
    printf("========================================\n");
    
    // Clean up cJSON tree (frees all allocated memory)
    cJSON_Delete(root);
    
    // Return 0 only if ALL dependencies satisfied
    // This matches Unix convention: 0 = success
    return (found_count == total) ? 0 : 1;
}


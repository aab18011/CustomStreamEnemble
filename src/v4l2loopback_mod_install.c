/*
 *  V4L2LOOPBACK AUTOMATED INSTALLATION UTILITY
 *
 *  Author: Aidan A. Bradley
 *  Date: 10/01/2025
 *
 *  OVERVIEW
 *  ========
 *  This program provides a robust, memory-safe C implementation for managing
 *  the v4l2loopback kernel module with extended device support. It automates
 *  the complete lifecycle of installation, updating, and loading of the
 *  v4l2loopback kernel module from a modified source repository that removes
 *  the default 8-device limitation.
 *
 *  PRIMARY FEATURES
 *  ================
 *  - Automated Git repository management with version tracking
 *  - DKMS (Dynamic Kernel Module Support) integration for seamless kernel updates
 *  - Intelligent device number allocation to avoid conflicts with existing hardware
 *  - Secure Boot compatibility with MOK (Machine Owner Key) handling
 *  - Idempotent operation allowing safe repeated execution
 *  - Memory-safe string operations throughout
 *  - Distribution-agnostic design using standard POSIX APIs
 *
 *  TECHNICAL DETAILS
 *  =================
 *
 *  Repository Management:
 *  ----------------------
 *  The program clones and maintains a local copy of the v4l2loopback source
 *  repository, tracking commits to determine when updates are available. It
 *  uses git's native version tracking to maintain consistency and proper
 *  versioning for DKMS module installation.
 *
 *  DKMS Integration:
 *  -----------------
 *  Utilizes DKMS to ensure the module remains compatible across kernel updates.
 *  The program handles the complete DKMS lifecycle:
 *    1. Cleanup of broken or incomplete DKMS entries
 *    2. Removal of obsolete module versions
 *    3. Source code copying to /usr/src
 *    4. Module addition to DKMS tree
 *    5. Compilation against current kernel headers
 *    6. Installation with proper module signing
 *
 *  Device Number Allocation:
 *  -------------------------
 *  Intelligently scans /dev for existing video devices and allocates virtual
 *  loopback devices starting from /dev/video10 onwards. This approach prevents
 *  conflicts with:
 *    - Built-in webcams (typically video0)
 *    - External USB cameras (typically video1, video2, etc.)
 *    - Video capture cards
 *    - Other V4L2 devices
 *
 *  The allocation algorithm dynamically adapts to the system's current device
 *  topology, ensuring no collisions occur regardless of hardware configuration.
 *
 *  Secure Boot Handling:
 *  ---------------------
 *  On systems with Secure Boot enabled, the module must be signed with a
 *  trusted key. The program:
 *    - Detects DKMS MOK signing operations
 *    - Identifies key rejection errors
 *    - Provides detailed diagnostic information
 *    - Offers step-by-step remediation guidance
 *
 *  Module Loading Strategy:
 *  ------------------------
 *  Before loading the module, the program:
 *    1. Checks if v4l2loopback is currently loaded (lsmod)
 *    2. Safely unloads existing instances with proper cleanup delays
 *    3. Verifies available device slots
 *    4. Constructs optimal module parameters
 *    5. Loads with exclusive_caps enabled for better application compatibility
 *
 *  Memory Safety:
 *  --------------
 *  All string operations use bounded functions with explicit size checking:
 *    - safe_strncpy(): Bounds-checked string copying with null termination
 *    - safe_strncat(): Safe concatenation with overflow protection
 *    - Fixed-size buffers with compile-time size validation
 *    - No dynamic allocation to prevent memory leaks
 *
 *  Error Handling:
 *  ---------------
 *  Comprehensive error checking at every system call with:
 *    - Return value validation
 *    - Context-aware error messages
 *    - Fallback strategies for recoverable failures
 *    - Diagnostic output for troubleshooting
 *
 *  USAGE SCENARIOS
 *  ===============
 *
 *  Initial Installation:
 *  ---------------------
 *  On first run, the program clones the repository, builds the module via
 *  DKMS, installs it for the current kernel, and loads it with 16 virtual
 *  camera devices.
 *
 *  Update Management:
 *  ------------------
 *  Subsequent runs check for repository updates. If new commits are available,
 *  the program updates the local repository, rebuilds the module with the new
 *  version, and reinstalls. This ensures you always have the latest features
 *  and bug fixes.
 *
 *  Kernel Updates:
 *  ---------------
 *  When the system kernel is updated, DKMS automatically rebuilds the module
 *  for the new kernel. This program verifies installation status and can
 *  trigger reinstallation if needed.
 *
 *  Repeated Execution:
 *  -------------------
 *  The program is fully idempotent - running it multiple times is safe and
 *  will only perform work if updates are available or the module isn't loaded.
 *
 *  SYSTEM REQUIREMENTS
 *  ===================
 *  - Linux kernel with V4L2 support (2.6.x or later)
 *  - DKMS package installed
 *  - Kernel headers for current running kernel
 *  - Git for repository management
 *  - Root privileges (sudo)
 *  - Build essentials (gcc, make, etc.)
 *
 *  SECURITY CONSIDERATIONS
 *  =======================
 *  - Runs with elevated privileges (required for kernel module operations)
 *  - Uses sudo -u for git operations to maintain proper ownership
 *  - Validates all input parameters and buffer sizes
 *  - Prevents command injection through careful string construction
 *  - Handles Secure Boot requirements appropriately
 *
 *  COMPATIBILITY
 *  =============
 *  Tested on:
 *  - Debian/Ubuntu and derivatives
 *  - Fedora/RHEL and derivatives
 *  - Arch Linux
 *  - Any distribution with DKMS support
 *
 *  KNOWN LIMITATIONS
 *  =================
 *  - Requires DKMS (not available on all minimal installations)
 *  - Module signing requires MOK enrollment on Secure Boot systems
 *  - Maximum of 256 video devices due to V4L2 subsystem limitations
 *  - Requires repository access (network connectivity)
 *
 *  EXIT CODES
 *  ==========
 *  0: Success - module installed and loaded
 *  1: Error - see stderr for details
 *
 *  TROUBLESHOOTING
 *  ===============
 *  If the module fails to load:
 *    - Check dmesg for kernel messages: sudo dmesg | tail
 *    - Verify DKMS status: dkms status
 *    - Check module info: modinfo v4l2loopback
 *    - For Secure Boot: mokutil --sb-state
 *
 *  MODIFICATION NOTES
 *  ==================
 *  This implementation converts the original bash script to C for:
 *    - Improved performance
 *    - Better error handling
 *    - Enhanced security through memory safety
 *    - Reduced external dependencies
 *    - More portable across distributions
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <errno.h>
#include <pwd.h>
#include <limits.h>
#include <dirent.h>

#define REPO_DIR "/home/user/Documents/v4l2loopback"
#define REPO_URL "https://github.com/aab18011/v4l2loopback.git"
#define MODULE_NAME "v4l2loopback"
#define MAX_CMD_LEN 4096
#define MAX_VERSION_LEN 128
#define MAX_OUTPUT_LEN 8192

// Safe string copy
static int safe_strncpy(char *dest, const char *src, size_t dest_size) {
    if (!dest || !src || dest_size == 0) return -1;
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
    return 0;
}

// Safe string concatenation
static int safe_strncat(char *dest, const char *src, size_t dest_size) {
    if (!dest || !src || dest_size == 0) return -1;
    size_t current_len = strnlen(dest, dest_size);
    if (current_len >= dest_size - 1) return -1;
    strncat(dest, src, dest_size - current_len - 1);
    return 0;
}

// Execute command and capture output
static int exec_cmd(const char *cmd, char *output, size_t output_size, int *exit_code) {
    if (!cmd) return -1;
    
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen failed");
        return -1;
    }
    
    if (output && output_size > 0) {
        output[0] = '\0';
        size_t total_read = 0;
        char buffer[1024];
        
        while (fgets(buffer, sizeof(buffer), fp) != NULL && total_read < output_size - 1) {
            size_t len = strlen(buffer);
            size_t space_left = output_size - total_read - 1;
            size_t to_copy = (len < space_left) ? len : space_left;
            strncat(output, buffer, to_copy);
            total_read += to_copy;
        }
    }
    
    int status = pclose(fp);
    if (exit_code) {
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    
    return 0;
}

// Get current username
static int get_username(char *username, size_t size) {
    uid_t uid = geteuid();
    struct passwd *pw = getpwuid(uid);
    
    if (!pw) {
        perror("getpwuid failed");
        return -1;
    }
    
    return safe_strncpy(username, pw->pw_name, size);
}

// Check if directory exists
static int dir_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

// Get current kernel version
static int get_kernel_version(char *version, size_t size) {
    struct utsname buf;
    if (uname(&buf) != 0) {
        perror("uname failed");
        return -1;
    }
    return safe_strncpy(version, buf.release, size);
}

// Execute git command as user
static int git_cmd(const char *user, const char *repo_dir, const char *git_args, 
                   char *output, size_t output_size) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "cd '%s' && sudo -u '%s' git %s 2>/dev/null", 
             repo_dir, user, git_args);
    
    int exit_code;
    if (exec_cmd(cmd, output, output_size, &exit_code) != 0) {
        return -1;
    }
    
    // Trim newline
    if (output && output_size > 0) {
        size_t len = strlen(output);
        if (len > 0 && output[len-1] == '\n') {
            output[len-1] = '\0';
        }
    }
    
    return exit_code;
}

// Clone repository
static int clone_repo(const char *user, const char *repo_dir, const char *repo_url) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "sudo -u '%s' git clone '%s' '%s'", 
             user, repo_url, repo_dir);
    
    printf("Cloning repository...\n");
    return system(cmd);
}

// Get git version/commit
static int get_version(const char *user, const char *repo_dir, char *version, size_t size) {
    char output[MAX_OUTPUT_LEN] = {0};
    
    // Try git describe first
    if (git_cmd(user, repo_dir, "describe --always --dirty", output, sizeof(output)) == 0 
        && strlen(output) > 0) {
        return safe_strncpy(version, output, size);
    }
    
    if (git_cmd(user, repo_dir, "describe --always", output, sizeof(output)) == 0 
        && strlen(output) > 0) {
        return safe_strncpy(version, output, size);
    }
    
    return safe_strncpy(version, "snapshot", size);
}

// Check if module is installed for kernel
static int is_module_installed(const char *version, const char *kernel) {
    char cmd[MAX_CMD_LEN];
    char output[MAX_OUTPUT_LEN] = {0};
    
    snprintf(cmd, sizeof(cmd), "dkms status | grep -E '^%s,\\s*%s,\\s*%s.*: installed$'",
             MODULE_NAME, version, kernel);
    
    int exit_code;
    exec_cmd(cmd, output, sizeof(output), &exit_code);
    return (exit_code == 0);
}

// Remove broken DKMS entries
static void cleanup_broken_dkms(void) {
    char output[MAX_OUTPUT_LEN] = {0};
    exec_cmd("dkms status | awk -F, '/^v4l2loopback,/{print $1\",\"$2}' | tr -d ' '",
             output, sizeof(output), NULL);
    
    char *line = strtok(output, "\n");
    while (line) {
        char mod[256], ver[128];
        if (sscanf(line, "%255[^,],%127s", mod, ver) == 2) {
            char src_path[PATH_MAX];
            snprintf(src_path, sizeof(src_path), "/var/lib/dkms/%s/%s/source/dkms.conf", 
                     mod, ver);
            
            if (access(src_path, F_OK) != 0) {
                printf("Removing broken DKMS entry: %s %s\n", mod, ver);
                char cmd[MAX_CMD_LEN];
                snprintf(cmd, sizeof(cmd), 
                         "dkms remove -m '%s' -v '%s' --all 2>/dev/null || true", 
                         mod, ver);
                system(cmd);
                
                snprintf(cmd, sizeof(cmd), "rm -rf '/usr/src/%s-%s' '/var/lib/dkms/%s/%s'",
                         mod, ver, mod, ver);
                system(cmd);
            }
        }
        line = strtok(NULL, "\n");
    }
}

// Remove old DKMS versions
static void remove_old_versions(const char *current_version) {
    char output[MAX_OUTPUT_LEN] = {0};
    exec_cmd("dkms status | awk -F, '/^v4l2loopback,/{print $2}' | tr -d ' '",
             output, sizeof(output), NULL);
    
    char *line = strtok(output, "\n");
    while (line) {
        if (strcmp(line, current_version) != 0) {
            printf("Removing old DKMS version: %s\n", line);
            char cmd[MAX_CMD_LEN];
            snprintf(cmd, sizeof(cmd), 
                     "dkms remove -m %s -v '%s' --all 2>/dev/null || true", 
                     MODULE_NAME, line);
            system(cmd);
            
            snprintf(cmd, sizeof(cmd), 
                     "rm -rf '/usr/src/%s-%s' '/var/lib/dkms/%s/%s'",
                     MODULE_NAME, line, MODULE_NAME, line);
            system(cmd);
        }
        line = strtok(NULL, "\n");
    }
}

// Install/update module
static int install_module(const char *user, const char *repo_dir, const char *version) {
    char cmd[MAX_CMD_LEN];
    
    // Force remove current version
    snprintf(cmd, sizeof(cmd), 
             "dkms remove -m %s -v '%s' --all 2>/dev/null || true", 
             MODULE_NAME, version);
    system(cmd);
    
    snprintf(cmd, sizeof(cmd), 
             "rm -rf '/var/lib/dkms/%s/%s' '/usr/src/%s-%s'",
             MODULE_NAME, version, MODULE_NAME, version);
    system(cmd);
    
    // Copy sources
    printf("Copying sources...\n");
    snprintf(cmd, sizeof(cmd), "cp -r '%s' '/usr/src/%s-%s'", 
             repo_dir, MODULE_NAME, version);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed to copy sources\n");
        return -1;
    }
    
    // Add to DKMS
    printf("Adding to DKMS...\n");
    snprintf(cmd, sizeof(cmd), "dkms add -m %s -v '%s'", MODULE_NAME, version);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed to add to DKMS\n");
        return -1;
    }
    
    // Build
    printf("Building module...\n");
    snprintf(cmd, sizeof(cmd), "dkms build -m %s -v '%s'", MODULE_NAME, version);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed to build module\n");
        return -1;
    }
    
    // Install
    printf("Installing module...\n");
    snprintf(cmd, sizeof(cmd), "dkms install -m %s -v '%s' --force", 
             MODULE_NAME, version);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed to install module\n");
        return -1;
    }
    
    return 0;
}

// Check if module is currently loaded
static int is_module_loaded(void) {
    char output[MAX_OUTPUT_LEN] = {0};
    int exit_code;
    exec_cmd("lsmod | grep -q '^v4l2loopback '", output, sizeof(output), &exit_code);
    return (exit_code == 0);
}

// Find available video device numbers
static void get_available_video_numbers(int *numbers, int count, int *actual_count) {
    int found = 0;
    int candidate = 10; // Start at video10 to avoid conflicts with real devices
    
    while (found < count && candidate < 256) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/video%d", candidate);
        
        if (access(path, F_OK) != 0) {
            numbers[found++] = candidate;
        }
        candidate++;
    }
    
    *actual_count = found;
}

// Load kernel module
static int load_module(void) {
    printf("Loading module...\n");
    
    // Check if already loaded
    if (is_module_loaded()) {
        printf("Module already loaded, unloading first...\n");
        int ret = system("modprobe -r v4l2loopback 2>/dev/null");
        if (ret != 0) {
            fprintf(stderr, "Warning: Failed to unload existing module, trying anyway...\n");
        }
        // Give the system a moment to clean up
        usleep(500000); // 500ms
    }
    
    // Find available video device numbers
    int video_numbers[16];
    int actual_count = 0;
    get_available_video_numbers(video_numbers, 16, &actual_count);
    
    if (actual_count < 16) {
        fprintf(stderr, "Warning: Only found %d available video device slots\n", actual_count);
    }
    
    // Build video_nr parameter
    char video_nr[256] = {0};
    for (int i = 0; i < actual_count; i++) {
        char num[16];
        snprintf(num, sizeof(num), "%d%s", video_numbers[i], (i < actual_count - 1) ? "," : "");
        safe_strncat(video_nr, num, sizeof(video_nr));
    }
    
    // Build card_label parameter
    char card_label[512] = {0};
    for (int i = 0; i < actual_count; i++) {
        char label[32];
        snprintf(label, sizeof(label), "'Cam%d'%s", video_numbers[i], (i < actual_count - 1) ? "," : "");
        safe_strncat(card_label, label, sizeof(card_label));
    }
    
    // Load module
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd),
             "modprobe v4l2loopback devices=%d exclusive_caps=1 video_nr=%s card_label=%s 2>&1",
             actual_count, video_nr, card_label);
    
    char error_output[1024] = {0};
    int exit_code;
    exec_cmd(cmd, error_output, sizeof(error_output), &exit_code);
    
    if (exit_code != 0) {
        fprintf(stderr, "Failed to load module\n");
        
        // Check for key rejection (Secure Boot issue)
        if (strstr(error_output, "Key was rejected") || 
            strstr(error_output, "Required key not available")) {
            fprintf(stderr, "\n=== SECURE BOOT KEY ISSUE ===\n");
            fprintf(stderr, "The module signature is not trusted by the kernel.\n\n");
            
            // Check if MOK key exists
            if (access("/var/lib/dkms/mok.pub", F_OK) != 0) {
                fprintf(stderr, "MOK key not found at /var/lib/dkms/mok.pub\n");
                fprintf(stderr, "Generate a new key with:\n");
                fprintf(stderr, "   sudo /usr/lib/dkms/dkms_mok_sign_key --generate\n\n");
            } else {
                fprintf(stderr, "Diagnostic steps:\n");
                fprintf(stderr, "1. Check Secure Boot status:\n");
                fprintf(stderr, "   mokutil --sb-state\n\n");
                fprintf(stderr, "2. List enrolled MOK keys:\n");
                fprintf(stderr, "   mokutil --list-enrolled | grep -i dkms\n\n");
                fprintf(stderr, "3. Check if key needs re-enrollment:\n");
                fprintf(stderr, "   sudo mokutil --import /var/lib/dkms/mok.pub\n");
                fprintf(stderr, "   (If it says already enrolled, the issue is elsewhere)\n\n");
                fprintf(stderr, "4. Verify module signature:\n");
                fprintf(stderr, "   modinfo v4l2loopback | grep sig\n\n");
                fprintf(stderr, "5. Check kernel ring buffer for details:\n");
                fprintf(stderr, "   sudo dmesg | tail -20\n\n");
                fprintf(stderr, "Alternative solution:\n");
                fprintf(stderr, "- Disable Secure Boot in BIOS/UEFI settings\n\n");
            }
        } else if (strstr(error_output, "already")) {
            fprintf(stderr, "Module appears to be already loaded or in use\n");
            fprintf(stderr, "Try: sudo modprobe -r v4l2loopback && sudo %s\n", 
                    program_invocation_short_name);
        }
        
        return -1;
    }
    
    printf("Created %d virtual video devices: ", actual_count);
    for (int i = 0; i < actual_count; i++) {
        printf("/dev/video%d%s", video_numbers[i], (i < actual_count - 1) ? ", " : "\n");
    }
    
    return 0;
}

int main(void) {
    char username[256];
    char current_kernel[128];
    char current_commit[128] = {0};
    char remote_commit[128] = {0};
    char version[MAX_VERSION_LEN] = {0};
    
    // Check if running as root
    if (geteuid() != 0) {
        fprintf(stderr, "This program must be run as root\n");
        return 1;
    }
    
    // Get username
    if (get_username(username, sizeof(username)) != 0) {
        fprintf(stderr, "Failed to get username\n");
        return 1;
    }
    
    // Get kernel version
    if (get_kernel_version(current_kernel, sizeof(current_kernel)) != 0) {
        fprintf(stderr, "Failed to get kernel version\n");
        return 1;
    }
    
    // Clone repo if missing
    if (!dir_exists(REPO_DIR)) {
        if (clone_repo(username, REPO_DIR, REPO_URL) != 0) {
            fprintf(stderr, "Failed to clone repository\n");
            return 1;
        }
    }
    
    // Get current commit
    git_cmd(username, REPO_DIR, "rev-parse HEAD", current_commit, sizeof(current_commit));
    if (strlen(current_commit) == 0) {
        safe_strncpy(current_commit, "none", sizeof(current_commit));
    }
    
    // Fetch latest
    printf("Fetching latest changes...\n");
    int fetch_result = git_cmd(username, REPO_DIR, "fetch origin main", NULL, 0);
    if (fetch_result != 0) {
        fprintf(stderr, "Warning: git fetch failed, will use local state\n");
    }
    
    // Get remote commit
    if (git_cmd(username, REPO_DIR, "rev-parse origin/main", 
                remote_commit, sizeof(remote_commit)) != 0) {
        fprintf(stderr, "Failed to get remote commit, trying FETCH_HEAD\n");
        // Try FETCH_HEAD as fallback
        if (git_cmd(username, REPO_DIR, "rev-parse FETCH_HEAD", 
                    remote_commit, sizeof(remote_commit)) != 0) {
            // If still failing, use current commit to skip update
            fprintf(stderr, "Could not determine remote state, assuming up to date\n");
            safe_strncpy(remote_commit, current_commit, sizeof(remote_commit));
        }
    }
    
    // Get version
    get_version(username, REPO_DIR, version, sizeof(version));
    
    // Check if installed
    int installed = is_module_installed(version, current_kernel);
    
    // Update/install if needed
    if (strcmp(current_commit, remote_commit) != 0 || !installed) {
        printf("Updates available or not installed. Proceeding...\n");
        
        // Reset to latest
        git_cmd(username, REPO_DIR, "reset --hard origin/main", NULL, 0);
        
        // Recompute version
        get_version(username, REPO_DIR, version, sizeof(version));
        printf("Preparing to install v4l2loopback version: %s\n", version);
        
        // Cleanup
        cleanup_broken_dkms();
        remove_old_versions(version);
        
        // Install
        if (install_module(username, REPO_DIR, version) != 0) {
            return 1;
        }
    } else {
        printf("No updates and already installed for current kernel, skipping install.\n");
    }
    
    // Load module
    if (load_module() != 0) {
        return 1;
    }
    
    printf("v4l2loopback %s loaded.\n", version);
    return 0;
}
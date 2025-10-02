#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <ctype.h>

// Include your module headers
#include "lan_check.h"
#include "wlan_check.h"
#include "python3_test.h"
#include "cJSON.h"

// External function declarations for modules without headers
extern int check_dependencies_from_json(const char *json_str);
extern int check_modules_from_json(const char *json_str);
// v4l2loopback installer - we'll call it as subprocess since it needs root
// videopipe - runs as separate daemon, we just verify it's available


bool create_camera_config_interactive(void); // to avoid errors for implicit declaration
// ============================================================================
// CONFIGURATION AND CONSTANTS
// ============================================================================

#define MAX_DAEMONS 8
#define PIPE_BUFFER_SIZE 4096
#define MIN_V4L2_DEVICE 10

// Configuration files
#define CAMERAS_CONFIG "/etc/roc/cameras.json"
#define DEPENDENCIES_CONFIG "/etc/roc/dependencies.json"
#define MODULES_CONFIG "/etc/roc/modules.json"

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    PHASE_INITIALIZATION,
    PHASE_RUNNING,
    PHASE_CLEANUP,
    PHASE_ERROR
} ProgramPhase;

typedef enum {
    DAEMON_NETWORK_MONITOR,
    DAEMON_CAMERA_STREAMER,
    DAEMON_SYSTEM_HEALTH,
} DaemonType;

typedef struct {
    int read_fd;
    int write_fd;
} Pipe;

typedef struct {
    DaemonType type;
    pthread_t thread;
    Pipe to_daemon;
    Pipe from_daemon;
    bool active;
    pthread_mutex_t mutex;
    void* context;
} Daemon;

// Initialization data collected during phase 1
typedef struct {
    // Network info
    lan_info_t lan_info;
    int wlan_available;
    
    // System info
    int python3_working;
    int ffmpeg_available;
    int v4l2loopback_loaded;
    int v4l2_device_count;
    
    // Dependencies status
    int all_deps_satisfied;
    int all_modules_available;
} InitData;

typedef struct {
    ProgramPhase phase;
    pthread_mutex_t phase_mutex;
    
    bool shutdown_requested;
    pthread_mutex_t shutdown_mutex;
    
    InitData init_data;
    
    Daemon daemons[MAX_DAEMONS];
    int daemon_count;
    pthread_mutex_t daemon_mutex;
} GlobalState;

// ============================================================================
// GLOBAL STATE
// ============================================================================

static GlobalState g_state;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

bool create_pipe_pair(Pipe* p) {
    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe creation failed");
        return false;
    }
    p->read_fd = fds[0];
    p->write_fd = fds[1];
    return true;
}

void close_pipe_pair(Pipe* p) {
    if (p->read_fd >= 0) close(p->read_fd);
    if (p->write_fd >= 0) close(p->write_fd);
}

bool is_shutdown_requested() {
    pthread_mutex_lock(&g_state.shutdown_mutex);
    bool req = g_state.shutdown_requested;
    pthread_mutex_unlock(&g_state.shutdown_mutex);
    return req;
}

void request_shutdown() {
    pthread_mutex_lock(&g_state.shutdown_mutex);
    g_state.shutdown_requested = true;
    pthread_mutex_unlock(&g_state.shutdown_mutex);
}

ProgramPhase get_phase() {
    pthread_mutex_lock(&g_state.phase_mutex);
    ProgramPhase phase = g_state.phase;
    pthread_mutex_unlock(&g_state.phase_mutex);
    return phase;
}

void set_phase(ProgramPhase phase) {
    pthread_mutex_lock(&g_state.phase_mutex);
    g_state.phase = phase;
    pthread_mutex_unlock(&g_state.phase_mutex);
}

// ============================================================================
// SIGNAL HANDLING
// ============================================================================

void signal_handler(int signum) {
    printf("\nReceived signal %d, initiating shutdown...\n", signum);
    request_shutdown();
}

void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
}

// ============================================================================
// INITIALIZATION PHASE - Sequential Checks
// ============================================================================

bool check_lan_connectivity(InitData* data) {
    printf("[INIT] Checking LAN connectivity...\n");
    
    if (check_LAN(&data->lan_info) == 0) {
        printf("  Interface: %s\n", data->lan_info.ifname);
        printf("  Local IP: %s\n", data->lan_info.local_addr);
        printf("  Gateway: %s\n", data->lan_info.gateway);
        printf("  Reachable: %s\n", data->lan_info.reachable ? "YES" : "NO");
        return true;
    }
    
    fprintf(stderr, "[ERROR] No default route found\n");
    return false;
}

bool check_wlan_connectivity(InitData* data) {
    printf("[INIT] Checking WLAN/Internet connectivity...\n");
    
    data->wlan_available = check_public_dns();
    
    if (data->wlan_available) {
        printf("  Internet access: CONFIRMED\n");
        return true;
    }
    
    fprintf(stderr, "[WARNING] No Internet access detected\n");
    return true; // Non-fatal, continue anyway
}

bool check_python3_installation(InitData* data) {
    printf("[INIT] Testing Python3 integration...\n");
    
    data->python3_working = (test_python_integration() == 0);
    
    if (data->python3_working) {
        printf("  Python3: WORKING\n");
        return true;
    }
    
    fprintf(stderr, "[ERROR] Python3 test failed\n");
    return false;
}

bool check_system_dependencies(InitData* data) {
    printf("[INIT] Checking system dependencies...\n");
    
    // Build minimal dependencies JSON
    const char *deps_json = 
        "{"
        "  \"dependencies\": [\"ffmpeg\", \"python3\", \"gcc\", \"make\"]"
        "}";
    
    // Check if config file exists, use it if available
    FILE *fp = fopen(DEPENDENCIES_CONFIG, "r");
    if (fp) {
        fclose(fp);
        // Use external depcheck module
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "depcheck %s >/dev/null 2>&1", DEPENDENCIES_CONFIG);
        data->all_deps_satisfied = (system(cmd) == 0);
    } else {
        // Use built-in check
        data->all_deps_satisfied = (check_dependencies_from_json(deps_json) == 0);
    }
    
    if (data->all_deps_satisfied) {
        printf("  All dependencies satisfied\n");
        return true;
    }
    
    fprintf(stderr, "[ERROR] Missing dependencies\n");
    return false;
}

bool check_kernel_modules(InitData* data) {
    printf("[INIT] Checking kernel modules...\n");
    
    const char *modules_json = 
        "{"
        "  \"modules\": ["
        "    {\"name\": \"v4l2loopback\", \"aliases\": []},"
        "    {\"name\": \"videodev\", \"aliases\": [\"v4l2_core\"]}"
        "  ]"
        "}";
    
    // Check if config exists
    FILE *fp = fopen(MODULES_CONFIG, "r");
    if (fp) {
        fclose(fp);
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "modulecheck %s >/dev/null 2>&1", MODULES_CONFIG);
        data->all_modules_available = (system(cmd) == 0);
    } else {
        data->all_modules_available = (check_modules_from_json(modules_json) == 0);
    }
    
    if (data->all_modules_available) {
        printf("  Kernel modules available\n");
        return true;
    }
    
    fprintf(stderr, "[WARNING] Some kernel modules unavailable\n");
    return true; // Non-fatal
}

#include <ctype.h>  // Add at top with other includes

// Check if a video device is a v4l2loopback device
bool is_v4l2loopback_device(int device_num) {
    char name_path[256];
    snprintf(name_path, sizeof(name_path), 
             "/sys/class/video4linux/video%d/name", device_num);
    
    FILE *fp = fopen(name_path, "r");
    if (!fp) return false;
    
    char name[128] = {0};
    if (fgets(name, sizeof(name), fp)) {
        fclose(fp);
        // v4l2loopback devices created by your installer are named "Cam10", "Cam11", etc.
        // Hardware cameras have different names like "Integrated Camera" or "USB Camera"
        return (strncmp(name, "Cam", 3) == 0 && isdigit((unsigned char)name[3]));
    }
    
    fclose(fp);
    return false;
}

bool count_v4l2loopback_devices(InitData* data) {
    data->v4l2_device_count = 0;
    
    printf("  DEBUG: Scanning /dev/video* devices from %d onwards:\n", MIN_V4L2_DEVICE);
    
    for (int i = MIN_V4L2_DEVICE; i <= 255; i++) {
        char dev_path[64];
        snprintf(dev_path, sizeof(dev_path), "/dev/video%d", i);
        
        if (access(dev_path, F_OK) == 0) {
            data->v4l2_device_count++;
            printf("  DEBUG:   Found /dev/video%d (count=%d)\n", i, data->v4l2_device_count);
        }
    }
    
    printf("  DEBUG: Total count = %d\n", data->v4l2_device_count);
    
    return (data->v4l2_device_count > 0);
}

bool install_v4l2loopback(InitData* data) {
    printf("[INIT] Installing/verifying v4l2loopback...\n");
    
    // Check if module is loaded
    if (system("lsmod | grep -q v4l2loopback") == 0) {
        printf("  v4l2loopback module is loaded\n");
        data->v4l2loopback_loaded = 1;
        
        // Count existing devices
        count_v4l2loopback_devices(data);
        printf("  Found %d v4l2loopback devices\n", data->v4l2_device_count);
        
        if (data->v4l2_device_count >= 16) {
            return true;
        }
        
        printf("  Insufficient devices, reloading...\n");
        system("modprobe -r v4l2loopback 2>/dev/null");
        usleep(500000);
    } else {
        data->v4l2loopback_loaded = 0;
        data->v4l2_device_count = 0;
    }
    
    // Check if installer exists in PATH
    if (system("which v4l2loopback_mod_install >/dev/null 2>&1") != 0) {
        fprintf(stderr, "[ERROR] v4l2loopback_mod_install not found in PATH\n");
        fprintf(stderr, "        Please compile and install: gcc -o v4l2loopback_mod_install v4l2loopback_mod_install.c\n");
        fprintf(stderr, "        Then: sudo cp v4l2loopback_mod_install /usr/local/bin/\n");
        return false;
    }
    
    printf("  Running v4l2loopback installer...\n");
    if (system("v4l2loopback_mod_install") != 0) {
        fprintf(stderr, "[ERROR] v4l2loopback installation failed\n");
        return false;
    }
    
    usleep(1000000);
    
    if (system("lsmod | grep -q v4l2loopback") != 0) {
        fprintf(stderr, "[ERROR] Module not loaded after installation\n");
        return false;
    }
    
    data->v4l2loopback_loaded = 1;
    count_v4l2loopback_devices(data);
    
    printf("  Created %d v4l2loopback devices\n", data->v4l2_device_count);
    
    return (data->v4l2_device_count >= 16);
}

bool verify_camera_config(InitData* data) {
    (void)data; // Suppress unused parameter warning
    
    printf("[INIT] Verifying camera configuration...\n");
    
    if (access(CAMERAS_CONFIG, F_OK) != 0) {
        printf("[INIT] Camera config not found: %s\n", CAMERAS_CONFIG);
        
        // Interactive config creation
        if (!create_camera_config_interactive()) {
            fprintf(stderr, "[ERROR] Camera configuration failed\n");
            return false;
        }
    }
    
    // Parse JSON to validate
    FILE *fp = fopen(CAMERAS_CONFIG, "r");
    if (!fp) {
        fprintf(stderr, "[ERROR] Cannot open %s\n", CAMERAS_CONFIG);
        return false;
    }
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char *json_str = malloc(size + 1);
    if (!json_str) {
        fclose(fp);
        return false;
    }
    
    fread(json_str, 1, size, fp);
    json_str[size] = '\0';
    fclose(fp);
    
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    
    if (!root || !cJSON_IsArray(root)) {
        fprintf(stderr, "[ERROR] Invalid camera config format\n");
        if (root) cJSON_Delete(root);
        return false;
    }
    
    int camera_count = cJSON_GetArraySize(root);
    printf("  Found %d cameras in config\n", camera_count);
    
    cJSON_Delete(root);
    return true;
}

bool run_initialization_phase() {
    printf("\n=== INITIALIZATION PHASE ===\n");
    set_phase(PHASE_INITIALIZATION);
    
    InitData* data = &g_state.init_data;
    memset(data, 0, sizeof(InitData));
    
    // Critical checks - fail fast
    if (!check_lan_connectivity(data)) {
        fprintf(stderr, "[FATAL] LAN connectivity check failed\n");
        return false;
    }
    
    if (!check_system_dependencies(data)) {
        fprintf(stderr, "[FATAL] System dependencies not satisfied\n");
        return false;
    }
    
    if (!check_python3_installation(data)) {
        fprintf(stderr, "[FATAL] Python3 not working\n");
        return false;
    }
    
    // Non-critical checks - warn but continue
    check_wlan_connectivity(data);
    check_kernel_modules(data);
    
    // Setup v4l2loopback
    if (!install_v4l2loopback(data)) {
        fprintf(stderr, "[FATAL] v4l2loopback setup failed\n");
        return false;
    }
    
    // Verify camera config
    if (!verify_camera_config(data)) {
        fprintf(stderr, "[FATAL] Camera configuration invalid\n");
        return false;
    }
    
    printf("[INIT] All initialization checks passed!\n");
    return true;
}

bool create_camera_config_interactive() {
    printf("\n[CONFIG] Camera configuration file not found.\n");
    printf("[CONFIG] Would you like to create it now? (y/n): ");
    fflush(stdout);
    
    char response[10];
    if (!fgets(response, sizeof(response), stdin)) {
        return false;
    }
    
    if (response[0] != 'y' && response[0] != 'Y') {
        fprintf(stderr, "[CONFIG] Configuration cancelled by user\n");
        return false;
    }
    
    // Create /etc/roc directory if it doesn't exist
    if (access("/etc/roc", F_OK) != 0) {
        if (mkdir("/etc/roc", 0755) != 0) {
            fprintf(stderr, "[ERROR] Failed to create /etc/roc directory: %s\n", strerror(errno));
            return false;
        }
        printf("[CONFIG] Created /etc/roc directory\n");
    }
    
    cJSON *cameras = cJSON_CreateArray();
    if (!cameras) {
        fprintf(stderr, "[ERROR] Failed to create JSON array\n");
        return false;
    }
    
    printf("\n[CONFIG] Camera Configuration Wizard\n");
    printf("[CONFIG] ===============================\n");
    printf("[CONFIG] You have %d v4l2loopback devices available (video10-video25)\n", 
           g_state.init_data.v4l2_device_count);
    printf("[CONFIG] Enter camera details (press Enter with empty IP to finish)\n\n");
    
    int camera_num = 0;
    while (camera_num < 16) {
        printf("[CONFIG] Camera %d:\n", camera_num + 1);
        
        char ip[128], user[64], password[128];
        
        // Get IP
        printf("  IP address: ");
        fflush(stdout);
        if (!fgets(ip, sizeof(ip), stdin)) break;
        ip[strcspn(ip, "\n")] = '\0';
        
        // Empty IP means done
        if (strlen(ip) == 0) break;
        
        // Validate IP format (basic check)
        int a, b, c, d;
        if (sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4 ||
            a < 0 || a > 255 || b < 0 || b > 255 ||
            c < 0 || c > 255 || d < 0 || d > 255) {
            fprintf(stderr, "  [ERROR] Invalid IP address format. Try again.\n");
            continue;
        }
        
        // Get username (default: admin)
        printf("  Username [admin]: ");
        fflush(stdout);
        if (!fgets(user, sizeof(user), stdin)) break;
        user[strcspn(user, "\n")] = '\0';
        if (strlen(user) == 0) {
            strcpy(user, "admin");
        }
        
        // Get password
        printf("  Password: ");
        fflush(stdout);
        if (!fgets(password, sizeof(password), stdin)) break;
        password[strcspn(password, "\n")] = '\0';
        
        if (strlen(password) == 0) {
            fprintf(stderr, "  [ERROR] Password cannot be empty. Try again.\n");
            continue;
        }
        
        // Create camera object
        cJSON *camera = cJSON_CreateObject();
        cJSON_AddStringToObject(camera, "ip", ip);
        cJSON_AddStringToObject(camera, "user", user);
        cJSON_AddStringToObject(camera, "password", password);
        cJSON_AddItemToArray(cameras, camera);
        
        printf("  [OK] Camera %d added\n\n", camera_num + 1);
        camera_num++;
    }
    
    if (camera_num == 0) {
        fprintf(stderr, "[ERROR] No cameras configured\n");
        cJSON_Delete(cameras);
        return false;
    }
    
    // Write to file
    char *json_str = cJSON_Print(cameras);
    if (!json_str) {
        fprintf(stderr, "[ERROR] Failed to serialize JSON\n");
        cJSON_Delete(cameras);
        return false;
    }
    
    FILE *fp = fopen(CAMERAS_CONFIG, "w");
    if (!fp) {
        fprintf(stderr, "[ERROR] Failed to create %s: %s\n", CAMERAS_CONFIG, strerror(errno));
        cJSON_free(json_str);
        cJSON_Delete(cameras);
        return false;
    }
    
    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    
    // Set proper permissions (readable by owner and group)
    chmod(CAMERAS_CONFIG, 0640);
    
    printf("\n[CONFIG] Configuration saved to %s\n", CAMERAS_CONFIG);
    printf("[CONFIG] %d camera(s) configured\n", camera_num);
    
    cJSON_free(json_str);
    cJSON_Delete(cameras);
    
    return true;
}

// ============================================================================
// DAEMON THREAD FUNCTIONS
// ============================================================================

void* network_monitor_daemon(void* arg) {
    Daemon* daemon = (Daemon*)arg;
    printf("[DAEMON] Network monitor started\n");
    
    while (!is_shutdown_requested() && daemon->active) {
        // Periodically check network status
        lan_info_t lan_info;
        if (check_LAN(&lan_info) == 0) {
            if (!lan_info.reachable) {
                printf("[WARN] LAN gateway not reachable\n");
            }
        }
        
        sleep(30); // Check every 30 seconds
    }
    
    printf("[DAEMON] Network monitor stopped\n");
    return NULL;
}

void* camera_health_daemon(void* arg) {
    Daemon* daemon = (Daemon*)arg;
    printf("[DAEMON] Camera health monitor started\n");
    
    const char *videopipe_path = "./bin/videopipe";
    
    while (!is_shutdown_requested() && daemon->active) {
        // Check if videopipe is running
        if (system("pgrep -x videopipe >/dev/null 2>&1") != 0) {
            printf("[WARN] videopipe not running, attempting restart\n");
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "%s &", videopipe_path);
            printf("[DEBUG] Executing: %s\n", cmd);
            int ret = system(cmd);
            if (ret != 0) {
                printf("[ERROR] Failed to restart videopipe (return code %d)\n", ret);
            } else {
                printf("[INFO] Restarted videopipe\n");
            }
        } else {
            printf("[INFO] videopipe is running\n");
            // Check videopipe log for errors
            char log_cmd[256];
            snprintf(log_cmd, sizeof(log_cmd), "tail -n 5 /var/log/videopipe.log 2>/dev/null");
            printf("[DEBUG] Checking videopipe log: %s\n", log_cmd);
            system(log_cmd);
        }
        
        sleep(60); // Check every minute
    }
    
    printf("[DAEMON] Camera health monitor stopped\n");
    // Ensure videopipe is terminated
    printf("[DEBUG] Terminating videopipe\n");
    system("pkill -x videopipe >/dev/null 2>&1");
    return NULL;
}

// ============================================================================
// DAEMON MANAGEMENT
// ============================================================================

bool spawn_daemon(DaemonType type, void* (*daemon_func)(void*)) {
    pthread_mutex_lock(&g_state.daemon_mutex);
    
    if (g_state.daemon_count >= MAX_DAEMONS) {
        fprintf(stderr, "[ERROR] Maximum daemon count reached\n");
        pthread_mutex_unlock(&g_state.daemon_mutex);
        return false;
    }
    
    Daemon* daemon = &g_state.daemons[g_state.daemon_count];
    daemon->type = type;
    daemon->active = true;
    daemon->context = NULL;
    
    pthread_mutex_init(&daemon->mutex, NULL);
    
    if (!create_pipe_pair(&daemon->to_daemon) || 
        !create_pipe_pair(&daemon->from_daemon)) {
        fprintf(stderr, "[ERROR] Failed to create pipes for daemon\n");
        pthread_mutex_unlock(&g_state.daemon_mutex);
        return false;
    }
    
    if (pthread_create(&daemon->thread, NULL, daemon_func, daemon) != 0) {
        fprintf(stderr, "[ERROR] Failed to spawn daemon thread\n");
        close_pipe_pair(&daemon->to_daemon);
        close_pipe_pair(&daemon->from_daemon);
        pthread_mutex_unlock(&g_state.daemon_mutex);
        return false;
    }
    
    g_state.daemon_count++;
    pthread_mutex_unlock(&g_state.daemon_mutex);
    
    printf("[MAIN] Spawned daemon type %d\n", type);
    return true;
}

void stop_all_daemons() {
    printf("\n[MAIN] Stopping all daemons...\n");
    
    pthread_mutex_lock(&g_state.daemon_mutex);
    
    for (int i = 0; i < g_state.daemon_count; i++) {
        g_state.daemons[i].active = false;
    }
    
    pthread_mutex_unlock(&g_state.daemon_mutex);
    
    for (int i = 0; i < g_state.daemon_count; i++) {
        pthread_join(g_state.daemons[i].thread, NULL);
        close_pipe_pair(&g_state.daemons[i].to_daemon);
        close_pipe_pair(&g_state.daemons[i].from_daemon);
        pthread_mutex_destroy(&g_state.daemons[i].mutex);
        printf("[MAIN] Daemon %d stopped and cleaned up\n", i);
    }
    
    g_state.daemon_count = 0;
}

// ============================================================================
// RUNNING PHASE
// ============================================================================

bool spawn_all_daemons() {
    printf("\n[MAIN] Spawning daemons...\n");
    
    if (!spawn_daemon(DAEMON_NETWORK_MONITOR, network_monitor_daemon)) return false;
    if (!spawn_daemon(DAEMON_CAMERA_STREAMER, camera_health_daemon)) return false;
    
    return true;
}

bool run_main_loop() {
    printf("\n=== RUNNING PHASE ===\n");
    set_phase(PHASE_RUNNING);
    
    if (!spawn_all_daemons()) {
        fprintf(stderr, "[ERROR] Failed to spawn daemons\n");
        return false;
    }
    
    printf("[MAIN] Entering main processing loop\n");
    printf("[MAIN] System is running. Press Ctrl+C to shut down.\n");
    
    while (!is_shutdown_requested()) {
        // Main loop - monitor daemons and handle events
        usleep(100000); // 100ms
    }
    
    printf("[MAIN] Exiting main processing loop\n");
    return true;
}

// ============================================================================
// CLEANUP PHASE
// ============================================================================

void run_cleanup_phase() {
    printf("\n=== CLEANUP PHASE ===\n");
    set_phase(PHASE_CLEANUP);
    
    stop_all_daemons();
    
    // Stop videopipe if running
    printf("[CLEANUP] Stopping videopipe...\n");
    system("pkill -TERM videopipe 2>/dev/null");
    
    pthread_mutex_destroy(&g_state.phase_mutex);
    pthread_mutex_destroy(&g_state.shutdown_mutex);
    pthread_mutex_destroy(&g_state.daemon_mutex);
    
    printf("[CLEANUP] All cleanup completed\n");
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int main(int argc, char* argv[]) {
    printf("===========================================\n");
    printf("  ROC System Main Controller\n");
    printf("===========================================\n");
    
    // Check if running as root
    if (geteuid() != 0) {
        fprintf(stderr, "This program requires root privileges\n");
        fprintf(stderr, "Please run with: sudo %s\n", argv[0]);
        return 1;
    }
    
    // Initialize global state
    memset(&g_state, 0, sizeof(GlobalState));
    g_state.phase = PHASE_INITIALIZATION;
    g_state.shutdown_requested = false;
    g_state.daemon_count = 0;
    
    pthread_mutex_init(&g_state.phase_mutex, NULL);
    pthread_mutex_init(&g_state.shutdown_mutex, NULL);
    pthread_mutex_init(&g_state.daemon_mutex, NULL);
    
    setup_signal_handlers();
    
    int exit_code = EXIT_SUCCESS;
    
    // PHASE 1: INITIALIZATION
    if (!run_initialization_phase()) {
        fprintf(stderr, "\n[FATAL] Initialization failed\n");
        set_phase(PHASE_ERROR);
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }
    
    // PHASE 2: RUNNING
    if (!run_main_loop()) {
        fprintf(stderr, "\n[ERROR] Main loop encountered an error\n");
        set_phase(PHASE_ERROR);
        exit_code = EXIT_FAILURE;
    }
    
cleanup:
    // PHASE 3: CLEANUP
    run_cleanup_phase();
    
    printf("\n===========================================\n");
    printf("  System Terminated (exit code: %d)\n", exit_code);
    printf("===========================================\n");
    
    return exit_code;
}
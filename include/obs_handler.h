#ifndef OBS_WEBSOCKET_H
#define OBS_WEBSOCKET_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

// Forward declarations
typedef struct obs_websocket obs_websocket_t;
typedef struct obs_command obs_command_t;

// OBS WebSocket connection states
typedef enum {
    OBS_STATE_UNINITIALIZED = 0,
    OBS_STATE_DISCONNECTED,
    OBS_STATE_CONNECTING,
    OBS_STATE_AUTHENTICATING,
    OBS_STATE_CONNECTED,
    OBS_STATE_ERROR,
    OBS_STATE_RECONNECTING,
    OBS_STATE_SHUTTING_DOWN
} obs_connection_state_t;

// OBS-specific error/status flags (bitfield)
typedef enum {
    OBS_FLAG_DAEMON_READY      = 1 << 0,   // Daemon thread started successfully
    OBS_FLAG_SOCKET_CONNECTED  = 1 << 1,   // TCP connection established
    OBS_FLAG_WEBSOCKET_READY   = 1 << 2,   // WebSocket handshake complete
    OBS_FLAG_AUTHENTICATED     = 1 << 3,   // OBS authentication successful
    OBS_FLAG_KEEPALIVE_OK      = 1 << 4,   // Recent pong received
    OBS_FLAG_SCENE_CACHE_VALID = 1 << 5,   // Current scene is cached and valid
    OBS_FLAG_COMMAND_QUEUE_OK  = 1 << 6,   // Command queue not full
    
    // Error flags
    OBS_FLAG_NETWORK_ERROR     = 1 << 8,   // Network/socket error
    OBS_FLAG_AUTH_ERROR        = 1 << 9,   // Authentication failed
    OBS_FLAG_PROTOCOL_ERROR    = 1 << 10,  // WebSocket protocol error
    OBS_FLAG_TIMEOUT_ERROR     = 1 << 11,  // Ping timeout or response timeout
    OBS_FLAG_QUEUE_FULL        = 1 << 12,  // Command queue is full
    OBS_FLAG_MEMORY_ERROR      = 1 << 13,  // Memory allocation failed
    OBS_FLAG_CONFIG_ERROR      = 1 << 14,  // Invalid configuration
    OBS_FLAG_SHUTDOWN_ERROR    = 1 << 15   // Error during shutdown
} obs_status_flags_t;

// Command types for the queue
typedef enum {
    OBS_CMD_SWITCH_SCENE,
    OBS_CMD_GET_CURRENT_SCENE,
    OBS_CMD_GET_SCENE_LIST,
    OBS_CMD_SET_SOURCE_VISIBILITY,
    OBS_CMD_PING,
    OBS_CMD_SHUTDOWN
} obs_command_type_t;

// Command priority levels
typedef enum {
    OBS_PRIORITY_LOW = 0,
    OBS_PRIORITY_NORMAL = 1,
    OBS_PRIORITY_HIGH = 2,
    OBS_PRIORITY_CRITICAL = 3  // For ultra-fast breakout sequences
} obs_command_priority_t;

// Configuration structure
typedef struct {
    char host[256];
    int port;
    char password[512];
    int max_retries;
    int retry_delay_ms;
    int ping_interval_ms;
    int ping_timeout_ms;
    int command_timeout_ms;
    size_t command_queue_size;
    bool enable_scene_cache;
    bool enable_keepalive;
} obs_config_t;

// Statistics structure for monitoring
typedef struct {
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t scene_switches;
    uint64_t reconnections;
    uint64_t ping_failures;
    uint64_t command_timeouts;
    uint64_t queue_overflows;
    time_t connection_start_time;
    time_t last_successful_ping;
    double avg_response_time_ms;
} obs_statistics_t;

// Callback function types
typedef void (*obs_state_callback_t)(obs_connection_state_t old_state, 
                                     obs_connection_state_t new_state, 
                                     void* user_data);

typedef void (*obs_error_callback_t)(const char* error_message, 
                                     obs_status_flags_t error_flags, 
                                     void* user_data);

typedef void (*obs_scene_callback_t)(const char* scene_name, void* user_data);

// Main OBS WebSocket interface functions

/**
 * Initialize OBS WebSocket daemon with configuration
 * Returns: 0 on success, negative on error
 */
int obs_websocket_init(obs_websocket_t** obs, const obs_config_t* config);

/**
 * Start the daemon thread (non-blocking)
 * The daemon will attempt connections in the background
 * Returns: 0 on success, negative on error
 */
int obs_websocket_start_daemon(obs_websocket_t* obs);

/**
 * Switch to a specific scene (mimics Python interface)
 * This queues the command and returns immediately for ultra-fast execution
 * Returns: 0 on success (queued), negative on error
 */
int obs_websocket_switch_scene(obs_websocket_t* obs, const char* scene_name, 
                              obs_command_priority_t priority);

/**
 * Get current connection state (thread-safe)
 * Returns: current state enum value
 */
obs_connection_state_t obs_websocket_get_state(obs_websocket_t* obs);

/**
 * Get current status flags (thread-safe bitfield)
 * Returns: current status flags
 */
uint32_t obs_websocket_get_status_flags(obs_websocket_t* obs);

/**
 * Get current scene name (thread-safe, cached)
 * Returns: scene name or NULL if not available
 * Note: Returned string is valid until next call or daemon shutdown
 */
const char* obs_websocket_get_current_scene(obs_websocket_t* obs);

/**
 * Get daemon statistics for monitoring
 * Returns: 0 on success, negative on error
 */
int obs_websocket_get_statistics(obs_websocket_t* obs, obs_statistics_t* stats);

/**
 * Check if daemon is ready to accept commands
 * Returns: true if ready, false otherwise
 */
bool obs_websocket_is_ready(obs_websocket_t* obs);

/**
 * Set callback functions for state changes and errors
 * These will be called from the daemon thread
 */
void obs_websocket_set_state_callback(obs_websocket_t* obs, 
                                     obs_state_callback_t callback, 
                                     void* user_data);

void obs_websocket_set_error_callback(obs_websocket_t* obs, 
                                     obs_error_callback_t callback, 
                                     void* user_data);

void obs_websocket_set_scene_callback(obs_websocket_t* obs, 
                                     obs_scene_callback_t callback, 
                                     void* user_data);

/**
 * Force immediate reconnection attempt
 * Returns: 0 on success, negative on error
 */
int obs_websocket_reconnect(obs_websocket_t* obs);

/**
 * Gracefully shutdown daemon and cleanup resources
 * This will block until daemon thread exits
 * Returns: 0 on success, negative on error
 */
int obs_websocket_cleanup(obs_websocket_t* obs);

// Utility functions for configuration
obs_config_t obs_websocket_default_config(void);
int obs_websocket_validate_config(const obs_config_t* config);
const char* obs_websocket_state_to_string(obs_connection_state_t state);
const char* obs_websocket_flags_to_string(uint32_t flags, char* buffer, size_t buffer_size);

// Constants
#define OBS_MAX_SCENE_NAME_LENGTH 256
#define OBS_MAX_ERROR_MESSAGE_LENGTH 512
#define OBS_DEFAULT_COMMAND_QUEUE_SIZE 64
#define OBS_DEFAULT_PORT 4455
#define OBS_DEFAULT_PING_INTERVAL_MS 10000
#define OBS_DEFAULT_PING_TIMEOUT_MS 5000
#define OBS_DEFAULT_COMMAND_TIMEOUT_MS 2000
#define OBS_DEFAULT_MAX_RETRIES 5
#define OBS_DEFAULT_RETRY_DELAY_MS 5000

#endif // OBS_WEBSOCKET_H
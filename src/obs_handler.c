#include "obs_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <cjson/cJSON.h>
#include <uuid/uuid.h>

// Internal command structure
struct obs_command {
    obs_command_type_t type;
    obs_command_priority_t priority;
    char scene_name[OBS_MAX_SCENE_NAME_LENGTH];
    char request_id[40]; // UUID string
    time_t created_time;
    struct obs_command* next;
};

// Main WebSocket structure
struct obs_websocket {
    // Connection state
    obs_connection_state_t state;
    int socket_fd;
    obs_config_t config;
    
    // Threading
    pthread_t daemon_thread;
    pthread_mutex_t state_mutex;
    pthread_mutex_t command_mutex;
    pthread_cond_t command_cond;
    bool should_exit;
    
    // Command queue (priority queue)
    obs_command_t* command_queue_head[4]; // One for each priority level
    size_t command_queue_count;
    
    // Status tracking
    uint32_t status_flags;
    char current_scene[OBS_MAX_SCENE_NAME_LENGTH];
    char error_message[OBS_MAX_ERROR_MESSAGE_LENGTH];
    
    // Timing and statistics
    obs_statistics_t stats;
    time_t last_ping_sent;
    time_t last_pong_received;
    int retry_count;
    
    // Authentication data
    char challenge[256];
    char salt[256];
    bool auth_required;
    
    // Callbacks
    obs_state_callback_t state_callback;
    obs_error_callback_t error_callback;
    obs_scene_callback_t scene_callback;
    void* callback_user_data;
    
    // WebSocket frame buffer
    char* frame_buffer;
    size_t frame_buffer_size;
    size_t frame_data_len;
};

// Internal function declarations
static void* obs_daemon_thread(void* arg);
static int obs_connect_socket(obs_websocket_t* obs);
static int obs_websocket_handshake(obs_websocket_t* obs);
static int obs_authenticate(obs_websocket_t* obs);
static int obs_send_frame(obs_websocket_t* obs, const char* data, size_t len, int opcode);
static int obs_read_frame(obs_websocket_t* obs, char** data, size_t* len, int* opcode);
static void obs_process_message(obs_websocket_t* obs, const char* message);
static void obs_process_command_queue(obs_websocket_t* obs);
static void obs_set_state(obs_websocket_t* obs, obs_connection_state_t new_state);
static void obs_set_error_flag(obs_websocket_t* obs, obs_status_flags_t flag, const char* message);
static void obs_clear_error_flag(obs_websocket_t* obs, obs_status_flags_t flag);
static int obs_send_ping(obs_websocket_t* obs);
static char* obs_generate_websocket_key(void);
static int obs_base64_encode(const unsigned char* input, int length, char** output);

// WebSocket opcodes
#define WS_OPCODE_TEXT 0x01
#define WS_OPCODE_CLOSE 0x08
#define WS_OPCODE_PING 0x09
#define WS_OPCODE_PONG 0x0A

// Default configuration
obs_config_t obs_websocket_default_config(void) {
    obs_config_t config = {0};
    strcpy(config.host, "localhost");
    config.port = OBS_DEFAULT_PORT;
    config.password[0] = '\0';
    config.max_retries = OBS_DEFAULT_MAX_RETRIES;
    config.retry_delay_ms = OBS_DEFAULT_RETRY_DELAY_MS;
    config.ping_interval_ms = OBS_DEFAULT_PING_INTERVAL_MS;
    config.ping_timeout_ms = OBS_DEFAULT_PING_TIMEOUT_MS;
    config.command_timeout_ms = OBS_DEFAULT_COMMAND_TIMEOUT_MS;
    config.command_queue_size = OBS_DEFAULT_COMMAND_QUEUE_SIZE;
    config.enable_scene_cache = true;
    config.enable_keepalive = true;
    return config;
}

int obs_websocket_validate_config(const obs_config_t* config) {
    if (!config) return -1;
    if (strlen(config->host) == 0) return -2;
    if (config->port <= 0 || config->port > 65535) return -3;
    if (config->command_queue_size == 0) return -4;
    return 0;
}

int obs_websocket_init(obs_websocket_t** obs, const obs_config_t* config) {
    if (!obs || !config) return -1;
    
    if (obs_websocket_validate_config(config) != 0) {
        return -2;
    }
    
    obs_websocket_t* new_obs = calloc(1, sizeof(obs_websocket_t));
    if (!new_obs) return -3;
    
    // Initialize structure
    new_obs->state = OBS_STATE_UNINITIALIZED;
    new_obs->socket_fd = -1;
    new_obs->config = *config;
    new_obs->should_exit = false;
    new_obs->status_flags = 0;
    new_obs->retry_count = 0;
    new_obs->auth_required = false;
    new_obs->command_queue_count = 0;
    
    // Initialize mutexes
    if (pthread_mutex_init(&new_obs->state_mutex, NULL) != 0) {
        free(new_obs);
        return -4;
    }
    
    if (pthread_mutex_init(&new_obs->command_mutex, NULL) != 0) {
        pthread_mutex_destroy(&new_obs->state_mutex);
        free(new_obs);
        return -4;
    }
    
    if (pthread_cond_init(&new_obs->command_cond, NULL) != 0) {
        pthread_mutex_destroy(&new_obs->command_mutex);
        pthread_mutex_destroy(&new_obs->state_mutex);
        free(new_obs);
        return -4;
    }
    
    // Allocate frame buffer
    new_obs->frame_buffer_size = 8192; // 8KB initial buffer
    new_obs->frame_buffer = malloc(new_obs->frame_buffer_size);
    if (!new_obs->frame_buffer) {
        pthread_cond_destroy(&new_obs->command_cond);
        pthread_mutex_destroy(&new_obs->command_mutex);
        pthread_mutex_destroy(&new_obs->state_mutex);
        free(new_obs);
        return -5;
    }
    
    // Initialize statistics
    memset(&new_obs->stats, 0, sizeof(obs_statistics_t));
    
    obs_set_state(new_obs, OBS_STATE_DISCONNECTED);
    
    *obs = new_obs;
    return 0;
}

int obs_websocket_start_daemon(obs_websocket_t* obs) {
    if (!obs) return -1;
    
    pthread_mutex_lock(&obs->state_mutex);
    
    if (obs->state != OBS_STATE_DISCONNECTED) {
        pthread_mutex_unlock(&obs->state_mutex);
        return -2;
    }
    
    int result = pthread_create(&obs->daemon_thread, NULL, obs_daemon_thread, obs);
    if (result == 0) {
        obs->status_flags |= OBS_FLAG_DAEMON_READY;
    }
    
    pthread_mutex_unlock(&obs->state_mutex);
    return result;
}

static void* obs_daemon_thread(void* arg) {
    obs_websocket_t* obs = (obs_websocket_t*)arg;
    struct pollfd pfd;
    int poll_timeout;
    time_t last_keepalive = 0;
    
    while (!obs->should_exit) {
        // Handle connection state
        switch (obs->state) {
            case OBS_STATE_DISCONNECTED:
            case OBS_STATE_RECONNECTING:
                if (obs_connect_socket(obs) == 0) {
                    if (obs_websocket_handshake(obs) == 0) {
                        obs_set_state(obs, obs->auth_required ? OBS_STATE_AUTHENTICATING : OBS_STATE_CONNECTED);
                    }
                }
                if (obs->state == OBS_STATE_ERROR || obs->state == OBS_STATE_DISCONNECTED) {
                    usleep(obs->config.retry_delay_ms * 1000);
                }
                break;
                
            case OBS_STATE_AUTHENTICATING:
                if (obs_authenticate(obs) == 0) {
                    obs_set_state(obs, OBS_STATE_CONNECTED);
                    obs->status_flags |= OBS_FLAG_AUTHENTICATED;
                    obs_clear_error_flag(obs, OBS_FLAG_AUTH_ERROR);
                }
                break;
                
            case OBS_STATE_CONNECTED:
                // Setup polling
                pfd.fd = obs->socket_fd;
                pfd.events = POLLIN;
                poll_timeout = 100; // 100ms timeout for responsiveness
                
                int poll_result = poll(&pfd, 1, poll_timeout);
                
                if (poll_result > 0 && (pfd.revents & POLLIN)) {
                    // Data available to read
                    char* frame_data = NULL;
                    size_t frame_len = 0;
                    int opcode = 0;
                    
                    if (obs_read_frame(obs, &frame_data, &frame_len, &opcode) == 0) {
                        if (opcode == WS_OPCODE_TEXT && frame_data) {
                            obs_process_message(obs, frame_data);
                        } else if (opcode == WS_OPCODE_PONG) {
                            obs->last_pong_received = time(NULL);
                            obs->status_flags |= OBS_FLAG_KEEPALIVE_OK;
                        }
                        
                        if (frame_data) {
                            free(frame_data);
                        }
                    }
                } else if (poll_result < 0) {
                    obs_set_error_flag(obs, OBS_FLAG_NETWORK_ERROR, "Poll error");
                    obs_set_state(obs, OBS_STATE_ERROR);
                    break;
                }
                
                // Process command queue
                obs_process_command_queue(obs);
                
                // Handle keepalive
                time_t now = time(NULL);
                if (obs->config.enable_keepalive && 
                    (now - last_keepalive) >= (obs->config.ping_interval_ms / 1000)) {
                    
                    if (obs_send_ping(obs) == 0) {
                        last_keepalive = now;
                    }
                    
                    // Check for ping timeout
                    if ((now - obs->last_pong_received) > (obs->config.ping_timeout_ms / 1000)) {
                        obs_set_error_flag(obs, OBS_FLAG_TIMEOUT_ERROR, "Ping timeout");
                        obs_set_state(obs, OBS_STATE_ERROR);
                    }
                }
                break;
                
            case OBS_STATE_ERROR:
                // Close socket and attempt reconnection
                if (obs->socket_fd >= 0) {
                    close(obs->socket_fd);
                    obs->socket_fd = -1;
                }
                
                obs->retry_count++;
                if (obs->retry_count < obs->config.max_retries) {
                    obs_set_state(obs, OBS_STATE_RECONNECTING);
                } else {
                    // Max retries exceeded, stay in error state
                    usleep(obs->config.retry_delay_ms * 1000);
                }
                break;
                
            case OBS_STATE_SHUTTING_DOWN:
                obs->should_exit = true;
                break;
                
            default:
                break;
        }
    }
    
    // Cleanup
    if (obs->socket_fd >= 0) {
        close(obs->socket_fd);
        obs->socket_fd = -1;
    }
    
    return NULL;
}

static int obs_connect_socket(obs_websocket_t* obs) {
    struct sockaddr_in server_addr;
    struct hostent* server;
    
    obs_set_state(obs, OBS_STATE_CONNECTING);
    
    // Create socket
    obs->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (obs->socket_fd < 0) {
        obs_set_error_flag(obs, OBS_FLAG_NETWORK_ERROR, "Failed to create socket");
        obs_set_state(obs, OBS_STATE_ERROR);
        return -1;
    }
    
    // Resolve hostname
    server = gethostbyname(obs->config.host);
    if (!server) {
        obs_set_error_flag(obs, OBS_FLAG_NETWORK_ERROR, "Failed to resolve hostname");
        obs_set_state(obs, OBS_STATE_ERROR);
        return -2;
    }
    
    // Setup server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(obs->config.port);
    
    // Connect
    if (connect(obs->socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        obs_set_error_flag(obs, OBS_FLAG_NETWORK_ERROR, "Failed to connect to OBS");
        obs_set_state(obs, OBS_STATE_ERROR);
        close(obs->socket_fd);
        obs->socket_fd = -1;
        return -3;
    }
    
    obs->status_flags |= OBS_FLAG_SOCKET_CONNECTED;
    obs_clear_error_flag(obs, OBS_FLAG_NETWORK_ERROR);
    return 0;
}

static int obs_websocket_handshake(obs_websocket_t* obs) {
    char* websocket_key = obs_generate_websocket_key();
    if (!websocket_key) return -1;
    
    // Send WebSocket upgrade request
    char request[1024];
    snprintf(request, sizeof(request),
        "GET / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: obswebsocket.json\r\n"
        "\r\n",
        obs->config.host, obs->config.port, websocket_key);
    
    if (send(obs->socket_fd, request, strlen(request), 0) < 0) {
        free(websocket_key);
        obs_set_error_flag(obs, OBS_FLAG_PROTOCOL_ERROR, "Failed to send WebSocket handshake");
        obs_set_state(obs, OBS_STATE_ERROR);
        return -2;
    }
    
    // Read response
    char response[2048] = {0};
    ssize_t bytes_received = recv(obs->socket_fd, response, sizeof(response) - 1, 0);
    
    free(websocket_key);
    
    if (bytes_received <= 0) {
        obs_set_error_flag(obs, OBS_FLAG_PROTOCOL_ERROR, "Failed to receive WebSocket handshake response");
        obs_set_state(obs, OBS_STATE_ERROR);
        return -3;
    }
    
    // Validate response (simple check for "101 Switching Protocols")
    if (!strstr(response, "101 Switching Protocols")) {
        obs_set_error_flag(obs, OBS_FLAG_PROTOCOL_ERROR, "Invalid WebSocket handshake response");
        obs_set_state(obs, OBS_STATE_ERROR);
        return -4;
    }
    
    obs->status_flags |= OBS_FLAG_WEBSOCKET_READY;
    obs_clear_error_flag(obs, OBS_FLAG_PROTOCOL_ERROR);
    return 0;
}

// Scene switching function (main interface)
int obs_websocket_switch_scene(obs_websocket_t* obs, const char* scene_name, obs_command_priority_t priority) {
    if (!obs || !scene_name) return -1;
    
    // Check if we're already on this scene (ultra-fast cache check)
    pthread_mutex_lock(&obs->state_mutex);
    if (obs->config.enable_scene_cache && 
        strcmp(obs->current_scene, scene_name) == 0) {
        pthread_mutex_unlock(&obs->state_mutex);
        return 0; // Already on this scene
    }
    pthread_mutex_unlock(&obs->state_mutex);
    
    // Create command
    obs_command_t* cmd = calloc(1, sizeof(obs_command_t));
    if (!cmd) return -2;
    
    cmd->type = OBS_CMD_SWITCH_SCENE;
    cmd->priority = priority;
    strncpy(cmd->scene_name, scene_name, OBS_MAX_SCENE_NAME_LENGTH - 1);
    cmd->created_time = time(NULL);
    
    // Generate UUID for request
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse(uuid, cmd->request_id);
    
    // Add to priority queue
    pthread_mutex_lock(&obs->command_mutex);
    
    if (obs->command_queue_count >= obs->config.command_queue_size) {
        pthread_mutex_unlock(&obs->command_mutex);
        free(cmd);
        obs_set_error_flag(obs, OBS_FLAG_QUEUE_FULL, "Command queue full");
        return -3;
    }
    
    // Insert into appropriate priority queue
    cmd->next = obs->command_queue_head[priority];
    obs->command_queue_head[priority] = cmd;
    obs->command_queue_count++;
    
    pthread_cond_signal(&obs->command_cond);
    pthread_mutex_unlock(&obs->command_mutex);
    
    return 0;
}

// State and status functions
obs_connection_state_t obs_websocket_get_state(obs_websocket_t* obs) {
    if (!obs) return OBS_STATE_UNINITIALIZED;
    
    pthread_mutex_lock(&obs->state_mutex);
    obs_connection_state_t state = obs->state;
    pthread_mutex_unlock(&obs->state_mutex);
    
    return state;
}

uint32_t obs_websocket_get_status_flags(obs_websocket_t* obs) {
    if (!obs) return 0;
    
    pthread_mutex_lock(&obs->state_mutex);
    uint32_t flags = obs->status_flags;
    pthread_mutex_unlock(&obs->state_mutex);
    
    return flags;
}

const char* obs_websocket_get_current_scene(obs_websocket_t* obs) {
    if (!obs) return NULL;
    
    pthread_mutex_lock(&obs->state_mutex);
    const char* scene = (obs->current_scene[0] != '\0') ? obs->current_scene : NULL;
    pthread_mutex_unlock(&obs->state_mutex);
    
    return scene;
}

int obs_websocket_get_statistics(obs_websocket_t* obs, obs_statistics_t* stats) {
    if (!obs || !stats) return -1;
    
    pthread_mutex_lock(&obs->state_mutex);
    *stats = obs->stats;
    pthread_mutex_unlock(&obs->state_mutex);
    
    return 0;
}

bool obs_websocket_is_ready(obs_websocket_t* obs) {
    if (!obs) return false;
    
    uint32_t flags = obs_websocket_get_status_flags(obs);
    return (obs_websocket_get_state(obs) == OBS_STATE_CONNECTED) &&
           (flags & OBS_FLAG_AUTHENTICATED) &&
           !(flags & (OBS_FLAG_NETWORK_ERROR | OBS_FLAG_AUTH_ERROR | OBS_FLAG_PROTOCOL_ERROR));
}

// Callback setters
void obs_websocket_set_state_callback(obs_websocket_t* obs, obs_state_callback_t callback, void* user_data) {
    if (!obs) return;
    
    pthread_mutex_lock(&obs->state_mutex);
    obs->state_callback = callback;
    obs->callback_user_data = user_data;
    pthread_mutex_unlock(&obs->state_mutex);
}

void obs_websocket_set_error_callback(obs_websocket_t* obs, obs_error_callback_t callback, void* user_data) {
    if (!obs) return;
    
    pthread_mutex_lock(&obs->state_mutex);
    obs->error_callback = callback;
    obs->callback_user_data = user_data;
    pthread_mutex_unlock(&obs->state_mutex);
}

void obs_websocket_set_scene_callback(obs_websocket_t* obs, obs_scene_callback_t callback, void* user_data) {
    if (!obs) return;
    
    pthread_mutex_lock(&obs->state_mutex);
    obs->scene_callback = callback;
    obs->callback_user_data = user_data;
    pthread_mutex_unlock(&obs->state_mutex);
}

// Force reconnection function
int obs_websocket_reconnect(obs_websocket_t* obs) {
    if (!obs) return -1;
    
    pthread_mutex_lock(&obs->state_mutex);
    if (obs->state == OBS_STATE_CONNECTED) {
        obs_set_state(obs, OBS_STATE_RECONNECTING);
        obs->retry_count = 0; // Reset retry count for manual reconnection
    }
    pthread_mutex_unlock(&obs->state_mutex);
    
    return 0;
}

// Cleanup
int obs_websocket_cleanup(obs_websocket_t* obs) {
    if (!obs) return -1;
    
    // Signal shutdown
    obs->should_exit = true;
    obs_set_state(obs, OBS_STATE_SHUTTING_DOWN);
    
    // Wake up daemon thread
    pthread_cond_signal(&obs->command_cond);
    
    // Wait for daemon thread to exit
    pthread_join(obs->daemon_thread, NULL);
    
    // Cleanup resources
    if (obs->socket_fd >= 0) {
        close(obs->socket_fd);
    }
    
    // Free command queue
    pthread_mutex_lock(&obs->command_mutex);
    for (int i = 0; i < 4; i++) {
        obs_command_t* cmd = obs->command_queue_head[i];
        while (cmd) {
            obs_command_t* next = cmd->next;
            free(cmd);
            cmd = next;
        }
    }
    pthread_mutex_unlock(&obs->command_mutex);
    
    // Destroy synchronization objects
    pthread_cond_destroy(&obs->command_cond);
    pthread_mutex_destroy(&obs->command_mutex);
    pthread_mutex_destroy(&obs->state_mutex);
    
    // Free buffers
    free(obs->frame_buffer);
    free(obs);
    
    return 0;
}

// Helper functions
static void obs_set_state(obs_websocket_t* obs, obs_connection_state_t new_state) {
    pthread_mutex_lock(&obs->state_mutex);
    obs_connection_state_t old_state = obs->state;
    obs->state = new_state;
    
    if (obs->state_callback && old_state != new_state) {
        obs->state_callback(old_state, new_state, obs->callback_user_data);
    }
    pthread_mutex_unlock(&obs->state_mutex);
}

static void obs_set_error_flag(obs_websocket_t* obs, obs_status_flags_t flag, const char* message) {
    pthread_mutex_lock(&obs->state_mutex);
    obs->status_flags |= flag;
    
    if (message) {
        strncpy(obs->error_message, message, OBS_MAX_ERROR_MESSAGE_LENGTH - 1);
        obs->error_message[OBS_MAX_ERROR_MESSAGE_LENGTH - 1] = '\0';
    }
    
    if (obs->error_callback) {
        obs->error_callback(message, flag, obs->callback_user_data);
    }
    pthread_mutex_unlock(&obs->state_mutex);
}

static void obs_clear_error_flag(obs_websocket_t* obs, obs_status_flags_t flag) {
    pthread_mutex_lock(&obs->state_mutex);
    obs->status_flags &= ~flag;
    pthread_mutex_unlock(&obs->state_mutex);
}

// Fixed base64 encoding function
static int obs_base64_encode(const unsigned char* input, int length, char** output) {
    BIO *bio, *b64;
    BUF_MEM *bufferPtr;
    
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, input, length);
    BIO_flush(bio);
    
    BIO_get_mem_ptr(bio, &bufferPtr);
    BIO_set_close(bio, BIO_NOCLOSE);
    BIO_free_all(bio);
    
    *output = malloc(bufferPtr->length + 1);
    if (!*output) {
        return -1;
    }
    
    memcpy(*output, bufferPtr->data, bufferPtr->length);
    (*output)[bufferPtr->length] = '\0';
    
    int result = bufferPtr->length;
    BUF_MEM_free(bufferPtr);
    
    return result;
}

// Placeholder implementations for WebSocket protocol functions
static char* obs_generate_websocket_key(void) {
    // Generate random 16-byte key and base64 encode
    unsigned char key[16];
    for (int i = 0; i < 16; i++) {
        key[i] = rand() % 256;
    }
    
    char* encoded = NULL;
    if (obs_base64_encode(key, 16, &encoded) < 0) {
        return NULL;
    }
    return encoded;
}

// WebSocket frame structure
typedef struct {
    uint8_t fin : 1;
    uint8_t rsv1 : 1;
    uint8_t rsv2 : 1;
    uint8_t rsv3 : 1;
    uint8_t opcode : 4;
    uint8_t mask : 1;
    uint8_t payload_len : 7;
} ws_header_t;

// WebSocket frame implementation
static int obs_send_frame(obs_websocket_t* obs, const char* data, size_t len, int opcode) {
    if (!obs || obs->socket_fd < 0) return -1;
    
    uint8_t header[14]; // Max header size
    size_t header_len = 2;
    
    // First byte: FIN=1, RSV=0, opcode
    header[0] = 0x80 | (opcode & 0x0F);
    
    // Second byte: MASK=1 (client must mask), payload length
    if (len < 126) {
        header[1] = 0x80 | (len & 0x7F);
    } else if (len < 65536) {
        header[1] = 0x80 | 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    } else {
        header[1] = 0x80 | 127;
        // 64-bit length (big endian)
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (len >> (8 * (7 - i))) & 0xFF;
        }
        header_len = 10;
    }
    
    // Generate masking key (clients must mask data)
    uint8_t mask_key[4];
    for (int i = 0; i < 4; i++) {
        mask_key[i] = rand() & 0xFF;
    }
    memcpy(header + header_len, mask_key, 4);
    header_len += 4;
    
    // Send header
    if (send(obs->socket_fd, header, header_len, 0) < 0) {
        obs_set_error_flag(obs, OBS_FLAG_NETWORK_ERROR, "Failed to send WebSocket header");
        return -1;
    }
    
    // Send masked payload
    if (len > 0 && data) {
        char* masked_data = malloc(len);
        if (!masked_data) return -1;
        
        for (size_t i = 0; i < len; i++) {
            masked_data[i] = data[i] ^ mask_key[i % 4];
        }
        
        ssize_t sent = send(obs->socket_fd, masked_data, len, 0);
        free(masked_data);
        
        if (sent < 0 || (size_t)sent != len) {
            obs_set_error_flag(obs, OBS_FLAG_NETWORK_ERROR, "Failed to send WebSocket payload");
            return -1;
        }
    }
    
    return 0;
}

static int obs_read_frame(obs_websocket_t* obs, char** data, size_t* len, int* opcode) {
    if (!obs || obs->socket_fd < 0) return -1;
    
    uint8_t header[2];
    ssize_t received = recv(obs->socket_fd, header, 2, 0);
    if (received != 2) return -1;
    
    // Parse header
    uint8_t fin = (header[0] >> 7) & 1;
    *opcode = header[0] & 0x0F;
    uint8_t masked = (header[1] >> 7) & 1;
    uint64_t payload_len = header[1] & 0x7F;
    
    // Extended payload length
    if (payload_len == 126) {
        uint8_t len_bytes[2];
        if (recv(obs->socket_fd, len_bytes, 2, 0) != 2) return -1;
        payload_len = (len_bytes[0] << 8) | len_bytes[1];
    } else if (payload_len == 127) {
        uint8_t len_bytes[8];
        if (recv(obs->socket_fd, len_bytes, 8, 0) != 8) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | len_bytes[i];
        }
    }
    
    // Read mask key if present (servers shouldn't mask, but handle it)
    uint8_t mask_key[4] = {0};
    if (masked) {
        if (recv(obs->socket_fd, mask_key, 4, 0) != 4) return -1;
    }
    
    // Allocate and read payload
    *len = payload_len;
    if (payload_len > 0) {
        *data = malloc(payload_len + 1); // +1 for null terminator
        if (!*data) return -1;
        
        ssize_t total_received = 0;
        while (total_received < (ssize_t)payload_len) {
            ssize_t chunk = recv(obs->socket_fd, *data + total_received, 
                                payload_len - total_received, 0);
            if (chunk <= 0) {
                free(*data);
                *data = NULL;
                return -1;
            }
            total_received += chunk;
        }
        
        // Unmask if necessary
        if (masked) {
            for (size_t i = 0; i < payload_len; i++) {
                (*data)[i] ^= mask_key[i % 4];
            }
        }
        
        (*data)[payload_len] = '\0'; // Null terminate for text frames
    } else {
        *data = NULL;
    }
    
    return fin ? 0 : 1; // 0 = final frame, 1 = more fragments
}

static void obs_process_message(obs_websocket_t* obs, const char* message) {
    if (!obs || !message) return;
    
    cJSON* json = cJSON_Parse(message);
    if (!json) {
        obs_set_error_flag(obs, OBS_FLAG_PROTOCOL_ERROR, "Invalid JSON received");
        return;
    }
    
    cJSON* op_item = cJSON_GetObjectItem(json, "op");
    if (!op_item || !cJSON_IsNumber(op_item)) {
        cJSON_Delete(json);
        return;
    }
    
    int op = (int)op_item->valuedouble;
    cJSON* d_item = cJSON_GetObjectItem(json, "d");
    
    switch (op) {
        case 0: // Hello message
            if (d_item && cJSON_IsObject(d_item)) {
                cJSON* auth_item = cJSON_GetObjectItem(d_item, "authentication");
                if (auth_item && cJSON_IsObject(auth_item)) {
                    // Extract challenge and salt for authentication
                    cJSON* challenge = cJSON_GetObjectItem(auth_item, "challenge");
                    cJSON* salt = cJSON_GetObjectItem(auth_item, "salt");
                    
                    if (challenge && cJSON_IsString(challenge) && 
                        salt && cJSON_IsString(salt)) {
                        strncpy(obs->challenge, challenge->valuestring, sizeof(obs->challenge) - 1);
                        strncpy(obs->salt, salt->valuestring, sizeof(obs->salt) - 1);
                        obs->auth_required = true;
                    }
                }
            }
            break;
            
        case 2: // Identified message
            obs->retry_count = 0; // Reset retry count on successful connection
            obs_clear_error_flag(obs, OBS_FLAG_AUTH_ERROR);
            break;
            
        case 5: // Event message
            if (d_item && cJSON_IsObject(d_item)) {
                cJSON* event_type = cJSON_GetObjectItem(d_item, "eventType");
                cJSON* event_data = cJSON_GetObjectItem(d_item, "eventData");
                
                if (event_type && cJSON_IsString(event_type)) {
                    if (strcmp(event_type->valuestring, "CurrentProgramSceneChanged") == 0 &&
                        event_data && cJSON_IsObject(event_data)) {
                        
                        cJSON* scene_name = cJSON_GetObjectItem(event_data, "sceneName");
                        if (scene_name && cJSON_IsString(scene_name)) {
                            pthread_mutex_lock(&obs->state_mutex);
                            strncpy(obs->current_scene, scene_name->valuestring, 
                                   OBS_MAX_SCENE_NAME_LENGTH - 1);
                            obs->current_scene[OBS_MAX_SCENE_NAME_LENGTH - 1] = '\0';
                            
                            if (obs->scene_callback) {
                                obs->scene_callback(obs->current_scene, obs->callback_user_data);
                            }
                            pthread_mutex_unlock(&obs->state_mutex);
                        }
                    }
                }
            }
            break;
            
        case 7: // RequestResponse message
            if (d_item && cJSON_IsObject(d_item)) {
                cJSON* request_status = cJSON_GetObjectItem(d_item, "requestStatus");
                if (request_status && cJSON_IsObject(request_status)) {
                    cJSON* result = cJSON_GetObjectItem(request_status, "result");
                    if (result && cJSON_IsBool(result) && !cJSON_IsTrue(result)) {
                        cJSON* comment = cJSON_GetObjectItem(request_status, "comment");
                        const char* error_msg = (comment && cJSON_IsString(comment)) ? 
                                               comment->valuestring : "Request failed";
                        obs_set_error_flag(obs, OBS_FLAG_PROTOCOL_ERROR, error_msg);
                    }
                }
            }
            break;
    }
    
    cJSON_Delete(json);
}

static void obs_process_command_queue(obs_websocket_t* obs) {
    pthread_mutex_lock(&obs->command_mutex);
    
    // Process commands by priority (highest first)
    for (int priority = OBS_PRIORITY_CRITICAL; priority >= OBS_PRIORITY_LOW; priority--) {
        obs_command_t* cmd = obs->command_queue_head[priority];
        if (!cmd) continue;
        
        // Remove from queue
        obs->command_queue_head[priority] = cmd->next;
        obs->command_queue_count--;
        
        pthread_mutex_unlock(&obs->command_mutex);
        
        // Process command
        if (cmd->type == OBS_CMD_SWITCH_SCENE) {
            cJSON* request = cJSON_CreateObject();
            cJSON* d = cJSON_CreateObject();
            cJSON* request_data = cJSON_CreateObject();
            
            if (request && d && request_data) {
                cJSON_AddNumberToObject(request, "op", 6);
                cJSON_AddStringToObject(d, "requestType", "SetCurrentProgramScene");
                cJSON_AddStringToObject(d, "requestId", cmd->request_id);
                cJSON_AddStringToObject(request_data, "sceneName", cmd->scene_name);
                cJSON_AddItemToObject(d, "requestData", request_data);
                cJSON_AddItemToObject(request, "d", d);
                
                char* json_string = cJSON_Print(request);
                if (json_string) {
                    obs_send_frame(obs, json_string, strlen(json_string), WS_OPCODE_TEXT);
                    free(json_string);
                    
                    // Update statistics
                    obs->stats.messages_sent++;
                    obs->stats.scene_switches++;
                }
            }
            
            cJSON_Delete(request);
        }
        
        free(cmd);
        pthread_mutex_lock(&obs->command_mutex);
        break; // Process one command per call to avoid blocking
    }
    
    pthread_mutex_unlock(&obs->command_mutex);
}

static int obs_authenticate(obs_websocket_t* obs) {
    if (!obs->auth_required) return 0;
    
    // Generate authentication response using SHA256
    unsigned char secret_hash[SHA256_DIGEST_LENGTH];
    unsigned char auth_hash[SHA256_DIGEST_LENGTH];
    
    // Step 1: secret = SHA256(password + salt)
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, obs->config.password, strlen(obs->config.password));
    SHA256_Update(&ctx, obs->salt, strlen(obs->salt));
    SHA256_Final(secret_hash, &ctx);
    
    // Step 2: auth = SHA256(base64(secret) + challenge)
    char* secret_b64 = NULL;
    obs_base64_encode(secret_hash, SHA256_DIGEST_LENGTH, &secret_b64);
    if (!secret_b64) return -1;
    
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, secret_b64, strlen(secret_b64));
    SHA256_Update(&ctx, obs->challenge, strlen(obs->challenge));
    SHA256_Final(auth_hash, &ctx);
    
    char* auth_b64 = NULL;
    obs_base64_encode(auth_hash, SHA256_DIGEST_LENGTH, &auth_b64);
    free(secret_b64);
    
    if (!auth_b64) return -1;
    
    // Send identify message with authentication
    cJSON* identify = cJSON_CreateObject();
    cJSON* d = cJSON_CreateObject();
    
    if (identify && d) {
        cJSON_AddNumberToObject(identify, "op", 1);
        cJSON_AddNumberToObject(d, "rpcVersion", 1);
        cJSON_AddStringToObject(d, "authentication", auth_b64);
        cJSON_AddNumberToObject(d, "eventSubscriptions", 33); // Scene events
        cJSON_AddItemToObject(identify, "d", d);
        
        char* json_string = cJSON_Print(identify);
        if (json_string) {
            int result = obs_send_frame(obs, json_string, strlen(json_string), WS_OPCODE_TEXT);
            free(json_string);
            free(auth_b64);
            cJSON_Delete(identify);
            return result;
        }
    }
    
    free(auth_b64);
    cJSON_Delete(identify);
    return -1;
}

static int obs_send_ping(obs_websocket_t* obs) {
    if (!obs || obs->socket_fd < 0) return -1;
    
    obs->last_ping_sent = time(NULL);
    return obs_send_frame(obs, NULL, 0, WS_OPCODE_PING);
}

// Utility functions
const char* obs_websocket_state_to_string(obs_connection_state_t state) {
    switch (state) {
        case OBS_STATE_UNINITIALIZED: return "Uninitialized";
        case OBS_STATE_DISCONNECTED: return "Disconnected";
        case OBS_STATE_CONNECTING: return "Connecting";
        case OBS_STATE_AUTHENTICATING: return "Authenticating";
        case OBS_STATE_CONNECTED: return "Connected";
        case OBS_STATE_ERROR: return "Error";
        case OBS_STATE_RECONNECTING: return "Reconnecting";
        case OBS_STATE_SHUTTING_DOWN: return "Shutting Down";
        default: return "Unknown";
    }
}

const char* obs_websocket_flags_to_string(uint32_t flags, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) return NULL;
    
    buffer[0] = '\0';
    bool first = true;
    
    if (flags & OBS_FLAG_DAEMON_READY) {
        strcat(buffer, first ? "DAEMON_READY" : "|DAEMON_READY");
        first = false;
    }
    if (flags & OBS_FLAG_SOCKET_CONNECTED) {
        strcat(buffer, first ? "SOCKET_CONNECTED" : "|SOCKET_CONNECTED");
        first = false;
    }
    if (flags & OBS_FLAG_WEBSOCKET_READY) {
        strcat(buffer, first ? "WEBSOCKET_READY" : "|WEBSOCKET_READY");
        first = false;
    }
    if (flags & OBS_FLAG_AUTHENTICATED) {
        strcat(buffer, first ? "AUTHENTICATED" : "|AUTHENTICATED");
        first = false;
    }
    if (flags & OBS_FLAG_KEEPALIVE_OK) {
        strcat(buffer, first ? "KEEPALIVE_OK" : "|KEEPALIVE_OK");
        first = false;
    }
    if (flags & OBS_FLAG_SCENE_CACHE_VALID) {
        strcat(buffer, first ? "SCENE_CACHE_VALID" : "|SCENE_CACHE_VALID");
        first = false;
    }
    if (flags & OBS_FLAG_COMMAND_QUEUE_OK) {
        strcat(buffer, first ? "COMMAND_QUEUE_OK" : "|COMMAND_QUEUE_OK");
        first = false;
    }
    
    // Error flags
    if (flags & OBS_FLAG_NETWORK_ERROR) {
        strcat(buffer, first ? "NETWORK_ERROR" : "|NETWORK_ERROR");
        first = false;
    }
    if (flags & OBS_FLAG_AUTH_ERROR) {
        strcat(buffer, first ? "AUTH_ERROR" : "|AUTH_ERROR");
        first = false;
    }
    if (flags & OBS_FLAG_PROTOCOL_ERROR) {
        strcat(buffer, first ? "PROTOCOL_ERROR" : "|PROTOCOL_ERROR");
        first = false;
    }
    if (flags & OBS_FLAG_TIMEOUT_ERROR) {
        strcat(buffer, first ? "TIMEOUT_ERROR" : "|TIMEOUT_ERROR");
        first = false;
    }
    if (flags & OBS_FLAG_QUEUE_FULL) {
        strcat(buffer, first ? "QUEUE_FULL" : "|QUEUE_FULL");
        first = false;
    }
    if (flags & OBS_FLAG_MEMORY_ERROR) {
        strcat(buffer, first ? "MEMORY_ERROR" : "|MEMORY_ERROR");
        first = false;
    }
    if (flags & OBS_FLAG_CONFIG_ERROR) {
        strcat(buffer, first ? "CONFIG_ERROR" : "|CONFIG_ERROR");
        first = false;
    }
    if (flags & OBS_FLAG_SHUTDOWN_ERROR) {
        strcat(buffer, first ? "SHUTDOWN_ERROR" : "|SHUTDOWN_ERROR");
        first = false;
    }
    
    if (first) {
        strcpy(buffer, "NONE");
    }
    
    return buffer;
}
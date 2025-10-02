#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <ctype.h>

#include <cjson/cJSON.h>

/* Explicit declaration of environ */
extern char **environ;

/* Configuration */
static const char *CAMERAS_CONFIG = "/etc/roc/cameras.json";
static const char *DISCOVERY_CACHE = "/var/lib/roc/camera_discovery.json";
static const char *LOG_DIR = "/var/log/cameras";
static const char *ERROR_LOG = "/var/log/ffmpeg_errors.log";
static const char *LOG_FILE = "/var/log/videopipe.log";
static const char *STREAM_TYPES[] = {"main", "ext", "sub"};
static const size_t STREAM_TYPES_COUNT = 3;
static const int CACHE_TTL_SECONDS = 14 * 24 * 60 * 60;
static const int TEST_TIMEOUT = 15;
static const int MAX_CAMERAS = 16; // Match Python's 16 camera limit
static const int VIDEO_DEVICE_OFFSET = 10; // Start from /dev/video10
static volatile sig_atomic_t exit_flag = 0;

/* Logging */
static FILE *logf = NULL;

static void log_open(void) {
    if (logf) {
        fprintf(stderr, "[DEBUG] Log file already open\n");
        return;
    }
    fprintf(stderr, "[DEBUG] Attempting to open log file %s\n", LOG_FILE);
    if (access(LOG_DIR, F_OK) != 0) {
        fprintf(stderr, "[DEBUG] Creating log directory %s\n", LOG_DIR);
        if (mkdir(LOG_DIR, 0755) != 0) {
            fprintf(stderr, "[ERROR] Failed to create %s: %s\n", LOG_DIR, strerror(errno));
        }
    }
    logf = fopen(LOG_FILE, "a");
    if (!logf) {
        fprintf(stderr, "[ERROR] Failed to open %s: %s, using stderr\n", LOG_FILE, strerror(errno));
        logf = stderr;
    }
    setvbuf(logf, NULL, _IOLBF, 0);
    fprintf(stderr, "[DEBUG] Log file opened successfully\n");
}

static void log_msg(const char *lvl, const char *fmt, ...) {
    FILE *out = logf ? logf : stderr;
    va_list ap; va_start(ap, fmt);
    char tbuf[64]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(out, "%s - %s - ", tbuf, lvl);
    vfprintf(out, fmt, ap);
    fprintf(out, "\n");
    if (out != stderr) fflush(out);
    va_end(ap);
}

static void handle_signal(int sig) { 
    log_msg("INFO", "Signal %d received, setting exit_flag", sig); 
    exit_flag = 1; 
}

/* Camera structures */
#define IP_MAX 128
#define USER_MAX 64
#define PASS_MAX 128
#define RES_MAX 64

struct camera_cfg { char ip[IP_MAX]; char user[USER_MAX]; char password[PASS_MAX]; };

struct discovery_entry { char ip[IP_MAX]; char best_stream[32]; char resolution[RES_MAX]; double fps; double score; time_t last_success; };

struct running_proc { pid_t pid; int cam_index; int stream_index; int alive; };

/* Safe strncpy */
static void safe_strncpy(char *dst, const char *src, size_t n) { 
    if (!dst) return; 
    if (!src) { if (n) dst[0] = '\0'; return; } 
    strncpy(dst, src, n-1); 
    dst[n-1] = '\0'; 
}

/* Check if a specific video device exists */
static int device_exists(int index) { 
    char name[64]; 
    snprintf(name, sizeof(name), "/dev/video%d", index + VIDEO_DEVICE_OFFSET); 
    int exists = access(name, F_OK) == 0;
    log_msg("DEBUG", "Checking device %s: %s", name, exists ? "exists" : "missing");
    return exists; 
}

/* List available video devices in /dev */
static int list_video_devices(int *video_indices, size_t *count) {
    log_msg("DEBUG", "Listing video devices in /dev");
    DIR *dir = opendir("/dev");
    if (!dir) {
        log_msg("ERROR", "Failed to open /dev: %s", strerror(errno));
        return -1;
    }
    size_t idx = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && idx < MAX_CAMERAS) {
        if (strncmp(entry->d_name, "video", 5) == 0 && strlen(entry->d_name) > 5) {
            int num = atoi(entry->d_name + 5);
            if (num >= VIDEO_DEVICE_OFFSET && num <= VIDEO_DEVICE_OFFSET + MAX_CAMERAS - 1) {
                video_indices[idx++] = num;
                log_msg("DEBUG", "Found video device /dev/video%d", num);
            }
        }
    }
    closedir(dir);
    *count = idx;
    log_msg("INFO", "Found %zu video devices", *count);
    return 0;
}

/* JSON-based config loader (cJSON) */
static int load_cameras_json(struct camera_cfg *cams, size_t *count) {
    log_msg("DEBUG", "Loading camera config from %s", CAMERAS_CONFIG);
    if (!cams || !count) {
        log_msg("ERROR", "Invalid arguments to load_cameras_json");
        return -1;
    }
    FILE *f = fopen(CAMERAS_CONFIG, "r"); 
    if (!f) { 
        log_msg("ERROR", "open %s: %s", CAMERAS_CONFIG, strerror(errno)); 
        return -1; 
    }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 10 * 1024 * 1024) { 
        fclose(f); 
        log_msg("ERROR", "Invalid config length %ld", len); 
        return -1; 
    }
    char *buf = malloc((size_t)len + 1); 
    if (!buf) { 
        fclose(f); 
        log_msg("ERROR", "malloc failed for config buffer"); 
        return -1; 
    }
    fread(buf, 1, (size_t)len, f); buf[len] = '\0'; fclose(f);
    log_msg("DEBUG", "Read %ld bytes from %s", len, CAMERAS_CONFIG);
    // Strip UTF-8 BOM if present
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        log_msg("DEBUG", "Stripping UTF-8 BOM from config");
        memmove(buf, buf + 3, len - 3 + 1);
        len -= 3;
    }
    cJSON *root = cJSON_Parse(buf); free(buf);
    if (!root || !cJSON_IsArray(root)) { 
        log_msg("ERROR", "cameras.json root not array"); 
        if (root) cJSON_Delete(root); 
        return -1; 
    }
    size_t idx = 0; 
    cJSON *item = NULL; 
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsObject(item)) {
            log_msg("WARNING", "Skipping non-object entry in cameras.json");
            continue;
        }
        cJSON *cip = cJSON_GetObjectItemCaseSensitive(item, "ip");
        cJSON *cpass = cJSON_GetObjectItemCaseSensitive(item, "password");
        if (!cJSON_IsString(cip) || !cJSON_IsString(cpass)) { 
            log_msg("WARNING", "Camera entry missing ip/password, skipping"); 
            continue; 
        }
        cJSON *cuser = cJSON_GetObjectItemCaseSensitive(item, "user");
        safe_strncpy(cams[idx].ip, cip->valuestring, IP_MAX);
        safe_strncpy(cams[idx].password, cpass->valuestring, PASS_MAX);
        if (cuser && cJSON_IsString(cuser)) 
            safe_strncpy(cams[idx].user, cuser->valuestring, USER_MAX); 
        else 
            safe_strncpy(cams[idx].user, "admin", USER_MAX);
        log_msg("DEBUG", "Parsed camera %zu: ip=%s, user=%s", idx, cams[idx].ip, cams[idx].user);
        idx++; 
        if (idx >= MAX_CAMERAS) {
            log_msg("WARNING", "Reached MAX_CAMERAS limit (%d)", MAX_CAMERAS);
            break;
        }
    }
    cJSON_Delete(root); 
    if (idx == 0) { 
        log_msg("ERROR", "No cameras parsed from config"); 
        return -1; 
    }
    *count = idx; 
    log_msg("INFO", "Loaded %zu cameras", *count);
    return 0;
}

/* JSON cache loader/saver */
static int load_cache_json(struct discovery_entry *entries, size_t *cnt) {
    log_msg("DEBUG", "Loading cache from %s", DISCOVERY_CACHE);
    *cnt = 0; 
    FILE *f = fopen(DISCOVERY_CACHE, "r"); 
    if (!f) {
        log_msg("INFO", "Cache file %s not found, starting fresh", DISCOVERY_CACHE); 
        return 0; 
    }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 20 * 1024 * 1024) { 
        fclose(f); 
        log_msg("ERROR", "Invalid cache length %ld", len); 
        return 0; 
    }
    char *buf = malloc((size_t)len + 1); 
    if (!buf) { 
        fclose(f); 
        log_msg("ERROR", "malloc failed for cache buffer"); 
        return 0; 
    }
    fread(buf, 1, (size_t)len, f); buf[len] = '\0'; fclose(f);
    log_msg("DEBUG", "Read %ld bytes from %s", len, DISCOVERY_CACHE);
    // Strip UTF-8 BOM if present
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        log_msg("DEBUG", "Stripping UTF-8 BOM from cache");
        memmove(buf, buf + 3, len - 3 + 1);
        len -= 3;
    }
    cJSON *root = cJSON_Parse(buf); free(buf);
    if (!root || !cJSON_IsArray(root)) { 
        log_msg("WARNING", "Cache root not array, ignoring"); 
        if (root) cJSON_Delete(root); 
        return 0; 
    }
    size_t idx = 0; 
    cJSON *it = NULL; 
    cJSON_ArrayForEach(it, root) {
        if (!cJSON_IsObject(it)) {
            log_msg("WARNING", "Skipping non-object entry in cache");
            continue;
        }
        cJSON *cip = cJSON_GetObjectItemCaseSensitive(it, "ip"); 
        if (!cJSON_IsString(cip)) {
            log_msg("WARNING", "Cache entry missing ip, skipping");
            continue;
        }
        safe_strncpy(entries[idx].ip, cip->valuestring, IP_MAX);
        cJSON *cstream = cJSON_GetObjectItemCaseSensitive(it, "stream"); 
        if (cstream && cJSON_IsString(cstream)) 
            safe_strncpy(entries[idx].best_stream, cstream->valuestring, sizeof(entries[idx].best_stream));
        cJSON *cres = cJSON_GetObjectItemCaseSensitive(it, "resolution"); 
        if (cres && cJSON_IsString(cres)) 
            safe_strncpy(entries[idx].resolution, cres->valuestring, RES_MAX);
        cJSON *cfps = cJSON_GetObjectItemCaseSensitive(it, "fps"); 
        if (cfps && cJSON_IsNumber(cfps)) 
            entries[idx].fps = cfps->valuedouble; 
        else 
            entries[idx].fps = 0.0;
        cJSON *cscore = cJSON_GetObjectItemCaseSensitive(it, "score"); 
        if (cscore && cJSON_IsNumber(cscore)) 
            entries[idx].score = cscore->valuedouble; 
        else 
            entries[idx].score = 0.0;
        cJSON *cl = cJSON_GetObjectItemCaseSensitive(it, "last"); 
        if (cl && cJSON_IsNumber(cl)) 
            entries[idx].last_success = (time_t)cl->valuedouble; 
        else 
            entries[idx].last_success = 0;
        log_msg("DEBUG", "Parsed cache entry %zu: ip=%s, stream=%s, resolution=%s, fps=%.2f, score=%.2f, last=%ld",
                idx, entries[idx].ip, entries[idx].best_stream, entries[idx].resolution, 
                entries[idx].fps, entries[idx].score, entries[idx].last_success);
        idx++; 
        if (idx >= MAX_CAMERAS) {
            log_msg("WARNING", "Reached MAX_CAMERAS limit (%d) for cache", MAX_CAMERAS);
            break;
        }
    }
    cJSON_Delete(root); 
    *cnt = idx; 
    log_msg("INFO", "Loaded %zu cache entries", *cnt);
    return 0;
}

static int save_cache_json(struct discovery_entry *entries, size_t cnt) {
    log_msg("DEBUG", "Saving cache to %s", DISCOVERY_CACHE);
    // Ensure /var/lib/roc exists
    char cache_dir[256];
    snprintf(cache_dir, sizeof(cache_dir), "%s", DISCOVERY_CACHE);
    char *last_slash = strrchr(cache_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (access(cache_dir, F_OK) != 0) {
            log_msg("DEBUG", "Creating cache directory %s", cache_dir);
            if (mkdir(cache_dir, 0755) != 0) {
                log_msg("ERROR", "Failed to create %s: %s", cache_dir, strerror(errno));
                return -1;
            }
        }
    }
    cJSON *root = cJSON_CreateArray(); 
    if (!root) {
        log_msg("ERROR", "Failed to create JSON array for cache");
        return -1;
    }
    for (size_t i = 0; i < cnt; ++i) {
        cJSON *o = cJSON_CreateObject(); 
        if (!o) { 
            log_msg("ERROR", "Failed to create JSON object for cache entry %zu", i);
            cJSON_Delete(root); 
            return -1; 
        }
        cJSON_AddStringToObject(o, "ip", entries[i].ip);
        cJSON_AddStringToObject(o, "stream", entries[i].best_stream);
        cJSON_AddStringToObject(o, "resolution", entries[i].resolution);
        cJSON_AddNumberToObject(o, "fps", entries[i].fps);
        cJSON_AddNumberToObject(o, "score", entries[i].score);
        cJSON_AddNumberToObject(o, "last", (double)entries[i].last_success);
        cJSON_AddItemToArray(root, o);
    }
    char *s = cJSON_PrintUnformatted(root);
    if (!s) { 
        log_msg("ERROR", "Failed to serialize cache JSON");
        cJSON_Delete(root); 
        return -1; 
    }
    char tmp[1024]; 
    snprintf(tmp, sizeof(tmp), "%s.tmp", DISCOVERY_CACHE);
    FILE *f = fopen(tmp, "w"); 
    if (!f) { 
        log_msg("ERROR", "open %s: %s", tmp, strerror(errno)); 
        cJSON_free(s); 
        cJSON_Delete(root); 
        return -1; 
    }
    fwrite(s, 1, strlen(s), f); 
    fflush(f); 
    fsync(fileno(f)); 
    fclose(f);
    log_msg("DEBUG", "Wrote cache to %s", tmp);
    if (rename(tmp, DISCOVERY_CACHE) != 0) { 
        log_msg("ERROR", "rename %s to %s: %s", tmp, DISCOVERY_CACHE, strerror(errno)); 
        unlink(tmp); 
        cJSON_free(s); 
        cJSON_Delete(root); 
        return -1; 
    }
    log_msg("DEBUG", "Renamed %s to %s", tmp, DISCOVERY_CACHE);
    cJSON_free(s); 
    cJSON_Delete(root); 
    log_msg("INFO", "Saved %zu cache entries", cnt);
    return 0;
}

/* Network helper - test TCP connection to port 1935 */
static int test_tcp_connect(const char *ip, int port, int timeout_sec) {
    log_msg("DEBUG", "Testing TCP connection to %s:%d with timeout %d sec", ip, port, timeout_sec);
    if (!ip) {
        log_msg("ERROR", "Null IP in test_tcp_connect");
        return 0;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log_msg("ERROR", "socket creation failed: %s", strerror(errno));
        return 0;
    }
    struct sockaddr_in addr; 
    memset(&addr, 0, sizeof(addr)); 
    addr.sin_family = AF_INET; 
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { 
        log_msg("ERROR", "Invalid IP address %s", ip);
        close(sock); 
        return 0; 
    }
    int flags = fcntl(sock, F_GETFL, 0); 
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int r = connect(sock, (struct sockaddr *)&addr, sizeof(addr)); 
    if (r == 0) { 
        log_msg("DEBUG", "Immediate connection success to %s:%d", ip, port);
        close(sock); 
        return 1; 
    }
    if (errno != EINPROGRESS) { 
        log_msg("ERROR", "connect to %s:%d failed: %s", ip, port, strerror(errno));
        close(sock); 
        return 0; 
    }
    fd_set wf; 
    FD_ZERO(&wf); 
    FD_SET(sock, &wf); 
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    r = select(sock + 1, NULL, &wf, NULL, &tv);
    if (r > 0 && FD_ISSET(sock, &wf)) { 
        int so = 0; 
        socklen_t sl = sizeof(so); 
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so, &sl); 
        close(sock); 
        if (so == 0) {
            log_msg("DEBUG", "Connection successful to %s:%d", ip, port);
            return 1;
        } else {
            log_msg("ERROR", "Connection to %s:%d failed: %s", ip, port, strerror(so));
            return 0;
        }
    }
    log_msg("ERROR", "Connection to %s:%d timed out", ip, port);
    close(sock); 
    return 0;
}

/* Probe stream using ffmpeg and parse output */
static int probe_stream(const char *ip, const char *user, const char *password, const char *stream_type,
                        int stream_num, int timeout_sec, char *out_res, size_t res_len, double *out_fps, double *out_score) {
    log_msg("DEBUG", "Probing stream for %s, type=%s, stream_num=%d", ip, stream_type, stream_num);
    if (!ip || !stream_type || !out_res || !out_fps || !out_score) {
        log_msg("ERROR", "Invalid arguments to probe_stream");
        return 0;
    }
    char rtmp[512]; 
    snprintf(rtmp, sizeof(rtmp), "rtmp://%s/bcs/channel0_%s.bcs?channel=0&stream=%d&user=%s&password=%s",
             ip, stream_type, stream_num, user ? user : "admin", password ? password : "");
    log_msg("DEBUG", "RTMP URL: %s", rtmp);
    char cmd[1024]; 
    snprintf(cmd, sizeof(cmd), "ffmpeg -hide_banner -nostdin -rtmp_live live -fflags nobuffer -flags low_delay -re -i '%s' -t 5 -f null - 2>&1", rtmp);
    log_msg("DEBUG", "Executing probe command: %s", cmd);
    FILE *fp = popen(cmd, "r"); 
    if (!fp) { 
        log_msg("ERROR", "popen probe failed for %s: %s", ip, strerror(errno)); 
        return 0; 
    }
    char buf[4096]; 
    char combined[128*1024]; 
    size_t pos = 0; 
    combined[0] = '\0'; 
    time_t start = time(NULL);
    while (fgets(buf, sizeof(buf), fp)) {
        size_t bl = strlen(buf);
        if (pos + bl < sizeof(combined) - 1) { 
            memcpy(combined + pos, buf, bl); 
            pos += bl; 
            combined[pos] = '\0'; 
        }
        if ((int)(time(NULL) - start) > timeout_sec) {
            log_msg("WARNING", "Probe for %s timed out after %d seconds", ip, timeout_sec);
            break;
        }
    }
    int rc = pclose(fp);
    log_msg("DEBUG", "Probe command returned %d", rc);
    char res[RES_MAX] = "0x0"; 
    double fps = 0.0; 
    int dup = 0;
    /* Parse resolution, fps, dup= */
    for (size_t i = 0; i + 4 < pos; ++i) {
        if (isdigit((unsigned char)combined[i])) {
            size_t j = i; 
            while (j < pos && isdigit((unsigned char)combined[j])) j++;
            if (j < pos && combined[j] == 'x') { 
                size_t k = j + 1; 
                if (k < pos && isdigit((unsigned char)combined[k])) {
                    size_t m = i; 
                    char tmp[RES_MAX]; 
                    size_t ln = 0;
                    while (m < pos && (isdigit((unsigned char)combined[m]) || combined[m] == 'x')) {
                        if (ln + 1 < sizeof(tmp)) {
                            tmp[ln++] = combined[m];
                            m++;
                        } else {
                            log_msg("WARNING", "Resolution string too long");
                            break;
                        }
                    }
                    tmp[ln] = '\0'; 
                    safe_strncpy(res, tmp, sizeof(res)); 
                    log_msg("DEBUG", "Parsed resolution: %s", res);
                    break; 
                }
            }
        }
    }
    char *fps_p = strstr(combined, " fps"); 
    if (fps_p) { 
        const char *q = fps_p; 
        while (q > combined && (isdigit((unsigned char)*(q-1)) || *(q-1) == '.')) q--; 
        char tmp[32]; 
        size_t l = (size_t)(fps_p - q); 
        if (l < sizeof(tmp)) { 
            memcpy(tmp, q, l); 
            tmp[l] = '\0'; 
            fps = atof(tmp); 
            log_msg("DEBUG", "Parsed FPS: %.2f", fps);
        } 
    }
    char *dup_p = strstr(combined, "dup="); 
    if (dup_p) {
        dup = atoi(dup_p + 4);
        log_msg("DEBUG", "Parsed dup: %d", dup);
    }
    int w = 0, h = 0; 
    if (res[0] != '0') { 
        if (sscanf(res, "%dx%d", &w, &h) != 2) sscanf(res, "%d x %d", &w, &h); 
        log_msg("DEBUG", "Resolution dimensions: %dx%d", w, h);
    }
    double score = (double)w * (double)h * fps * (1.0 - (double)dup / 1000.0);
    if (rc == 0 && res[0] != '0') { 
        safe_strncpy(out_res, res, res_len); 
        *out_fps = fps; 
        *out_score = score; 
        log_msg("INFO", "Probe %s %s -> %s @ %.2ffps score=%.2f", ip, stream_type, out_res, *out_fps, *out_score); 
        return 1; 
    }
    log_msg("WARNING", "Probe failed for %s %s (rc=%d)", ip, stream_type, rc); 
    return 0;
}

/* Spawn optimized ffmpeg process */
static pid_t spawn_ffmpeg(int camera_index, const struct camera_cfg *cam, const char *stream_type, double fps) {
    log_msg("DEBUG", "Spawning FFmpeg for camera %d, ip=%s, stream=%s, fps=%.2f", 
            camera_index, cam->ip, stream_type, fps);
    if (!cam || !stream_type) {
        log_msg("ERROR", "Invalid arguments to spawn_ffmpeg");
        return -1;
    }
    char rtmp[512]; 
    snprintf(rtmp, sizeof(rtmp), "rtmp://%s/bcs/channel0_%s.bcs?channel=0&stream=%d&user=%s&password=%s",
             cam->ip, stream_type, (strcmp(stream_type, "sub") == 0) ? 1 : 0, 
             cam->user[0] ? cam->user : "admin", cam->password);
    log_msg("DEBUG", "FFmpeg RTMP URL: %s", rtmp);
    char devpath[64]; 
    snprintf(devpath, sizeof(devpath), "/dev/video%d", camera_index + VIDEO_DEVICE_OFFSET);
    char logfile[256]; 
    snprintf(logfile, sizeof(logfile), "%s/camera%d.log", LOG_DIR, camera_index);
    log_msg("DEBUG", "FFmpeg output device: %s, log: %s", devpath, logfile);
    pid_t pid = fork();
    if (pid < 0) { 
        log_msg("ERROR", "fork failed: %s", strerror(errno)); 
        return -1; 
    }
    if (pid == 0) {
        /* Child */
        log_msg("DEBUG", "In FFmpeg child process");
        int fd = open(logfile, O_CREAT | O_WRONLY | O_APPEND, 0644); 
        if (fd >= 0) { 
            dup2(fd, STDOUT_FILENO); 
            dup2(fd, STDERR_FILENO); 
            if (fd > STDERR_FILENO) close(fd); 
            log_msg("DEBUG", "Redirected FFmpeg output to %s", logfile);
        } else {
            log_msg("ERROR", "Failed to open %s: %s", logfile, strerror(errno));
        }
        environ = NULL; /* Clear environment */
        char fpsbuf[32]; 
        snprintf(fpsbuf, sizeof(fpsbuf), "%.2f", fps > 0.0 ? fps : 15.0);
        char vfbuf[64]; 
        snprintf(vfbuf, sizeof(vfbuf), "fps=fps=%.2f", fps > 0.0 ? fps : 15.0);
        log_msg("DEBUG", "FFmpeg args: fps=%s, vf=%s", fpsbuf, vfbuf);
        /* Build argv: tuned for low-latency */
        char *argv[32]; 
        int ai = 0;
        argv[ai++] = "ffmpeg";
        argv[ai++] = "-hide_banner";
        argv[ai++] = "-nostdin";
        argv[ai++] = "-re";
        argv[ai++] = "-rtmp_live"; argv[ai++] = "live";
        argv[ai++] = "-fflags"; argv[ai++] = "nobuffer";
        argv[ai++] = "-flags"; argv[ai++] = "low_delay";
        argv[ai++] = "-probesize"; argv[ai++] = "32";
        argv[ai++] = "-analyzeduration"; argv[ai++] = "0";
        argv[ai++] = "-i"; argv[ai++] = rtmp;
        argv[ai++] = "-vf"; argv[ai++] = vfbuf;
        argv[ai++] = "-vsync"; argv[ai++] = "1";
        argv[ai++] = "-r"; argv[ai++] = fpsbuf;
        argv[ai++] = "-pix_fmt"; argv[ai++] = "yuv420p"; // Added pixel format
        argv[ai++] = "-f"; argv[ai++] = "v4l2";
        argv[ai++] = devpath;
        argv[ai] = NULL;
        log_msg("DEBUG", "Executing FFmpeg with args: %s", argv[0]);
        execvp("ffmpeg", argv);
        log_msg("ERROR", "execvp ffmpeg failed: %s", strerror(errno));
        _exit(127);
    }
    log_msg("INFO", "Spawned FFmpeg pid=%d for camera %d (%s) -> %s", 
            (int)pid, camera_index, cam->ip, devpath);
    return pid;
}

static int find_cache_entry(struct discovery_entry *entries, size_t cnt, const char *ip) { 
    log_msg("DEBUG", "Searching cache for ip=%s", ip);
    for (size_t i = 0; i < cnt; ++i) 
        if (strcmp(entries[i].ip, ip) == 0) {
            log_msg("DEBUG", "Found cache entry for %s at index %zu", ip, i);
            return (int)i; 
        }
    log_msg("DEBUG", "No cache entry for %s", ip);
    return -1; 
}

int main(void) {
    log_open(); // Open log file at start
    log_msg("INFO", "Starting videopipe");
    signal(SIGINT, handle_signal); 
    signal(SIGTERM, handle_signal);
    
    log_msg("DEBUG", "Creating error log %s", ERROR_LOG);
    FILE *ef = fopen(ERROR_LOG, "w"); 
    if (ef) {
        fclose(ef);
        log_msg("DEBUG", "Created error log %s", ERROR_LOG);
    } else {
        log_msg("ERROR", "Failed to create %s: %s", ERROR_LOG, strerror(errno));
    }

    // Verify v4l2loopback devices
    int video_indices[MAX_CAMERAS];
    size_t video_count = 0;
    if (list_video_devices(video_indices, &video_count) != 0 || video_count == 0) {
        log_msg("ERROR", "No v4l2loopback devices found in /dev. Check module loading.");
        if (logf && logf != stderr) fclose(logf);
        return 1;
    }

    struct camera_cfg cams[MAX_CAMERAS]; 
    size_t cam_count = 0;
    log_msg("DEBUG", "Attempting to load camera configuration");
    if (load_cameras_json(cams, &cam_count) != 0) { 
        log_msg("ERROR", "Failed to load cameras config, exiting");
        if (logf && logf != stderr) fclose(logf);
        return 1; 
    }

    struct discovery_entry cache[MAX_CAMERAS]; 
    size_t cache_count = 0; 
    log_msg("DEBUG", "Loading discovery cache");
    load_cache_json(cache, &cache_count);

    struct running_proc procs[MAX_CAMERAS]; 
    memset(procs, 0, sizeof(procs));
    log_msg("DEBUG", "Initialized %d process slots", MAX_CAMERAS);

    /* Quick-start using cache when fresh */
    log_msg("DEBUG", "Starting camera processing loop");
    for (size_t i = 0; i < cam_count && i < MAX_CAMERAS; ++i) {
        struct camera_cfg *c = &cams[i]; 
        log_msg("DEBUG", "Processing camera %zu: ip=%s", i, c->ip);
        if (strlen(c->ip) == 0 || strlen(c->password) == 0) { 
            log_msg("ERROR", "Camera %zu missing ip/password, skipping", i); 
            continue; 
        }
        if (!device_exists((int)i)) { 
            log_msg("ERROR", "/dev/video%zu missing, skipping", i + VIDEO_DEVICE_OFFSET); 
            continue; 
        }
        int ci = find_cache_entry(cache, cache_count, c->ip);
        int used_cache = 0;
        if (ci >= 0) {
            time_t now = time(NULL);
            log_msg("DEBUG", "Cache entry found for %s: stream=%s, age=%ld seconds", 
                    c->ip, cache[ci].best_stream, now - cache[ci].last_success);
            if ((now - cache[ci].last_success) < CACHE_TTL_SECONDS) {
                log_msg("DEBUG", "Cache entry is fresh, testing connection");
                if (test_tcp_connect(c->ip, 1935, 2)) {
                    int sidx = 0; 
                    for (; sidx < (int)STREAM_TYPES_COUNT; ++sidx) 
                        if (strcmp(STREAM_TYPES[sidx], cache[ci].best_stream) == 0) break; 
                    if (sidx >= (int)STREAM_TYPES_COUNT) {
                        log_msg("WARNING", "Invalid cached stream type %s, defaulting to main", cache[ci].best_stream);
                        sidx = 0;
                    }
                    log_msg("DEBUG", "Using cached stream type %s", STREAM_TYPES[sidx]);
                    pid_t pid = spawn_ffmpeg((int)i, c, STREAM_TYPES[sidx], cache[ci].fps > 0 ? cache[ci].fps : 15.0);
                    if (pid > 0) { 
                        procs[i].pid = pid; 
                        procs[i].cam_index = (int)i; 
                        procs[i].stream_index = sidx; 
                        procs[i].alive = 1; 
                        used_cache = 1; 
                        log_msg("DEBUG", "Started FFmpeg from cache for camera %zu", i);
                    } else {
                        log_msg("ERROR", "Failed to start FFmpeg for camera %zu", i);
                    }
                } else { 
                    log_msg("WARNING", "Cached camera %s not reachable; will probe", c->ip); 
                }
            } else {
                log_msg("DEBUG", "Cache entry for %s is stale", c->ip);
            }
        }
        if (!used_cache) {
            log_msg("DEBUG", "No valid cache, probing camera %s", c->ip);
            const char *best_stream = NULL; 
            double best_score = 0.0; 
            char best_res[RES_MAX] = {0}; 
            double best_fps = 0.0;
            if (!test_tcp_connect(c->ip, 1935, 2)) { 
                log_msg("WARNING", "Camera %s unreachable on 1935; skipping probe", c->ip); 
                continue; 
            }
            for (size_t st = 0; st < STREAM_TYPES_COUNT; ++st) {
                int s_num = (strcmp(STREAM_TYPES[st], "sub") == 0) ? 1 : 0; 
                char res[RES_MAX] = {0}; 
                double fps = 0.0, score = 0.0;
                log_msg("DEBUG", "Probing stream type %s (num=%d)", STREAM_TYPES[st], s_num);
                if (probe_stream(c->ip, c->user, c->password, STREAM_TYPES[st], s_num, TEST_TIMEOUT, res, sizeof(res), &fps, &score)) { 
                    if (score > best_score) { 
                        best_score = score; 
                        best_stream = STREAM_TYPES[st]; 
                        safe_strncpy(best_res, res, sizeof(best_res)); 
                        best_fps = fps; 
                        log_msg("DEBUG", "New best stream: %s, score=%.2f", best_stream, best_score);
                    } 
                }
            }
            if (best_stream) {
                log_msg("DEBUG", "Selected best stream %s for %s", best_stream, c->ip);
                pid_t pid = spawn_ffmpeg((int)i, c, best_stream, best_fps);
                if (pid > 0) {
                    procs[i].pid = pid; 
                    procs[i].cam_index = (int)i; 
                    procs[i].alive = 1;
                    for (size_t t = 0; t < STREAM_TYPES_COUNT; ++t) 
                        if (strcmp(STREAM_TYPES[t], best_stream) == 0) procs[i].stream_index = (int)t;
                    int idx = find_cache_entry(cache, cache_count, c->ip); 
                    if (idx < 0 && cache_count < MAX_CAMERAS) idx = (int)(cache_count++);
                    safe_strncpy(cache[idx].ip, c->ip, IP_MAX); 
                    safe_strncpy(cache[idx].best_stream, best_stream, sizeof(cache[idx].best_stream)); 
                    safe_strncpy(cache[idx].resolution, best_res, RES_MAX);
                    cache[idx].fps = best_fps; 
                    cache[idx].score = best_score; 
                    cache[idx].last_success = time(NULL); 
                    log_msg("DEBUG", "Updating cache for %s: stream=%s, resolution=%s, fps=%.2f, score=%.2f",
                            c->ip, best_stream, best_res, best_fps, best_score);
                    save_cache_json(cache, cache_count);
                } else {
                    log_msg("ERROR", "Failed to start FFmpeg for %s", c->ip);
                }
            } else { 
                log_msg("ERROR", "No valid stream for camera %s", c->ip); 
            }
        }
    }

    /* Start background tail -> error log */
    log_msg("DEBUG", "Starting tail for error log");
    char tail_cmd[1024]; 
    snprintf(tail_cmd, sizeof(tail_cmd), "bash -c 'tail -n+1 -F %s/*.log 2>/dev/null | grep -iE \"error|failed|timeout|connection refused|input/output error|end of file\" >> %s &'", LOG_DIR, ERROR_LOG);
    log_msg("DEBUG", "Executing tail command: %s", tail_cmd);
    system(tail_cmd);

    /* Monitor loop: react to child exits */
    log_msg("DEBUG", "Entering monitor loop");
    time_t last_save = time(NULL);
    time_t last_probe_time = time(NULL);
    int retry_delay = 5;
    while (!exit_flag) {
        int status; 
        pid_t r = waitpid(-1, &status, WNOHANG);
        if (r > 0) {
            int which = -1; 
            for (size_t i = 0; i < cam_count && i < MAX_CAMERAS; ++i) { 
                if (procs[i].alive && procs[i].pid == r) { 
                    which = (int)i; 
                    break; 
                } 
            }
            if (which >= 0) {
                procs[which].alive = 0; 
                log_msg("WARNING", "FFmpeg for camera %d (%s) exited with status=%d", 
                        which, cams[which].ip, WEXITSTATUS(status));
                /* Try to recover with fallback probes */
                int retry = 0; 
                const int max_retry = 12; 
                const char *chosen = NULL; 
                double chosen_fps = 0.0; 
                char chosen_res[RES_MAX] = {0}; 
                double chosen_score = 0.0;
                log_msg("DEBUG", "Attempting recovery for camera %d", which);
                while (!exit_flag && retry < max_retry) {
                    if (!device_exists(which)) { 
                        log_msg("ERROR", "/dev/video%d missing, aborting restart", which + VIDEO_DEVICE_OFFSET); 
                        break; 
                    }
                    if (!test_tcp_connect(cams[which].ip, 1935, 2)) { 
                        log_msg("WARNING", "Camera %s unreachable, retry %d/%d, delaying %ds", 
                                cams[which].ip, retry + 1, max_retry, retry_delay); 
                        sleep(retry_delay); 
                        retry_delay = (int)(retry_delay * 1.5) > 30 ? 30 : (int)(retry_delay * 1.5);
                        retry++; 
                        continue; 
                    }
                    for (size_t st = 0; st < STREAM_TYPES_COUNT; ++st) {
                        char res[RES_MAX] = {0}; 
                        double fps = 0.0, score = 0.0; 
                        int sn = (strcmp(STREAM_TYPES[st], "sub") == 0) ? 1 : 0;
                        log_msg("DEBUG", "Retrying probe for %s stream type %s", 
                                cams[which].ip, STREAM_TYPES[st]);
                        if (probe_stream(cams[which].ip, cams[which].user, cams[which].password, 
                                         STREAM_TYPES[st], sn, TEST_TIMEOUT, res, sizeof(res), &fps, &score)) {
                            if (score > chosen_score) { 
                                chosen_score = score; 
                                chosen = STREAM_TYPES[st]; 
                                chosen_fps = fps; 
                                safe_strncpy(chosen_res, res, sizeof(chosen_res)); 
                                log_msg("DEBUG", "New best recovery stream: %s, score=%.2f", 
                                        chosen, chosen_score);
                            }
                        }
                    }
                    if (chosen) {
                        log_msg("DEBUG", "Recovery selected stream %s", chosen);
                        break;
                    }
                    retry++;
                    retry_delay = (int)(retry_delay * 1.5) > 30 ? 30 : (int)(retry_delay * 1.5);
                    sleep(retry_delay);
                }
                if (chosen) {
                    log_msg("DEBUG", "Restarting FFmpeg with stream %s", chosen);
                    pid_t pid = spawn_ffmpeg(which, &cams[which], chosen, chosen_fps);
                    if (pid > 0) { 
                        procs[which].pid = pid; 
                        procs[which].alive = 1; 
                        for (size_t t = 0; t < STREAM_TYPES_COUNT; ++t) 
                            if (strcmp(STREAM_TYPES[t], chosen) == 0) procs[which].stream_index = (int)t;
                        int ci = find_cache_entry(cache, cache_count, cams[which].ip); 
                        if (ci < 0 && cache_count < MAX_CAMERAS) ci = (int)(cache_count++);
                        safe_strncpy(cache[ci].ip, cams[which].ip, IP_MAX); 
                        safe_strncpy(cache[ci].best_stream, chosen, sizeof(cache[ci].best_stream)); 
                        safe_strncpy(cache[ci].resolution, chosen_res, RES_MAX);
                        cache[ci].fps = chosen_fps; 
                        cache[ci].score = chosen_score; 
                        cache[ci].last_success = time(NULL); 
                        log_msg("DEBUG", "Updated cache for %s after recovery", cams[which].ip);
                        save_cache_json(cache, cache_count);
                        retry_delay = 5; // Reset delay
                    } else {
                        log_msg("ERROR", "Failed to restart FFmpeg for %s", cams[which].ip);
                    }
                } else { 
                    log_msg("ERROR", "Could not recover camera %d (%s)", which, cams[which].ip); 
                }
            }
        }
        time_t now = time(NULL); 
        if (now - last_save > 60) { 
            log_msg("DEBUG", "Periodic cache save");
            save_cache_json(cache, cache_count); 
            last_save = now; 
        }
        if (now - last_probe_time > 60) { 
            for (size_t i = 0; i < cam_count && i < MAX_CAMERAS; ++i) {
                if (procs[i].alive) {
                    log_msg("DEBUG", "Active probe check for camera %zu: ip=%s", i, cams[i].ip);
                    if (!test_tcp_connect(cams[i].ip, 1935, 2)) {
                        log_msg("WARNING", "Active probe failed for camera %zu (%s), killing FFmpeg pid=%d to trigger recovery", 
                                i, cams[i].ip, (int)procs[i].pid);
                        kill(procs[i].pid, SIGTERM);
                    }
                }
            }
            last_probe_time = now;
        }
        log_msg("DEBUG", "Monitor loop iteration, exit_flag=%d", exit_flag);
        sleep(1);
    }

    log_msg("INFO", "Shutting down, terminating children");
    for (size_t i = 0; i < cam_count && i < MAX_CAMERAS; ++i) 
        if (procs[i].alive && procs[i].pid > 0) { 
            log_msg("DEBUG", "Terminating FFmpeg pid=%d for camera %d", 
                    (int)procs[i].pid, (int)i);
            kill(procs[i].pid, SIGTERM); 
            waitpid(procs[i].pid, NULL, 0); 
        }
    log_msg("DEBUG", "Saving final cache");
    save_cache_json(cache, cache_count);
    log_msg("DEBUG", "Closing log file");
    if (logf && logf != stderr) fclose(logf);
    log_msg("INFO", "Exiting videopipe");
    return 0;
}
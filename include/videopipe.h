/*
 * videopipe.h
 * --------------------------------------------------------------
 * Public header for the FFmpeg-to-V4L2 device attachment system.
 *
 * This module manages camera stream discovery, persistent caching,
 * and attachment of IP camera RTSP/HTTP feeds to virtual /dev/video*
 * devices via FFmpeg and v4l2loopback.
 *
 * Key features:
 *  - Uses JSON (cJSON) for configuration and cache persistence.
 *  - Supports long-lived discovery cache (default TTL: 14 days).
 *  - Optimizes FFmpeg parameters for low latency and high resolution.
 *  - Avoids redundant re-checking of stable cameras.
 *  - Only re-probes problematic cameras when needed.
 *  - Designed for clean integration with systemd.
 *
 * Author: Aidan A. Bradley
 * Date:   10/01/2025
 */

#ifndef VIDEOPIPE_H
#define VIDEOPIPE_H

#include <time.h>
#include <cjson/cJSON.h>

// ============================================================================
// CONFIGURATION CONSTANTS
// ============================================================================

/**
 * Path to JSON configuration file listing cameras.
 * Example schema:
 * {
 *   "cameras": [
 *     { "id": "cam1", "url": "rtsp://...", "device": 10, "fps": 30 },
 *     { "id": "cam2", "url": "rtsp://...", "device": 11, "fps": 25 }
 *   ]
 * }
 */
#define CAMERA_CONFIG_FILE "/etc/roc/cameras.json"

/**
 * Path to persistent discovery cache.
 * Contains per-camera info including last known good stream,
 * quality metrics, and last successful probe timestamp.
 */
#define DISCOVERY_CACHE_FILE "/var/lib/roc/camera_discovery.json"

/**
 * Default cache time-to-live (TTL) in seconds.
 * Current value: 14 days.
 */
#define CACHE_TTL_SECONDS (14 * 24 * 60 * 60)

/**
 * Max FFmpeg arguments constructed for a single camera.
 */
#define MAX_FFMPEG_ARGS 64

/**
 * Max string buffer sizes for paths, URLs, and IDs.
 */
#define MAX_STR_LEN 512

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * Camera configuration as read from cameras.json.
 */
typedef struct {
    char id[MAX_STR_LEN];   ///< Unique camera identifier
    char url[MAX_STR_LEN];  ///< Stream URL (RTSP/HTTP)
    int device;             ///< Target /dev/videoX number
    int fps;                ///< Desired frames per second
} CameraConfig;

/**
 * Cached discovery state for a camera.
 * Stores last successful probe and preferred parameters.
 */
typedef struct {
    char id[MAX_STR_LEN];       ///< Camera ID matching CameraConfig
    time_t last_success;        ///< Timestamp of last successful probe
    char best_url[MAX_STR_LEN]; ///< Cached best working stream URL
    int last_fps;               ///< Cached FPS used successfully
} CameraCache;

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

/**
 * @brief Load camera configurations from JSON file.
 *
 * Parses CAMERA_CONFIG_FILE and populates an array of CameraConfig.
 *
 * @param[out] configs  Array of CameraConfig to populate.
 * @param[in]  max      Maximum number of configs that can be stored.
 * @return Number of cameras loaded, -1 on error.
 */
int load_camera_config(CameraConfig *configs, int max);

/**
 * @brief Load cached discovery data from DISCOVERY_CACHE_FILE.
 *
 * Parses JSON cache file if it exists, populating CameraCache entries.
 *
 * @param[out] caches  Array of CameraCache to populate.
 * @param[in]  max     Maximum number of caches that can be stored.
 * @return Number of caches loaded, -1 on error.
 */
int load_discovery_cache(CameraCache *caches, int max);

/**
 * @brief Save updated discovery cache to DISCOVERY_CACHE_FILE.
 *
 * Writes JSON atomically (temp file + rename) to prevent corruption.
 *
 * @param[in] caches  Array of CameraCache entries to write.
 * @param[in] count   Number of entries.
 * @return 0 on success, -1 on error.
 */
int save_discovery_cache(const CameraCache *caches, int count);

/**
 * @brief Probe a single camera stream for connectivity.
 *
 * Uses ffmpeg in probe mode to test if a camera URL is reachable.
 * Updates the provided CameraCache on success.
 *
 * @param[in]  config  Camera configuration.
 * @param[out] cache   Cache entry to update.
 * @return 0 on success, -1 on failure.
 */
int probe_camera(const CameraConfig *config, CameraCache *cache);

/**
 * @brief Attach a camera stream to a virtual /dev/video* device.
 *
 * Spawns an ffmpeg process using optimized low-latency arguments.
 * Intended to run until process exit (failure or shutdown).
 *
 * @param[in] config  Camera configuration.
 * @param[in] cache   Cached discovery info (for optimized launch).
 * @return Child PID on success, -1 on error.
 */
int attach_camera_stream(const CameraConfig *config, const CameraCache *cache);

/**
 * @brief Main loop to manage all cameras.
 *
 * Loads configs, loads caches, attaches streams, and monitors ffmpeg
 * processes. Periodically saves cache to disk.
 *
 * @return 0 on success, non-zero on error.
 */
int run_camera_manager(void);

#endif /* VIDEOPIPE_H */

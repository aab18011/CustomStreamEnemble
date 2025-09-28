#ifndef CONFIG_HANDLER_H
#define CONFIG_HANDLER_H

#include <stdbool.h>

typedef enum {
    CONFIG_INT,
    CONFIG_STRING,
    // Add more types as needed, e.g., CONFIG_BOOL, CONFIG_DOUBLE
} ConfigType;

typedef struct {
    const char *key;
    void *value_ptr;
    ConfigType type;
} ConfigItem;

/**
 * Registers a configurable variable with its key, pointer to the variable, and type.
 * This allows the config loader to recognize and override it from JSON files.
 *
 * @param key The string key to match in JSON (e.g., "max_streams").
 * @param value_ptr Pointer to the variable to override.
 * @param type The type of the variable (CONFIG_INT, CONFIG_STRING, etc.).
 */
void register_config(const char *key, void *value_ptr, ConfigType type);

/**
 * Loads and applies configurations from all JSON files in the specified directory.
 * Parses each JSON file, looks for recognized keys (registered via register_config),
 * and overrides the corresponding variables based on the JSON values.
 * Processes files in the order returned by the OS (last override wins if duplicates).
 * Assumes JSON files contain flat key-value objects (e.g., {"max_streams": 10}).
 * Ignores unrecognized keys and mismatched types.
 *
 * @param dir_path The path to the directory containing JSON config files (e.g., "configs/").
 * @return true if the directory was opened and processed successfully, false otherwise.
 */
bool load_configs(const char *dir_path);

#endif // CONFIG_HANDLER_H
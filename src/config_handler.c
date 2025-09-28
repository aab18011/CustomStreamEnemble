#include "config_handler.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"  // Requires cJSON library (include cJSON.h and cJSON.c in your project)
// Download cJSON from https://github.com/DaveGamble/cJSON if not already added.

#define MAX_CONFIGS 100  // Maximum number of registrable config items (increase if needed)

static ConfigItem configs[MAX_CONFIGS];
static int num_configs = 0;

void register_config(const char *key, void *value_ptr, ConfigType type) {
    if (num_configs < MAX_CONFIGS) {
        configs[num_configs].key = key;
        configs[num_configs].value_ptr = value_ptr;
        configs[num_configs].type = type;
        num_configs++;
    } else {
        fprintf(stderr, "Warning: Max config items reached, cannot register '%s'\n", key);
    }
}

static char* read_file_content(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error: Could not open file '%s'\n", filename);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buffer = (char *)malloc(file_size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }
    size_t read_size = fread(buffer, 1, file_size, fp);
    buffer[read_size] = '\0';
    fclose(fp);
    return buffer;
}

bool load_configs(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "Error: Could not open directory '%s'\n", dir_path);
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {  // Regular file
            size_t name_len = strlen(entry->d_name);
            if (name_len > 5 && strcmp(entry->d_name + name_len - 5, ".json") == 0) {
                char full_path[1024];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

                char *content = read_file_content(full_path);
                if (content) {
                    cJSON *json = cJSON_Parse(content);
                    if (json && cJSON_IsObject(json)) {
                        cJSON *json_item = json->child;
                        while (json_item != NULL) {
                            const char *key = json_item->string;
                            for (int i = 0; i < num_configs; i++) {
                                if (strcmp(configs[i].key, key) == 0) {
                                    switch (configs[i].type) {
                                        case CONFIG_INT:
                                            if (cJSON_IsNumber(json_item)) {
                                                *(int *)configs[i].value_ptr = (int)json_item->valuedouble;
                                            } else {
                                                fprintf(stderr, "Warning: Type mismatch for key '%s' (expected int)\n", key);
                                            }
                                            break;
                                        case CONFIG_STRING:
                                            if (cJSON_IsString(json_item)) {
                                                // Free existing string if allocated (assumes initial value is NULL or malloc'd)
                                                char **str_ptr = (char **)configs[i].value_ptr;
                                                if (*str_ptr) free(*str_ptr);
                                                *str_ptr = strdup(json_item->valuestring);
                                            } else {
                                                fprintf(stderr, "Warning: Type mismatch for key '%s' (expected string)\n", key);
                                            }
                                            break;
                                        // Add cases for more types as needed
                                    }
                                    break;  // Stop searching after match
                                }
                            }
                            json_item = json_item->next;
                        }
                    } else {
                        fprintf(stderr, "Warning: Invalid JSON in file '%s'\n", full_path);
                    }
                    cJSON_Delete(json);
                    free(content);
                }
            }
        }
    }
    closedir(dir);
    return true;
}
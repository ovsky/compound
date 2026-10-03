#ifndef POUND_CORE_CONFIG_H
#define POUND_CORE_CONFIG_H

//! Configuration - user settings (single source of truth).

#include "attributes.h"
#include <stdbool.h>
#include <stddef.h>

#define POUND_CONFIG_KEY_PATH_MAX 512
#define POUND_CONFIG_DIR_MAX 512

typedef enum
{
    POUND_LOG_LEVEL_FATAL = 0,
    POUND_LOG_LEVEL_ERROR,
    POUND_LOG_LEVEL_WARN,
    POUND_LOG_LEVEL_INFO,
    POUND_LOG_LEVEL_DEBUG
} pound_log_level_t;

typedef struct
{
    size_t            jit_cache_size_mb;
    char              key_file_path[POUND_CONFIG_KEY_PATH_MAX];
    char              content_dir[POUND_CONFIG_DIR_MAX];
    pound_log_level_t log_level;
    bool              theme_dark;
    bool              initialized;
    char              pad[6];
} pound_config_t;

void pound_config_init_defaults(pound_config_t *POUND_RESTRICT config);
void pound_config_reset_to_defaults(pound_config_t *POUND_RESTRICT config);
void pound_config_set_log_level(pound_config_t *POUND_RESTRICT config, pound_log_level_t level);
pound_log_level_t pound_config_get_log_level(const pound_config_t *POUND_RESTRICT config);

#endif // POUND_CORE_CONFIG_H

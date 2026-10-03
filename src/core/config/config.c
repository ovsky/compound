//! Configuration implementation.

#include "config.h"
#include "log.h"
#include <string.h>

#define POUND_CONFIG_DEFAULT_JIT_CACHE_MB 512
#define POUND_CONFIG_DEFAULT_KEY_PATH "keys.txt"
#define POUND_CONFIG_DEFAULT_CONTENT_DIR "."

void pound_config_init_defaults(pound_config_t *POUND_RESTRICT config)
{
    if (NULL == config)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: config is NULL.");
        return;
    }
    memset(config, 0, sizeof(pound_config_t));
    config->jit_cache_size_mb = POUND_CONFIG_DEFAULT_JIT_CACHE_MB;
    strncpy(config->key_file_path, POUND_CONFIG_DEFAULT_KEY_PATH, POUND_CONFIG_KEY_PATH_MAX - 1);
    config->key_file_path[POUND_CONFIG_KEY_PATH_MAX - 1] = '\0';
    strncpy(config->content_dir, POUND_CONFIG_DEFAULT_CONTENT_DIR, POUND_CONFIG_DIR_MAX - 1);
    config->content_dir[POUND_CONFIG_DIR_MAX - 1] = '\0';
    config->log_level = POUND_LOG_LEVEL_WARN;
    config->theme_dark = true;
    config->initialized = true;
}

void pound_config_reset_to_defaults(pound_config_t *POUND_RESTRICT config)
{
    pound_config_init_defaults(config);
}

void pound_config_set_log_level(pound_config_t *POUND_RESTRICT config, pound_log_level_t level)
{
    if (NULL == config)
        return;
    config->log_level = level;
}

pound_log_level_t pound_config_get_log_level(const pound_config_t *POUND_RESTRICT config)
{
    if (NULL == config || !config->initialized)
        return POUND_LOG_LEVEL_WARN;
    return config->log_level;
}

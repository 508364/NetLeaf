/**
 * @file plugin.c
 * @brief NetLeaf Plugin Development Template Implementation
 * @version 1.0.0
 * @date 2026-09-12
 */

#include "netleaf_plugin_template.h"
#include "netleaf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =========================================
// Internal State
// =========================================

static int g_plugin_initialized = 0;
static int g_plugin_status = 0;
static char g_last_error[256] = {0};

// =========================================
// Plugin Info Structure
// =========================================

static nl_plugin_info_t g_plugin_info = {
    .name = MY_PLUGIN_NAME,
    .id = MY_PLUGIN_ID,
    .version = MY_PLUGIN_VERSION,
    .description = MY_PLUGIN_DESCRIPTION,
    .author = MY_PLUGIN_AUTHOR,
    .url = MY_PLUGIN_URL,
    .license = MY_PLUGIN_LICENSE,

    .capabilities = MY_PLUGIN_CAPABILITIES,
    .required_capabilities = MY_PLUGIN_REQUIRED_CAPABILITIES,

    .platform_windows = MY_PLUGIN_PLATFORM_WIN,
    .platform_linux = MY_PLUGIN_PLATFORM_LINUX,
    .platform_macos = MY_PLUGIN_PLATFORM_MACOS,

    .min_nl_version = MY_PLUGIN_MIN_NL_VERSION,

    .init = plugin_init,
    .shutdown = plugin_shutdown,
    .register_module = plugin_register_module,
    .is_available = plugin_is_available,
    .on_event = NULL,
    .userdata = NULL,

    // Dependency management (NEW)
    .dependencies = NULL,
    .dependency_array = NULL,
    .dependency_count = 0,
    .required_dep_count = 0,
    .optional_dep_count = 0,
    .resolve_dependencies = NULL,  // Implement if needed
    .get_extension = NULL,         // Use nl_extension_access()
    .get_plugin = NULL,            // Use nl_plugin_handle APIs
    .get_function = NULL,          // Use nl_extension_get_func_by_id()
    .get_main_library = NULL,      // Returns NULL by default
    .get_all_extensions = NULL,    // Use nl_extension_get_all()
    .get_all_plugins = NULL        // Use nl_plugin_get_all()
};

// =========================================
// Required Plugin Functions
// =========================================

/**
 * @brief Get plugin info - MUST be exported as "plugin_get_info"
 */
MY_PLUGIN_API nl_plugin_info_t* plugin_get_info(void) {
    return &g_plugin_info;
}

/**
 * @brief Initialize plugin
 * @return 0 on success, negative on error
 */
MY_PLUGIN_API int plugin_init(void) {
    if (g_plugin_initialized) {
        return 0;  // Already initialized
    }
    
    // Your initialization code here
    g_plugin_initialized = 1;
    g_plugin_status = 1;
    
    printf("[Plugin] %s v%s initialized successfully\n", 
           MY_PLUGIN_NAME, MY_PLUGIN_VERSION);
    
    // Example: Register a module if needed
    // nl_module_info_t* module_info = NULL;
    // plugin_register_module(&module_info);
    
    return 0;
}

/**
 * @brief Shutdown plugin
 */
MY_PLUGIN_API void plugin_shutdown(void) {
    if (!g_plugin_initialized) {
        return;
    }
    
    // Your cleanup code here
    g_plugin_status = 0;
    g_plugin_initialized = 0;
    
    printf("[Plugin] %s shutdown completed\n", MY_PLUGIN_NAME);
}

/**
 * @brief Check if plugin is available
 * @return 1 if available, 0 if not
 */
MY_PLUGIN_API int plugin_is_available(void) {
    return g_plugin_initialized;
}

/**
 * @brief Register plugin as a module (optional)
 * @param module_info Output parameter for module info
 * @return 0 on success, negative on error
 */
MY_PLUGIN_API int plugin_register_module(nl_module_info_t** module_info) {
    if (!module_info) {
        return -1;
    }
    
    // Create module info structure
    static nl_module_info_t module_info_struct = {
        .type = NL_MODULE_CUSTOM,
        .name = MY_PLUGIN_ID,
        .version = MY_PLUGIN_VERSION,
        .capabilities = MY_PLUGIN_CAPABILITIES,
        .status = NL_MODULE_STATUS_UNINITIALIZED,
        .platform_windows = MY_PLUGIN_PLATFORM_WIN,
        .platform_linux = MY_PLUGIN_PLATFORM_LINUX,
        .platform_macos = MY_PLUGIN_PLATFORM_MACOS,
        .init = plugin_init,
        .shutdown = plugin_shutdown,
        .is_available = plugin_is_available,
        .get_version = NULL,
        .description = MY_PLUGIN_DESCRIPTION,
        .author = MY_PLUGIN_AUTHOR,
        .lazy_load = NULL,
        .lazy_unload = NULL,
        .lazy_status = NL_MODULE_LAZY_UNLOADED,
        .next = NULL,
        .dependencies = NULL
    };
    
    *module_info = &module_info_struct;
    return 0;
}

// =========================================
// Custom Plugin Functions (Examples)
// =========================================

/**
 * @brief Process data through plugin
 */
MY_PLUGIN_API int my_plugin_process(const char* input, char* output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return -1;
    }
    
    if (!g_plugin_initialized) {
        snprintf(g_last_error, sizeof(g_last_error), "Plugin not initialized");
        return -2;
    }
    
    // Your processing logic here
    snprintf(output, output_size, "[%s] Processed: %s", MY_PLUGIN_NAME, input);
    
    return 0;
}

/**
 * @brief Get plugin status
 */
MY_PLUGIN_API int my_plugin_get_status(void) {
    return g_plugin_status;
}

/**
 * @brief Get last error message
 */
MY_PLUGIN_API const char* my_plugin_get_last_error(void) {
    return g_last_error;
}

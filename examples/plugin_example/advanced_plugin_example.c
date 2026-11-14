/**
 * @file advanced_plugin_example.c
 * @brief Advanced NetLeaf Plugin Example with Dependency Management
 * @version 1.0.0
 * @date 2026-09-12
 */

#include "netleaf_module.h"
#include "netleaf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =========================================
// Plugin Configuration
// =========================================

#define ADV_PLUGIN_ID     "advanced_plugin"
#define ADV_PLUGIN_NAME   "Advanced Plugin Example"
#define ADV_PLUGIN_VER    "1.0.0"
#define ADV_PLUGIN_AUTHOR "508364"
#define ADV_PLUGIN_DESC   "演示依赖管理和跨扩展访问的高级插件示例"

// =========================================
// Internal State
// =========================================

static int g_initialized = 0;
static int g_ipc_available = 0;
static int g_linkagg_available = 0;
static char g_last_result[1024] = {0};

// =========================================
// Function Declarations
// =========================================

static int adv_init(void);
static void adv_shutdown(void);
static int adv_is_available(void);
static int adv_resolve_dependencies(nl_extension_info_t* self);
static nl_extension_info_t* adv_get_extension(const char* library_id);
static void* adv_get_function(const char* target_id, const char* func_name);
static void* adv_get_main_library(void);
static int adv_check_ipc_function(void);

// =========================================
// Plugin Info Structure
// =========================================

static nl_plugin_info_t g_plugin_info = {
    .name = ADV_PLUGIN_NAME,
    .id = ADV_PLUGIN_ID,
    .version = ADV_PLUGIN_VER,
    .description = ADV_PLUGIN_DESC,
    .author = ADV_PLUGIN_AUTHOR,
    .capabilities = NL_CAP_THREAD_SAFE | NL_CAP_DYNAMIC | NL_CAP_PLUGIN | NL_CAP_EXT_SYSTEM,
    .required_capabilities = NL_CAP_DYNAMIC,
    .platform_windows = 1,
    .platform_linux = 1,
    .platform_macos = 1,
    .min_nl_version = "2.4.0",
    .init = adv_init,
    .shutdown = adv_shutdown,
    .is_available = adv_is_available,
    .on_event = NULL,
    .userdata = NULL,

    // Dependency management
    .dependencies = NULL,
    .dependency_array = NULL,
    .dependency_count = 0,
    .required_dep_count = 0,
    .optional_dep_count = 0,
    .resolve_dependencies = adv_resolve_dependencies,

    // Extension access functions
    .get_extension = adv_get_extension,
    .get_function = adv_get_function,
    .get_main_library = adv_get_main_library,
    .get_all_extensions = NULL,  // Use built-in nl_extension_get_all
    .get_all_plugins = NULL     // Use built-in nl_plugin_get_all
};

// =========================================
// Required Plugin Functions
// =========================================

/**
 * @brief Initialize plugin
 */
static int adv_init(void) {
    if (g_initialized) return 0;

    printf("[AdvPlugin] Initializing v%s\n", ADV_PLUGIN_VER);

    // Try to access IPC extension
    nl_extension_info_t* ipc_ext = nl_extension_access("ipc");
    if (ipc_ext) {
        g_ipc_available = 1;
        printf("[AdvPlugin] IPC extension accessed successfully\n");
    } else {
        printf("[AdvPlugin] IPC extension not available\n");
    }

    // Try to access LinkAgg extension
    nl_extension_info_t* lagg_ext = nl_extension_access("linkagg");
    if (lagg_ext) {
        g_linkagg_available = 1;
        printf("[AdvPlugin] LinkAgg extension accessed successfully\n");
    } else {
        printf("[AdvPlugin] LinkAgg extension not available\n");
    }

    g_initialized = 1;
    return 0;
}

/**
 * @brief Shutdown plugin
 */
static void adv_shutdown(void) {
    if (!g_initialized) return;

    printf("[AdvPlugin] Shutting down\n");
    g_initialized = 0;
    g_ipc_available = 0;
    g_linkagg_available = 0;
}

/**
 * @brief Check if plugin is available
 */
static int adv_is_available(void) {
    return g_initialized;
}

/**
 * @brief Resolve plugin dependencies
 * @return 0 on success, negative on failure
 */
static int adv_resolve_dependencies(nl_extension_info_t* self) {
    printf("[AdvPlugin] Resolving dependencies...\n");

    // Clear existing dependencies
    nl_plugin_clear_dependencies(NULL);

    // Add required dependency on core (always available)
    nl_plugin_add_dependency(NULL, "core", NL_EXT_DEP_REQUIRED, "2.4.0");

    // Add optional dependencies
    nl_plugin_add_dependency(NULL, "ipc", NL_EXT_DEP_OPTIONAL, NULL);
    nl_plugin_add_dependency(NULL, "linkagg", NL_EXT_DEP_OPTIONAL, NULL);

    // Check if all required dependencies are met
    if (!nl_plugin_are_dependencies_met(NULL)) {
        printf("[AdvPlugin] Required dependencies not met\n");
        return -1;
    }

    printf("[AdvPlugin] Dependencies resolved successfully\n");
    return 0;
}

/**
 * @brief Get extension by ID
 */
static nl_extension_info_t* adv_get_extension(const char* library_id) {
    return nl_extension_access(library_id);
}

/**
 * @brief Get function pointer from extension/plugin
 */
static void* adv_get_function(const char* target_id, const char* func_name) {
    return nl_extension_get_func_by_id(target_id, func_name);
}

/**
 * @brief Get main library handle
 */
static void* adv_get_main_library(void) {
    // Return NULL - in real implementation, this would return a function table
    return NULL;
}

// =========================================
// Public API
// =========================================

#ifdef _WIN32
    #ifdef NL_PLUGIN_EXPORTS
        #define ADV_PLUGIN_API __declspec(dllexport)
    #else
        #define ADV_PLUGIN_API __declspec(dllimport)
    #endif
#else
    #define ADV_PLUGIN_API
#endif

/**
 * @brief Get plugin info - REQUIRED
 */
ADV_PLUGIN_API nl_plugin_info_t* plugin_get_info(void) {
    return &g_plugin_info;
}

/**
 * @brief Initialize - REQUIRED
 */
ADV_PLUGIN_API int plugin_init(void) {
    return adv_init();
}

/**
 * @brief Shutdown - REQUIRED
 */
ADV_PLUGIN_API void plugin_shutdown(void) {
    adv_shutdown();
}

/**
 * @brief Check availability - REQUIRED
 */
ADV_PLUGIN_API int plugin_is_available(void) {
    return adv_is_available();
}

/**
 * @brief Register module - OPTIONAL
 */
ADV_PLUGIN_API int plugin_register_module(nl_module_info_t** module_info) {
    if (!module_info) return -1;

    static nl_module_info_t module = {
        .type = NL_MODULE_CUSTOM,
        .name = ADV_PLUGIN_ID,
        .version = ADV_PLUGIN_VER,
        .capabilities = NL_CAP_THREAD_SAFE | NL_CAP_PLUGIN,
        .status = NL_MODULE_STATUS_UNINITIALIZED,
        .platform_windows = 1,
        .platform_linux = 1,
        .platform_macos = 1,
        .init = adv_init,
        .shutdown = adv_shutdown,
        .is_available = adv_is_available,
        .description = ADV_PLUGIN_DESC,
        .author = ADV_PLUGIN_AUTHOR,
        .next = NULL,
        .dependencies = NULL
    };

    *module_info = &module;
    return 0;
}

// =========================================
// Custom Plugin Functions
// =========================================

/**
 * @brief Check if IPC is available and get version
 */
ADV_PLUGIN_API int adv_check_ipc_status(char* version_buf, size_t buf_size) {
    if (!g_ipc_available) {
        snprintf(g_last_result, sizeof(g_last_result), "IPC not available");
        return -1;
    }

    nl_extension_info_t* ipc = nl_extension_access("ipc");
    if (ipc && ipc->get_version) {
        const char* ver = ipc->get_version();
        snprintf(g_last_result, sizeof(g_last_result), "IPC version: %s", ver);
        if (version_buf && buf_size > 0) {
            snprintf(version_buf, buf_size, "%s", ver);
        }
        return 0;
    }

    snprintf(g_last_result, sizeof(g_last_result), "IPC info unavailable");
    return -1;
}

/**
 * @brief Check if LinkAgg is available and get version
 */
ADV_PLUGIN_API int adv_check_linkagg_status(char* version_buf, size_t buf_size) {
    if (!g_linkagg_available) {
        snprintf(g_last_result, sizeof(g_last_result), "LinkAgg not available");
        return -1;
    }

    nl_extension_info_t* lagg = nl_extension_access("linkagg");
    if (lagg && lagg->get_version) {
        const char* ver = lagg->get_version();
        snprintf(g_last_result, sizeof(g_last_result), "LinkAgg version: %s", ver);
        if (version_buf && buf_size > 0) {
            snprintf(version_buf, buf_size, "%s", ver);
        }
        return 0;
    }

    snprintf(g_last_result, sizeof(g_last_result), "LinkAgg info unavailable");
    return -1;
}

/**
 * @brief Get comprehensive status
 */
ADV_PLUGIN_API void adv_get_status(char* buf, size_t size) {
    if (!buf || size == 0) return;

    snprintf(buf, size,
        "Plugin: %s v%s\n"
        "Author: %s\n"
        "Status: %s\n"
        "IPC Available: %s\n"
        "LinkAgg Available: %s\n"
        "Dependencies: %d total, %d required, %d optional",
        ADV_PLUGIN_NAME,
        ADV_PLUGIN_VER,
        ADV_PLUGIN_AUTHOR,
        g_initialized ? "Active" : "Inactive",
        g_ipc_available ? "Yes" : "No",
        g_linkagg_available ? "Yes" : "No",
        g_plugin_info.dependency_count,
        g_plugin_info.required_dep_count,
        g_plugin_info.optional_dep_count
    );
}

/**
 * @brief List all available extensions
 */
ADV_PLUGIN_API int adv_list_extensions(char* buf, size_t size) {
    if (!buf || size == 0) return -1;

    int count = 0;
    nl_extension_info_t** extensions = nl_extension_get_all(&count);

    if (!extensions) {
        snprintf(buf, size, "No extensions loaded");
        return 0;
    }

    size_t offset = 0;
    offset += snprintf(buf + offset, size - offset, "Available Extensions (%d):\n", count);
    for (int i = 0; i < count; i++) {
        offset += snprintf(buf + offset, size - offset,
            "  [%s] %s v%s by %s\n",
            extensions[i]->library_id,
            extensions[i]->library_name,
            extensions[i]->version,
            extensions[i]->author
        );
    }

    if (extensions) free(extensions);
    return 0;
}

/**
 * @brief Call a function from another extension
 */
ADV_PLUGIN_API int adv_call_extension_function(const char* ext_id, const char* func_name, char* result, size_t result_size) {
    void* func = nl_extension_get_func_by_id(ext_id, func_name);
    if (!func) {
        if (result && result_size > 0) {
            snprintf(result, result_size, "Function '%s' not found in extension '%s'", func_name, ext_id);
        }
        return -1;
    }

    if (result && result_size > 0) {
        snprintf(result, result_size, "Function '%s' from extension '%s' is available", func_name, ext_id);
    }
    return 0;
}

/**
 * @brief Get last result
 */
ADV_PLUGIN_API const char* adv_get_last_result(void) {
    return g_last_result;
}

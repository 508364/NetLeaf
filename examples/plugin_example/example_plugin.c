/**
 * @file example_plugin.c
 * @brief Example NetLeaf Plugin demonstrating new API features
 * @version 1.0.0
 */

#include "netleaf_module.h"
#include "netleaf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =========================================
// Plugin Configuration
// =========================================

#define EXAMPLE_PLUGIN_ID     "example_plugin"
#define EXAMPLE_PLUGIN_NAME   "Example Plugin"
#define EXAMPLE_PLUGIN_VER    "1.0.0"
#define EXAMPLE_PLUGIN_AUTHOR "508364"
#define EXAMPLE_PLUGIN_DESC   "演示NL扩展系统新接口的示例插件"
#define EXAMPLE_PLUGIN_URL    "https://github.com/508364/netleaf-plugin-example"
#define EXAMPLE_PLUGIN_LICENSE "MIT"
#define EXAMPLE_PLUGIN_MIN_NL_VER "2.4.0"

// =========================================
// Internal State
// =========================================

static int g_initialized = 0;
static int g_message_count = 0;
static char g_last_message[1024] = {0};

// =========================================
// Event Handler
// =========================================

static void on_plugin_event(const char* event_name, void* data, void* userdata) {
    printf("[ExamplePlugin] Event received: %s\n", event_name);
    if (strcmp(event_name, "test.event") == 0) {
        g_message_count++;
        snprintf(g_last_message, sizeof(g_last_message), "Event #%d received", g_message_count);
    }
}

// =========================================
// Plugin Info Structure
// =========================================

static nl_plugin_info_t g_plugin_info = {
    .name = EXAMPLE_PLUGIN_NAME,
    .id = EXAMPLE_PLUGIN_ID,
    .version = EXAMPLE_PLUGIN_VER,
    .description = EXAMPLE_PLUGIN_DESC,
    .author = EXAMPLE_PLUGIN_AUTHOR,
    .url = EXAMPLE_PLUGIN_URL,
    .license = EXAMPLE_PLUGIN_LICENSE,
    
    .capabilities = NL_CAP_THREAD_SAFE | NL_CAP_DYNAMIC | NL_CAP_PLUGIN | NL_CAP_EXT_SYSTEM,
    .required_capabilities = NL_CAP_DYNAMIC,
    
    .platform_windows = 1,
    .platform_linux = 1,
    .platform_macos = 1,
    
    .min_nl_version = EXAMPLE_PLUGIN_MIN_NL_VER,
    
    .init = NULL,      // Set dynamically below
    .shutdown = NULL,
    .register_module = NULL,
    .is_available = NULL,
    .on_event = on_plugin_event,
    .userdata = NULL
};

// =========================================
// Required Plugin Functions
// =========================================

// Initialize plugin
static int example_init(void) {
    if (g_initialized) return 0;
    g_initialized = 1;
    printf("[ExamplePlugin] Initialized v%s\n", EXAMPLE_PLUGIN_VER);
    return 0;
}

// Shutdown plugin
static void example_shutdown(void) {
    if (!g_initialized) return;
    g_initialized = 0;
    g_message_count = 0;
    g_last_message[0] = '\0';
    printf("[ExamplePlugin] Shutdown\n");
}

// Check availability
static int example_is_available(void) {
    return g_initialized;
}

// Register as module (optional)
static int example_register_module(nl_module_info_t** module_info) {
    if (!module_info) return -1;
    
    static nl_module_info_t module = {
        .type = NL_MODULE_CUSTOM,
        .name = EXAMPLE_PLUGIN_ID,
        .version = EXAMPLE_PLUGIN_VER,
        .capabilities = NL_CAP_THREAD_SAFE | NL_CAP_PLUGIN,
        .status = NL_MODULE_STATUS_UNINITIALIZED,
        .platform_windows = 1,
        .platform_linux = 1,
        .platform_macos = 1,
        .init = example_init,
        .shutdown = example_shutdown,
        .is_available = example_is_available,
        .get_version = NULL,
        .description = EXAMPLE_PLUGIN_DESC,
        .author = EXAMPLE_PLUGIN_AUTHOR,
        .lazy_load = NULL,
        .lazy_unload = NULL,
        .lazy_status = NL_MODULE_LAZY_UNLOADED,
        .next = NULL,
        .dependencies = NULL
    };
    
    *module_info = &module;
    return 0;
}

// =========================================
// Public API
// =========================================

#ifdef _WIN32
    #ifdef NL_PLUGIN_EXPORTS
        #define EXAMPLE_API __declspec(dllexport)
    #else
        #define EXAMPLE_API __declspec(dllimport)
    #endif
#else
    #define EXAMPLE_API
#endif

// REQUIRED: Get plugin info
EXAMPLE_API nl_plugin_info_t* plugin_get_info(void) {
    g_plugin_info.init = example_init;
    g_plugin_info.shutdown = example_shutdown;
    g_plugin_info.register_module = example_register_module;
    g_plugin_info.is_available = example_is_available;
    return &g_plugin_info;
}

// REQUIRED: Initialize
EXAMPLE_API int plugin_init(void) {
    return example_init();
}

// REQUIRED: Shutdown
EXAMPLE_API void plugin_shutdown(void) {
    example_shutdown();
}

// REQUIRED: Check availability
EXAMPLE_API int plugin_is_available(void) {
    return example_is_available();
}

// OPTIONAL: Register module
EXAMPLE_API int plugin_register_module(nl_module_info_t** module_info) {
    return example_register_module(module_info);
}

// =========================================
// Custom Plugin Functions
// =========================================

/**
 * @brief Send a chat message
 */
EXAMPLE_API int example_send_message(const char* user, const char* message) {
    if (!user || !message || !g_initialized) return -1;
    
    snprintf(g_last_message, sizeof(g_last_message), "[%s]: %s", user, message);
    g_message_count++;
    
    printf("[ExamplePlugin] Message #%d: %s\n", g_message_count, g_last_message);
    return 0;
}

/**
 * @brief Get last message
 */
EXAMPLE_API const char* example_get_last_message(void) {
    return g_last_message;
}

/**
 * @brief Get message count
 */
EXAMPLE_API int example_get_message_count(void) {
    return g_message_count;
}

/**
 * @brief Get plugin info string for debugging
 */
EXAMPLE_API void example_get_info_string(char* buf, size_t size) {
    if (!buf || size == 0) return;
    
    snprintf(buf, size, 
        "Plugin: %s v%s\n"
        "Author: %s\n"
        "ID: %s\n"
        "Status: %s\n"
        "Messages: %d\n"
        "Last: %s",
        EXAMPLE_PLUGIN_NAME,
        EXAMPLE_PLUGIN_VER,
        EXAMPLE_PLUGIN_AUTHOR,
        EXAMPLE_PLUGIN_ID,
        g_initialized ? "Active" : "Inactive",
        g_message_count,
        g_last_message
    );
}

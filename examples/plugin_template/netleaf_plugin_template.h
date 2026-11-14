#ifndef NETLEAF_PLUGIN_TEMPLATE_H
#define NETLEAF_PLUGIN_TEMPLATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

/**
 * @file netleaf_plugin_template.h
 * @brief NetLeaf Plugin Development Template v1.0.0
 *
 * This template provides a complete skeleton for developing NetLeaf plugins.
 * Follow the instructions below to create your own plugin.
 *
 * PLUGIN DEVELOPMENT GUIDE:
 * 1. Copy this template to your plugin project
 * 2. Replace all "PLUGIN_NAME" with your plugin's actual name
 * 3. Implement the required functions (see plugin.c)
 * 4. Export the plugin_info function as shown in plugin.c
 * 5. Build as shared library (.dll on Windows, .so on Linux/macOS)
 * 6. Place the output in the extensions/ directory next to netleaf.dll/.so
 */

// =========================================
// Plugin Configuration (CHANGE THESE)
// =========================================

#define MY_PLUGIN_ID          "my_plugin_v1"
#define MY_PLUGIN_NAME        "My Custom Plugin"
#define MY_PLUGIN_VERSION     "1.0.0"
#define MY_PLUGIN_AUTHOR      "Your Name"
#define MY_PLUGIN_DESCRIPTION "A sample NetLeaf plugin demonstrating development patterns"
#define MY_PLUGIN_URL         "https://github.com/yourusername/my-plugin"
#define MY_PLUGIN_LICENSE     "MIT"
#define MY_PLUGIN_MIN_NL_VERSION "2.4.0"

// Platform support: set to 1 for supported platforms, 0 for unsupported
#define MY_PLUGIN_PLATFORM_WIN    1
#define MY_PLUGIN_PLATFORM_LINUX  1
#define MY_PLUGIN_PLATFORM_MACOS  0

// =========================================
// Plugin Capabilities (combine with |)
// =========================================

// Capability flags:
// NL_CAP_NONE           - No capabilities
// NL_CAP_SERVER         - Can create server instances
// NL_CAP_CLIENT         - Can create client instances  
// NL_CAP_ASYNC          - Supports async operations
// NL_CAP_THREAD_SAFE    - Thread-safe operations
// NL_CAP_PLATFORM_WIN   - Windows only
// NL_CAP_PLATFORM_LINUX - Linux only
// NL_CAP_PLATFORM_MACOS - macOS only
// NL_CAP_PLATFORM_ALL   - All platforms
// NL_CAP_LAZY_LOAD      - Supports lazy loading
// NL_CAP_DYNAMIC        - Can be dynamically loaded
// NL_CAP_PLUGIN         - Is a plugin module
// NL_CAP_EXT_SYSTEM     - Part of NL Extension System

#define MY_PLUGIN_CAPABILITIES \
    (NL_CAP_THREAD_SAFE | NL_CAP_DYNAMIC | NL_CAP_PLUGIN | NL_CAP_EXT_SYSTEM)

// Required host capabilities (capabilities the host must have)
#define MY_PLUGIN_REQUIRED_CAPABILITIES \
    (NL_CAP_DYNAMIC)

// =========================================
// Plugin API Export Macros
// =========================================

#ifdef _WIN32
    #ifdef NL_PLUGIN_EXPORTS
        #define MY_PLUGIN_API __declspec(dllexport)
    #else
        #define MY_PLUGIN_API __declspec(dllimport)
    #endif
#else
    #define MY_PLUGIN_API
#endif

// =========================================
// Plugin Public API (Declare your functions here)
// =========================================

// Required: Get plugin info (called by host during loading)
MY_PLUGIN_API nl_plugin_info_t* plugin_get_info(void);

// Required: Initialize plugin (called after loading)
MY_PLUGIN_API int plugin_init(void);

// Required: Shutdown plugin (called before unloading)
MY_PLUGIN_API void plugin_shutdown(void);

// Required: Check if plugin is available
MY_PLUGIN_API int plugin_is_available(void);

// Required: Register as module (optional - call if plugin adds modules)
MY_PLUGIN_API int plugin_register_module(nl_module_info_t** module_info);

// =========================================
// Your Plugin's Custom Functions (Example)
// =========================================

// Add your custom public API functions below

/**
 * @brief Example: Process data through your plugin
 * @param input Input data string
 * @param output Output buffer
 * @param output_size Size of output buffer
 * @return 0 on success, negative on error
 */
MY_PLUGIN_API int my_plugin_process(const char* input, char* output, size_t output_size);

/**
 * @brief Example: Get plugin status
 * @return Plugin status code (0 = offline, 1 = online)
 */
MY_PLUGIN_API int my_plugin_get_status(void);

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_PLUGIN_TEMPLATE_H

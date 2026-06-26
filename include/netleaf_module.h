#ifndef NETLEAF_MODULE_H
#define NETLEAF_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NL_MODULE_VERSION "2.2.2"

// =========================================
// Module Type Definitions
// =========================================

typedef enum {
    NL_MODULE_CORE = 0,       // Core library (netleaf)
    NL_MODULE_IPC = 1,        // IPC communication
    NL_MODULE_LINKAGG = 2,    // Link aggregation
    NL_MODULE_AUTOROUTE = 3,  // Auto-route suggestions
    NL_MODULE_AUTOCOMPLETE = 4, // Auto-complete
    NL_MODULE_ERRORPAGE = 5,  // Error page templates
    NL_MODULE_LANG = 6,       // Multi-language support
    NL_MODULE_VUE = 7,        // Vue.js backend support
    NL_MODULE_CUSTOM = 8,     // Custom/third-party modules
    NL_MODULE_MAX = 32        // Maximum modules (extended for plugins)
} nl_module_type_t;

// Module capability flags
typedef enum {
    NL_CAP_NONE = 0,
    NL_CAP_SERVER = 1 << 0,      // Can create server
    NL_CAP_CLIENT = 1 << 1,      // Can create client
    NL_CAP_ASYNC = 1 << 2,       // Supports async operations
    NL_CAP_THREAD_SAFE = 1 << 3, // Thread-safe operations
    NL_CAP_PLATFORM_WIN = 1 << 4, // Windows only
    NL_CAP_PLATFORM_LINUX = 1 << 5, // Linux only
    NL_CAP_PLATFORM_MACOS = 1 << 6, // macOS only
    NL_CAP_PLATFORM_ALL = (NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX | NL_CAP_PLATFORM_MACOS),
    NL_CAP_LAZY_LOAD = 1 << 7,   // Supports lazy loading
    NL_CAP_DYNAMIC = 1 << 8,     // Can be dynamically loaded
    NL_CAP_PLUGIN = 1 << 9       // Is a plugin module
} nl_module_cap_t;

// Module status
typedef enum {
    NL_MODULE_STATUS_UNINITIALIZED = 0,
    NL_MODULE_STATUS_INITIALIZED = 1,
    NL_MODULE_STATUS_ERROR = 2,
    NL_MODULE_STATUS_DISABLED = 3,
    NL_MODULE_STATUS_LOADING = 4,
    NL_MODULE_STATUS_STOPPED = 5
} nl_module_status_t;

// Lazy loading status
typedef enum {
    NL_LAZY_STATUS_UNLOADED = 0,
    NL_LAZY_STATUS_LOADING = 1,
    NL_LAZY_STATUS_LOADED = 2,
    NL_LAZY_STATUS_STOPPING = 3,
    NL_LAZY_STATUS_STOPPED = 4
} nl_lazy_status_t;

// =========================================
// Module Information Structure
// =========================================

typedef struct nl_module_info {
    nl_module_type_t type;
    const char* name;           // Module name (e.g., "ipc", "linkagg")
    const char* version;        // Version string (e.g., "2.2.2")
    uint32_t capabilities;      // Capability flags
    nl_module_status_t status;  // Current status
    
    int platform_windows;       // 1 if supported on Windows
    int platform_linux;         // 1 if supported on Linux
    int platform_macos;         // 1 if supported on macOS
    
    int (*init)(void);          // Initialize module
    void (*shutdown)(void);     // Shutdown module
    int (*is_available)(void);  // Check if module is available
    const char* (*get_version)(void); // Get version string
    
    const char* description;    // Short description
    const char* author;         // Author info (optional)
    
    void* (*lazy_load)(void);   // Lazy load function (optional)
    void (*lazy_unload)(void);  // Lazy unload function (optional)
    nl_lazy_status_t lazy_status; // Lazy loading status
    
    struct nl_module_info* next; // Linked list next pointer
    struct nl_module_info* dependencies; // Module dependencies
} nl_module_info_t;

// =========================================
// Module Registration Macros
// =========================================

#define NL_MODULE_DEFINE(module_type, module_name, module_version, \
                         caps, win_support, linux_support, macos_support, \
                         init_fn, shutdown_fn, available_fn, version_fn, \
                         desc, author) \
    static nl_module_info_t nl_module_info_##module_name = { \
        .type = module_type, \
        .name = #module_name, \
        .version = module_version, \
        .capabilities = caps, \
        .status = NL_MODULE_STATUS_UNINITIALIZED, \
        .platform_windows = win_support, \
        .platform_linux = linux_support, \
        .platform_macos = macos_support, \
        .init = init_fn, \
        .shutdown = shutdown_fn, \
        .is_available = available_fn, \
        .get_version = version_fn, \
        .description = desc, \
        .author = author, \
        .lazy_load = NULL, \
        .lazy_unload = NULL, \
        .lazy_status = NL_LAZY_STATUS_UNLOADED, \
        .next = NULL, \
        .dependencies = NULL \
    }

#define NL_MODULE_DEFINE_LAZY(module_type, module_name, module_version, \
                             caps, win_support, linux_support, macos_support, \
                             init_fn, shutdown_fn, available_fn, version_fn, \
                             desc, author, lazy_load_fn, lazy_unload_fn) \
    static nl_module_info_t nl_module_info_##module_name = { \
        .type = module_type, \
        .name = #module_name, \
        .version = module_version, \
        .capabilities = caps | NL_CAP_LAZY_LOAD, \
        .status = NL_MODULE_STATUS_UNINITIALIZED, \
        .platform_windows = win_support, \
        .platform_linux = linux_support, \
        .platform_macos = macos_support, \
        .init = init_fn, \
        .shutdown = shutdown_fn, \
        .is_available = available_fn, \
        .get_version = version_fn, \
        .description = desc, \
        .author = author, \
        .lazy_load = lazy_load_fn, \
        .lazy_unload = lazy_unload_fn, \
        .lazy_status = NL_LAZY_STATUS_UNLOADED, \
        .next = NULL, \
        .dependencies = NULL \
    }

#define NL_MODULE_GET_INFO(module_name) (&nl_module_info_##module_name)

// =========================================
// Module Capability Helpers
// =========================================

#define NL_CAP_HAS(capabilities, cap) (((capabilities) & (cap)) != 0)
#define NL_CAP_ADD(capabilities, cap) ((capabilities) |= (cap))
#define NL_CAP_REMOVE(capabilities, cap) ((capabilities) &= ~(cap))

// =========================================
// Module Query API (Main Library)
// =========================================

int nl_module_register(nl_module_info_t* info);
int nl_module_unregister(nl_module_type_t type);
nl_module_info_t* nl_module_get_info(nl_module_type_t type);
nl_module_info_t* nl_module_get_info_by_name(const char* name);
int nl_module_get_all(nl_module_info_t** modules, int max_count);
int nl_module_get_count(void);
int nl_module_is_platform_supported(nl_module_type_t type);
nl_module_status_t nl_module_get_status(nl_module_type_t type);
int nl_module_set_enabled(nl_module_type_t type, int enabled);

// =========================================
// Lazy Loading API
// =========================================

void nl_module_lazy_enable(int enable);
void nl_module_lazy_enable_module(nl_module_type_t type);
void nl_module_lazy_disable_module(nl_module_type_t type);
int nl_module_lazy_is_enabled(nl_module_type_t type);
void nl_module_lazy_clear_cache(void);
int nl_module_lazy_load(nl_module_type_t type);
int nl_module_lazy_unload(nl_module_type_t type);
nl_lazy_status_t nl_module_lazy_get_status(nl_module_type_t type);
int nl_module_lazy_is_loaded(nl_module_type_t type);
void nl_module_lazy_preload_all(void);
void nl_module_lazy_unload_all(void);

// =========================================
// Plugin System API
// =========================================

typedef void* nl_plugin_handle_t;

typedef struct {
    const char* name;
    const char* version;
    const char* description;
    const char* author;
    int (*init)(void);
    void (*shutdown)(void);
    int (*register_module)(nl_module_info_t** module_info);
} nl_plugin_descriptor_t;

#define NL_PLUGIN_EXPORT extern "C" __declspec(dllexport)

nl_plugin_handle_t nl_plugin_load(const char* plugin_path);
int nl_plugin_unload(nl_plugin_handle_t handle);
int nl_plugin_register(nl_plugin_handle_t handle);
nl_plugin_descriptor_t* nl_plugin_get_descriptor(nl_plugin_handle_t handle);
int nl_plugin_is_loaded(nl_plugin_handle_t handle);
int nl_plugin_get_count(void);
nl_plugin_handle_t* nl_plugin_get_all(int* count);
const char* nl_plugin_get_error(void);

// =========================================
// Module Dependency API
// =========================================

int nl_module_add_dependency(nl_module_type_t module, nl_module_type_t dependency);
int nl_module_remove_dependency(nl_module_type_t module, nl_module_type_t dependency);
int nl_module_check_dependencies(nl_module_type_t module);
nl_module_info_t* nl_module_get_dependencies(nl_module_type_t module);

// =========================================
// Convenience Macros for Each Module
// =========================================

#define NL_MODULE_IPC_INFO \
    NL_MODULE_DEFINE(NL_MODULE_IPC, ipc, NL_IPC_VERSION, \
                     NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_THREAD_SAFE | \
                     NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX, \
                     1, 1, 0, \
                     NULL, NULL, NULL, NULL, \
                     "Inter-process communication (Named Pipe/Unix Socket)", \
                     "508364")

#define NL_MODULE_LINKAGG_INFO \
    NL_MODULE_DEFINE(NL_MODULE_LINKAGG, linkagg, NL_LINKAGG_VERSION, \
                     NL_CAP_SERVER | NL_CAP_THREAD_SAFE | \
                     NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX, \
                     1, 1, 0, \
                     NULL, NULL, NULL, NULL, \
                     "Same-port link aggregation and load balancing", \
                     "508364")

#define NL_MODULE_AUTOROUTE_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_AUTOROUTE, autoroute, NL_AUTOROUTE_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL, \
                         1, 1, 1, \
                         nl_autoroute_init, NULL, nl_autoroute_is_available, nl_autoroute_version, \
                         "Automatic route suggestions and matching", \
                         "508364", NULL, NULL)

#define NL_MODULE_AUTOCOMPLETE_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_AUTOCOMPLETE, autocomplete, NL_AUTOCOMPLETE_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL, \
                         1, 1, 1, \
                         nl_autocomplete_init, NULL, nl_autocomplete_is_available, nl_autocomplete_version, \
                         "Auto-completion for charset and Vue imports", \
                         "508364", NULL, NULL)

#define NL_MODULE_ERRORPAGE_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_ERRORPAGE, errorpage, NL_ERRORPAGE_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL, \
                         1, 1, 1, \
                         nl_errorpage_init, NULL, nl_errorpage_is_available, nl_errorpage_version, \
                         "Template-based error pages", \
                         "508364", NULL, NULL)

#define NL_MODULE_LANG_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_LANG, lang, NL_LANG_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_ASYNC | NL_CAP_PLATFORM_ALL, \
                         1, 1, 1, \
                         nl_lang_init, NULL, nl_lang_is_available, nl_lang_version, \
                         "Multi-language error message translation", \
                         "508364", NULL, NULL)

#define NL_MODULE_VUE_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_VUE, vue, NL_VUE_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL, \
                         1, 1, 1, \
                         nl_vue_init, nl_vue_shutdown, nl_vue_is_available, nl_vue_version, \
                         "Vue.js backend support and HTML generation", \
                         "508364", NULL, NULL)

#define NL_MODULE_CORE_INFO \
    NL_MODULE_DEFINE(NL_MODULE_CORE, netleaf, NETLEAF_VERSION, \
                     NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_ASYNC | NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL, \
                     1, 1, 1, \
                     NULL, NULL, NULL, nl_version_string, \
                     "Core networking library (TCP/UDP/HTTP/WebSocket)", \
                     "508364")

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_MODULE_H
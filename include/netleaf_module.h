#ifndef NETLEAF_MODULE_H
#define NETLEAF_MODULE_H

/**
 * @file netleaf_module.h
 * @brief NL扩展系统 (NL Extension System) - 动态模块加载与管理框架
 * @version 2.4.0
 * @date 2026-09-18
 *
 * NL扩展系统是 NetLeaf 的运行时动态扩展框架，支持：
 * - 动态链接库加载与卸载
 * - 扩展自动发现与注册
 * - 扩展依赖管理与版本检查
 * - 多线程安全扩展加载
 * - 扩展生命周期管理
 * - 扩展间通信与函数调用
 * - 插件热重载
 *
 * @note 此系统通过检测 include/netleaf_xxx.h 头文件是否存在来判断扩展库是否可用
 */

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
    #ifdef NL_EXPORTS
        #define NL_API __declspec(dllexport)
    #else
        #define NL_API __declspec(dllimport)
    #endif
#else
    #define NL_API
#endif

// =========================================
// Extension Interface Export Macro (NL_EXT_API)
// =========================================
// 用于扩展库导出接口，允许插件通过 nl_extension_get_func() 访问
#ifdef _WIN32
    #ifdef NL_EXTENSION_EXPORTS
        #define NL_EXT_API __declspec(dllexport)
    #else
        #define NL_EXT_API __declspec(dllimport)
    #endif
#else
    #define NL_EXT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief NL扩展系统版本号
 */
#define NL_MODULE_VERSION "2.4.1"

/**
 * @brief NL扩展系统名称
 */
#define NL_SYSTEM_NAME "NL扩展系统"

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
    NL_MODULE_MQTT = 8,       // MQTT protocol support
    NL_MODULE_TLS = 9,        // TLS/SSL encryption support
    NL_MODULE_CUSTOM = 32,    // Custom/third-party modules
    NL_MODULE_MAX = 64        // Maximum modules (extended for plugins)
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
    NL_CAP_PLUGIN = 1 << 9,      // Is a plugin module
    NL_CAP_EXT_SYSTEM = 1 << 10, // Part of NL Extension System
    NL_CAP_TLS = 1 << 11         // TLS/SSL support
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

// Module lazy loading status
typedef enum {
    NL_MODULE_LAZY_UNLOADED = 0,
    NL_MODULE_LAZY_LOADING = 1,
    NL_MODULE_LAZY_LOADED = 2,
    NL_MODULE_LAZY_STOPPING = 3,
    NL_MODULE_LAZY_STOPPED = 4
} nl_module_lazy_status_t;

// =========================================
// Lazy Module Types (for nl_lazy_* API)
// =========================================

typedef unsigned char nl_lazy_module_t;
typedef int nl_lazy_status_t;

// Lazy module bitflags
#define NL_LAZY_MODULE_NONE         0x00
#define NL_LAZY_MODULE_HTTP         0x01
#define NL_LAZY_MODULE_WEBSOCKET    0x02
#define NL_LAZY_MODULE_TCP          0x04
#define NL_LAZY_MODULE_UDP          0x08
#define NL_LAZY_MODULE_TOML         0x10
#define NL_LAZY_MODULE_JSON         0x20
#define NL_LAZY_MODULE_SYSINFO      0x40
#define NL_LAZY_MODULE_ALL          0xFF

// Lazy status values
#define NL_LAZY_STATUS_UNLOADED     0
#define NL_LAZY_STATUS_LOADING      1
#define NL_LAZY_STATUS_LOADED       2
#define NL_LAZY_STATUS_STOPPING     3
#define NL_LAZY_STATUS_STOPPED      4

// =========================================
// Module Information Structure
// =========================================

typedef struct nl_module_info {
    nl_module_type_t type;
    const char* name;           // Module name (e.g., "ipc", "linkagg")
    const char* version;        // Version string (e.g., "2.4.0")
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
    nl_module_lazy_status_t lazy_status; // Lazy loading status

    struct nl_module_info* next; // Linked list next pointer
    struct nl_module_info* dependencies; // Module dependencies
} nl_module_info_t;

// =========================================
// Module Registration Macros
// =========================================

#define NL_MODULE_DEFINE(module_type, module_name, module_version, \
                         caps, win_support, linux_support, macos_support, \
                         init_fn, shutdown_fn, available_fn, version_fn, \
                         desc, author_str) \
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
        .author = author_str, \
        .lazy_load = NULL, \
        .lazy_unload = NULL, \
        .lazy_status = NL_MODULE_LAZY_UNLOADED, \
        .next = NULL, \
        .dependencies = NULL \
    }

#define NL_MODULE_DEFINE_LAZY(module_type, module_name, module_version, \
                             caps, win_support, linux_support, macos_support, \
                             init_fn, shutdown_fn, available_fn, version_fn, \
                             desc, author_str, lazy_load_fn, lazy_unload_fn) \
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
        .author = author_str, \
        .lazy_load = lazy_load_fn, \
        .lazy_unload = lazy_unload_fn, \
        .lazy_status = NL_MODULE_LAZY_UNLOADED, \
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

NL_API int nl_module_register(nl_module_info_t* info);
NL_API int nl_module_unregister(nl_module_type_t type);
NL_API nl_module_info_t* nl_module_get_info(nl_module_type_t type);
NL_API nl_module_info_t* nl_module_get_info_by_name(const char* name);
NL_API int nl_module_get_all(nl_module_info_t** modules, int max_count);
NL_API int nl_module_get_count(void);
NL_API int nl_module_is_platform_supported(nl_module_type_t type);
NL_API nl_module_status_t nl_module_get_status(nl_module_type_t type);
NL_API int nl_module_set_enabled(nl_module_type_t type, int enabled);
NL_API const char* nl_module_get_name(nl_module_type_t type);
NL_API const char* nl_module_get_version(nl_module_type_t type);
NL_API const char* nl_module_get_description(nl_module_type_t type);
NL_API int nl_module_has_capability(nl_module_type_t type, int cap);
NL_API int nl_module_get_capabilities(nl_module_type_t type);

// =========================================
// Lazy Loading API
// =========================================

NL_API void nl_module_lazy_enable(int enable);
NL_API void nl_module_lazy_enable_module(nl_module_type_t type);
NL_API void nl_module_lazy_disable_module(nl_module_type_t type);
NL_API int nl_module_lazy_is_enabled(nl_module_type_t type);
NL_API void nl_module_lazy_clear_cache(void);
NL_API int nl_module_lazy_load(nl_module_type_t type);
NL_API int nl_module_lazy_unload(nl_module_type_t type);
NL_API nl_module_lazy_status_t nl_module_lazy_get_status(nl_module_type_t type);
NL_API int nl_module_lazy_is_loaded(nl_module_type_t type);
NL_API void nl_module_lazy_preload_all(void);
NL_API void nl_module_lazy_unload_all(void);

// =========================================
// Plugin System API (Enhanced for Third-Party Developers)
// =========================================

typedef void* nl_plugin_handle_t;

// Plugin states
typedef enum {
    NL_PLUGIN_STATE_UNLOADED = 0,
    NL_PLUGIN_STATE_LOADING,
    NL_PLUGIN_STATE_LOADED,
    NL_PLUGIN_STATE_ERROR,
    NL_PLUGIN_STATE_UNLOADING
} nl_plugin_state_t;

// Plugin info structure (filled by plugin developer)
// Forward declarations for types defined later (struct only, typedef at definition)
struct nl_extension_dependency;
struct nl_extension_info;
struct nl_plugin_info;
// Typedef forward declarations for use inside struct nl_plugin_info
typedef struct nl_extension_dependency nl_extension_dependency_t;
typedef struct nl_extension_info nl_extension_info_t;
typedef struct nl_plugin_info nl_plugin_info_t;

struct nl_plugin_info {
    const char* name;              // Plugin display name
    const char* id;                // Unique plugin identifier (e.g., "my_plugin_v1")
    const char* version;           // Semantic version string (e.g., "1.0.0")
    const char* description;       // Short description (max 100 chars)
    const char* author;            // Author name
    const char* url;               // Plugin URL/repository (optional)
    const char* license;           // License type (e.g., "MIT", "GPL-3.0")
    
    // Capabilities
    uint32_t capabilities;         // Plugin capability flags
    uint32_t required_capabilities;// Required capabilities from host
    
    // Platform support
    int platform_windows;
    int platform_linux;
    int platform_macos;
    
    // Version requirements
    const char* min_nl_version;    // Minimum NL system version required
    
    // Callbacks
    int (*init)(void);             // Called on plugin load
    void (*shutdown)(void);        // Called on plugin unload
    int (*register_module)(nl_module_info_t** module_info); // Register as module
    int (*is_available)(void);     // Check runtime availability
    
    // Plugin-specific callbacks
    void (*on_event)(const char* event_name, void* data, void* userdata); // Event handler
    void* userdata;                // User-defined context pointer

    // =========================================
    // Plugin Dependency Management (NEW)
    // =========================================
    nl_extension_dependency_t* dependencies;      // Linked list of dependencies
    nl_extension_dependency_t** dependency_array; // Array of dependencies (for quick lookup)
    int dependency_count;                          // Total number of dependencies
    int required_dep_count;                        // Number of required dependencies
    int optional_dep_count;                        // Number of optional dependencies

    // Callback for dependency resolution
    int (*resolve_dependencies)(nl_plugin_info_t* self);

    // Function to get access to other extensions/plugins (read-only)
    nl_extension_info_t* (*get_extension)(const char* library_id);
    nl_plugin_handle_t* (*get_plugin)(const char* plugin_id);

    // Function to call functions from other extensions/plugins
    void* (*get_function)(const char* target_id, const char* func_name);

    // Access to main library functions
    void* (*get_main_library)(void);

    // Access to all loaded extensions (read-only iteration)
    nl_extension_info_t** (*get_all_extensions)(int* count);
    nl_plugin_handle_t** (*get_all_plugins)(int* count);

};

// Plugin function type definitions (for dynamic loading)
typedef nl_plugin_info_t* (*nl_plugin_get_info_func)(void);
typedef int (*nl_plugin_init_func)(void);
typedef void (*nl_plugin_shutdown_func)(void);
typedef int (*nl_plugin_register_func)(nl_module_info_t** module_info);
typedef int (*nl_plugin_is_available_func)(void);
typedef void (*nl_plugin_event_func)(const char* event_name, void* data, void* userdata);

// Plugin descriptor (retrieved after loading)
typedef struct {
    const char* name;
    const char* id;
    const char* version;
    const char* description;
    const char* author;
    int (*init)(void);
    void (*shutdown)(void);
    int (*register_module)(nl_module_info_t** module_info);
} nl_plugin_descriptor_t;

// =========================================
// Extension Library Definition (NL扩展系统)
// =========================================

// Extension library info structure for user-defined extensions
// NL扩展系统 v2.4.0: Extensions must be dynamically loaded to share global state
// Description limit: 50 Chinese characters (or equivalent byte size)

#define NL_EXTENSION_NAME_MAX_LEN 64
#define NL_EXTENSION_ID_MAX_LEN 32
#define NL_EXTENSION_VERSION_MAX_LEN 16
#define NL_EXTENSION_AUTHOR_MAX_LEN 64
#define NL_EXTENSION_DESC_MAX_LEN 150  // ~50 Chinese characters (3 bytes each)

// Extension dependency type
typedef enum {
    NL_EXT_DEP_NONE = 0,          // No dependencies
    NL_EXT_DEP_REQUIRED = 1 << 0, // Required dependency (load failure = plugin failure)
    NL_EXT_DEP_OPTIONAL = 1 << 1  // Optional dependency (missing = feature unavailable)
} nl_extension_dep_type_t;

// Extension dependency structure
struct nl_extension_dependency {
    const char* library_id;           // ID of dependent extension/library
    nl_extension_dep_type_t type;     // Required or optional
    const char* min_version;          // Minimum version required (optional)
    void* user_data;                  // User-defined context for callback
    struct nl_extension_dependency* next;
};

struct nl_extension_info {
    const char* library_name;     // Library display name (e.g., "My Extension")
    const char* library_id;       // Unique identifier string (e.g., "my_ext")
    const char* version;          // Version string (e.g., "1.0.0")
    const char* author;           // Author name (e.g., "508364")
    const char* description;      // Brief description (max 50 Chinese chars)
    const char* platforms;        // Platform support string (e.g., "Windows,Linux,MacOS")

    uint32_t capabilities;        // Capability flags
    int platform_windows;         // 1 if supported on Windows (auto-set from platforms)
    int platform_linux;           // 1 if supported on Linux (auto-set from platforms)
    int platform_macos;           // 1 if supported on macOS (auto-set from platforms)

    int32_t library_value;        // Auto-assigned value by main library (read-only, DO NOT set)

    int (*init)(void);            // Initialize extension
    void (*shutdown)(void);       // Shutdown extension
    int (*is_available)(void);     // Check if extension is available
    const char* (*get_version)(void); // Get version string

    void* (*lazy_load)(void);     // Lazy load function (optional)
    void (*lazy_unload)(void);    // Lazy unload function (optional)
    nl_module_lazy_status_t lazy_status;

    struct nl_extension_info* next;

    // =========================================
    // Dependency Management (NEW)
    // =========================================
    nl_extension_dependency_t* dependencies;      // Linked list of dependencies
    nl_extension_dependency_t** dependency_array; // Array of dependencies (for quick lookup)
    int dependency_count;                          // Total number of dependencies
    int required_dep_count;                        // Number of required dependencies
    int optional_dep_count;                        // Number of optional dependencies

    // Callback for dependency resolution
    // Return 0 on success, negative on failure
    int (*resolve_dependencies)(nl_extension_info_t* self);

    // Function to get access to other extensions (read-only)
    // Pass library_id and get extension info pointer
    nl_extension_info_t* (*get_extension)(const char* library_id);

    // Function to call functions from other extensions
    // Returns function pointer or NULL if not found
    void* (*get_extension_function)(const char* library_id, const char* func_name);

    // Access to main library functions (via function table)
    void* (*get_main_library)(void);

    // User data
    void* userdata;
};

// Platform string parsing helper (internal use)
// Parse platforms string like "Windows,Linux,MacOS" (case insensitive, any order)
#define NL_PARSE_PLATFORMS_WIN(platforms_str) \
    (strstr(platforms_str, "windows") != NULL || strstr(platforms_str, "Windows") != NULL || \
     strstr(platforms_str, "win") != NULL || strstr(platforms_str, "WIN") != NULL)

#define NL_PARSE_PLATFORMS_LINUX(platforms_str) \
    (strstr(platforms_str, "linux") != NULL || strstr(platforms_str, "Linux") != NULL || \
     strstr(platforms_str, "lin") != NULL || strstr(platforms_str, "LIN") != NULL)

#define NL_PARSE_PLATFORMS_MACOS(platforms_str) \
    (strstr(platforms_str, "macos") != NULL || strstr(platforms_str, "MacOS") != NULL || \
     strstr(platforms_str, "mac") != NULL || strstr(platforms_str, "MAC") != NULL || \
     strstr(platforms_str, "darwin") != NULL || strstr(platforms_str, "Darwin") != NULL)

// Simplified extension definition macro with platform string
// platforms: "Windows,Linux,MacOS" (case insensitive, any order, or "all" for all platforms)
// Note: library_value is auto-assigned by main library, do NOT set it manually
#define NL_EXTENSION_DEFINE(ext_id, ext_name, ext_version, ext_author, ext_desc, \
                           platforms_str, caps, \
                           init_fn, shutdown_fn, available_fn, version_fn) \
    static nl_extension_info_t nl_extension_info_##ext_id = { \
        .library_name = ext_name, \
        .library_id = #ext_id, \
        .version = ext_version, \
        .author = ext_author, \
        .description = ext_desc, \
        .platforms = platforms_str, \
        .capabilities = caps | NL_CAP_DYNAMIC | NL_CAP_LAZY_LOAD | NL_CAP_EXT_SYSTEM, \
        .platform_windows = 0, \
        .platform_linux = 0, \
        .platform_macos = 0, \
        .library_value = 0, \
        .init = init_fn, \
        .shutdown = shutdown_fn, \
        .is_available = available_fn, \
        .get_version = version_fn, \
        .lazy_load = NULL, \
        .lazy_unload = NULL, \
        .lazy_status = NL_MODULE_LAZY_UNLOADED, \
        .next = NULL \
    }

#define NL_EXTENSION_DEFINE_LAZY(ext_id, ext_name, ext_version, ext_author, ext_desc, \
                                 platforms_str, caps, \
                                 init_fn, shutdown_fn, available_fn, version_fn, \
                                 lazy_load_fn, lazy_unload_fn) \
    static nl_extension_info_t nl_extension_info_##ext_id = { \
        .library_name = ext_name, \
        .library_id = #ext_id, \
        .version = ext_version, \
        .author = ext_author, \
        .description = ext_desc, \
        .platforms = platforms_str, \
        .capabilities = caps | NL_CAP_DYNAMIC | NL_CAP_LAZY_LOAD | NL_CAP_EXT_SYSTEM, \
        .platform_windows = 0, \
        .platform_linux = 0, \
        .platform_macos = 0, \
        .library_value = 0, \
        .init = init_fn, \
        .shutdown = shutdown_fn, \
        .is_available = available_fn, \
        .get_version = version_fn, \
        .lazy_load = lazy_load_fn, \
        .lazy_unload = lazy_unload_fn, \
        .lazy_status = NL_MODULE_LAZY_UNLOADED, \
        .next = NULL \
    }

#define NL_EXTENSION_GET_INFO(ext_id) (&nl_extension_info_##ext_id)

// Extension API functions
NL_API nl_extension_info_t* nl_extension_get_info(const char* library_id);
NL_API int nl_extension_register(nl_extension_info_t* info);
NL_API int nl_extension_unregister(const char* library_id);
NL_API int nl_extension_get_count(void);
NL_API nl_extension_info_t** nl_extension_get_all(int* count);
NL_API int nl_extension_validate_description(const char* description);

// Query API - get value by identifier
NL_API int32_t nl_extension_get_value_by_id(const char* library_id);
NL_API const char* nl_extension_get_id_by_value(int32_t library_value);

// Auto-load extensions from directory
// Extensions should be placed in "extensions/" subdirectory next to netleaf.dll
// Called automatically during nl_modules_init()
NL_API int nl_extension_auto_load(void);
NL_API int nl_extension_auto_load_from_dir(const char* directory);
NL_API void nl_extension_set_auto_load_dir(const char* directory);
NL_API const char* nl_extension_get_auto_load_dir(void);

// =========================================
// Enhanced Extension Management API (v2.4.0)
// =========================================

// Extension lifecycle management
NL_API int nl_extension_init(const char* library_id);
NL_API int nl_extension_shutdown(const char* library_id);
NL_API int nl_extension_force_shutdown(const char* library_id);

// Extension state queries
NL_API int nl_extension_is_initialized(const char* library_id);
NL_API int nl_extension_is_running(const char* library_id);
NL_API nl_module_status_t nl_extension_get_state(const char* library_id);

// Extension info queries (convenience functions)
NL_API const char* nl_extension_get_name(const char* library_id);
NL_API const char* nl_extension_get_author(const char* library_id);
NL_API const char* nl_extension_get_description(const char* library_id);
NL_API uint32_t nl_extension_get_caps(const char* library_id);
NL_API int nl_extension_supports_platform(const char* library_id, const char* platform);

// Batch operations
NL_API int nl_extension_init_all(void);
NL_API int nl_extension_shutdown_all(void);
NL_API int nl_extension_force_shutdown_all(void);

// Extension discovery with filters
NL_API int nl_extension_find_by_capability(uint32_t cap, nl_extension_info_t** results, int max_count);
NL_API int nl_extension_find_by_platform(const char* platform, nl_extension_info_t** results, int max_count);
NL_API int nl_extension_find_by_name_pattern(const char* pattern, nl_extension_info_t** results, int max_count);

// Extension hot-reload support
NL_API int nl_extension_reload(const char* library_id);
NL_API int nl_extension_reload_all(void);

// Extension metadata
NL_API int nl_extension_get_metadata(const char* library_id, const char* key, char* value, size_t val_size);
NL_API int nl_extension_set_metadata(const char* library_id, const char* key, const char* value);
// Free all extension metadata (call on shutdown or manual cleanup)
NL_API int nl_extension_clear_metadata(void);

// Extension lazy loading (v2.4.1)
NL_API int nl_extension_lazy_load(const char* library_id);
NL_API int nl_extension_lazy_unload(const char* library_id);
NL_API nl_module_lazy_status_t nl_extension_lazy_get_status(const char* library_id);
NL_API int nl_extension_lazy_is_loaded(const char* library_id);

#ifdef __cplusplus
#define NL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define NL_PLUGIN_EXPORT __declspec(dllexport)
#endif

NL_API nl_plugin_handle_t nl_plugin_load(const char* plugin_path);
NL_API int nl_plugin_unload(nl_plugin_handle_t handle);
NL_API int nl_plugin_register(nl_plugin_handle_t handle);
NL_API nl_plugin_descriptor_t* nl_plugin_get_descriptor(nl_plugin_handle_t handle);
// Copy the plugin descriptor into a caller-provided buffer (thread-safe; recommended)
NL_API int nl_plugin_get_descriptor_into(nl_plugin_handle_t handle, nl_plugin_descriptor_t* desc);
NL_API int nl_plugin_is_loaded(nl_plugin_handle_t handle);
NL_API int nl_plugin_get_count(void);
NL_API nl_plugin_handle_t* nl_plugin_get_all(int* count);
NL_API const char* nl_plugin_get_error(void);

// =========================================
// Enhanced Plugin Management API
// =========================================

// Plugin discovery - scan directories for available plugins
NL_API int nl_plugin_discover(const char* directory, nl_plugin_handle_t** plugins, int max_count);
NL_API int nl_plugin_discover_all(nl_plugin_handle_t** plugins, int max_count);
NL_API int nl_plugin_search(const char* keyword, nl_plugin_handle_t** results, int max_count);

// Plugin info queries
NL_API nl_plugin_state_t nl_plugin_get_state(nl_plugin_handle_t handle);
NL_API const char* nl_plugin_get_id(nl_plugin_handle_t handle);
NL_API const char* nl_plugin_get_name(nl_plugin_handle_t handle);
NL_API const char* nl_plugin_get_version(nl_plugin_handle_t handle);
NL_API const char* nl_plugin_get_description(nl_plugin_handle_t handle);
NL_API const char* nl_plugin_get_author(nl_plugin_handle_t handle);

// Plugin dependency management
NL_API int nl_plugin_check_dependencies(nl_plugin_handle_t handle, int* missing_deps);
NL_API int nl_plugin_get_dependencies(nl_plugin_handle_t handle, nl_plugin_handle_t** deps, int max_count);
NL_API int nl_plugin_validate(nl_plugin_handle_t handle, char* error_msg, size_t error_size);

// Plugin hot reload
NL_API int nl_plugin_reload(nl_plugin_handle_t handle);
NL_API int nl_plugin_reload_all(void);

// Plugin event system
NL_API int nl_plugin_emit_event(const char* event_name, void* data, size_t data_size);
NL_API int nl_plugin_subscribe(nl_plugin_handle_t handle, const char* event_name);
NL_API int nl_plugin_unsubscribe(nl_plugin_handle_t handle, const char* event_name);

// Plugin version compatibility
NL_API int nl_plugin_check_version_compatibility(nl_plugin_handle_t handle, const char* nl_version);
NL_API const char* nl_plugin_get_min_version(nl_plugin_handle_t handle);

// Plugin sandbox/safety (optional)
NL_API int nl_plugin_enable_sandbox(nl_plugin_handle_t handle, int enable);
NL_API int nl_plugin_is_sandboxed(nl_plugin_handle_t handle);

// =========================================
// Extension Dependency Management API
// =========================================

// Add/remove dependencies
NL_API int nl_extension_add_dependency(nl_extension_info_t* ext, const char* dep_id, nl_extension_dep_type_t type, const char* min_version);
NL_API int nl_extension_remove_dependency(nl_extension_info_t* ext, const char* dep_id);
NL_API int nl_extension_clear_dependencies(nl_extension_info_t* ext);

// Query dependencies
NL_API int nl_extension_get_dependency_count(nl_extension_info_t* ext);
NL_API nl_extension_dependency_t* nl_extension_get_dependencies(nl_extension_info_t* ext);
NL_API nl_extension_dependency_t* nl_extension_get_dependency_by_id(nl_extension_info_t* ext, const char* dep_id);
NL_API int nl_extension_has_dependency(nl_extension_info_t* ext, const char* dep_id);
NL_API int nl_extension_get_required_deps(nl_extension_info_t* ext, nl_extension_dependency_t** deps, int max_count);
NL_API int nl_extension_get_optional_deps(nl_extension_info_t* ext, nl_extension_dependency_t** deps, int max_count);

// Check and resolve dependencies
NL_API int nl_extension_check_dependencies(nl_extension_info_t* ext, int require_all);
NL_API int nl_extension_resolve_dependencies(nl_extension_info_t* ext);
NL_API int nl_extension_are_dependencies_met(nl_extension_info_t* ext);

// =========================================
// Extension Access API (NEW - for plugin/extension communication)
// =========================================

// Get extension by ID (read-only access)
NL_API nl_extension_info_t* nl_extension_access(const char* library_id);

// Get function from extension by name
NL_API void* nl_extension_get_func(nl_extension_info_t* ext, const char* func_name);
NL_API void* nl_extension_get_func_by_id(const char* ext_id, const char* func_name);

// Check if extension is loaded and available
NL_API int nl_extension_is_loaded(const char* library_id);
NL_API int nl_extension_is_available(const char* library_id);

// Iterate over all extensions
NL_API int nl_extension_iterate(int (*callback)(nl_extension_info_t* ext, void* userdata), void* userdata);

// =========================================
// Plugin Dependency Management API (NEW)
// =========================================

// Add/remove plugin dependencies
NL_API int nl_plugin_add_dependency(nl_plugin_handle_t handle, const char* dep_id, nl_extension_dep_type_t type, const char* min_version);
NL_API int nl_plugin_remove_dependency(nl_plugin_handle_t handle, const char* dep_id);
NL_API int nl_plugin_clear_dependencies(nl_plugin_handle_t handle);

// Query plugin dependencies
NL_API int nl_plugin_get_dependency_count(nl_plugin_handle_t handle);
NL_API nl_extension_dependency_t* nl_plugin_get_plugin_dependencies(nl_plugin_handle_t handle);
NL_API nl_extension_dependency_t* nl_plugin_get_plugin_dependency_by_id(nl_plugin_handle_t handle, const char* dep_id);
NL_API int nl_plugin_has_dependency(nl_plugin_handle_t handle, const char* dep_id);

// Check and resolve plugin dependencies
NL_API int nl_plugin_check_plugin_dependencies(nl_plugin_handle_t handle, int require_all);
NL_API int nl_plugin_resolve_plugin_dependencies(nl_plugin_handle_t handle);
NL_API int nl_plugin_are_plugin_dependencies_met(nl_plugin_handle_t handle);

// =========================================
// Extension Version Query API
// =========================================

NL_API int nl_extension_get_version_by_id(const char* library_id, char* version_buf, size_t buf_size);
NL_API const char* nl_extension_get_platforms(const char* library_id);
NL_API uint32_t nl_extension_get_capabilities(const char* library_id);

// =========================================
// Module Dependency API
// =========================================

NL_API int nl_module_add_dependency(nl_module_type_t module, nl_module_type_t dependency);
NL_API int nl_module_remove_dependency(nl_module_type_t module, nl_module_type_t dependency);
NL_API int nl_module_check_dependencies(nl_module_type_t module);
NL_API nl_module_info_t* nl_module_get_dependencies(nl_module_type_t module);

// =========================================
// Convenience Macros for Each Module
// =========================================

#define NL_MODULE_IPC_INFO \
    NL_MODULE_DEFINE(NL_MODULE_IPC, ipc, NL_IPC_VERSION, \
                     NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_THREAD_SAFE | \
                     NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX | NL_CAP_EXT_SYSTEM, \
                     1, 1, 0, \
                     NULL, NULL, NULL, NULL, \
                     "Inter-process communication (Named Pipe/Unix Socket)", \
                     "508364")

#define NL_MODULE_LINKAGG_INFO \
    NL_MODULE_DEFINE(NL_MODULE_LINKAGG, linkagg, NL_LINKAGG_VERSION, \
                     NL_CAP_SERVER | NL_CAP_THREAD_SAFE | \
                     NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX | NL_CAP_EXT_SYSTEM, \
                     1, 1, 0, \
                     NULL, NULL, NULL, NULL, \
                     "Same-port link aggregation and load balancing", \
                     "508364")

#define NL_MODULE_AUTOROUTE_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_AUTOROUTE, autoroute, NL_AUTOROUTE_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL | NL_CAP_EXT_SYSTEM, \
                         1, 1, 1, \
                         nl_autoroute_init, NULL, nl_autoroute_is_available, nl_autoroute_version, \
                         "Automatic route suggestions and matching", \
                         "508364", NULL, NULL)

#define NL_MODULE_AUTOCOMPLETE_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_AUTOCOMPLETE, autocomplete, NL_AUTOCOMPLETE_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL | NL_CAP_EXT_SYSTEM, \
                         1, 1, 1, \
                         nl_autocomplete_init, NULL, nl_autocomplete_is_available, nl_autocomplete_version, \
                         "Auto-completion for charset and Vue imports", \
                         "508364", NULL, NULL)

#define NL_MODULE_ERRORPAGE_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_ERRORPAGE, errorpage, NL_ERRORPAGE_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL | NL_CAP_EXT_SYSTEM, \
                         1, 1, 1, \
                         nl_errorpage_init, NULL, nl_errorpage_is_available, nl_errorpage_version, \
                         "Template-based error pages", \
                         "508364", NULL, NULL)

#define NL_MODULE_LANG_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_LANG, lang, NL_LANG_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_ASYNC | NL_CAP_PLATFORM_ALL | NL_CAP_EXT_SYSTEM, \
                         1, 1, 1, \
                         nl_lang_init, NULL, nl_lang_is_available, nl_lang_version, \
                         "Multi-language error message translation", \
                         "508364", NULL, NULL)

#define NL_MODULE_VUE_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_VUE, vue, NL_VUE_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL | NL_CAP_EXT_SYSTEM, \
                         1, 1, 1, \
                         nl_vue_init, nl_vue_shutdown, nl_vue_is_available, nl_vue_version, \
                         "Vue.js backend support and HTML generation", \
                         "508364", NULL, NULL)

#define NL_MODULE_MQTT_INFO \
    NL_MODULE_DEFINE(NL_MODULE_MQTT, mqtt, NL_MQTT_VERSION, \
                     NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL | NL_CAP_EXT_SYSTEM, \
                     1, 1, 1, \
                     nl_mqtt_init, NULL, nl_mqtt_is_available, nl_mqtt_version, \
                     "MQTT v3.1.1/v5.0 protocol support", \
                     "508364")

#define NL_MODULE_TLS_INFO \
    NL_MODULE_DEFINE_LAZY(NL_MODULE_TLS, tls, NL_TLS_VERSION, \
                         NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL | NL_CAP_TLS | NL_CAP_EXT_SYSTEM, \
                         1, 1, 1, \
                         nl_tls_init, NULL, nl_tls_is_available, nl_tls_version, \
                         "TLS/SSL encryption support using mbedTLS", \
                         "508364", NULL, NULL)

#define NL_MODULE_CORE_INFO \
    NL_MODULE_DEFINE(NL_MODULE_CORE, netleaf, NETLEAF_VERSION, \
                     NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_ASYNC | NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL | NL_CAP_EXT_SYSTEM, \
                     1, 1, 1, \
                     NULL, NULL, NULL, nl_version_string, \
                     "Core networking library (TCP/UDP/HTTP/WebSocket)", \
                     "508364")

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_MODULE_H

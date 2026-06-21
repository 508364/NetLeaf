#ifndef NETLEAF_MODULE_H
#define NETLEAF_MODULE_H

#include <stddef.h>
#include <stdint.h>

// Forward declarations for Lang module functions (lazy loading)
#ifdef __cplusplus
extern "C" {
#endif
const char* nl_lang_version(void);
int nl_lang_is_available(void);
int nl_lang_init(void);
void nl_lang_shutdown(void);
#ifdef __cplusplus
}
#endif

// Lang module version (for module info)
#ifndef NL_LANG_VERSION
#define NL_LANG_VERSION "2.2.1"
#endif

#ifdef __cplusplus
extern "C" {
#endif

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
    NL_MODULE_MAX = 16        // Maximum modules
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
    NL_CAP_PLATFORM_ALL = (NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX | NL_CAP_PLATFORM_MACOS)
} nl_module_cap_t;

// Module status
typedef enum {
    NL_MODULE_STATUS_UNINITIALIZED = 0,
    NL_MODULE_STATUS_INITIALIZED = 1,
    NL_MODULE_STATUS_ERROR = 2,
    NL_MODULE_STATUS_DISABLED = 3
} nl_module_status_t;

// =========================================
// Module Information Structure
// =========================================

typedef struct nl_module_info {
    // Basic identification
    nl_module_type_t type;
    const char* name;           // Module name (e.g., "ipc", "linkagg")
    const char* version;        // Version string (e.g., "2.2.1")
    
    // Capabilities and status
    uint32_t capabilities;      // Capability flags
    nl_module_status_t status;  // Current status
    
    // Platform support
    int platform_windows;       // 1 if supported on Windows
    int platform_linux;         // 1 if supported on Linux
    int platform_macos;         // 1 if supported on macOS
    
    // Module functions (optional, NULL if not applicable)
    int (*init)(void);          // Initialize module
    void (*shutdown)(void);     // Shutdown module
    int (*is_available)(void);  // Check if module is available
    const char* (*get_version)(void); // Get version string
    
    // Description
    const char* description;    // Short description
    const char* author;         // Author info (optional)
    
    // Internal linkage
    struct nl_module_info* next; // Linked list next pointer
} nl_module_info_t;

// =========================================
// Module Registration Macro
// =========================================

// Helper macro to define module info structure
#define NL_MODULE_DEFINE(module_type, module_name, module_version, \
                         caps, win_support, linux_support, macos_support, \
                         init_fn, shutdown_fn, available_fn, version_fn, \
                         desc, author) \
    static nl_module_info_t nl_module_info_##module_name = { \
        .type = module_type, \
        .name = module_name, \
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
        .next = NULL \
    }

// Macro to get module info pointer
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

// Get module info by type
// Returns NULL if module not found or not available

// Register a module
int nl_module_register(nl_module_info_t* info);

// Unregister a module
int nl_module_unregister(nl_module_type_t type);

// Get module info by type
nl_module_info_t* nl_module_get_info(nl_module_type_t type);

// Get module info by name
nl_module_info_t* nl_module_get_info_by_name(const char* name);

// Get all registered modules
int nl_module_get_all(nl_module_info_t** modules, int max_count);

// Get module count (internal)
int nl_module_get_count(void);

// Check if module is available on current platform
int nl_module_is_platform_supported(nl_module_type_t type);

// Get module status
nl_module_status_t nl_module_get_status(nl_module_type_t type);

// Enable/disable module
int nl_module_set_enabled(nl_module_type_t type, int enabled);

// =========================================
// Convenience Macros for Each Module
// =========================================

// IPC Module Info
#define NL_MODULE_IPC_INFO \
    NL_MODULE_DEFINE(NL_MODULE_IPC, "ipc", NL_IPC_VERSION, \
                     NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_THREAD_SAFE | \
                     NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX, \
                     1, 1, 0, \
                     NULL, NULL, NULL, NULL, \
                     "Inter-process communication (Named Pipe/Unix Socket)", \
                     "NetLeaf Team")

// LinkAgg Module Info
#define NL_MODULE_LINKAGG_INFO \
    NL_MODULE_DEFINE(NL_MODULE_LINKAGG, "linkagg", NL_LINKAGG_VERSION, \
                     NL_CAP_SERVER | NL_CAP_THREAD_SAFE | \
                     NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX, \
                     1, 1, 0, \
                     NULL, NULL, NULL, NULL, \
                     "Same-port link aggregation and load balancing", \
                     "NetLeaf Team")

// Autoroute Module Info
#define NL_MODULE_AUTOROUTE_INFO \
    NL_MODULE_DEFINE(NL_MODULE_AUTOROUTE, "autoroute", NL_AUTOROUTE_VERSION, \
                     NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL, \
                     1, 1, 1, \
                     nl_autoroute_init, NULL, nl_autoroute_is_available, nl_autoroute_version, \
                     "Automatic route suggestions and matching", \
                     "NetLeaf Team")

// Autocomplete Module Info
#define NL_MODULE_AUTOCOMPLETE_INFO \
    NL_MODULE_DEFINE(NL_MODULE_AUTOCOMPLETE, "autocomplete", NL_AUTOCOMPLETE_VERSION, \
                     NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL, \
                     1, 1, 1, \
                     nl_autocomplete_init, NULL, nl_autocomplete_is_available, nl_autocomplete_version, \
                     "Auto-completion for charset and Vue imports", \
                     "NetLeaf Team")

// ErrorPage Module Info
#define NL_MODULE_ERRORPAGE_INFO \
    NL_MODULE_DEFINE(NL_MODULE_ERRORPAGE, "errorpage", NL_ERRORPAGE_VERSION, \
                     NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL, \
                     1, 1, 1, \
                     nl_errorpage_init, NULL, nl_errorpage_is_available, nl_errorpage_version, \
                     "Template-based error pages", \
                     "NetLeaf Team")

// Lang Module Info
// Note: Lang module uses lazy loading - errors are registered on-demand
#define NL_MODULE_LANG_INFO \
    NL_MODULE_DEFINE(NL_MODULE_LANG, "lang", NL_LANG_VERSION, \
                     NL_CAP_THREAD_SAFE | NL_CAP_ASYNC | NL_CAP_PLATFORM_ALL, \
                     1, 1, 1, \
                     nl_lang_init, NULL, nl_lang_is_available, nl_lang_version, \
                     "Multi-language error message translation", \
                     "NetLeaf Team")

// Core Module Info
#define NL_MODULE_CORE_INFO \
    NL_MODULE_DEFINE(NL_MODULE_CORE, "netleaf", NETLEAF_VERSION, \
                     NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_ASYNC | NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL, \
                     1, 1, 1, \
                     NULL, NULL, NULL, nl_version_string, \
                     "Core networking library (TCP/UDP/HTTP/WebSocket)", \
                     "NetLeaf Team")

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_MODULE_H
#include "netleaf.h"
#include "netleaf_module.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define getcwd _getcwd
#else
#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>
#endif

static nl_module_info_t* g_module_list = NULL;
static int g_module_count = 0;
static int g_modules_initialized = 0;
static int g_lazy_enabled = 1;

static nl_plugin_handle_t* g_plugin_handles = NULL;
static int g_plugin_count = 0;
static char g_last_error[512] = "";

static nl_module_info_t g_core_module_info = {
    .type = NL_MODULE_CORE,
    .name = "netleaf",
    .version = NETLEAF_VERSION,
    .capabilities = NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_ASYNC | NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL,
    .status = NL_MODULE_STATUS_INITIALIZED,
    .platform_windows = 1,
    .platform_linux = 1,
    .platform_macos = 1,
    .init = NULL,
    .shutdown = NULL,
    .is_available = NULL,
    .get_version = nl_version_string,
    .description = "Core networking library (TCP/UDP/HTTP/WebSocket)",
    .author = "508364",
    .lazy_load = NULL,
    .lazy_unload = NULL,
    .lazy_status = NL_MODULE_LAZY_UNLOADED,
    .next = NULL,
    .dependencies = NULL
};

NL_API nl_module_info_t* nl_core_get_module_info(void) {
    return &g_core_module_info;
}

int nl_module_register(nl_module_info_t* info) {
    if (!info) return NL_EINVAL;
    
    nl_module_info_t* existing = g_module_list;
    while (existing) {
        if (existing->type == info->type) {
            return NL_OK;
        }
        existing = existing->next;
    }
    
    info->next = g_module_list;
    g_module_list = info;
    g_module_count++;
    
    return NL_OK;
}

int nl_module_unregister(nl_module_type_t type) {
    nl_module_info_t* prev = NULL;
    nl_module_info_t* current = g_module_list;
    
    while (current) {
        if (current->type == type) {
            if (prev) {
                prev->next = current->next;
            } else {
                g_module_list = current->next;
            }
            g_module_count--;
            return NL_OK;
        }
        prev = current;
        current = current->next;
    }
    
    return NL_EINVAL;
}

nl_module_info_t* nl_module_get_info(nl_module_type_t type) {
    nl_module_info_t* current = g_module_list;
    while (current) {
        if (current->type == type) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

nl_module_info_t* nl_module_get_info_by_name(const char* name) {
    if (!name) return NULL;
    
    nl_module_info_t* current = g_module_list;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

int nl_module_get_all(nl_module_info_t** modules, int max_count) {
    if (!modules || max_count <= 0) return 0;
    
    int count = 0;
    nl_module_info_t* current = g_module_list;
    while (current && count < max_count) {
        modules[count++] = current;
        current = current->next;
    }
    
    return count;
}

int nl_module_get_count(void) {
    return g_module_count;
}

nl_module_status_t nl_module_get_status(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    return info ? info->status : NL_MODULE_STATUS_UNINITIALIZED;
}

int nl_module_set_enabled(nl_module_type_t type, int enabled) {
    nl_module_info_t* info = nl_module_get_info(type);
    if (!info) return NL_EINVAL;
    
    if (enabled) {
        info->status = NL_MODULE_STATUS_UNINITIALIZED;
    } else {
        info->status = NL_MODULE_STATUS_DISABLED;
    }
    return NL_OK;
}

int nl_module_is_platform_supported(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    if (!info) return 0;
    
#ifdef _WIN32
    return info->platform_windows;
#elif defined(__linux__)
    return info->platform_linux;
#elif defined(__APPLE__)
    return info->platform_macos;
#else
    return 0;
#endif
}

// =========================================
// Lazy Loading Implementation
// =========================================

void nl_module_lazy_enable(int enable) {
    g_lazy_enabled = enable;
}

void nl_module_lazy_enable_module(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    if (info) {
        NL_CAP_ADD(info->capabilities, NL_CAP_LAZY_LOAD);
    }
}

void nl_module_lazy_disable_module(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    if (info) {
        NL_CAP_REMOVE(info->capabilities, NL_CAP_LAZY_LOAD);
    }
}

int nl_module_lazy_is_enabled(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    if (!info) return 0;
    return NL_CAP_HAS(info->capabilities, NL_CAP_LAZY_LOAD) && g_lazy_enabled;
}

void nl_module_lazy_clear_cache(void) {
    nl_module_info_t* current = g_module_list;
    while (current) {
        if (NL_CAP_HAS(current->capabilities, NL_CAP_LAZY_LOAD)) {
            current->lazy_status = NL_MODULE_LAZY_UNLOADED;
        }
        current = current->next;
    }
}

int nl_module_lazy_load(nl_module_type_t type) {
    if (!g_lazy_enabled) return NL_EINVAL;
    
    nl_module_info_t* info = nl_module_get_info(type);
    if (!info) return NL_EINVAL;
    
    if (!NL_CAP_HAS(info->capabilities, NL_CAP_LAZY_LOAD)) return NL_EINVAL;
    
    if (info->lazy_status == NL_MODULE_LAZY_LOADED) return NL_OK;
    
    info->lazy_status = NL_MODULE_LAZY_LOADING;
    
    if (info->lazy_load) {
        info->lazy_load();
    } else if (info->init) {
        info->init();
    }
    
    info->lazy_status = NL_MODULE_LAZY_LOADED;
    info->status = NL_MODULE_STATUS_INITIALIZED;
    
    return NL_OK;
}

int nl_module_lazy_unload(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    if (!info) return NL_EINVAL;
    
    if (!NL_CAP_HAS(info->capabilities, NL_CAP_LAZY_LOAD)) return NL_EINVAL;
    
    if (info->lazy_status != NL_MODULE_LAZY_LOADED) return NL_OK;
    
    info->lazy_status = NL_MODULE_LAZY_STOPPING;
    
    if (info->lazy_unload) {
        info->lazy_unload();
    } else if (info->shutdown) {
        info->shutdown();
    }
    
    info->lazy_status = NL_MODULE_LAZY_STOPPED;
    info->status = NL_MODULE_STATUS_STOPPED;
    
    return NL_OK;
}

nl_module_lazy_status_t nl_module_lazy_get_status(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    return info ? info->lazy_status : NL_MODULE_LAZY_UNLOADED;
}

int nl_module_lazy_is_loaded(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    return info && info->lazy_status == NL_MODULE_LAZY_LOADED;
}

void nl_module_lazy_preload_all(void) {
    if (!g_lazy_enabled) return;
    
    nl_module_info_t* current = g_module_list;
    while (current) {
        if (NL_CAP_HAS(current->capabilities, NL_CAP_LAZY_LOAD)) {
            nl_module_lazy_load(current->type);
        }
        current = current->next;
    }
}

void nl_module_lazy_unload_all(void) {
    nl_module_info_t* current = g_module_list;
    while (current) {
        if (NL_CAP_HAS(current->capabilities, NL_CAP_LAZY_LOAD)) {
            nl_module_lazy_unload(current->type);
        }
        current = current->next;
    }
}

// =========================================
// Plugin System Implementation
// =========================================

static void set_last_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, args);
    va_end(args);
}

NL_API nl_plugin_handle_t nl_plugin_load(const char* plugin_path) {
    if (!plugin_path) return NULL;
    
    nl_plugin_handle_t handle = NULL;
    
#ifdef _WIN32
    handle = (nl_plugin_handle_t)LoadLibraryA(plugin_path);
    if (!handle) {
        DWORD err = GetLastError();
        char msg[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       msg, sizeof(msg), NULL);
        set_last_error("Failed to load plugin %s: %s", plugin_path, msg);
        return NULL;
    }
#else
    handle = (nl_plugin_handle_t)dlopen(plugin_path, RTLD_LAZY);
    if (!handle) {
        set_last_error("Failed to load plugin %s: %s", plugin_path, dlerror());
        return NULL;
    }
#endif
    
    nl_plugin_handle_t* new_handles = realloc(g_plugin_handles, (g_plugin_count + 1) * sizeof(nl_plugin_handle_t));
    if (!new_handles) {
#ifdef _WIN32
        FreeLibrary((HMODULE)handle);
#else
        dlclose(handle);
#endif
        set_last_error("Failed to allocate memory for plugin handles");
        return NULL;
    }
    
    g_plugin_handles = new_handles;
    g_plugin_handles[g_plugin_count++] = handle;
    
    return handle;
}

NL_API int nl_plugin_unload(nl_plugin_handle_t handle) {
    if (!handle) return NL_EINVAL;
    
    int i;
    for (i = 0; i < g_plugin_count; i++) {
        if (g_plugin_handles[i] == handle) break;
    }
    
    if (i >= g_plugin_count) return NL_EINVAL;
    
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
    
    for (int j = i; j < g_plugin_count - 1; j++) {
        g_plugin_handles[j] = g_plugin_handles[j + 1];
    }
    g_plugin_count--;
    
    if (g_plugin_count == 0) {
        free(g_plugin_handles);
        g_plugin_handles = NULL;
    }
    
    return NL_OK;
}

NL_API int nl_plugin_register(nl_plugin_handle_t handle) {
    if (!handle) return NL_EINVAL;
    
#ifdef _WIN32
    FARPROC fn = GetProcAddress((HMODULE)handle, "nl_plugin_register");
    if (!fn) {
        set_last_error("Plugin does not export nl_plugin_register");
        return NL_EINVAL;
    }
#else
    void (*fn)(void) = dlsym(handle, "nl_plugin_register");
    if (!fn) {
        set_last_error("Plugin does not export nl_plugin_register: %s", dlerror());
        return NL_EINVAL;
    }
#endif
    
    ((void (*)(void))fn)();
    return NL_OK;
}

NL_API nl_plugin_descriptor_t* nl_plugin_get_descriptor(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    
#ifdef _WIN32
    FARPROC fn = GetProcAddress((HMODULE)handle, "nl_plugin_get_descriptor");
    if (!fn) return NULL;
    return ((nl_plugin_descriptor_t* (*)(void))fn)();
#else
    nl_plugin_descriptor_t* (*fn)(void) = (nl_plugin_descriptor_t* (*)(void))dlsym(handle, "nl_plugin_get_descriptor");
    if (!fn) return NULL;
    return fn();
#endif
}

NL_API int nl_plugin_is_loaded(nl_plugin_handle_t handle) {
    if (!handle) return 0;
    
    for (int i = 0; i < g_plugin_count; i++) {
        if (g_plugin_handles[i] == handle) return 1;
    }
    return 0;
}

NL_API int nl_plugin_get_count(void) {
    return g_plugin_count;
}

NL_API nl_plugin_handle_t* nl_plugin_get_all(int* count) {
    if (!count) return NULL;
    *count = g_plugin_count;
    return (nl_plugin_handle_t*)g_plugin_handles;
}

NL_API const char* nl_plugin_get_error(void) {
    return g_last_error;
}

// =========================================
// Module Dependency Implementation
// =========================================

int nl_module_add_dependency(nl_module_type_t module, nl_module_type_t dependency) {
    nl_module_info_t* module_info = nl_module_get_info(module);
    nl_module_info_t* dep_info = nl_module_get_info(dependency);
    
    if (!module_info || !dep_info) return NL_EINVAL;
    
    nl_module_info_t* current = module_info->dependencies;
    while (current) {
        if (current->type == dependency) return NL_OK;
        current = current->next;
    }
    
    nl_module_info_t* new_dep = malloc(sizeof(nl_module_info_t));
    if (!new_dep) return NL_ENOMEM;
    
    memcpy(new_dep, dep_info, sizeof(nl_module_info_t));
    new_dep->next = module_info->dependencies;
    module_info->dependencies = new_dep;
    
    return NL_OK;
}

int nl_module_remove_dependency(nl_module_type_t module, nl_module_type_t dependency) {
    nl_module_info_t* module_info = nl_module_get_info(module);
    if (!module_info) return NL_EINVAL;
    
    nl_module_info_t* prev = NULL;
    nl_module_info_t* current = module_info->dependencies;
    
    while (current) {
        if (current->type == dependency) {
            if (prev) {
                prev->next = current->next;
            } else {
                module_info->dependencies = current->next;
            }
            free(current);
            return NL_OK;
        }
        prev = current;
        current = current->next;
    }
    
    return NL_EINVAL;
}

int nl_module_check_dependencies(nl_module_type_t module) {
    nl_module_info_t* module_info = nl_module_get_info(module);
    if (!module_info) return NL_EINVAL;
    
    nl_module_info_t* current = module_info->dependencies;
    while (current) {
        if (current->status != NL_MODULE_STATUS_INITIALIZED) {
            return NL_EAGAIN;
        }
        current = current->next;
    }
    
    return NL_OK;
}

nl_module_info_t* nl_module_get_dependencies(nl_module_type_t module) {
    nl_module_info_t* module_info = nl_module_get_info(module);
    return module_info ? module_info->dependencies : NULL;
}

// =========================================
// Main Library Module API
// =========================================

NL_API nl_module_info_t* nl_get_module(nl_module_type_t type) {
    return nl_module_get_info(type);
}

NL_API nl_module_info_t* nl_get_module_by_name(const char* name) {
    return nl_module_get_info_by_name(name);
}

NL_API int nl_get_modules(nl_module_info_t** modules, int max_count) {
    return nl_module_get_all(modules, max_count);
}

NL_API int nl_get_module_count(void) {
    return nl_module_get_count();
}

NL_API int nl_module_available(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    if (!info) return 0;
    
#ifdef _WIN32
    if (!info->platform_windows) return 0;
#elif defined(__linux__)
    if (!info->platform_linux) return 0;
#elif defined(__APPLE__)
    if (!info->platform_macos) return 0;
#endif
    
    if (info->is_available) {
        return info->is_available();
    }
    
    return 1;
}

NL_API int nl_modules_init(void) {
    if (g_modules_initialized) return NL_OK;
    
    nl_module_register(&g_core_module_info);
    
    nl_module_info_t* current = g_module_list;
    while (current) {
        if (current->init) {
            int result = current->init();
            if (result == NL_OK) {
                current->status = NL_MODULE_STATUS_INITIALIZED;
            } else {
                current->status = NL_MODULE_STATUS_ERROR;
            }
        }
        current = current->next;
    }
    
    // Auto-load extensions from extensions/ directory
    int ext_loaded = nl_extension_auto_load();
    if (ext_loaded > 0) {
        printf("[NetLeaf] Auto-loaded %d extension(s)\n", ext_loaded);
    }
    
    g_modules_initialized = 1;
    return NL_OK;
}

NL_API void nl_modules_shutdown(void) {
    if (!g_modules_initialized) return;
    
    nl_module_info_t* current = g_module_list;
    while (current) {
        if (current->shutdown) {
            current->shutdown();
        }
        current->status = NL_MODULE_STATUS_UNINITIALIZED;
        current = current->next;
    }
    
    g_modules_initialized = 0;
}

NL_API void nl_print_modules(void) {
    printf("\n========================================\n");
    printf("  NetLeaf Modules Status\n");
    printf("========================================\n\n");
    
    nl_module_info_t* current = g_module_list;
    while (current) {
        const char* status_str;
        switch (current->status) {
            case NL_MODULE_STATUS_UNINITIALIZED: status_str = "Uninitialized"; break;
            case NL_MODULE_STATUS_INITIALIZED: status_str = "Initialized"; break;
            case NL_MODULE_STATUS_ERROR: status_str = "Error"; break;
            case NL_MODULE_STATUS_DISABLED: status_str = "Disabled"; break;
            case NL_MODULE_STATUS_LOADING: status_str = "Loading"; break;
            case NL_MODULE_STATUS_STOPPED: status_str = "Stopped"; break;
            default: status_str = "Unknown"; break;
        }
        
        const char* lazy_str;
        switch (current->lazy_status) {
            case NL_MODULE_LAZY_UNLOADED: lazy_str = "Unloaded"; break;
            case NL_MODULE_LAZY_LOADING: lazy_str = "Loading"; break;
            case NL_MODULE_LAZY_LOADED: lazy_str = "Loaded"; break;
            case NL_MODULE_LAZY_STOPPING: lazy_str = "Stopping"; break;
            case NL_MODULE_LAZY_STOPPED: lazy_str = "Stopped"; break;
            default: lazy_str = "N/A"; break;
        }
        
        printf("  [%s] %s v%s\n", current->name, current->name, current->version);
        printf("    Status: %s\n", status_str);
        printf("    Lazy: %s\n", lazy_str);
        printf("    Description: %s\n", current->description);
        printf("    Platforms: Win=%d, Linux=%d, macOS=%d\n", 
               current->platform_windows, current->platform_linux, current->platform_macos);
        printf("\n");
        
        current = current->next;
    }
    
    printf("========================================\n");
    printf("  Total modules: %d\n", g_module_count);
    printf("  Total plugins: %d\n", g_plugin_count);
    printf("========================================\n\n");
}

// =========================================
// Extension Library API Implementation
// =========================================

static nl_extension_info_t* g_extension_list = NULL;
static int g_extension_count = 0;
static int32_t g_extension_value_counter = 1;  // Start from 1, 0 is reserved

// Validate description length (max 50 Chinese characters or equivalent)
NL_API int nl_extension_validate_description(const char* description) {
    if (!description) return NL_OK; // Description is optional
    
    size_t len = strlen(description);
    if (len > NL_EXTENSION_DESC_MAX_LEN) {
        return NL_EINVAL;
    }
    
    // Check for reasonable character count
    // Chinese characters are typically 3 bytes in UTF-8
    // Allow mixed content with reasonable limits
    int char_count = 0;
    const char* p = description;
    while (*p) {
        if ((*p & 0x80) == 0) {
            // ASCII character (1 byte)
            p++;
            char_count++;
        } else if ((*p & 0xE0) == 0xC0) {
            // 2-byte UTF-8
            p += 2;
            char_count++;
        } else if ((*p & 0xF0) == 0xE0) {
            // 3-byte UTF-8 (Chinese characters)
            p += 3;
            char_count++;
        } else if ((*p & 0xF8) == 0xF4) {
            // 4-byte UTF-8
            p += 4;
            char_count++;
        } else {
            p++;
            char_count++;
        }
    }
    
    // Max 50 "character units" (Chinese chars count as 1)
    if (char_count > 50) {
        return NL_EINVAL;
    }
    
    return NL_OK;
}

NL_API nl_extension_info_t* nl_extension_get_info(const char* library_id) {
    if (!library_id) return NULL;
    
    nl_extension_info_t* current = g_extension_list;
    while (current) {
        if (strcmp(current->library_id, library_id) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

NL_API int nl_extension_register(nl_extension_info_t* info) {
    if (!info) return NL_EINVAL;
    if (!info->library_id) return NL_EINVAL;
    if (!info->library_name) return NL_EINVAL;
    if (!info->version) return NL_EINVAL;
    if (!info->author) return NL_EINVAL;
    
    // Validate description length
    if (nl_extension_validate_description(info->description) != NL_OK) {
        set_last_error("Extension description exceeds limit (max 50 Chinese characters)");
        return NL_EINVAL;
    }
    
    // Check for duplicate
    nl_extension_info_t* existing = g_extension_list;
    while (existing) {
        if (strcmp(existing->library_id, info->library_id) == 0) {
            return NL_OK; // Already registered
        }
        existing = existing->next;
    }
    
    // Parse platforms string at runtime
    if (info->platforms) {
        const char* plat = info->platforms;
        info->platform_windows = (strstr(plat, "windows") || strstr(plat, "Windows") || 
                                  strstr(plat, "win") || strstr(plat, "WIN") ||
                                  strcmp(plat, "all") == 0);
        info->platform_linux = (strstr(plat, "linux") || strstr(plat, "Linux") || 
                                strstr(plat, "lin") || strstr(plat, "LIN") ||
                                strcmp(plat, "all") == 0);
        info->platform_macos = (strstr(plat, "macos") || strstr(plat, "MacOS") || 
                                strstr(plat, "mac") || strstr(plat, "MAC") ||
                                strstr(plat, "darwin") || strstr(plat, "Darwin") ||
                                strcmp(plat, "all") == 0);
    } else {
        info->platform_windows = 1;
        info->platform_linux = 1;
        info->platform_macos = 1;
    }
    
    // Auto-assign library_value
    info->library_value = g_extension_value_counter++;
    
    // Add to list
    info->next = g_extension_list;
    g_extension_list = info;
    g_extension_count++;
    
    // Initialize if init function provided
    if (info->init) {
        info->init();
    }
    
    return NL_OK;
}

NL_API int nl_extension_unregister(const char* library_id) {
    if (!library_id) return NL_EINVAL;
    
    nl_extension_info_t* prev = NULL;
    nl_extension_info_t* current = g_extension_list;
    
    while (current) {
        if (strcmp(current->library_id, library_id) == 0) {
            // Shutdown if function provided
            if (current->shutdown) {
                current->shutdown();
            }
            
            // Remove from list
            if (prev) {
                prev->next = current->next;
            } else {
                g_extension_list = current->next;
            }
            
            g_extension_count--;
            return NL_OK;
        }
        prev = current;
        current = current->next;
    }
    
    return NL_EINVAL;
}

NL_API int nl_extension_get_count(void) {
    return g_extension_count;
}

NL_API nl_extension_info_t** nl_extension_get_all(int* count) {
    if (!count) return NULL;
    
    *count = g_extension_count;
    if (g_extension_count == 0) return NULL;
    
    nl_extension_info_t** array = malloc(g_extension_count * sizeof(nl_extension_info_t*));
    if (!array) return NULL;
    
    nl_extension_info_t* current = g_extension_list;
    int i = 0;
    while (current && i < g_extension_count) {
        array[i++] = current;
        current = current->next;
    }
    
    return array;
}

// Query API - get value by identifier
NL_API int32_t nl_extension_get_value_by_id(const char* library_id) {
    if (!library_id) return 0;
    
    nl_extension_info_t* current = g_extension_list;
    while (current) {
        if (strcmp(current->library_id, library_id) == 0) {
            return current->library_value;
        }
        current = current->next;
    }
    
    return 0; // Not found
}

// Query API - get identifier by value
NL_API const char* nl_extension_get_id_by_value(int32_t library_value) {
    if (library_value == 0) return NULL;
    
    nl_extension_info_t* current = g_extension_list;
    while (current) {
        if (current->library_value == library_value) {
            return current->library_id;
        }
        current = current->next;
    }
    
    return NULL; // Not found
}

// =========================================
// Extension Auto-Load Implementation
// =========================================

static char g_extension_dir[256] = "extensions";

void nl_extension_set_auto_load_dir(const char* directory) {
    if (directory) {
        strncpy(g_extension_dir, directory, sizeof(g_extension_dir) - 1);
        g_extension_dir[sizeof(g_extension_dir) - 1] = '\0';
    }
}

NL_API const char* nl_extension_get_auto_load_dir(void) {
    return g_extension_dir;
}

NL_API int nl_extension_auto_load_from_dir(const char* directory) {
    if (!directory) return NL_EINVAL;
    
    int loaded_count = 0;
    
#ifdef _WIN32
    char search_path[512];
    snprintf(search_path, sizeof(search_path), "%s\\*.dll", directory);
    
    WIN32_FIND_DATAA find_data;
    HANDLE h_find = FindFirstFileA(search_path, &find_data);
    
    if (h_find == INVALID_HANDLE_VALUE) {
        return 0; // No extensions found
    }
    
    do {
        char ext_path[512];
        snprintf(ext_path, sizeof(ext_path), "%s\\%s", directory, find_data.cFileName);
        
        // Skip netleaf.dll itself
        if (strstr(find_data.cFileName, "netleaf.dll") != NULL ||
            strstr(find_data.cFileName, "netleaf.") == NULL) {
            continue;
        }
        
        nl_plugin_handle_t ext = nl_plugin_load(ext_path);
        if (ext) {
            // Try to get extension info
            typedef nl_extension_info_t* (*get_ext_info_func)(void);
            get_ext_info_func get_info = (get_ext_info_func)GetProcAddress((HMODULE)ext, "nl_extension_get_info");
            
            // Also try legacy function names
            if (!get_info) {
                get_info = (get_ext_info_func)GetProcAddress((HMODULE)ext, "nl_example_get_extension_info");
            }
            
            if (get_info) {
                nl_extension_info_t* info = get_info();
                if (info && nl_extension_register(info) == NL_OK) {
                    loaded_count++;
                }
            }
        }
    } while (FindNextFileA(h_find, &find_data));
    
    FindClose(h_find);
    
#else
    // Linux/macOS implementation
    DIR* dir = opendir(directory);
    if (!dir) return 0;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".so") == NULL) continue;
        
        // Skip netleaf.so itself
        if (strstr(entry->d_name, "netleaf.so") != NULL) continue;
        
        char ext_path[512];
        snprintf(ext_path, sizeof(ext_path), "%s/%s", directory, entry->d_name);
        
        nl_plugin_handle_t ext = nl_plugin_load(ext_path);
        if (ext) {
            typedef nl_extension_info_t* (*get_ext_info_func)(void);
            get_ext_info_func get_info = (get_ext_info_func)dlsym(ext, "nl_extension_get_info");
            
            if (!get_info) {
                get_info = (get_ext_info_func)dlsym(ext, "nl_example_get_extension_info");
            }
            
            if (get_info) {
                nl_extension_info_t* info = get_info();
                if (info && nl_extension_register(info) == NL_OK) {
                    loaded_count++;
                }
            }
        }
    }
    
    closedir(dir);
#endif
    
    return loaded_count;
}

NL_API int nl_extension_auto_load(void) {
    // Try multiple directories in order
    char cwd[256];
    getcwd(cwd, sizeof(cwd));
    
    // 1. Try extensions/ subdirectory relative to current working directory
    char ext_dir[512];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", cwd);
    int count = nl_extension_auto_load_from_dir(ext_dir);
    
    // 2. Try extensions/ relative to executable location
#ifdef _WIN32
    char exe_path[256];
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    // Get directory part
    char* last_slash = strrchr(exe_path, '\\');
    if (last_slash) {
        *last_slash = '\0';
        snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", exe_path);
        count += nl_extension_auto_load_from_dir(ext_dir);
    }
#endif
    
    // 3. Try configured directory
    if (strcmp(g_extension_dir, "extensions") != 0) {
        count += nl_extension_auto_load_from_dir(g_extension_dir);
    }
    
    return count;
}

// =========================================
// Boolean Value Conversion Implementation
// =========================================

// Helper function for case-insensitive string comparison
static int str_equals_ci(const char* str, const char* match) {
    if (!str || !match) return 0;
    
    while (*str && *match) {
        char c1 = *str;
        char c2 = *match;
        
        // Convert to lowercase
        if (c1 >= 'A' && c1 <= 'Z') c1 = c1 - 'A' + 'a';
        if (c2 >= 'A' && c2 <= 'Z') c2 = c2 - 'A' + 'a';
        
        if (c1 != c2) return 0;
        str++;
        match++;
    }
    
    return (*str == '\0' && *match == '\0');
}

// Check if string represents a true value
NL_API int nl_bool_is_true(const char* str) {
    if (!str) return 0;
    
    // Trim leading whitespace
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    
    // Check for true-like values
    if (str_equals_ci(str, "true")) return 1;
    if (str_equals_ci(str, "yes")) return 1;
    if (str_equals_ci(str, "on")) return 1;
    if (str_equals_ci(str, "1")) return 1;
    if (str_equals_ci(str, "enabled")) return 1;
    if (str_equals_ci(str, "enable")) return 1;
    if (str_equals_ci(str, "active")) return 1;
    if (str_equals_ci(str, "ok")) return 1;
    
    return 0;
}

// Check if string represents a false value
NL_API int nl_bool_is_false(const char* str) {
    if (!str) return 0;
    
    // Trim leading whitespace
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    
    // Check for false-like values
    if (str_equals_ci(str, "false")) return 1;
    if (str_equals_ci(str, "no")) return 1;
    if (str_equals_ci(str, "off")) return 1;
    if (str_equals_ci(str, "0")) return 1;
    if (str_equals_ci(str, "disabled")) return 1;
    if (str_equals_ci(str, "disable")) return 1;
    if (str_equals_ci(str, "inactive")) return 1;
    
    return 0;
}

// Convert string to boolean value
NL_API int nl_bool_from_string(const char* str) {
    if (!str) return 0;
    
    // First check for true values
    if (nl_bool_is_true(str)) return 1;
    
    // Then check for false values
    if (nl_bool_is_false(str)) return 0;
    
    // Default: any non-empty non-false string is considered true
    return 0;
}

// Convert boolean value to string
NL_API const char* nl_bool_to_string(int value) {
    return value ? "true" : "false";
}
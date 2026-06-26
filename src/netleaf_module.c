#include "netleaf.h"
#include "netleaf_module.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static nl_module_info_t* g_module_list = NULL;
static int g_module_count = 0;
static int g_modules_initialized = 0;
static int g_lazy_enabled = 1;

#ifdef _WIN32
static HMODULE* g_plugin_handles = NULL;
static int g_plugin_count = 0;
static char g_last_error[512] = "";
#else
static void** g_plugin_handles = NULL;
static int g_plugin_count = 0;
static char g_last_error[512] = "";
#endif

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
    .lazy_status = NL_LAZY_STATUS_UNLOADED,
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
            current->lazy_status = NL_LAZY_STATUS_UNLOADED;
        }
        current = current->next;
    }
}

int nl_module_lazy_load(nl_module_type_t type) {
    if (!g_lazy_enabled) return NL_EINVAL;
    
    nl_module_info_t* info = nl_module_get_info(type);
    if (!info) return NL_EINVAL;
    
    if (!NL_CAP_HAS(info->capabilities, NL_CAP_LAZY_LOAD)) return NL_EINVAL;
    
    if (info->lazy_status == NL_LAZY_STATUS_LOADED) return NL_OK;
    
    info->lazy_status = NL_LAZY_STATUS_LOADING;
    
    if (info->lazy_load) {
        info->lazy_load();
    } else if (info->init) {
        info->init();
    }
    
    info->lazy_status = NL_LAZY_STATUS_LOADED;
    info->status = NL_MODULE_STATUS_INITIALIZED;
    
    return NL_OK;
}

int nl_module_lazy_unload(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    if (!info) return NL_EINVAL;
    
    if (!NL_CAP_HAS(info->capabilities, NL_CAP_LAZY_LOAD)) return NL_EINVAL;
    
    if (info->lazy_status != NL_LAZY_STATUS_LOADED) return NL_OK;
    
    info->lazy_status = NL_LAZY_STATUS_STOPPING;
    
    if (info->lazy_unload) {
        info->lazy_unload();
    } else if (info->shutdown) {
        info->shutdown();
    }
    
    info->lazy_status = NL_LAZY_STATUS_STOPPED;
    info->status = NL_MODULE_STATUS_STOPPED;
    
    return NL_OK;
}

nl_lazy_status_t nl_module_lazy_get_status(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    return info ? info->lazy_status : NL_LAZY_STATUS_UNLOADED;
}

int nl_module_lazy_is_loaded(nl_module_type_t type) {
    nl_module_info_t* info = nl_module_get_info(type);
    return info && info->lazy_status == NL_LAZY_STATUS_LOADED;
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

nl_plugin_handle_t nl_plugin_load(const char* plugin_path) {
    if (!plugin_path) return NULL;
    
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(plugin_path);
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
    void* handle = dlopen(plugin_path, RTLD_LAZY);
    if (!handle) {
        set_last_error("Failed to load plugin %s: %s", plugin_path, dlerror());
        return NULL;
    }
#endif
    
    HMODULE* new_handles = realloc(g_plugin_handles, (g_plugin_count + 1) * sizeof(HMODULE));
    if (!new_handles) {
#ifdef _WIN32
        FreeLibrary(handle);
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

int nl_plugin_unload(nl_plugin_handle_t handle) {
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

int nl_plugin_register(nl_plugin_handle_t handle) {
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

nl_plugin_descriptor_t* nl_plugin_get_descriptor(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    
#ifdef _WIN32
    FARPROC fn = GetProcAddress((HMODULE)handle, "nl_plugin_get_descriptor");
    if (!fn) return NULL;
#else
    nl_plugin_descriptor_t* (*fn)(void) = dlsym(handle, "nl_plugin_get_descriptor");
    if (!fn) return NULL;
#endif
    
    return ((nl_plugin_descriptor_t* (*)(void))fn)();
}

int nl_plugin_is_loaded(nl_plugin_handle_t handle) {
    if (!handle) return 0;
    
    for (int i = 0; i < g_plugin_count; i++) {
        if (g_plugin_handles[i] == handle) return 1;
    }
    return 0;
}

int nl_plugin_get_count(void) {
    return g_plugin_count;
}

nl_plugin_handle_t* nl_plugin_get_all(int* count) {
    if (!count) return NULL;
    *count = g_plugin_count;
    return (nl_plugin_handle_t*)g_plugin_handles;
}

const char* nl_plugin_get_error(void) {
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
        if (current->status != NL_MODULE_STATUS_INITIALIZED &&
            current->status != NL_MODULE_STATUS_LOADED) {
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
            case NL_LAZY_STATUS_UNLOADED: lazy_str = "Unloaded"; break;
            case NL_LAZY_STATUS_LOADING: lazy_str = "Loading"; break;
            case NL_LAZY_STATUS_LOADED: lazy_str = "Loaded"; break;
            case NL_LAZY_STATUS_STOPPING: lazy_str = "Stopping"; break;
            case NL_LAZY_STATUS_STOPPED: lazy_str = "Stopped"; break;
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
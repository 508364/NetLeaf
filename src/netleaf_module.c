/**
 * NetLeaf Module Management Implementation
 * Unified interface for all independent modules
 */

#include "netleaf.h"
#include "netleaf_module.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =========================================
// Global Module Registry
// =========================================

static nl_module_info_t* g_module_list = NULL;
static int g_module_count = 0;
static int g_modules_initialized = 0;

// =========================================
// Core Module Info
// =========================================

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
    .author = "NetLeaf Team",
    .next = NULL
};

NL_API nl_module_info_t* nl_core_get_module_info(void) {
    return &g_core_module_info;
}

// =========================================
// Module Registry Implementation
// =========================================

int nl_module_register(nl_module_info_t* info) {
    if (!info) return NL_EINVAL;
    
    // Check if already registered
    nl_module_info_t* existing = g_module_list;
    while (existing) {
        if (existing->type == info->type) {
            return NL_OK; // Already registered
        }
        existing = existing->next;
    }
    
    // Add to list
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
    
    // Check platform support
#ifdef _WIN32
    if (!info->platform_windows) return 0;
#elif defined(__linux__)
    if (!info->platform_linux) return 0;
#elif defined(__APPLE__)
    if (!info->platform_macos) return 0;
#endif
    
    // Check if module has availability function
    if (info->is_available) {
        return info->is_available();
    }
    
    return 1;
}

NL_API int nl_modules_init(void) {
    if (g_modules_initialized) return NL_OK;
    
    // Register core module first
    nl_module_register(&g_core_module_info);
    
    // Initialize each module
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
    
    // Shutdown each module
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
            default: status_str = "Unknown"; break;
        }
        
        printf("  [%s] %s v%s\n", current->name, current->name, current->version);
        printf("    Status: %s\n", status_str);
        printf("    Description: %s\n", current->description);
        printf("    Platforms: Win=%d, Linux=%d, macOS=%d\n", 
               current->platform_windows, current->platform_linux, current->platform_macos);
        printf("\n");
        
        current = current->next;
    }
    
    printf("========================================\n");
    printf("  Total modules: %d\n", g_module_count);
    printf("========================================\n\n");
}
// Link Aggregation module - platform-specific implementations
// This file provides stubs; actual implementations are in platform-specific files.
#include "netleaf_linkagg.h"
#include "netleaf_module.h"
#include "netleaf_linkagg_internal.h"
#include "netleaf_linkagg_lang.h"
#include <stdlib.h>
#include <string.h>

// =========================================
// External function declarations (implemented in platform files)
// =========================================

extern void nl_lagg_init_global_mutex(void);
extern void nl_lagg_mutex_lock(nl_lagg_mutex_t* m);
extern void nl_lagg_mutex_unlock(nl_lagg_mutex_t* m);

// =========================================
// Global ID Registry for Conflict Detection
// =========================================

static nl_lagg_id_entry_t* g_id_registry = NULL;
static nl_lagg_mutex_t g_id_registry_mutex;

bool nl_lagg_id_exists(const char* id) {
    if (!id) return false;
    nl_lagg_init_global_mutex();
    nl_lagg_mutex_lock(&g_id_registry_mutex);
    nl_lagg_id_entry_t* entry = g_id_registry;
    while (entry) {
        if (strcmp(entry->id, id) == 0) {
            nl_lagg_mutex_unlock(&g_id_registry_mutex);
            return true;
        }
        entry = entry->next;
    }
    nl_lagg_mutex_unlock(&g_id_registry_mutex);
    return false;
}

bool nl_lagg_id_register(const char* id) {
    if (!id) return false;
    if (nl_lagg_id_exists(id)) return false;

    nl_lagg_id_entry_t* entry = (nl_lagg_id_entry_t*)malloc(sizeof(nl_lagg_id_entry_t));
    if (!entry) return false;

    strncpy(entry->id, id, sizeof(entry->id) - 1);
    entry->id[sizeof(entry->id) - 1] = '\0';
    entry->next = NULL;

    nl_lagg_init_global_mutex();
    nl_lagg_mutex_lock(&g_id_registry_mutex);
    entry->next = g_id_registry;
    g_id_registry = entry;
    nl_lagg_mutex_unlock(&g_id_registry_mutex);
    return true;
}

void nl_lagg_id_unregister(const char* id) {
    if (!id) return;
    nl_lagg_init_global_mutex();
    nl_lagg_mutex_lock(&g_id_registry_mutex);
    nl_lagg_id_entry_t* prev = NULL;
    nl_lagg_id_entry_t* entry = g_id_registry;
    while (entry) {
        if (strcmp(entry->id, id) == 0) {
            if (prev) prev->next = entry->next;
            else g_id_registry = entry->next;
            free(entry);
            break;
        }
        prev = entry;
        entry = entry->next;
    }
    nl_lagg_mutex_unlock(&g_id_registry_mutex);
}

// =========================================
// ID Format Validation
// =========================================

int nl_lagg_validate_id_format(const char* id) {
    if (!id) return NL_LINKAGG_ERROR_INVALID_PARAM;

    size_t len = strlen(id);

    // Check length
    if (len < NL_LINKAGG_ID_MIN_LENGTH || len >= NL_LINKAGG_ID_MAX_LENGTH) {
        return NL_LINKAGG_ERROR_ID_FORMAT;
    }

    // Must contain exactly one dot
    const char* dot = strchr(id, '.');
    if (!dot) return NL_LINKAGG_ERROR_ID_FORMAT;

    // Dot must not be at start or end
    if (dot == id || dot[1] == '\0') return NL_LINKAGG_ERROR_ID_FORMAT;

    // Must not have additional dots
    if (strchr(dot + 1, '.') != NULL) return NL_LINKAGG_ERROR_ID_FORMAT;

    // Both parts before and after dot must not be empty
    if (dot == id || dot[1] == '\0') return NL_LINKAGG_ERROR_ID_FORMAT;

    return NL_LINKAGG_OK;
}

// =========================================
// Port ID Management
// =========================================

int nl_lagg_set_port_id(nl_lagg_server_t* server, const char* id) {
    if (!server || !id) return NL_LINKAGG_ERROR_INVALID_PARAM;

    // Validate ID format (must be xxx.xxx)
    int validate_result = nl_lagg_validate_id_format(id);
    if (validate_result != NL_LINKAGG_OK) {
        return validate_result;
    }

    // Check for ID conflict (process-wide)
    if (nl_lagg_id_exists(id)) {
        return NL_LINKAGG_ERROR_ID_CONFLICT;  // "不接入，ID被占用"
    }

    // Unregister old ID if exists
    if (server->port_id[0] != '\0') {
        nl_lagg_id_unregister(server->port_id);
    }

    // Register new ID
    if (!nl_lagg_id_register(id)) {
        return NL_LINKAGG_ERROR_ID_CONFLICT;
    }

    // Set the port ID
    strncpy(server->port_id, id, sizeof(server->port_id) - 1);
    server->port_id[sizeof(server->port_id) - 1] = '\0';

    return NL_LINKAGG_OK;
}

const char* nl_lagg_get_port_id(nl_lagg_server_t* server) {
    if (!server) return NULL;
    return server->port_id[0] ? server->port_id : NULL;
}

// Stub implementations for module info functions
int nl_lagg_is_available(void) {
#if defined(_WIN32) || defined(__linux__)
    return 1;
#else
    return 0;
#endif
}

const char* nl_lagg_version(void) {
    return NL_LINKAGG_VERSION;
}

// Module initialization - registers language errors
static int linkagg_init(void) {
    NL_LINKAGG_REGISTER_LANG();
    return 0;
}

// Module shutdown
static void linkagg_shutdown(void) {
    nl_lang_unregister_lib(NL_LIB_LINKAGG);
}

// Module info structure
static nl_module_info_t g_linkagg_module_info = {
    .type = NL_MODULE_LINKAGG,
    .name = "linkagg",
    .version = NL_LINKAGG_VERSION,
    .capabilities = NL_CAP_SERVER | NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX,
    .status = NL_MODULE_STATUS_UNINITIALIZED,
    .platform_windows = 1,
    .platform_linux = 1,
    .platform_macos = 0,
    .init = linkagg_init,
    .shutdown = linkagg_shutdown,
    .is_available = nl_lagg_is_available,
    .get_version = nl_lagg_version,
    .description = "Same-port link aggregation and load balancing",
    .author = "NetLeaf Team",
    .next = NULL
};

nl_module_info_t* nl_lagg_get_module_info(void) {
    return &g_linkagg_module_info;
}

// Stub implementations - platform files override these
// The real implementations are in netleaf_linkagg_windows.c / netleaf_linkagg_linux.c

// These symbols will be linked from platform-specific object files
// when building with BUILD_LINKAGG=ON

#include "netleaf_ipc.h"
#include "netleaf_ipc_internal.h"
#include "netleaf_module.h"
#include <string.h>
#include <ctype.h>

// Module state
static int g_ipc_enabled = 0;
static int g_ipc_available = 0;

// Module info structure
static nl_module_info_t g_ipc_module_info = {
    .type = NL_MODULE_IPC,
    .name = "ipc",
    .version = NL_IPC_VERSION,
    .capabilities = NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX,
    .status = NL_MODULE_STATUS_UNINITIALIZED,
    .platform_windows = 1,
    .platform_linux = 1,
    .platform_macos = 0,
    .init = NULL,
    .shutdown = NULL,
    .is_available = nl_ipc_is_available,
    .get_version = nl_ipc_version,
    .description = "Inter-process communication (Named Pipe/Unix Socket)",
    .author = "NetLeaf Team",
    .next = NULL
};

nl_module_info_t* nl_ipc_get_module_info(void) {
    return &g_ipc_module_info;
}

// Internal helper: case-insensitive string comparison
static int strncasecmp_custom(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2 || tolower((unsigned char)*s1) == tolower((unsigned char)*s2))) {
        s1++; s2++; n--;
    }
    return (n == 0) ? 0 : *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

void nl_ipc_enable_ex(const char* enable_str) {
    if (!enable_str) return;
    const char* s = enable_str;
    if (strncasecmp_custom(s, "true", 4) == 0) {
        g_ipc_enabled = 1;
        g_ipc_available = 1;
    } else if (strncasecmp_custom(s, "false", 5) == 0) {
        g_ipc_enabled = 0;
    } else if (strncasecmp_custom(s, "on", 2) == 0 || strncasecmp_custom(s, "yes", 3) == 0) {
        g_ipc_enabled = 1;
        g_ipc_available = 1;
    } else if (strncasecmp_custom(s, "off", 3) == 0 || strncasecmp_custom(s, "no", 2) == 0) {
        g_ipc_enabled = 0;
    } else if (s[0] == '1') {
        g_ipc_enabled = 1;
        g_ipc_available = 1;
    } else if (s[0] == '0') {
        g_ipc_enabled = 0;
    }
}

int nl_ipc_is_enabled(void) {
    return g_ipc_enabled;
}

void nl_ipc_enable(int enable) {
    g_ipc_enabled = enable;
    g_ipc_available = (enable != 0);
}

int nl_ipc_is_available(void) {
    return g_ipc_available;
}

const char* nl_ipc_version(void) {
    return NL_IPC_VERSION;
}

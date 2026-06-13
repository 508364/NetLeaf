#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#include "../../include/netleaf.h"

static int g_lazy_enabled = 1;
static unsigned char g_loaded_modules = 0;
static unsigned char g_enabled_modules = NL_LAZY_MODULE_ALL;
static unsigned char g_module_status[8] = {0};
static int g_thread_count = 4;
static pthread_mutex_t g_lazy_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_module_status(nl_lazy_module_t module, nl_lazy_status_t status) {
    unsigned char idx = 0;
    nl_lazy_module_t m = 1;
    while (m <= module && idx < 8) {
        if (module & m) {
            g_module_status[idx] = status;
        }
        m <<= 1;
        idx++;
    }
}

static nl_lazy_status_t get_module_status(nl_lazy_module_t module) {
    unsigned char idx = 0;
    nl_lazy_module_t m = 1;
    while (m <= module && idx < 8) {
        if (module & m) {
            return g_module_status[idx];
        }
        m <<= 1;
        idx++;
    }
    return NL_LAZY_STATUS_UNLOADED;
}

static void load_http_module(void) {
    if (g_loaded_modules & NL_LAZY_MODULE_HTTP) return;
    
    pthread_mutex_lock(&g_lazy_mutex);
    if (!(g_loaded_modules & NL_LAZY_MODULE_HTTP)) {
        set_module_status(NL_LAZY_MODULE_HTTP, NL_LAZY_STATUS_LOADING);
        g_loaded_modules |= NL_LAZY_MODULE_HTTP;
        set_module_status(NL_LAZY_MODULE_HTTP, NL_LAZY_STATUS_LOADED);
    }
    pthread_mutex_unlock(&g_lazy_mutex);
}

static void load_websocket_module(void) {
    if (g_loaded_modules & NL_LAZY_MODULE_WEBSOCKET) return;
    
    pthread_mutex_lock(&g_lazy_mutex);
    if (!(g_loaded_modules & NL_LAZY_MODULE_WEBSOCKET)) {
        set_module_status(NL_LAZY_MODULE_WEBSOCKET, NL_LAZY_STATUS_LOADING);
        g_loaded_modules |= NL_LAZY_MODULE_WEBSOCKET;
        set_module_status(NL_LAZY_MODULE_WEBSOCKET, NL_LAZY_STATUS_LOADED);
    }
    pthread_mutex_unlock(&g_lazy_mutex);
}

static void load_tcp_module(void) {
    if (g_loaded_modules & NL_LAZY_MODULE_TCP) return;
    
    pthread_mutex_lock(&g_lazy_mutex);
    if (!(g_loaded_modules & NL_LAZY_MODULE_TCP)) {
        set_module_status(NL_LAZY_MODULE_TCP, NL_LAZY_STATUS_LOADING);
        g_loaded_modules |= NL_LAZY_MODULE_TCP;
        set_module_status(NL_LAZY_MODULE_TCP, NL_LAZY_STATUS_LOADED);
    }
    pthread_mutex_unlock(&g_lazy_mutex);
}

static void load_udp_module(void) {
    if (g_loaded_modules & NL_LAZY_MODULE_UDP) return;
    
    pthread_mutex_lock(&g_lazy_mutex);
    if (!(g_loaded_modules & NL_LAZY_MODULE_UDP)) {
        set_module_status(NL_LAZY_MODULE_UDP, NL_LAZY_STATUS_LOADING);
        g_loaded_modules |= NL_LAZY_MODULE_UDP;
        set_module_status(NL_LAZY_MODULE_UDP, NL_LAZY_STATUS_LOADED);
    }
    pthread_mutex_unlock(&g_lazy_mutex);
}

static void load_toml_module(void) {
    if (g_loaded_modules & NL_LAZY_MODULE_TOML) return;
    
    pthread_mutex_lock(&g_lazy_mutex);
    if (!(g_loaded_modules & NL_LAZY_MODULE_TOML)) {
        set_module_status(NL_LAZY_MODULE_TOML, NL_LAZY_STATUS_LOADING);
        g_loaded_modules |= NL_LAZY_MODULE_TOML;
        set_module_status(NL_LAZY_MODULE_TOML, NL_LAZY_STATUS_LOADED);
    }
    pthread_mutex_unlock(&g_lazy_mutex);
}

static void load_json_module(void) {
    if (g_loaded_modules & NL_LAZY_MODULE_JSON) return;
    
    pthread_mutex_lock(&g_lazy_mutex);
    if (!(g_loaded_modules & NL_LAZY_MODULE_JSON)) {
        set_module_status(NL_LAZY_MODULE_JSON, NL_LAZY_STATUS_LOADING);
        g_loaded_modules |= NL_LAZY_MODULE_JSON;
        set_module_status(NL_LAZY_MODULE_JSON, NL_LAZY_STATUS_LOADED);
    }
    pthread_mutex_unlock(&g_lazy_mutex);
}

static void load_sysinfo_module(void) {
    if (g_loaded_modules & NL_LAZY_MODULE_SYSINFO) return;
    
    pthread_mutex_lock(&g_lazy_mutex);
    if (!(g_loaded_modules & NL_LAZY_MODULE_SYSINFO)) {
        set_module_status(NL_LAZY_MODULE_SYSINFO, NL_LAZY_STATUS_LOADING);
        g_loaded_modules |= NL_LAZY_MODULE_SYSINFO;
        set_module_status(NL_LAZY_MODULE_SYSINFO, NL_LAZY_STATUS_LOADED);
    }
    pthread_mutex_unlock(&g_lazy_mutex);
}

static void unload_module(nl_lazy_module_t module) {
    pthread_mutex_lock(&g_lazy_mutex);
    set_module_status(module, NL_LAZY_STATUS_STOPPING);
    g_loaded_modules &= ~module;
    set_module_status(module, NL_LAZY_STATUS_STOPPED);
    pthread_mutex_unlock(&g_lazy_mutex);
}

NL_API void nl_lazy_enable(int enable) {
    pthread_mutex_lock(&g_lazy_mutex);
    g_lazy_enabled = enable;
    pthread_mutex_unlock(&g_lazy_mutex);
}

NL_API void nl_lazy_enable_module(nl_lazy_module_t module) {
    pthread_mutex_lock(&g_lazy_mutex);
    g_enabled_modules |= module;
    pthread_mutex_unlock(&g_lazy_mutex);
}

NL_API void nl_lazy_disable_module(nl_lazy_module_t module) {
    pthread_mutex_lock(&g_lazy_mutex);
    g_enabled_modules &= ~module;
    pthread_mutex_unlock(&g_lazy_mutex);
}

NL_API int nl_lazy_is_enabled(nl_lazy_module_t module) {
    return (g_enabled_modules & module) != 0;
}

NL_API void nl_lazy_clear_all_cache(void) {
    pthread_mutex_lock(&g_lazy_mutex);
    g_loaded_modules = 0;
    memset(g_module_status, 0, sizeof(g_module_status));
    pthread_mutex_unlock(&g_lazy_mutex);
    
    nl_sys_info_clear_cache();
}

NL_API void nl_lazy_preload_module(nl_lazy_module_t module) {
    if (!g_lazy_enabled) return;
    
    if (module & NL_LAZY_MODULE_HTTP) load_http_module();
    if (module & NL_LAZY_MODULE_WEBSOCKET) load_websocket_module();
    if (module & NL_LAZY_MODULE_TCP) load_tcp_module();
    if (module & NL_LAZY_MODULE_UDP) load_udp_module();
    if (module & NL_LAZY_MODULE_TOML) load_toml_module();
    if (module & NL_LAZY_MODULE_JSON) load_json_module();
    if (module & NL_LAZY_MODULE_SYSINFO) load_sysinfo_module();
}

NL_API void nl_lazy_stop_module(nl_lazy_module_t module) {
    if (module & NL_LAZY_MODULE_HTTP) unload_module(NL_LAZY_MODULE_HTTP);
    if (module & NL_LAZY_MODULE_WEBSOCKET) unload_module(NL_LAZY_MODULE_WEBSOCKET);
    if (module & NL_LAZY_MODULE_TCP) unload_module(NL_LAZY_MODULE_TCP);
    if (module & NL_LAZY_MODULE_UDP) unload_module(NL_LAZY_MODULE_UDP);
    if (module & NL_LAZY_MODULE_TOML) unload_module(NL_LAZY_MODULE_TOML);
    if (module & NL_LAZY_MODULE_JSON) unload_module(NL_LAZY_MODULE_JSON);
    if (module & NL_LAZY_MODULE_SYSINFO) {
        unload_module(NL_LAZY_MODULE_SYSINFO);
        nl_sys_info_clear_cache();
    }
}

NL_API nl_lazy_status_t nl_lazy_get_module_status(nl_lazy_module_t module) {
    return get_module_status(module);
}

NL_API int nl_lazy_is_module_loaded(nl_lazy_module_t module) {
    return (g_loaded_modules & module) != 0;
}

NL_API int nl_lazy_set_thread_count(int count) {
    if (count < 1 || count > 256) return -1;
    pthread_mutex_lock(&g_lazy_mutex);
    g_thread_count = count;
    pthread_mutex_unlock(&g_lazy_mutex);
    return 0;
}

NL_API int nl_lazy_get_thread_count(void) {
    return g_thread_count;
}

void nl_lazy_ensure_module(nl_lazy_module_t module) {
    if (!g_lazy_enabled) return;
    
    nl_lazy_preload_module(module);
}

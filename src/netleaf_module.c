#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// L3: 元数据互斥量改用 InitOnceExecuteOnce 做一次性初始化，该 API 要求 Vista+ 目标。
// 在包含任何系统头之前给出安全下限，未显式指定 _WIN32_WINNT 时才生效。
#if defined(_WIN32) && !defined(_WIN32_WINNT)
    #define _WIN32_WINNT 0x0600
#endif

/**
 * @brief NetLeaf Extension System Implementation
 * @version 2.4.0-00000000
 * @date 2026-09-13
 *
 * Implements module registry, extension management, plugin loading,
 * and dependency resolution for the NL control system.
 *
 * Architecture: Pure core library with runtime-loaded extensions.
 * Each extension (lang, ipc, autoroute, etc.) is a separate shared library.
 */

#include "netleaf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>   // tolower() 在所有平台均被使用，需无条件包含

#ifdef _WIN32
    #include <windows.h>
    // MSVC/Windows 不提供 <strings.h> 与 strcasecmp()，用 _stricmp() 兼容（M12）
    #define strcasecmp _stricmp
#else
    #include <strings.h>
    #include <pthread.h>
    #include <dirent.h>
    #include <dlfcn.h>
    #include <unistd.h>
    #include <sys/stat.h>
#endif

// Portable strdup for Windows
#if defined(_WIN32) && !defined(__cplusplus)
    #ifndef strdup
        #define strdup _strdup
    #endif
#endif

#define MAX_MODULES 64
#define MAX_EXTENSIONS 64
#define MAX_PLUGINS 32

// ============================================================
// 线程安全原语（纯 C99 跨平台，用于扩展元数据链表加锁）
// ============================================================
#ifdef _WIN32
    typedef CRITICAL_SECTION nl_meta_mutex_t;
    #define NL_META_MUTEX_INIT(m)    InitializeCriticalSection(m)
    #define NL_META_MUTEX_LOCK(m)    EnterCriticalSection(m)
    #define NL_META_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
    #define NL_META_MUTEX_DESTROY(m) DeleteCriticalSection(m)
    // L3: 一次性初始化回调（供 InitOnceExecuteOnce 调用），保证多线程首调时锁只初始化一次
    static BOOL CALLBACK nl_meta_mutex_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
        (void)once;
        (void)ctx;
        NL_META_MUTEX_INIT((nl_meta_mutex_t*)param);
        return TRUE;
    }
#else
    typedef pthread_mutex_t nl_meta_mutex_t;
    #define NL_META_MUTEX_INIT(m)    pthread_mutex_init(m, NULL)
    #define NL_META_MUTEX_LOCK(m)    pthread_mutex_lock(m)
    #define NL_META_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
    #define NL_META_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

// ============================================================
// Module Registry
// ============================================================

static nl_module_info_t* g_module_registry[MAX_MODULES];
static int g_module_count = 0;
static int g_modules_initialized = 0;
static int g_lazy_enabled = 1;

// ============================================================
// Extension Registry
// ============================================================

static nl_extension_info_t* g_extension_registry[MAX_EXTENSIONS];
static int g_extension_count = 0;
static char* g_extension_ids[MAX_EXTENSIONS];
static char* g_extension_names[MAX_EXTENSIONS];
static int g_extension_values[MAX_EXTENSIONS];
// H7: 保留动态加载扩展的库句柄（dlopen/LoadLibrary）。
// 注册表登记的是扩展自身静态结构体指针，一旦卸载该库，指针即悬垂，
// 因此必须把句柄一直持有，直到 unregister 或关机时才真正卸载。
static void* g_extension_dl_handles[MAX_EXTENSIONS];
static int g_next_library_value = 1;
static char g_auto_load_dir[512] = "";

// ============================================================
// Plugin Registry
// ============================================================

typedef struct {
    nl_plugin_handle_t handle;
    nl_plugin_state_t state;
    nl_plugin_info_t* info;
    void* dl_handle;
    // L4: 每个插件在注册表槽位内各自持有一份描述符，
    // 取代原先 nl_plugin_get_descriptor() 的函数内 static 存储，
    // 避免多句柄/多线程调用时互相覆盖。
    nl_plugin_descriptor_t descriptor;
} nl_plugin_entry_t;

static nl_plugin_entry_t g_plugin_registry[MAX_PLUGINS];
// H8: g_plugin_count 现在表示“有效槽位数量”（装载 +1，卸载 -1），
// 不再是曾用过的槽位上界，故所有遍历必须以 MAX_PLUGINS 为界。
static int g_plugin_count = 0;
static char g_plugin_error[512] = "";

// L4: 把插件信息填充到调用方给定的描述符缓冲
static void fill_plugin_descriptor(const nl_plugin_info_t* info, nl_plugin_descriptor_t* desc) {
    if (!desc) return;
    desc->name = info ? info->name : NULL;
    desc->id = info ? info->id : NULL;
    desc->version = info ? info->version : NULL;
    desc->description = info ? info->description : NULL;
    desc->author = info ? info->author : NULL;
    desc->init = info ? info->init : NULL;
    desc->shutdown = info ? info->shutdown : NULL;
    desc->register_module = info ? info->register_module : NULL;
}

// ============================================================
// Core Module Info (defined here since netleaf.c has no standalone module info)
// ============================================================

static nl_module_info_t g_core_module_info = {
    .type = NL_MODULE_CORE,
    .name = "netleaf",
    .version = NETLEAF_VERSION,
    .capabilities = NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_ASYNC | NL_CAP_THREAD_SAFE |
                    NL_CAP_PLATFORM_ALL | NL_CAP_EXT_SYSTEM,
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
    .next = NULL
};

// ============================================================
// Platform Detection
// ============================================================

static int is_windows(void) {
#ifdef _WIN32
    return 1;
#endif
    return 0;
}

static int is_linux(void) {
#ifndef _WIN32
#ifdef __linux__
    return 1;
#endif
#endif
    (void)is_windows;
    return 0;
}

static int is_macos(void) {
#ifdef __APPLE__
    return 1;
#endif
    (void)is_windows;
    (void)is_linux;
    return 0;
}

// ============================================================
// Internal Helpers
// ============================================================

static int find_module_index(nl_module_type_t type) {
    (void)type;
    return -1;
}

static int find_extension_index(const char* library_id) {
    for (int i = 0; i < g_extension_count; i++) {
        if (g_extension_ids[i] && strcmp(g_extension_ids[i], library_id) == 0) {
            return i;
        }
    }
    return -1;
}

// H7: 统一关闭动态库句柄（跨平台）
static void close_extension_library(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

// H7: 把已加载扩展的库句柄登记到注册表槽位。
// 注册的是扩展库内部静态结构体的指针，只有一直持有句柄才能保证该指针有效。
static void retain_extension_dl_handle(nl_extension_info_t* ext, void* handle) {
    if (!handle) return;
    if (!ext || !ext->library_id) {
        close_extension_library(handle);
        return;
    }
    int idx = find_extension_index(ext->library_id);
    if (idx < 0 || idx >= MAX_EXTENSIONS) {
        close_extension_library(handle);
        return;
    }
    // 若该槽位已有句柄（重复加载），先关闭旧句柄防止泄漏
    if (g_extension_dl_handles[idx] && g_extension_dl_handles[idx] != handle) {
        close_extension_library(g_extension_dl_handles[idx]);
    }
    g_extension_dl_handles[idx] = handle;
}

static int get_or_assign_library_value(nl_extension_info_t* ext) {
    int idx = find_extension_index(ext->library_id);
    if (idx >= 0) {
        return g_extension_values[idx];
    }
    int val = g_next_library_value++;
    if (g_extension_count < MAX_EXTENSIONS) {
        g_extension_values[g_extension_count] = val;
    }
    return val;
}

// L12: 读取一个版本分段（以 '.' 分隔），输出其中“有效数字”的起止区间（跳过前导零），
// 并把游标推进到下一个分段起点。仅做字符串级比较，不再把数字段累加进整型，
// 从根本上杜绝超长纯数字段 `n = n*10 + ...` 造成的 int 溢出（UB）。
static void read_version_segment(const char** pos, const char** start, size_t* len) {
    const char* p = *pos;
    const char* seg_begin = p;
    while (*p && *p != '.') p++;            // 分段结束于 '.' 或字符串末尾
    const char* seg_end = p;
    const char* d = seg_begin;
    while (d < seg_end && *d == '0') d++;   // 跳过前导零，得到有效数字区间
    *start = d;
    *len = (size_t)(seg_end - d);
    if (*p == '.') p++;                     // 跳过分隔符，指向下一分段
    *pos = p;
}

static int version_compare(const char* v1, const char* v2) {
    if (!v1 || !v2) return 0;
    while (*v1 || *v2) {
        const char* s1 = NULL;
        const char* s2 = NULL;
        size_t l1 = 0, l2 = 0;
        read_version_segment(&v1, &s1, &l1);
        read_version_segment(&v2, &s2, &l2);
        // 有效位数不同：位数多者更大（例如 "10" > "9"）
        if (l1 != l2) return l1 < l2 ? -1 : 1;
        // 位数相同且非空：逐位比较即可，数值必然可比较且不溢出
        if (l1 > 0) {
            int cmp = memcmp(s1, s2, l1);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
        }
    }
    return 0;
}

static int parse_platforms(const char* platforms_str, int* win, int* lin, int* mac) {
    // L8: 输出指针判空，避免调用方漏传导致崩溃
    if (!win || !lin || !mac) return 0;
    // L8: platforms 字符串判空；为空时视为不支持任何平台
    if (!platforms_str) {
        *win = *lin = *mac = 0;
        return 0;
    }
    char buf[256];
    strncpy(buf, platforms_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char* p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);

    *win = (strstr(buf, "windows") != NULL || strstr(buf, "win") != NULL);
    *lin = (strstr(buf, "linux") != NULL || strstr(buf, "lin") != NULL);
    *mac = (strstr(buf, "macos") != NULL || strstr(buf, "mac") != NULL || strstr(buf, "darwin") != NULL);

    if (strstr(buf, "all") != NULL) {
        *win = *lin = *mac = 1;
    }
    return 1;
}

static const char* get_platform_string(int win, int lin, int mac) {
    (void)win; (void)lin; (void)mac;
    static char buf[64];
    buf[0] = '\0';
    return buf;
}

// ============================================================
// Dependency Management for Extensions
// ============================================================

NL_API int nl_extension_add_dependency(nl_extension_info_t* ext, const char* dep_id, nl_extension_dep_type_t type, const char* min_version) {
    if (!ext || !dep_id) return -1;

    nl_extension_dependency_t* dep = (nl_extension_dependency_t*)malloc(sizeof(nl_extension_dependency_t));
    if (!dep) return -1;

    dep->library_id = dep_id;
    dep->type = type;
    dep->min_version = min_version;
    dep->user_data = NULL;
    dep->next = ext->dependencies;
    ext->dependencies = dep;
    ext->dependency_count++;

    if (type & NL_EXT_DEP_REQUIRED) ext->required_dep_count++;
    if (type & NL_EXT_DEP_OPTIONAL) ext->optional_dep_count++;

    return 0;
}

NL_API int nl_extension_remove_dependency(nl_extension_info_t* ext, const char* dep_id) {
    if (!ext || !dep_id) return -1;

    nl_extension_dependency_t** pp = &ext->dependencies;
    while (*pp) {
        if (strcmp((*pp)->library_id, dep_id) == 0) {
            nl_extension_dependency_t* tmp = *pp;
            *pp = tmp->next;
            tmp->next = NULL;
            ext->dependency_count--;
            if (tmp->type & NL_EXT_DEP_REQUIRED) ext->required_dep_count--;
            if (tmp->type & NL_EXT_DEP_OPTIONAL) ext->optional_dep_count--;
            free(tmp);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

NL_API int nl_extension_clear_dependencies(nl_extension_info_t* ext) {
    if (!ext) return -1;

    nl_extension_dependency_t* curr = ext->dependencies;
    while (curr) {
        nl_extension_dependency_t* next = curr->next;
        curr->next = NULL;
        free(curr);
        curr = next;
    }
    ext->dependencies = NULL;
    ext->dependency_count = 0;
    ext->required_dep_count = 0;
    ext->optional_dep_count = 0;
    return 0;
}

NL_API int nl_extension_get_dependency_count(nl_extension_info_t* ext) {
    if (!ext) return 0;
    return ext->dependency_count;
}

NL_API nl_extension_dependency_t* nl_extension_get_dependencies(nl_extension_info_t* ext) {
    if (!ext) return NULL;
    return ext->dependencies;
}

NL_API nl_extension_dependency_t* nl_extension_get_dependency_by_id(nl_extension_info_t* ext, const char* dep_id) {
    if (!ext || !dep_id) return NULL;
    for (nl_extension_dependency_t* d = ext->dependencies; d; d = d->next) {
        if (strcmp(d->library_id, dep_id) == 0) return d;
    }
    return NULL;
}

NL_API int nl_extension_has_dependency(nl_extension_info_t* ext, const char* dep_id) {
    return nl_extension_get_dependency_by_id(ext, dep_id) != NULL;
}

NL_API int nl_extension_get_required_deps(nl_extension_info_t* ext, nl_extension_dependency_t** deps, int max_count) {
    if (!ext) return 0;
    int count = 0;
    for (nl_extension_dependency_t* d = ext->dependencies; d && count < max_count; d = d->next) {
        if (d->type & NL_EXT_DEP_REQUIRED) deps[count++] = d;
    }
    return count;
}

NL_API int nl_extension_get_optional_deps(nl_extension_info_t* ext, nl_extension_dependency_t** deps, int max_count) {
    if (!ext) return 0;
    int count = 0;
    for (nl_extension_dependency_t* d = ext->dependencies; d && count < max_count; d = d->next) {
        if (d->type & NL_EXT_DEP_OPTIONAL) deps[count++] = d;
    }
    return count;
}

NL_API int nl_extension_check_dependencies(nl_extension_info_t* ext, int require_all) {
    if (!ext) return -1;
    for (nl_extension_dependency_t* d = ext->dependencies; d; d = d->next) {
        if (d->type & NL_EXT_DEP_REQUIRED) {
            if (find_extension_index(d->library_id) < 0) {
                if (require_all) return -1;
            }
        }
    }
    return 0;
}

NL_API int nl_extension_resolve_dependencies(nl_extension_info_t* ext) {
    if (!ext) return -1;
    if (ext->resolve_dependencies) {
        return ext->resolve_dependencies(ext);
    }
    return nl_extension_check_dependencies(ext, 1);
}

NL_API int nl_extension_are_dependencies_met(nl_extension_info_t* ext) {
    if (!ext) return 0;
    return nl_extension_check_dependencies(ext, 1) == 0;
}

// ============================================================
// Extension Access API
// ============================================================

NL_API nl_extension_info_t* nl_extension_access(const char* library_id) {
    if (!library_id) return NULL;
    int idx = find_extension_index(library_id);
    if (idx >= 0) return g_extension_registry[idx];
    return NULL;
}

NL_API void* nl_extension_get_func(nl_extension_info_t* ext, const char* func_name) {
    if (!ext || !func_name) return NULL;
    if (ext->get_extension_function) {
        return ext->get_extension_function(ext->library_id, func_name);
    }
    return NULL;
}

NL_API void* nl_extension_get_func_by_id(const char* ext_id, const char* func_name) {
    if (!ext_id || !func_name) return NULL;
    nl_extension_info_t* ext = nl_extension_access(ext_id);
    if (!ext) return NULL;
    return nl_extension_get_func(ext, func_name);
}

NL_API int nl_extension_is_loaded(const char* library_id) {
    return find_extension_index(library_id) >= 0;
}

NL_API int nl_extension_is_available(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_access(library_id);
    if (!ext) return 0;
    if (ext->is_available) return ext->is_available();
    return 1;
}

NL_API int nl_extension_iterate(int (*callback)(nl_extension_info_t* ext, void* userdata), void* userdata) {
    if (!callback) return -1;
    for (int i = 0; i < g_extension_count; i++) {
        if (g_extension_registry[i]) {
            if (callback(g_extension_registry[i], userdata) != 0) return i;
        }
    }
    return g_extension_count;
}

// ============================================================
// Extension Core API
// ============================================================

NL_API nl_extension_info_t* nl_extension_get_info(const char* library_id) {
    return nl_extension_access(library_id);
}

NL_API int nl_extension_register(nl_extension_info_t* info) {
    if (!info || !info->library_id) return -1;

    if (find_extension_index(info->library_id) >= 0) return 0;
    if (g_extension_count >= MAX_EXTENSIONS) return -1;

    int idx = g_extension_count;

    // L4: 先在栈上完成全部内存分配，任一 strdup 失败即整体回滚、不占用槽位。
    // 原实现先递增 g_extension_count 再 strdup，一旦 OOM 使 g_extension_ids[idx]=NULL，
    // find_extension_index() 会跳过该 NULL 槽位，导致同一个 library_id 被重复注册
    // （同时留下无法访问的“幽灵槽位”）。
    char* id_copy = strdup(info->library_id);
    if (!id_copy) return -1;
    const char* name_src = info->library_name ? info->library_name : info->library_id;
    char* name_copy = strdup(name_src);
    if (!name_copy) {
        free(id_copy);
        return -1;
    }

    // 先分配 library_value（从 1 开始，0 保留），再登记 id。
    // 注意：取值必须在 g_extension_count 递增前完成，
    // 否则该 helper 会扫描到本槽位（id 已插入但值仍为 0）并错误地返回 0。
    int value = get_or_assign_library_value(info);

    // 分配全部成功后再一次性提交，保证注册表状态要么完整、要么不变。
    g_extension_values[idx] = value;
    g_extension_registry[idx] = info;
    g_extension_ids[idx] = id_copy;
    g_extension_names[idx] = name_copy;
    g_extension_count = idx + 1;

    info->library_value = (int32_t)value;
    parse_platforms(info->platforms, &info->platform_windows, &info->platform_linux, &info->platform_macos);

    return 0;
}

NL_API int nl_extension_unregister(const char* library_id) {
    if (!library_id) return -1;
    int idx = find_extension_index(library_id);
    if (idx < 0) return -1;

    free(g_extension_ids[idx]);
    free(g_extension_names[idx]);

    // H7: 仅在显式注销时卸载动态库（此前库已被保留在注册表中）
    close_extension_library(g_extension_dl_handles[idx]);

    for (int i = idx; i < g_extension_count - 1; i++) {
        g_extension_registry[i] = g_extension_registry[i + 1];
        g_extension_ids[i] = g_extension_ids[i + 1];
        g_extension_names[i] = g_extension_names[i + 1];
        g_extension_values[i] = g_extension_values[i + 1];
        g_extension_dl_handles[i] = g_extension_dl_handles[i + 1];
    }
    g_extension_registry[--g_extension_count] = NULL;
    g_extension_ids[g_extension_count] = NULL;
    g_extension_names[g_extension_count] = NULL;
    g_extension_dl_handles[g_extension_count] = NULL;
    return 0;
}

NL_API int nl_extension_get_count(void) {
    return g_extension_count;
}

NL_API nl_extension_info_t** nl_extension_get_all(int* count) {
    if (count) *count = g_extension_count;
    nl_extension_info_t** arr = (nl_extension_info_t**)malloc(sizeof(nl_extension_info_t*) * (size_t)(g_extension_count + 1));
    if (!arr) { if (count) *count = 0; return NULL; }
    for (int i = 0; i < g_extension_count; i++) arr[i] = g_extension_registry[i];
    arr[g_extension_count] = NULL;
    return arr;
}

NL_API int nl_extension_validate_description(const char* description) {
    if (!description) return 1;
    size_t len = strlen(description);
    return (int)(len <= NL_EXTENSION_DESC_MAX_LEN);
}

NL_API int32_t nl_extension_get_value_by_id(const char* library_id) {
    if (!library_id) return -1;
    int idx = find_extension_index(library_id);
    return idx >= 0 ? (int32_t)g_extension_values[idx] : -1;
}

NL_API const char* nl_extension_get_id_by_value(int32_t library_value) {
    for (int i = 0; i < g_extension_count; i++) {
        if (g_extension_values[i] == (int)library_value) return g_extension_ids[i];
    }
    return NULL;
}

// ============================================================
// Extension Version/Info Query API
// ============================================================

NL_API int nl_extension_get_version_by_id(const char* library_id, char* version_buf, size_t buf_size) {
    nl_extension_info_t* ext = nl_extension_access(library_id);
    if (!ext) return -1;
    if (ext->get_version) {
        const char* ver = ext->get_version();
        if (version_buf && buf_size > 0) snprintf(version_buf, buf_size, "%s", ver ? ver : "");
        return 0;
    }
    if (version_buf && buf_size > 0) snprintf(version_buf, buf_size, "%s", ext->version ? ext->version : "");
    return 0;
}

NL_API const char* nl_extension_get_platforms(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_access(library_id);
    return ext ? ext->platforms : NULL;
}

NL_API uint32_t nl_extension_get_capabilities(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_access(library_id);
    return ext ? ext->capabilities : 0;
}

// ============================================================
// Auto-Load Extensions
// ============================================================

// Symbol name patterns to try when loading an extension by filename
// e.g., "netleaf_lang.dll" → tries "nl_lang_get_extension_info", then "nl_lang_get_module_info"
static const char* g_ext_symbol_patterns[] = {
    // Primary: get_extension_info pattern (module_name → nl_module_name_get_extension_info)
    "nl_lang_get_extension_info",
    "nl_ipc_get_extension_info",
    "nl_autoroute_get_extension_info",
    "nl_autocomplete_get_extension_info",
    "nl_errorpage_get_extension_info",
    "nl_vue_get_extension_info",
    "nl_lagg_get_extension_info",
    // Fallback: get_module_info (cast to extension info)
    "nl_lang_get_module_info",
    "nl_ipc_get_module_info",
    "nl_autoroute_get_module_info",
    "nl_autocomplete_get_module_info",
    "nl_errorpage_get_module_info",
    "nl_vue_get_module_info",
    "nl_lagg_get_module_info",
    // Legacy example pattern
    "nl_example_get_extension_info",
    NULL
};

static int auto_load_extension_file(const char* filepath) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(filepath);
    if (!h) return -1;

    typedef nl_extension_info_t* (*GetExtInfoFunc)(void);

    // Try standard symbol names first
    for (int i = 0; g_ext_symbol_patterns[i] != NULL; i++) {
        GetExtInfoFunc get_info = (GetExtInfoFunc)GetProcAddress(h, g_ext_symbol_patterns[i]);
        if (get_info) {
            nl_extension_info_t* ext = get_info();
            if (ext) {
                // L5: 注册失败（注册表已满 / 内存不足）必须视为加载失败，
                // 不能忽略返回值继续 return 0 假装成功。
                if (nl_extension_register(ext) != 0) {
                    FreeLibrary(h);
                    return -1;
                }
                // H7: 句柄交由注册表保留，不能在此 FreeLibrary，
                // 否则 ext 指向的模块内存被释放，注册表将持有悬垂指针。
                retain_extension_dl_handle(ext, (void*)h);
                return 0;
            }
        }
    }

    FreeLibrary(h);
    return -1;
#else
    void* h = dlopen(filepath, RTLD_NOW | RTLD_LOCAL);
    if (!h) return -1;

    typedef nl_extension_info_t* (*GetExtInfoFunc)(void);

    // Try standard symbol names first
    for (int i = 0; g_ext_symbol_patterns[i] != NULL; i++) {
        GetExtInfoFunc get_info = (GetExtInfoFunc)dlsym(h, g_ext_symbol_patterns[i]);
        if (get_info) {
            nl_extension_info_t* ext = get_info();
            if (ext) {
                // L5: 注册失败（注册表已满 / 内存不足）必须视为加载失败，
                // 此时尚未保留句柄，直接 dlclose 并返回错误。
                if (nl_extension_register(ext) != 0) {
                    dlclose(h);
                    return -1;
                }
                // H7: 保留 dlopen 句柄到注册表，禁止此处 dlclose。
                // 原实现加载成功后立即 dlclose，注册表持有悬垂指针（use-after-unload）。
                retain_extension_dl_handle(ext, h);
                return 0;
            }
        }
    }

    dlclose(h);
    return -1;
#endif
}

NL_API int nl_extension_auto_load(void) {
    return nl_extension_auto_load_from_dir(g_auto_load_dir[0] ? g_auto_load_dir : NULL);
}

NL_API int nl_extension_auto_load_from_dir(const char* directory) {
    if (!directory) {
#ifdef _WIN32
        directory = "extensions";
#else
        directory = "./extensions";
#endif
    }

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s/*", directory);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int loaded = 0;
    do {
        size_t len = strlen(fd.cFileName);
        if (len < 4) continue;
        const char* ext = fd.cFileName + len - 4;
        if (strcasecmp(ext, ".dll") != 0) continue;
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", directory, fd.cFileName);
        if (auto_load_extension_file(filepath) == 0) loaded++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return loaded;
#else
    DIR* dir = opendir(directory);
    if (!dir) return 0;

    int loaded = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 4) continue;
        const char* ext = ent->d_name + len - 4;
        if (strcmp(ext, ".so") != 0 && strcmp(ext, ".so.1") != 0) continue;
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", directory, ent->d_name);
        if (auto_load_extension_file(filepath) == 0) loaded++;
    }
    closedir(dir);
    return loaded;
#endif
}

NL_API void nl_extension_set_auto_load_dir(const char* directory) {
    if (directory) {
        strncpy(g_auto_load_dir, directory, sizeof(g_auto_load_dir) - 1);
        g_auto_load_dir[sizeof(g_auto_load_dir) - 1] = '\0';
    } else {
        g_auto_load_dir[0] = '\0';
    }
}

NL_API const char* nl_extension_get_auto_load_dir(void) {
    return g_auto_load_dir;
}

// ============================================================
// Enhanced Extension Management API (v2.4.0)
// ============================================================

NL_API int nl_extension_init(const char* library_id) {
    if (!library_id) return -1;
    nl_extension_info_t* ext = nl_extension_access(library_id);
    if (!ext) return -1;
    if (ext->init) {
        int rc = ext->init();
        if (rc == 0) {
            // Update status in registry if it exists
            int idx = find_extension_index(library_id);
            if (idx >= 0) {
                // Note: we don't store status in the registry, but we can check the extension directly
            }
        }
        return rc;
    }
    return 0;
}

NL_API int nl_extension_shutdown(const char* library_id) {
    if (!library_id) return -1;
    nl_extension_info_t* ext = nl_extension_access(library_id);
    if (!ext) return -1;
    if (ext->shutdown) {
        ext->shutdown();
        return 0;
    }
    return 0;
}

NL_API int nl_extension_force_shutdown(const char* library_id) {
    if (!library_id) return -1;
    // Unregister and clean up
    return nl_extension_unregister(library_id);
}

NL_API int nl_extension_is_initialized(const char* library_id) {
    if (!library_id) return 0;
    nl_extension_info_t* ext = nl_extension_access(library_id);
    if (!ext) return 0;
    if (ext->is_available) return ext->is_available();
    return 1;
}

NL_API int nl_extension_is_running(const char* library_id) {
    if (!library_id) return 0;
    nl_extension_info_t* ext = nl_extension_access(library_id);
    if (!ext) return 0;
    if (ext->is_available) return ext->is_available();
    return 1;
}

NL_API nl_module_status_t nl_extension_get_state(const char* library_id) {
    if (!library_id) return NL_MODULE_STATUS_UNINITIALIZED;
    nl_extension_info_t* ext = nl_extension_access(library_id);
    if (!ext) return NL_MODULE_STATUS_UNINITIALIZED;
    if (ext->is_available && ext->is_available()) {
        return NL_MODULE_STATUS_INITIALIZED;
    }
    return NL_MODULE_STATUS_UNINITIALIZED;
}

NL_API const char* nl_extension_get_name(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_access(library_id);
    return ext ? ext->library_name : NULL;
}

NL_API const char* nl_extension_get_author(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_access(library_id);
    return ext ? ext->author : NULL;
}

NL_API const char* nl_extension_get_description(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_access(library_id);
    return ext ? ext->description : NULL;
}

NL_API uint32_t nl_extension_get_caps(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_access(library_id);
    return ext ? ext->capabilities : 0;
}

NL_API int nl_extension_supports_platform(const char* library_id, const char* platform) {
    if (!library_id || !platform) return 0;
    nl_extension_info_t* ext = nl_extension_access(library_id);
    if (!ext) return 0;
    if (strcasecmp(platform, "windows") == 0 || strcasecmp(platform, "win") == 0) {
        return ext->platform_windows;
    } else if (strcasecmp(platform, "linux") == 0) {
        return ext->platform_linux;
    } else if (strcasecmp(platform, "macos") == 0 || strcasecmp(platform, "darwin") == 0) {
        return ext->platform_macos;
    }
    return 0;
}

NL_API int nl_extension_init_all(void) {
    int count = 0;
    for (int i = 0; i < g_extension_count; i++) {
        if (g_extension_registry[i] && g_extension_registry[i]->init) {
            if (g_extension_registry[i]->init() == 0) count++;
        }
    }
    return count;
}

NL_API int nl_extension_shutdown_all(void) {
    int count = 0;
    for (int i = 0; i < g_extension_count; i++) {
        if (g_extension_registry[i] && g_extension_registry[i]->shutdown) {
            g_extension_registry[i]->shutdown();
            count++;
        }
    }
    return count;
}

NL_API int nl_extension_force_shutdown_all(void) {
    int count = g_extension_count;
    // H7: 关机时统一卸载所有动态加载的扩展库
    for (int i = 0; i < g_extension_count; i++) {
        close_extension_library(g_extension_dl_handles[i]);
        g_extension_dl_handles[i] = NULL;
        free(g_extension_ids[i]);
        free(g_extension_names[i]);
        g_extension_ids[i] = NULL;
        g_extension_names[i] = NULL;
        g_extension_registry[i] = NULL;
    }
    g_extension_count = 0;
    return count;
}

NL_API int nl_extension_find_by_capability(uint32_t cap, nl_extension_info_t** results, int max_count) {
    if (!results || max_count <= 0) return 0;
    int count = 0;
    for (int i = 0; i < g_extension_count && count < max_count; i++) {
        if (g_extension_registry[i] && NL_CAP_HAS(g_extension_registry[i]->capabilities, cap)) {
            results[count++] = g_extension_registry[i];
        }
    }
    return count;
}

NL_API int nl_extension_find_by_platform(const char* platform, nl_extension_info_t** results, int max_count) {
    if (!platform || !results || max_count <= 0) return 0;
    int count = 0;
    for (int i = 0; i < g_extension_count && count < max_count; i++) {
        if (!g_extension_registry[i]) continue;
        nl_extension_info_t* ext = g_extension_registry[i];
        int supported = 0;
        if (strcasecmp(platform, "windows") == 0 || strcasecmp(platform, "win") == 0) {
            supported = ext->platform_windows;
        } else if (strcasecmp(platform, "linux") == 0) {
            supported = ext->platform_linux;
        } else if (strcasecmp(platform, "macos") == 0 || strcasecmp(platform, "darwin") == 0) {
            supported = ext->platform_macos;
        }
        if (supported) {
            results[count++] = ext;
        }
    }
    return count;
}

NL_API int nl_extension_find_by_name_pattern(const char* pattern, nl_extension_info_t** results, int max_count) {
    if (!pattern || !results || max_count <= 0) return 0;
    int count = 0;
    for (int i = 0; i < g_extension_count && count < max_count; i++) {
        if (!g_extension_registry[i]) continue;
        nl_extension_info_t* ext = g_extension_registry[i];
        if (ext->library_id && strstr(ext->library_id, pattern)) {
            results[count++] = ext;
        } else if (ext->library_name && strstr(ext->library_name, pattern)) {
            results[count++] = ext;
        }
    }
    return count;
}

NL_API int nl_extension_reload(const char* library_id) {
    if (!library_id) return -1;
    // Force shutdown and re-initialize
    nl_extension_shutdown(library_id);
    return nl_extension_init(library_id);
}

NL_API int nl_extension_reload_all(void) {
    int count = 0;
    for (int i = 0; i < g_extension_count; i++) {
        if (g_extension_registry[i] && g_extension_registry[i]->library_id) {
            if (nl_extension_reload(g_extension_registry[i]->library_id) == 0) count++;
        }
    }
    return count;
}

// 扩展元数据（key/value）存储（v2.4.1）
typedef struct nl_extension_metadata_entry {
    char* library_id;
    char* key;
    char* value;
    struct nl_extension_metadata_entry* next;
} nl_extension_metadata_entry_t;

static nl_extension_metadata_entry_t* g_extension_metadata = NULL;

// L9: 元数据链表此前无锁保护、无释放接口。这里为其加锁并提供清理 API。
#ifdef _WIN32
static nl_meta_mutex_t g_metadata_mutex;
#else
// L3: POSIX 直接用静态初始化器，编译期即完成初始化，无需运行时判断，天然线程安全
static nl_meta_mutex_t g_metadata_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static void ensure_metadata_mutex(void) {
#ifdef _WIN32
    // L3: 原先的 `if(!ready){INIT;ready=1;}` 非原子，多线程首次调用可能重复初始化锁。
    // 改用 InitOnceExecuteOnce，由系统保证回调至多执行一次。
    static INIT_ONCE g_metadata_mutex_once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&g_metadata_mutex_once, nl_meta_mutex_init_once, &g_metadata_mutex, NULL);
#else
    // POSIX 分支：静态初始化器已就绪，无需额外动作
#endif
}

// L9: 释放全部扩展元数据（关机或主动清理时调用）
NL_API int nl_extension_clear_metadata(void) {
    ensure_metadata_mutex();
    NL_META_MUTEX_LOCK(&g_metadata_mutex);
    nl_extension_metadata_entry_t* cur = g_extension_metadata;
    while (cur) {
        nl_extension_metadata_entry_t* next = cur->next;
        free(cur->library_id);
        free(cur->key);
        free(cur->value);
        free(cur);
        cur = next;
    }
    g_extension_metadata = NULL;
    NL_META_MUTEX_UNLOCK(&g_metadata_mutex);
    return 0;
}

NL_API int nl_extension_set_metadata(const char* library_id, const char* key, const char* value) {
    nl_extension_metadata_entry_t* cur;
    if (!library_id || !key) return -1;

    ensure_metadata_mutex();
    NL_META_MUTEX_LOCK(&g_metadata_mutex);

    for (cur = g_extension_metadata; cur; cur = cur->next) {
        if (strcmp(cur->library_id, library_id) == 0 && strcmp(cur->key, key) == 0) {
            char* new_value = value ? strdup(value) : NULL;
            if (value && !new_value) {
                NL_META_MUTEX_UNLOCK(&g_metadata_mutex);
                return -1;
            }
            free(cur->value);
            cur->value = new_value;
            NL_META_MUTEX_UNLOCK(&g_metadata_mutex);
            return 0;
        }
    }

    cur = (nl_extension_metadata_entry_t*)calloc(1, sizeof(*cur));
    if (!cur) {
        NL_META_MUTEX_UNLOCK(&g_metadata_mutex);
        return -1;
    }
    cur->library_id = strdup(library_id);
    cur->key = strdup(key);
    cur->value = value ? strdup(value) : NULL;
    if (!cur->library_id || !cur->key || (value && !cur->value)) {
        free(cur->library_id);
        free(cur->key);
        free(cur->value);
        free(cur);
        NL_META_MUTEX_UNLOCK(&g_metadata_mutex);
        return -1;
    }
    cur->next = g_extension_metadata;
    g_extension_metadata = cur;
    NL_META_MUTEX_UNLOCK(&g_metadata_mutex);
    return 0;
}

NL_API int nl_extension_get_metadata(const char* library_id, const char* key, char* value, size_t val_size) {
    nl_extension_metadata_entry_t* cur;
    if (!library_id || !key) return -1;

    ensure_metadata_mutex();
    NL_META_MUTEX_LOCK(&g_metadata_mutex);

    for (cur = g_extension_metadata; cur; cur = cur->next) {
        if (strcmp(cur->library_id, library_id) == 0 && strcmp(cur->key, key) == 0) {
            if (value && val_size > 0) {
                if (cur->value) {
                    strncpy(value, cur->value, val_size - 1);
                    value[val_size - 1] = '\0';
                } else {
                    value[0] = '\0';
                }
            }
            NL_META_MUTEX_UNLOCK(&g_metadata_mutex);
            return 0;
        }
    }
    NL_META_MUTEX_UNLOCK(&g_metadata_mutex);
    return -1;
}

// ============================================================
// 扩展懒加载 API（v2.4.1）
// 复用扩展结构体中的 lazy_load / lazy_unload / lazy_status 字段
// ============================================================

NL_API int nl_extension_lazy_load(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_get_info(library_id);
    if (!ext || !ext->lazy_load) return -1;
    if (ext->lazy_status == NL_MODULE_LAZY_LOADED) return 0;

    ext->lazy_status = NL_MODULE_LAZY_LOADING;
    if (ext->lazy_load() == NULL) {
        ext->lazy_status = NL_MODULE_LAZY_UNLOADED;
        return -1;
    }
    ext->lazy_status = NL_MODULE_LAZY_LOADED;
    return 0;
}

NL_API int nl_extension_lazy_unload(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_get_info(library_id);
    if (!ext || !ext->lazy_unload) return -1;
    if (ext->lazy_status != NL_MODULE_LAZY_LOADED) return -1;

    ext->lazy_status = NL_MODULE_LAZY_STOPPING;
    ext->lazy_unload();
    ext->lazy_status = NL_MODULE_LAZY_STOPPED;
    return 0;
}

NL_API nl_module_lazy_status_t nl_extension_lazy_get_status(const char* library_id) {
    nl_extension_info_t* ext = nl_extension_get_info(library_id);
    if (!ext) return NL_MODULE_LAZY_UNLOADED;
    return ext->lazy_status;
}

NL_API int nl_extension_lazy_is_loaded(const char* library_id) {
    return nl_extension_lazy_get_status(library_id) == NL_MODULE_LAZY_LOADED;
}

// ============================================================
// Module Registry API
// ============================================================

NL_API int nl_module_register(nl_module_info_t* info) {
    if (!info || !info->name) return -1;

    if (nl_module_get_info(info->type)) return 0;
    if (g_module_count >= MAX_MODULES) return -1;

    g_module_registry[g_module_count++] = info;
    info->status = NL_MODULE_STATUS_INITIALIZED;
    return 0;
}

NL_API int nl_module_unregister(nl_module_type_t type) {
    int idx = -1;
    for (int i = 0; i < g_module_count; i++) {
        if (g_module_registry[i] && g_module_registry[i]->type == type) { idx = i; break; }
    }
    if (idx < 0) return -1;

    if (g_module_registry[idx]->shutdown) g_module_registry[idx]->shutdown();
    g_module_registry[idx]->status = NL_MODULE_STATUS_UNINITIALIZED;

    for (int i = idx; i < g_module_count - 1; i++) {
        g_module_registry[i] = g_module_registry[i + 1];
    }
    g_module_registry[--g_module_count] = NULL;
    return 0;
}

NL_API nl_module_info_t* nl_module_get_info(nl_module_type_t type) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_module_registry[i] && g_module_registry[i]->type == type) return g_module_registry[i];
    }
    return NULL;
}

NL_API nl_module_info_t* nl_module_get_info_by_name(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_module_count; i++) {
        if (g_module_registry[i] && g_module_registry[i]->name && strcmp(g_module_registry[i]->name, name) == 0) return g_module_registry[i];
    }
    return NULL;
}

NL_API int nl_module_get_all(nl_module_info_t** modules, int max_count) {
    // L11: 入口判空，避免 modules == NULL 或 max_count <= 0 时对空缓冲写入
    if (!modules || max_count <= 0) return 0;
    int count = 0;
    for (int i = 0; i < g_module_count && count < max_count; i++) {
        if (g_module_registry[i]) modules[count++] = g_module_registry[i];
    }
    return count;
}

NL_API int nl_module_get_count(void) {
    return g_module_count;
}

NL_API int nl_module_is_platform_supported(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    if (!mod) return 0;
    return (is_windows() && mod->platform_windows) ||
           (is_linux() && mod->platform_linux) ||
           (is_macos() && mod->platform_macos);
}

NL_API nl_module_status_t nl_module_get_status(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    return mod ? mod->status : NL_MODULE_STATUS_UNINITIALIZED;
}

NL_API int nl_module_set_enabled(nl_module_type_t type, int enabled) {
    nl_module_info_t* mod = nl_module_get_info(type);
    if (!mod) return -1;
    if (enabled) {
        if (!mod->init || mod->status == NL_MODULE_STATUS_INITIALIZED) {
            mod->status = NL_MODULE_STATUS_INITIALIZED;
            return 0;
        }
        int rc = mod->init();
        if (rc == 0) mod->status = NL_MODULE_STATUS_INITIALIZED;
        return rc;
    } else {
        if (mod->shutdown) mod->shutdown();
        mod->status = NL_MODULE_STATUS_DISABLED;
        return 0;
    }
}

NL_API const char* nl_module_get_name(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    return mod ? mod->name : NULL;
}

NL_API const char* nl_module_get_version(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    return mod ? mod->version : NULL;
}

NL_API const char* nl_module_get_description(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    return mod ? mod->description : NULL;
}

NL_API int nl_module_has_capability(nl_module_type_t type, int cap) {
    nl_module_info_t* mod = nl_module_get_info(type);
    return mod ? NL_CAP_HAS(mod->capabilities, cap) : 0;
}

NL_API int nl_module_get_capabilities(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    return mod ? mod->capabilities : 0;
}

// ============================================================
// nl_get_module - get module by type (exposed via netleaf.h)
// ============================================================

NL_API nl_module_info_t* nl_get_module(nl_module_type_t type) {
    return nl_module_get_info(type);
}

// ============================================================
// Lazy Loading API
// ============================================================

NL_API void nl_module_lazy_enable(int enable) { g_lazy_enabled = enable; }
NL_API void nl_module_lazy_enable_module(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    if (mod) mod->lazy_status = NL_MODULE_LAZY_LOADING;
}
NL_API void nl_module_lazy_disable_module(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    if (mod) mod->lazy_status = NL_MODULE_LAZY_STOPPED;
}
NL_API int nl_module_lazy_is_enabled(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    return mod && NL_CAP_HAS(mod->capabilities, NL_CAP_LAZY_LOAD) ? 1 : 0;
}
NL_API void nl_module_lazy_clear_cache(void) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_module_registry[i] && g_module_registry[i]->lazy_status == NL_MODULE_LAZY_LOADED) {
            if (g_module_registry[i]->lazy_unload) g_module_registry[i]->lazy_unload();
            g_module_registry[i]->lazy_status = NL_MODULE_LAZY_UNLOADED;
        }
    }
}
NL_API int nl_module_lazy_load(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    if (!mod || !mod->lazy_load) return -1;
    mod->lazy_status = NL_MODULE_LAZY_LOADING;
    void* ptr = mod->lazy_load();
    if (ptr) { mod->lazy_status = NL_MODULE_LAZY_LOADED; return 0; }
    mod->lazy_status = NL_MODULE_LAZY_UNLOADED;
    return -1;
}
NL_API int nl_module_lazy_unload(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    if (!mod || !mod->lazy_unload) return -1;
    mod->lazy_status = NL_MODULE_LAZY_STOPPING;
    mod->lazy_unload();
    mod->lazy_status = NL_MODULE_LAZY_STOPPED;
    return 0;
}
NL_API nl_module_lazy_status_t nl_module_lazy_get_status(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    return mod ? mod->lazy_status : NL_MODULE_LAZY_UNLOADED;
}
NL_API int nl_module_lazy_is_loaded(nl_module_type_t type) {
    nl_module_info_t* mod = nl_module_get_info(type);
    return mod && mod->lazy_status == NL_MODULE_LAZY_LOADED ? 1 : 0;
}
NL_API void nl_module_lazy_preload_all(void) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_module_registry[i] && NL_CAP_HAS(g_module_registry[i]->capabilities, NL_CAP_LAZY_LOAD)) {
            nl_module_lazy_load(g_module_registry[i]->type);
        }
    }
}
NL_API void nl_module_lazy_unload_all(void) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_module_registry[i] && NL_CAP_HAS(g_module_registry[i]->capabilities, NL_CAP_LAZY_LOAD)) {
            nl_module_lazy_unload(g_module_registry[i]->type);
        }
    }
}

// ============================================================
// nl_modules_init - Initialize core and auto-load extensions
// ============================================================

NL_API int nl_modules_init(void) {
    if (g_modules_initialized) return 0;

    // Register core module (defined in this file)
    nl_module_register(&g_core_module_info);

    // Auto-load extensions from extensions directory
    nl_extension_auto_load();

    g_modules_initialized = 1;
    return 0;
}

NL_API int nl_modules_shutdown(void) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_module_registry[i] && g_module_registry[i]->shutdown) {
            g_module_registry[i]->shutdown();
            g_module_registry[i]->status = NL_MODULE_STATUS_UNINITIALIZED;
        }
    }
    g_module_count = 0;
    // H7: 关机时统一卸载所有已加载的扩展动态库，避免句柄泄漏与悬垂指针
    for (int i = 0; i < g_extension_count; i++) {
        close_extension_library(g_extension_dl_handles[i]);
        g_extension_dl_handles[i] = NULL;
        free(g_extension_ids[i]);
        free(g_extension_names[i]);
        g_extension_ids[i] = NULL;
        g_extension_names[i] = NULL;
        g_extension_registry[i] = NULL;
    }
    g_extension_count = 0;
    // L9: 关机时释放扩展元数据链表，避免泄漏
    nl_extension_clear_metadata();
    g_modules_initialized = 0;
    return 0;
}

// ============================================================
// nl_print_modules
// ============================================================

NL_API void nl_print_modules(void) {
    printf("Modules (%d):\n", g_module_count);
    for (int i = 0; i < g_module_count; i++) {
        nl_module_info_t* m = g_module_registry[i];
        if (!m) continue;
        printf("  [%s] v%s cap=0x%x status=%d\n",
               m->name, m->version ? m->version : "0.0.0",
               m->capabilities, m->status);
    }
    printf("Extensions (%d):\n", g_extension_count);
    for (int i = 0; i < g_extension_count; i++) {
        nl_extension_info_t* e = g_extension_registry[i];
        if (!e) continue;
        printf("  [%s] v%s cap=0x%x\n",
               e->library_id, e->version ? e->version : "0.0.0",
               e->capabilities);
    }
}

// ============================================================
// nl_module_available / nl_get_modules
// ============================================================

NL_API int nl_module_available(const char* module_name) {
    return nl_module_get_info_by_name(module_name) != NULL;
}

NL_API int nl_get_module_count(void) {
    return g_module_count;
}

NL_API int nl_get_modules(const char** modules, int max_count) {
    int count = 0;
    for (int i = 0; i < g_module_count && count < max_count; i++) {
        if (g_module_registry[i] && g_module_registry[i]->name) {
            modules[count++] = g_module_registry[i]->name;
        }
    }
    return count;
}

// ============================================================
// Module Dependency API
// ============================================================

NL_API int nl_module_add_dependency(nl_module_type_t module, nl_module_type_t dependency) {
    nl_module_info_t* mod = nl_module_get_info(module);
    if (!mod) return -1;
    nl_module_info_t* dep = nl_module_get_info(dependency);
    if (!dep) return -1;
    mod->dependencies = dep;
    return 0;
}

NL_API int nl_module_remove_dependency(nl_module_type_t module, nl_module_type_t dependency) {
    (void)module;
    (void)dependency;
    return 0;
}

NL_API int nl_module_check_dependencies(nl_module_type_t module) {
    nl_module_info_t* mod = nl_module_get_info(module);
    if (!mod) return -1;
    if (!mod->dependencies) return 0;
    return nl_module_get_info(mod->dependencies->type) ? 0 : -1;
}

NL_API nl_module_info_t* nl_module_get_dependencies(nl_module_type_t module) {
    nl_module_info_t* mod = nl_module_get_info(module);
    return mod ? mod->dependencies : NULL;
}

// ============================================================
// Plugin System
// ============================================================

static int load_plugin_from_path(const char* path, nl_plugin_handle_t* out_handle) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        snprintf(g_plugin_error, sizeof(g_plugin_error), "Failed to load: %s (error %lu)", path, (unsigned long)GetLastError());
        return -1;
    }

    typedef nl_plugin_info_t* (*GetPluginInfoFunc)(void);
    GetPluginInfoFunc get_info = (GetPluginInfoFunc)GetProcAddress(h, "plugin_get_info");
    if (!get_info) {
        snprintf(g_plugin_error, sizeof(g_plugin_error), "Symbol plugin_get_info not found in %s", path);
        FreeLibrary(h);
        return -1;
    }

    typedef int (*PluginInitFunc)(void);
    PluginInitFunc init_fn = (PluginInitFunc)GetProcAddress(h, "plugin_init");

    nl_plugin_entry_t* entry = NULL;
    // H8: 以 MAX_PLUGINS 为界扫描空槽（g_plugin_count 已改为有效计数语义）
    for (int i = 0; i < MAX_PLUGINS; i++) {
        if (!g_plugin_registry[i].handle) { entry = &g_plugin_registry[i]; break; }
    }
    if (!entry) {
        snprintf(g_plugin_error, sizeof(g_plugin_error), "Max plugins reached (%d)", MAX_PLUGINS);
        FreeLibrary(h);
        return -1;
    }

    nl_plugin_info_t* info = get_info();
    entry->handle = (nl_plugin_handle_t)entry;
    entry->info = info;
    entry->dl_handle = (void*)h;
    entry->state = NL_PLUGIN_STATE_LOADED;
    // L4: 描述符随插件一起存入注册表槽位
    fill_plugin_descriptor(info, &entry->descriptor);
    if (init_fn) init_fn();

    if (out_handle) *out_handle = (nl_plugin_handle_t)entry;
    g_plugin_count++;
    return 0;
#else
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(g_plugin_error, sizeof(g_plugin_error), "Failed to load: %s (%s)", path, dlerror());
        return -1;
    }

    typedef nl_plugin_info_t* (*GetPluginInfoFunc)(void);
    GetPluginInfoFunc get_info = (GetPluginInfoFunc)dlsym(h, "plugin_get_info");
    if (!get_info) {
        snprintf(g_plugin_error, sizeof(g_plugin_error), "Symbol plugin_get_info not found in %s", path);
        dlclose(h);
        return -1;
    }

    typedef int (*PluginInitFunc)(void);
    PluginInitFunc init_fn = (PluginInitFunc)dlsym(h, "plugin_init");

    nl_plugin_entry_t* entry = NULL;
    // H8: 以 MAX_PLUGINS 为界扫描空槽（g_plugin_count 已改为有效计数语义）
    for (int i = 0; i < MAX_PLUGINS; i++) {
        if (!g_plugin_registry[i].handle) { entry = &g_plugin_registry[i]; break; }
    }
    if (!entry) {
        snprintf(g_plugin_error, sizeof(g_plugin_error), "Max plugins reached (%d)", MAX_PLUGINS);
        dlclose(h);
        return -1;
    }

    nl_plugin_info_t* info = get_info();
    entry->handle = (nl_plugin_handle_t)entry;
    entry->info = info;
    entry->dl_handle = h;
    entry->state = NL_PLUGIN_STATE_LOADED;
    // L4: 描述符随插件一起存入注册表槽位
    fill_plugin_descriptor(info, &entry->descriptor);
    if (init_fn) init_fn();

    if (out_handle) *out_handle = (nl_plugin_handle_t)entry;
    g_plugin_count++;
    return 0;
#endif
}

NL_API nl_plugin_handle_t nl_plugin_load(const char* plugin_path) {
    nl_plugin_handle_t handle = NULL;
    if (load_plugin_from_path(plugin_path, &handle) != 0) return NULL;
    return handle;
}

NL_API int nl_plugin_unload(nl_plugin_handle_t handle) {
    if (!handle) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (entry->state != NL_PLUGIN_STATE_LOADED) return -1;

    if (entry->info && entry->info->shutdown) entry->info->shutdown();
    entry->state = NL_PLUGIN_STATE_UNLOADED;
    entry->handle = NULL;
    entry->info = NULL;
    // L4: 释放槽位内的描述符引用，避免残留已卸载插件的悬垂字符串
    fill_plugin_descriptor(NULL, &entry->descriptor);

    if (entry->dl_handle) {
#ifdef _WIN32
        FreeLibrary((HMODULE)entry->dl_handle);
#else
        dlclose(entry->dl_handle);
#endif
        entry->dl_handle = NULL;
    }
    // H8: 卸载后递减有效槽位计数（避免只增不减导致越界读）
    if (g_plugin_count > 0) g_plugin_count--;
    return 0;
}

NL_API int nl_plugin_register(nl_plugin_handle_t handle) {
    if (!handle) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (entry->state != NL_PLUGIN_STATE_LOADED) return -1;
    entry->state = NL_PLUGIN_STATE_LOADED;
    return 0;
}

// L4: 调用方缓冲版本（推荐，完全避免共享可变状态）
NL_API int nl_plugin_get_descriptor_into(nl_plugin_handle_t handle, nl_plugin_descriptor_t* desc) {
    if (!handle || !desc) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return -1;
    *desc = entry->descriptor;
    return 0;
}

NL_API nl_plugin_descriptor_t* nl_plugin_get_descriptor(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return NULL;
    // L4: 返回该插件注册表槽位内自有的描述符（加载时填充、卸载时清空），
    // 不再是函数内 static 结构，因此不同句柄/并发调用不会互相覆盖。
    return &entry->descriptor;
}

NL_API int nl_plugin_is_loaded(nl_plugin_handle_t handle) {
    if (!handle) return 0;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    return entry->state == NL_PLUGIN_STATE_LOADED ? 1 : 0;
}

NL_API int nl_plugin_get_count(void) {
    return g_plugin_count;
}

NL_API nl_plugin_handle_t* nl_plugin_get_all(int* count) {
    // H8: 先统计有效槽位，再分配；遍历一律以 MAX_PLUGINS 为界
    int active = 0;
    for (int i = 0; i < MAX_PLUGINS; i++) {
        if (g_plugin_registry[i].handle) active++;
    }
    nl_plugin_handle_t* arr = (nl_plugin_handle_t*)malloc(sizeof(nl_plugin_handle_t) * (size_t)(active > 0 ? active : 1));
    if (!arr) { if (count) *count = 0; return NULL; }
    int c = 0;
    for (int i = 0; i < MAX_PLUGINS; i++) {
        if (g_plugin_registry[i].handle) arr[c++] = g_plugin_registry[i].handle;
    }
    if (count) *count = c;
    return arr;
}

NL_API const char* nl_plugin_get_error(void) {
    return g_plugin_error;
}

// ============================================================
// Enhanced Plugin Management API
// ============================================================

NL_API int nl_plugin_discover(const char* directory, nl_plugin_handle_t** plugins, int max_count) {
    if (!directory) return 0;
    int count = 0;
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s/*.dll", directory);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (count >= max_count) break;
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", directory, fd.cFileName);
        nl_plugin_handle_t ph = nl_plugin_load(filepath);
        if (ph) {
            if (plugins) plugins[count++] = ph;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* dir = opendir(directory);
    if (!dir) return 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && count < max_count) {
        size_t len = strlen(ent->d_name);
        if (len < 4) continue;
        if (strcmp(ent->d_name + len - 4, ".so") != 0) continue;
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", directory, ent->d_name);
        nl_plugin_handle_t ph = nl_plugin_load(filepath);
        if (ph) {
            if (plugins) plugins[count++] = ph;
        }
    }
    closedir(dir);
#endif
    return count;
}

NL_API int nl_plugin_discover_all(nl_plugin_handle_t** plugins, int max_count) {
    return nl_plugin_discover(NULL, plugins, max_count);
}

NL_API int nl_plugin_search(const char* keyword, nl_plugin_handle_t** results, int max_count) {
    if (!keyword) return 0;
    int count = 0;
    for (int i = 0; i < MAX_PLUGINS && count < max_count; i++) {
        if (!g_plugin_registry[i].handle) continue;
        nl_plugin_entry_t* entry = &g_plugin_registry[i];
        if (entry->state != NL_PLUGIN_STATE_LOADED) continue;
        if (entry->info) {
            if (entry->info->name && strstr(entry->info->name, keyword)) {
                if (results) results[count++] = entry->handle;
            } else if (entry->info->id && strstr(entry->info->id, keyword)) {
                if (results) results[count++] = entry->handle;
            }
        }
    }
    return count;
}

NL_API nl_plugin_state_t nl_plugin_get_state(nl_plugin_handle_t handle) {
    if (!handle) return NL_PLUGIN_STATE_UNLOADED;
    return ((nl_plugin_entry_t*)handle)->state;
}

NL_API const char* nl_plugin_get_id(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    return entry->info ? entry->info->id : NULL;
}

NL_API const char* nl_plugin_get_name(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    return entry->info ? entry->info->name : NULL;
}

NL_API const char* nl_plugin_get_version(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    return entry->info ? entry->info->version : NULL;
}

NL_API const char* nl_plugin_get_description(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    return entry->info ? entry->info->description : NULL;
}

NL_API const char* nl_plugin_get_author(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    return entry->info ? entry->info->author : NULL;
}

// ============================================================
// Plugin Dependency Management API
// ============================================================

NL_API int nl_plugin_add_dependency(nl_plugin_handle_t handle, const char* dep_id, nl_extension_dep_type_t type, const char* min_version) {
    if (!handle || !dep_id) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return -1;
    return nl_extension_add_dependency((nl_extension_info_t*)entry->info, dep_id, type, min_version);
}

NL_API int nl_plugin_remove_dependency(nl_plugin_handle_t handle, const char* dep_id) {
    if (!handle || !dep_id) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return -1;
    return nl_extension_remove_dependency((nl_extension_info_t*)entry->info, dep_id);
}

NL_API int nl_plugin_clear_dependencies(nl_plugin_handle_t handle) {
    if (!handle) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return -1;
    return nl_extension_clear_dependencies((nl_extension_info_t*)entry->info);
}

NL_API int nl_plugin_get_dependency_count(nl_plugin_handle_t handle) {
    if (!handle) return 0;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return 0;
    return entry->info->dependency_count;
}

NL_API nl_extension_dependency_t* nl_plugin_get_plugin_dependencies(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return NULL;
    return entry->info->dependencies;
}

NL_API nl_extension_dependency_t* nl_plugin_get_plugin_dependency_by_id(nl_plugin_handle_t handle, const char* dep_id) {
    if (!handle || !dep_id) return NULL;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return NULL;
    return nl_extension_get_dependency_by_id((nl_extension_info_t*)entry->info, dep_id);
}

NL_API int nl_plugin_has_dependency(nl_plugin_handle_t handle, const char* dep_id) {
    if (!handle || !dep_id) return 0;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return 0;
    return nl_extension_has_dependency((nl_extension_info_t*)entry->info, dep_id);
}

NL_API int nl_plugin_check_plugin_dependencies(nl_plugin_handle_t handle, int require_all) {
    if (!handle) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return -1;
    return nl_extension_check_dependencies((nl_extension_info_t*)entry->info, require_all);
}

NL_API int nl_plugin_resolve_plugin_dependencies(nl_plugin_handle_t handle) {
    if (!handle) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return -1;
    return nl_extension_resolve_dependencies((nl_extension_info_t*)entry->info);
}

NL_API int nl_plugin_are_plugin_dependencies_met(nl_plugin_handle_t handle) {
    if (!handle) return 0;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return 0;
    return nl_extension_are_dependencies_met((nl_extension_info_t*)entry->info);
}

NL_API int nl_plugin_check_dependencies(nl_plugin_handle_t handle, int* missing_deps) {
    if (!handle) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return -1;
    int result = nl_extension_check_dependencies((nl_extension_info_t*)entry->info, 1);
    if (missing_deps) *missing_deps = (result < 0) ? 1 : 0;
    return result;
}

NL_API int nl_plugin_get_dependencies(nl_plugin_handle_t handle, nl_plugin_handle_t** deps, int max_count) {
    if (!handle) return 0;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return 0;
    int count = 0;
    for (nl_extension_dependency_t* d = entry->info->dependencies; d && count < max_count; d = d->next) {
        nl_plugin_handle_t ph = NULL;
        for (int i = 0; i < MAX_PLUGINS; i++) {
            if (g_plugin_registry[i].handle && g_plugin_registry[i].info &&
                g_plugin_registry[i].info->id && strcmp(g_plugin_registry[i].info->id, d->library_id) == 0) {
                ph = g_plugin_registry[i].handle;
                break;
            }
        }
        if (ph && deps) deps[count++] = ph;
    }
    return count;
}

NL_API int nl_plugin_validate(nl_plugin_handle_t handle, char* error_msg, size_t error_size) {
    if (!handle) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info) return -1;
    if (!entry->info->name || !entry->info->id || !entry->info->version) {
        if (error_msg && error_size > 0) snprintf(error_msg, error_size, "Missing required fields (name/id/version)");
        return -1;
    }
    return 0;
}

NL_API int nl_plugin_reload(nl_plugin_handle_t handle) {
    if (!handle) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    // Cannot reload without storing path - simplified
    (void)entry;
    return -1;
}

NL_API int nl_plugin_reload_all(void) {
    return -1;
}

NL_API int nl_plugin_emit_event(const char* event_name, void* data, size_t data_size) {
    (void)data_size;
    if (!event_name) return -1;
    for (int i = 0; i < MAX_PLUGINS; i++) {
        if (!g_plugin_registry[i].handle) continue;
        nl_plugin_entry_t* entry = &g_plugin_registry[i];
        if (entry->state != NL_PLUGIN_STATE_LOADED || !entry->info) continue;
        if (entry->info->on_event) {
            entry->info->on_event(event_name, data, entry->info->userdata);
        }
    }
    return 0;
}

NL_API int nl_plugin_subscribe(nl_plugin_handle_t handle, const char* event_name) {
    (void)handle;
    (void)event_name;
    return 0;
}

NL_API int nl_plugin_unsubscribe(nl_plugin_handle_t handle, const char* event_name) {
    (void)handle;
    (void)event_name;
    return 0;
}

NL_API int nl_plugin_check_version_compatibility(nl_plugin_handle_t handle, const char* nl_version) {
    if (!handle || !nl_version) return -1;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    if (!entry->info || !entry->info->min_nl_version) return 1;
    return version_compare(entry->info->min_nl_version, nl_version) <= 0 ? 1 : 0;
}

NL_API const char* nl_plugin_get_min_version(nl_plugin_handle_t handle) {
    if (!handle) return NULL;
    nl_plugin_entry_t* entry = (nl_plugin_entry_t*)handle;
    return entry->info ? entry->info->min_nl_version : NULL;
}

NL_API int nl_plugin_enable_sandbox(nl_plugin_handle_t handle, int enable) {
    (void)handle;
    (void)enable;
    return 0;
}

NL_API int nl_plugin_is_sandboxed(nl_plugin_handle_t handle) {
    (void)handle;
    return 0;
}

#define _POSIX_C_SOURCE 200809L
#include "netleaf_lang.h"
#include "netleaf_module.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
    #include <windows.h>
    #define nl_strdup _strdup
    typedef CRITICAL_SECTION nl_lang_mutex_t;
    #define NL_LANG_MUTEX_INIT(m)   InitializeCriticalSection(m)
    #define NL_LANG_MUTEX_LOCK(m)   EnterCriticalSection(m)
    #define NL_LANG_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
    #define NL_LANG_MUTEX_DESTROY(m) DeleteCriticalSection(m)
    // CRITICAL_SECTION 没有静态初始化器，因此用 INIT_ONCE 保证互斥量在多线程
    // 首次调用时也只被初始化一次（L3：避免重复 InitializeCriticalSection）
    typedef INIT_ONCE nl_lang_once_t;
    #define NL_LANG_ONCE_INIT           INIT_ONCE_STATIC_INIT
    #define NL_LANG_ONCE_DEFINE(fn)     static BOOL CALLBACK fn(PINIT_ONCE once, PVOID param, PVOID* ctx)
    #define NL_LANG_ONCE_BODY(m)        do { (void)once; (void)param; (void)ctx; NL_LANG_MUTEX_INIT(&(m)); return TRUE; } while (0)
    #define NL_LANG_ONCE_RUN(once, fn)  InitOnceExecuteOnce(&(once), (fn), NULL, NULL)
#else
    #include <pthread.h>
    #define nl_strdup strdup
    typedef pthread_mutex_t nl_lang_mutex_t;
    #define NL_LANG_MUTEX_INIT(m)   pthread_mutex_init(m, NULL)
    #define NL_LANG_MUTEX_LOCK(m)   pthread_mutex_lock(m)
    #define NL_LANG_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
    #define NL_LANG_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
    // POSIX 侧用 pthread_once 达到同样的一次性初始化语义（L3）
    typedef pthread_once_t nl_lang_once_t;
    #define NL_LANG_ONCE_INIT           PTHREAD_ONCE_INIT
    #define NL_LANG_ONCE_DEFINE(fn)     static void fn(void)
    #define NL_LANG_ONCE_BODY(m)        do { NL_LANG_MUTEX_INIT(&(m)); } while (0)
    #define NL_LANG_ONCE_RUN(once, fn)  pthread_once(&(once), (fn))
#endif

// 线程局部存储：用于把加锁期间取得的内部数据复制成调用线程私有副本，
// 避免解锁后返回指向内部存储的悬垂指针。编译器不支持时降级为普通静态变量。
#if defined(_MSC_VER)
    #define NL_LANG_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
    #define NL_LANG_THREAD_LOCAL __thread
#else
    #define NL_LANG_THREAD_LOCAL
#endif

// =========================================
// Internal State
// =========================================

#define MAX_LIBRARIES 32
#define MAX_LANGUAGES 16
#define MAX_URI_HANDLERS 8

static char g_current_lang[NL_LANG_CODE_MAX_LEN] = "en_us";
static char g_default_lang[NL_LANG_CODE_MAX_LEN] = "en_us";

static nl_error_registry_t g_lib_registry[MAX_LIBRARIES];
static int g_lib_count = 0;
static nl_lang_mutex_t g_registry_mutex;
static nl_lang_once_t g_registry_once = NL_LANG_ONCE_INIT;

static char* g_all_languages[MAX_LANGUAGES];
static int g_all_lang_count = 0;

typedef struct {
    char scheme[16];
    nl_uri_handler_t handler;
} nl_uri_handler_entry_t;

static nl_uri_handler_entry_t g_uri_handlers[MAX_URI_HANDLERS];
static int g_uri_handler_count = 0;

typedef struct {
    int lib_id;
    char name[32];
} nl_lib_name_entry_t;

static nl_lib_name_entry_t g_lib_names[MAX_LIBRARIES];
static int g_lib_name_count = 0;

// =========================================
// Internal Helpers
// =========================================

// 一次性初始化回调：无论多少线程并发首次调用，互斥量只会被初始化一次（L3）
NL_LANG_ONCE_DEFINE(init_registry_mutex_once) {
    NL_LANG_ONCE_BODY(g_registry_mutex);
}

static void init_mutex(void) {
    NL_LANG_ONCE_RUN(g_registry_once, init_registry_mutex_once);
}

static int find_lib_index(int lib_id) {
    for (int i = 0; i < g_lib_count; i++) {
        if (g_lib_registry[i].lib_id == lib_id) {
            return i;
        }
    }
    return -1;
}

static int find_lang_index_in_lib(nl_error_registry_t* reg, const char* lang_code) {
    for (int i = 0; i < reg->lang_count; i++) {
        if (strcmp(reg->languages[i], lang_code) == 0) {
            return i;
        }
    }
    return -1;
}

static nl_error_entry_t* find_error_entry(nl_error_registry_t* reg, int code) {
    nl_error_entry_t* entry = reg->errors;
    while (entry) {
        if (entry->code == code) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

static void add_language_global(const char* lang_code) {
    for (int i = 0; i < g_all_lang_count; i++) {
        if (strcmp(g_all_languages[i], lang_code) == 0) {
            return;  // Already exists
        }
    }
    if (g_all_lang_count < MAX_LANGUAGES) {
        char* dup = nl_strdup(lang_code);
        if (!dup) return;  // OOM：放弃登记，保持既有状态
        g_all_languages[g_all_lang_count] = dup;
        g_all_lang_count++;
    }
}

// =========================================
// Language Code Validation (xx_xx format, case-insensitive)
// =========================================

int nl_lang_validate_code(const char* code) {
    if (!code) return -1;

    size_t len = strlen(code);
    if (len < 2 || len > NL_LANG_CODE_MAX_LEN) return 0;

    // Must contain exactly one underscore
    const char* underscore = strchr(code, '_');
    if (!underscore) return 0;
    if (strchr(underscore + 1, '_') != NULL) return 0;  // Multiple underscores

    // Underscore must not be at start or end
    if (underscore == code || underscore[1] == '\0') return 0;

    // All characters must be letters (case-insensitive) or digits
    for (const char* p = code; *p; p++) {
        if (*p == '_') continue;
        if (!isalpha((unsigned char)*p) && !isdigit((unsigned char)*p)) return 0;
    }

    // Language part (before underscore) must be 2-3 letters
    size_t lang_len = underscore - code;
    if (lang_len < 2 || lang_len > 3) return 0;

    // Country part (after underscore) must be 2-3 letters
    size_t country_len = strlen(underscore + 1);
    if (country_len < 2 || country_len > 3) return 0;

    return 1;
}

// Normalize language code to lowercase (xx_xx)
static void nl_lang_normalize_code(const char* src, char* dest, size_t dest_size) {
    // 入口校验：dst_size==0 时 dest_size-1 会发生无符号下溢，必须先判 0
    if (!src || !dest || dest_size == 0) return;
    size_t i;
    for (i = 0; i < dest_size - 1 && src[i]; i++) {
        dest[i] = (char)tolower((unsigned char)src[i]);
    }
    dest[i] = '\0';
}

// =========================================
// Language Configuration
// =========================================

const char* nl_lang_get(void) {
    return g_current_lang;
}

int nl_lang_set(const char* code) {
    if (nl_lang_validate_code(code) != 1) return -1;
    nl_lang_normalize_code(code, g_current_lang, sizeof(g_current_lang));
    return 0;
}

const char* nl_lang_get_default(void) {
    return g_default_lang;
}

int nl_lang_set_default(const char* code) {
    if (nl_lang_validate_code(code) != 1) return -1;
    nl_lang_normalize_code(code, g_default_lang, sizeof(g_default_lang));
    return 0;
}

const char** nl_lang_get_all(void) {
    return (const char**)g_all_languages;
}

int nl_lang_get_count(void) {
    return g_all_lang_count;
}

// =========================================
// Library Error Registration
// =========================================

int nl_lang_register_lib(int lib_id, const char** languages, int lang_count) {
    if (!languages || lang_count <= 0 || lang_count > MAX_LANGUAGES) return -1;
    
    // Validate all language codes
    for (int i = 0; i < lang_count; i++) {
        if (nl_lang_validate_code(languages[i]) != 1) return -1;
    }
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    // Normalize and store language codes
    char normalized[MAX_LANGUAGES][NL_LANG_CODE_MAX_LEN];
    for (int i = 0; i < lang_count; i++) {
        nl_lang_normalize_code(languages[i], normalized[i], sizeof(normalized[i]));
    }
    
    // Check if already registered
    int idx = find_lib_index(lib_id);
    if (idx >= 0) {
        // Update existing：先构造新数组，成功后再替换，避免 OOM 时破坏既有状态
        char** new_langs = (char**)malloc((size_t)lang_count * sizeof(char*));
        if (!new_langs) {
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
        for (int i = 0; i < lang_count; i++) {
            new_langs[i] = nl_strdup(normalized[i]);
            if (!new_langs[i]) {
                for (int j = 0; j < i; j++) free(new_langs[j]);
                free(new_langs);
                NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
                return -1;
            }
        }
        for (int i = 0; i < g_lib_registry[idx].lang_count; i++) {
            free(g_lib_registry[idx].languages[i]);
        }
        free(g_lib_registry[idx].languages);
        g_lib_registry[idx].languages = new_langs;
        g_lib_registry[idx].lang_count = lang_count;
        for (int i = 0; i < lang_count; i++) {
            add_language_global(normalized[i]);
        }
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return 0;
    }
    
    // Add new
    if (g_lib_count >= MAX_LIBRARIES) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    char** new_langs = (char**)malloc((size_t)lang_count * sizeof(char*));
    if (!new_langs) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    for (int i = 0; i < lang_count; i++) {
        new_langs[i] = nl_strdup(normalized[i]);
        if (!new_langs[i]) {
            for (int j = 0; j < i; j++) free(new_langs[j]);
            free(new_langs);
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
    }
    
    g_lib_registry[g_lib_count].lib_id = lib_id;
    g_lib_registry[g_lib_count].languages = new_langs;
    g_lib_registry[g_lib_count].lang_count = lang_count;
    g_lib_registry[g_lib_count].errors = NULL;
    
    for (int i = 0; i < lang_count; i++) {
        add_language_global(normalized[i]);
    }
    
    g_lib_count++;
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

void nl_lang_unregister_lib(int lib_id) {
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx >= 0) {
        // Free languages
        for (int i = 0; i < g_lib_registry[idx].lang_count; i++) {
            free(g_lib_registry[idx].languages[i]);
        }
        free(g_lib_registry[idx].languages);
        
        // Free errors
        nl_error_entry_t* entry = g_lib_registry[idx].errors;
        while (entry) {
            for (int i = 0; i < entry->lang_count; i++) {
                free(entry->messages[i]);
            }
            free(entry->messages);
            nl_error_entry_t* next = entry->next;
            free(entry);
            entry = next;
        }
        
        // Shift entries
        for (int i = idx; i < g_lib_count - 1; i++) {
            g_lib_registry[i] = g_lib_registry[i + 1];
        }
        g_lib_count--;
    }
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
}

int nl_lang_is_registered(int lib_id) {
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    int idx = find_lib_index(lib_id);
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return (idx >= 0) ? 1 : 0;
}

// Register a single language to a library
int nl_lang_register_language(int lib_id, const char* lang_code) {
    if (!lang_code) return -1;
    if (nl_lang_validate_code(lang_code) != 1) return -1;
    
    char normalized[NL_LANG_CODE_MAX_LEN];
    nl_lang_normalize_code(lang_code, normalized, sizeof(normalized));
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        // Create new library entry
        if (g_lib_count >= MAX_LIBRARIES) {
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
        
        g_lib_registry[g_lib_count].lib_id = lib_id;
        char** langs0 = (char**)malloc(sizeof(char*));
        if (!langs0) {
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
        char* dup0 = nl_strdup(normalized);
        if (!dup0) {
            free(langs0);
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
        langs0[0] = dup0;
        g_lib_registry[g_lib_count].languages = langs0;
        g_lib_registry[g_lib_count].lang_count = 1;
        g_lib_registry[g_lib_count].errors = NULL;
        g_lib_count++;
        
        add_language_global(normalized);
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return 0;
    }
    
    // Check if language already registered
    nl_error_registry_t* reg = &g_lib_registry[idx];
    if (find_lang_index_in_lib(reg, normalized) >= 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return 0;  // Already exists, not an error
    }
    
    // Add new language：先构造全部新数组，全部成功后再提交，避免 OOM 时状态不一致
    int new_count = reg->lang_count + 1;
    char** new_languages = (char**)malloc((size_t)new_count * sizeof(char*));
    if (!new_languages) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    for (int i = 0; i < reg->lang_count; i++) {
        new_languages[i] = reg->languages[i];
    }
    new_languages[reg->lang_count] = nl_strdup(normalized);
    if (!new_languages[reg->lang_count]) {
        free(new_languages);
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    // Expand all error entries to accommodate new language
    nl_error_entry_t* entry;
    int entry_count = 0;
    for (entry = reg->errors; entry; entry = entry->next) entry_count++;
    char*** staged = NULL;
    if (entry_count > 0) {
        staged = (char***)malloc((size_t)entry_count * sizeof(char**));
        if (!staged) {
            free(new_languages[reg->lang_count]);
            free(new_languages);
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
    }
    int k = 0;
    int fail = 0;
    for (entry = reg->errors; entry; entry = entry->next, k++) {
        char** nm = (char**)malloc((size_t)new_count * sizeof(char*));
        if (nm) {
            for (int i = 0; i < entry->lang_count; i++) nm[i] = entry->messages[i];
            nm[entry->lang_count] = NULL;  // Empty message for new language
        }
        staged[k] = nm;
        if (!nm) { fail = 1; k++; break; }
    }
    if (fail) {
        for (int i = 0; i < k; i++) free(staged[i]);
        free(staged);
        free(new_languages[reg->lang_count]);
        free(new_languages);
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    // Commit
    free(reg->languages);
    reg->languages = new_languages;
    reg->lang_count = new_count;
    k = 0;
    for (entry = reg->errors; entry; entry = entry->next, k++) {
        free(entry->messages);
        entry->messages = staged[k];
        entry->lang_count = new_count;
    }
    free(staged);
    
    add_language_global(normalized);
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

// Unregister a language from a library
int nl_lang_unregister_language(int lib_id, const char* lang_code) {
    if (!lang_code) return -1;
    
    char normalized[NL_LANG_CODE_MAX_LEN];
    nl_lang_normalize_code(lang_code, normalized, sizeof(normalized));
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    int lang_idx = find_lang_index_in_lib(reg, normalized);
    if (lang_idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;  // Language not found
    }
    
    // Remove language from array
    free(reg->languages[lang_idx]);
    for (int i = lang_idx; i < reg->lang_count - 1; i++) {
        reg->languages[i] = reg->languages[i + 1];
    }
    reg->lang_count--;
    
    // Remove messages for this language from all error entries
    nl_error_entry_t* entry = reg->errors;
    while (entry) {
        free(entry->messages[lang_idx]);
        for (int i = lang_idx; i < entry->lang_count - 1; i++) {
            entry->messages[i] = entry->messages[i + 1];
        }
        entry->lang_count--;
        entry = entry->next;
    }
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

// Check if a language is registered for a library
int nl_lang_has_language(int lib_id, const char* lang_code) {
    if (!lang_code) return 0;
    
    char normalized[NL_LANG_CODE_MAX_LEN];
    nl_lang_normalize_code(lang_code, normalized, sizeof(normalized));
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return 0;
    }
    
    int result = (find_lang_index_in_lib(&g_lib_registry[idx], normalized) >= 0) ? 1 : 0;
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return result;
}

int nl_lang_add_error(int lib_id, int code, const char** messages) {
    // 入口校验：消息数组不能为空
    if (!messages) return -1;

    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    
    // 无已注册语言时无法推断消息数组长度，直接拒绝，避免越界读取 messages
    if (reg->lang_count <= 0 || !reg->languages) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    // 长度校验：调用方必须提供与已注册语言数量一致的有效条目
    for (int i = 0; i < reg->lang_count; i++) {
        if (!messages[i]) {
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
    }
    
    // Check if error already exists
    nl_error_entry_t* existing = find_error_entry(reg, code);
    if (existing) {
        // Update existing：先复制再替换，strdup 失败则保持原值并返回错误
        int n = existing->lang_count < reg->lang_count ? existing->lang_count : reg->lang_count;
        for (int i = 0; i < n; i++) {
            char* dup = nl_strdup(messages[i]);
            if (!dup) {
                NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
                return -1;
            }
            free(existing->messages[i]);
            existing->messages[i] = dup;
        }
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return 0;
    }
    
    // Create new entry
    nl_error_entry_t* entry = (nl_error_entry_t*)malloc(sizeof(nl_error_entry_t));
    if (!entry) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    char** msgs = (char**)malloc((size_t)reg->lang_count * sizeof(char*));
    if (!msgs) {
        free(entry);
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    entry->code = code;
    entry->lang_count = reg->lang_count;
    entry->messages = msgs;
    
    for (int i = 0; i < reg->lang_count; i++) {
        msgs[i] = nl_strdup(messages[i]);
        if (!msgs[i]) {
            // OOM：回收已分配条目，不改变链表状态
            for (int j = 0; j < i; j++) free(msgs[j]);
            free(msgs);
            free(entry);
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
    }
    
    entry->next = reg->errors;
    reg->errors = entry;
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

int nl_lang_add_error_single(int lib_id, int code, const char* lang_code, const char* message) {
    if (!lang_code || !message) return -1;
    if (nl_lang_validate_code(lang_code) != 1) return -1;
    
    char normalized[NL_LANG_CODE_MAX_LEN];
    nl_lang_normalize_code(lang_code, normalized, sizeof(normalized));
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    int lang_idx = find_lang_index_in_lib(reg, normalized);
    if (lang_idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;  // Language not registered for this lib
    }
    
    nl_error_entry_t* entry = find_error_entry(reg, code);
    if (!entry) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;  // Error code not found
    }
    
    char* dup = nl_strdup(message);
    if (!dup) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    free(entry->messages[lang_idx]);
    entry->messages[lang_idx] = dup;
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

// Set error message for a single language (creates error entry if not exists)
int nl_lang_set_error(int lib_id, int code, const char* lang_code, const char* message) {
    if (!lang_code || !message) return -1;
    if (nl_lang_validate_code(lang_code) != 1) return -1;
    
    char normalized[NL_LANG_CODE_MAX_LEN];
    nl_lang_normalize_code(lang_code, normalized, sizeof(normalized));
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    int lang_idx = find_lang_index_in_lib(reg, normalized);
    if (lang_idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;  // Language not registered for this lib
    }
    
    // 先复制消息，strdup 失败时不会留下半成品状态
    char* dup = nl_strdup(message);
    if (!dup) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_entry_t* entry = find_error_entry(reg, code);
    if (!entry) {
        // Create new error entry
        entry = (nl_error_entry_t*)malloc(sizeof(nl_error_entry_t));
        if (!entry) {
            free(dup);
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
        
        char** msgs = (char**)malloc((size_t)reg->lang_count * sizeof(char*));
        if (!msgs) {
            free(entry);
            free(dup);
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
        
        entry->code = code;
        entry->lang_count = reg->lang_count;
        entry->messages = msgs;
        
        // Initialize all messages to NULL
        for (int i = 0; i < reg->lang_count; i++) {
            msgs[i] = NULL;
        }
        
        entry->next = reg->errors;
        reg->errors = entry;
    }
    
    // Set message for this language
    if (entry->messages[lang_idx]) {
        free(entry->messages[lang_idx]);
    }
    entry->messages[lang_idx] = dup;
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

// Remove error message for a single language
int nl_lang_remove_error_single(int lib_id, int code, const char* lang_code) {
    if (!lang_code) return -1;
    
    char normalized[NL_LANG_CODE_MAX_LEN];
    nl_lang_normalize_code(lang_code, normalized, sizeof(normalized));
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    int lang_idx = find_lang_index_in_lib(reg, normalized);
    if (lang_idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_entry_t* entry = find_error_entry(reg, code);
    if (!entry) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    if (entry->messages[lang_idx]) {
        free(entry->messages[lang_idx]);
        entry->messages[lang_idx] = NULL;
    }
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

// Register custom error code
int nl_lang_register_custom_code(int lib_id, int code) {
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    
    // Check if code already exists
    if (find_error_entry(reg, code)) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;  // Code already exists
    }
    
    // Create new error entry with empty messages
    nl_error_entry_t* entry = (nl_error_entry_t*)malloc(sizeof(nl_error_entry_t));
    if (!entry) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    char** msgs = (char**)malloc((size_t)reg->lang_count * sizeof(char*));
    if (!msgs) {
        free(entry);
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    entry->code = code;
    entry->lang_count = reg->lang_count;
    entry->messages = msgs;
    
    for (int i = 0; i < reg->lang_count; i++) {
        msgs[i] = NULL;
    }
    
    entry->next = reg->errors;
    reg->errors = entry;
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

// Check if error code exists
int nl_lang_has_error(int lib_id, int code) {
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return 0;
    }
    
    int result = (find_error_entry(&g_lib_registry[idx], code) != NULL) ? 1 : 0;
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return result;
}

// Get all registered error codes
int* nl_lang_get_error_codes(int lib_id, int* count) {
    // L11：入口校验输出指针非空；负数库 ID 视为未注册，直接返回空结果
    if (!count) return NULL;
    if (lib_id < 0) { *count = 0; return NULL; }
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        *count = 0;
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return NULL;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    
    // Count errors
    int error_count = 0;
    nl_error_entry_t* entry = reg->errors;
    while (entry) {
        error_count++;
        entry = entry->next;
    }
    
    *count = error_count;
    if (error_count == 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return NULL;
    }
    
    // Allocate and fill array
    int* codes = (int*)malloc((error_count + 1) * sizeof(int));
    if (!codes) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return NULL;
    }
    
    entry = reg->errors;
    for (int i = 0; i < error_count && entry; i++) {
        codes[i] = entry->code;
        entry = entry->next;
    }
    codes[error_count] = 0;  // Terminator
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return codes;
}

int nl_lang_remove_error(int lib_id, int code) {
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    nl_error_entry_t* prev = NULL;
    nl_error_entry_t* entry = reg->errors;
    
    while (entry) {
        if (entry->code == code) {
            if (prev) prev->next = entry->next;
            else reg->errors = entry->next;
            
            for (int i = 0; i < entry->lang_count; i++) {
                free(entry->messages[i]);
            }
            free(entry->messages);
            free(entry);
            
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return 0;
        }
        prev = entry;
        entry = entry->next;
    }
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return -1;  // Not found
}

// =========================================
// Error Message Lookup
// =========================================

const char* nl_lang_get_error(int lib_id, int error_code) {
    return nl_lang_get_error_for(lib_id, error_code, g_current_lang);
}

// 线程局部消息副本：环形多槽以降低连续调用互相覆盖的影响
#define NL_LANG_MSG_COPY_SLOTS 4
#define NL_LANG_MSG_COPY_SIZE  1024

const char* nl_lang_get_error_for(int lib_id, int error_code, const char* lang_code) {
    if (!lang_code) return NULL;
    
    // Normalize lang_code for lookup
    char normalized[NL_LANG_CODE_MAX_LEN];
    nl_lang_normalize_code(lang_code, normalized, sizeof(normalized));
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return NULL;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    int lang_idx = find_lang_index_in_lib(reg, normalized);
    if (lang_idx < 0) {
        // Try default language
        lang_idx = find_lang_index_in_lib(reg, g_default_lang);
        if (lang_idx < 0) {
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return NULL;
        }
    }
    
    nl_error_entry_t* entry = find_error_entry(reg, error_code);
    if (!entry) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return NULL;
    }
    
    // 索引越界与空消息都视为未找到，避免读取越界或返回 NULL 指针
    const char* msg = (lang_idx < entry->lang_count) ? entry->messages[lang_idx] : NULL;
    if (!msg) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return NULL;
    }
    
    // 在持锁期间复制到线程局部缓冲：解锁后不再返回指向内部存储的指针，
    // 防止其它线程注销/改写导致悬垂指针
    static NL_LANG_THREAD_LOCAL char copy_slots[NL_LANG_MSG_COPY_SLOTS][NL_LANG_MSG_COPY_SIZE];
    static NL_LANG_THREAD_LOCAL int copy_idx = 0;
    char* slot = copy_slots[copy_idx];
    copy_idx = (copy_idx + 1) % NL_LANG_MSG_COPY_SLOTS;
    size_t n = strlen(msg);
    if (n >= (size_t)NL_LANG_MSG_COPY_SIZE) n = (size_t)NL_LANG_MSG_COPY_SIZE - 1;
    memcpy(slot, msg, n);
    slot[n] = '\0';
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return slot;
}

const char* nl_lang_format_error(char* buffer, size_t buffer_size,
    int lib_id, int error_code, const char* context) {
    return nl_lang_format_error_for(buffer, buffer_size, lib_id, error_code, g_current_lang, context);
}

const char* nl_lang_format_error_for(char* buffer, size_t buffer_size,
    int lib_id, int error_code, const char* lang_code, const char* context) {
    if (!buffer || buffer_size == 0) return NULL;
    
    const char* msg = nl_lang_get_error_for(lib_id, error_code, lang_code);
    const char* category = nl_lang_get_error_category(error_code);
    const char* lib_name = nl_lang_get_lib_name(lib_id);
    
    if (msg) {
        if (context) {
            snprintf(buffer, buffer_size, "[%s/%s] %d: %s (%s)",
                lib_name ? lib_name : "unknown", category, error_code, msg, context);
        } else {
            snprintf(buffer, buffer_size, "[%s/%s] %d: %s",
                lib_name ? lib_name : "unknown", category, error_code, msg);
        }
        return buffer;
    }
    
    // Not found
    if (context) {
        snprintf(buffer, buffer_size, "[%s/Unknown] %d (lib=%d, context=%s)",
            lib_name ? lib_name : "unknown", error_code, lib_id, context);
    } else {
        snprintf(buffer, buffer_size, "[%s/Unknown] %d (lib=%d)",
            lib_name ? lib_name : "unknown", error_code, lib_id);
    }
    return buffer;
}

// =========================================
// Utility Functions
// =========================================

int nl_error_is_success(int error_code) {
    return error_code == 0;
}

const char* nl_lang_get_error_category(int error_code) {
    if (error_code == 0) return "Success";
    if (error_code > 0) return "Warning";
    if (error_code >= -10) return "Parameter";
    if (error_code >= -20) return "Resource";
    if (error_code >= -30) return "Platform";
    if (error_code >= -40) return "Network";
    if (error_code >= -50) return "Protocol";
    return "Unknown";
}

const char* nl_lang_get_lib_name(int lib_id) {
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    const char* result = NULL;
    for (int i = 0; i < g_lib_name_count; i++) {
        if (g_lib_names[i].lib_id == lib_id) {
            result = g_lib_names[i].name;
            break;
        }
    }
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return result;
}

void nl_lang_register_lib_name(int lib_id, const char* name) {
    if (!name) return;
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    // Check if exists
    for (int i = 0; i < g_lib_name_count; i++) {
        if (g_lib_names[i].lib_id == lib_id) {
            strncpy(g_lib_names[i].name, name, sizeof(g_lib_names[i].name) - 1);
            g_lib_names[i].name[sizeof(g_lib_names[i].name) - 1] = '\0';  // strncpy 不保证终止符
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return;
        }
    }
    
    // Add new
    if (g_lib_name_count < MAX_LIBRARIES) {
        g_lib_names[g_lib_name_count].lib_id = lib_id;
        strncpy(g_lib_names[g_lib_name_count].name, name, sizeof(g_lib_names[g_lib_name_count].name) - 1);
        g_lib_names[g_lib_name_count].name[sizeof(g_lib_names[g_lib_name_count].name) - 1] = '\0';
        g_lib_name_count++;
    }
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
}

// =========================================
// URI Handler Registration
// =========================================

void nl_lang_set_uri_handler(const char* scheme, nl_uri_handler_t handler) {
    if (!scheme || !handler) return;
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    // Check if exists
    for (int i = 0; i < g_uri_handler_count; i++) {
        if (strcmp(g_uri_handlers[i].scheme, scheme) == 0) {
            g_uri_handlers[i].handler = handler;
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return;
        }
    }
    
    // Add new
    if (g_uri_handler_count < MAX_URI_HANDLERS) {
        strncpy(g_uri_handlers[g_uri_handler_count].scheme, scheme, sizeof(g_uri_handlers[g_uri_handler_count].scheme) - 1);
        g_uri_handlers[g_uri_handler_count].scheme[sizeof(g_uri_handlers[g_uri_handler_count].scheme) - 1] = '\0';
        g_uri_handlers[g_uri_handler_count].handler = handler;
        g_uri_handler_count++;
    }
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
}

void nl_lang_unset_uri_handler(const char* scheme) {
    if (!scheme) return;
    
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    for (int i = 0; i < g_uri_handler_count; i++) {
        if (strcmp(g_uri_handlers[i].scheme, scheme) == 0) {
            for (int j = i; j < g_uri_handler_count - 1; j++) {
                g_uri_handlers[j] = g_uri_handlers[j + 1];
            }
            g_uri_handler_count--;
            break;
        }
    }
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
}

// =========================================
// File Loading (JSON) - Stub implementations
// =========================================

int nl_lang_load_json(const char* filepath) {
    (void)filepath;  // Suppress unused parameter warning
    // TODO: Implement JSON parsing
    return -1;
}

int nl_lang_load_json_for(int lib_id, const char* filepath) {
    (void)lib_id;    // Suppress unused parameter warning
    (void)filepath;  // Suppress unused parameter warning
    // TODO: Implement JSON parsing
    return -1;
}

int nl_lang_save_json(int lib_id, const char* filepath) {
    (void)lib_id;    // Suppress unused parameter warning
    (void)filepath;  // Suppress unused parameter warning
    // TODO: Implement JSON saving
    return -1;
}

// =========================================
// URL/URI Loading - Stub implementations
// =========================================

int nl_lang_load_url(const char* url) {
    (void)url;  // Suppress unused parameter warning
    // TODO: Implement URL loading
    return -1;
}

int nl_lang_load_url_for(int lib_id, const char* url) {
    (void)lib_id;  // Suppress unused parameter warning
    (void)url;     // Suppress unused parameter warning
    // TODO: Implement URL loading
    return -1;
}

// =========================================
// Multi-File Support - Stub implementations
// =========================================

int nl_lang_load_json_multi(int lib_id, const char** files, int file_count) {
    (void)lib_id;
    if (!files || file_count <= 0) return -1;
    return -1;
}

// =========================================
// Shared File Support - Stub implementations
// =========================================

int nl_lang_register_shared_file(const nl_lang_shared_file_t* config) {
    if (!config) return -1;
    // TODO: Implement shared file registration
    return -1;
}

int nl_lang_load_shared_file(const char* filepath) {
    (void)filepath;
    return -1;
}

int nl_lang_is_shared_lib(int lib_id) {
    (void)lib_id;
    return 0;
}

int* nl_lang_get_shared_libraries(int lib_id, int* count) {
    (void)lib_id;
    if (!count) return NULL;
    *count = 0;
    return NULL;
}

// =========================================
// Duplicate Code Detection
// =========================================

static int g_strict_duplicates = 1;  // Enabled by default

int nl_lang_check_code_conflict(int lib_id, int code) {
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    // Check all registered libraries for this code
    for (int i = 0; i < g_lib_count; i++) {
        if (g_lib_registry[i].lib_id == lib_id) continue;  // Skip self
        if (find_error_entry(&g_lib_registry[i], code)) {
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return g_lib_registry[i].lib_id;  // Return conflicting lib_id
        }
    }
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;  // No conflict
}

void nl_lang_set_strict_duplicates(int enabled) {
    g_strict_duplicates = enabled;
}

int nl_lang_get_strict_duplicates(void) {
    return g_strict_duplicates;
}

// =========================================
// Multi-URL Support - Stub implementations
// =========================================

int nl_lang_load_url_multi(int lib_id, const char** urls, int url_count) {
    (void)lib_id;
    if (!urls || url_count <= 0) return -1;
    return -1;
}

// =========================================
// Async Loading Support - Stub implementations
// =========================================

int nl_lang_load_json_async(int lib_id, const char* filepath,
    nl_lang_async_callback_t callback, void* user_data) {
    (void)lib_id; (void)filepath; (void)callback; (void)user_data;
    return -1;
}

int nl_lang_load_json_multi_async(int lib_id, const char** files, int file_count,
    nl_lang_async_callback_t callback, void* user_data) {
    (void)lib_id; (void)files; (void)file_count; (void)callback; (void)user_data;
    return -1;
}

int nl_lang_load_url_async(int lib_id, const char* url,
    nl_lang_async_callback_t callback, void* user_data) {
    (void)lib_id; (void)url; (void)callback; (void)user_data;
    return -1;
}

int nl_lang_load_url_multi_async(int lib_id, const char** urls, int url_count,
    nl_lang_async_callback_t callback, void* user_data) {
    (void)lib_id; (void)urls; (void)url_count; (void)callback; (void)user_data;
    return -1;
}

int nl_lang_load_shared_file_async(const char* filepath,
    nl_lang_async_callback_t callback, void* user_data) {
    (void)filepath; (void)callback; (void)user_data;
    return -1;
}

int nl_lang_is_async_loading(int lib_id) {
    (void)lib_id;
    return 0;
}

int nl_lang_wait_async(int lib_id, int timeout_ms) {
    (void)lib_id; (void)timeout_ms;
    return -1;
}

int nl_lang_cancel_async(int lib_id) {
    (void)lib_id;
    return -1;
}

int nl_lang_get_async_progress(int lib_id) {
    (void)lib_id;
    return 0;
}

// =========================================
// Table-driven error registration (v2.4.1)
// =========================================

NL_LANG_API int nl_lang_register_errors_ex(int lib_id, const nl_lang_error_def_t* table, int count) {
    static const char* langs[] = { "en_us", "zh_cn" };
    if (!table || count <= 0) return -1;

    if (!nl_lang_is_registered(lib_id)) {
        nl_lang_register_lib(lib_id, langs, 2);
    } else {
        nl_lang_register_language(lib_id, "en_us");
        nl_lang_register_language(lib_id, "zh_cn");
    }

    for (int i = 0; i < count; ++i) {
        if (table[i].en) nl_lang_set_error(lib_id, table[i].code, "en_us", table[i].en);
        if (table[i].zh) nl_lang_set_error(lib_id, table[i].code, "zh_cn", table[i].zh);
    }
    return 0;
}

// =========================================
// Variable Substitution System (v2.4.0)
// =========================================

#define MAX_VARIABLES 128
#define MAX_SCRIPT_ENGINES 8
#define MAX_SCRIPT_RESULT 256

static nl_lang_variable_t* g_var_head = NULL;
static nl_lang_mutex_t g_var_mutex;
static nl_lang_once_t g_var_once = NL_LANG_ONCE_INIT;

typedef struct {
    char id[32];
    nl_script_exec_func_t exec_func;
    void* userdata;
} nl_script_engine_entry_t;

static nl_script_engine_entry_t g_script_engines[MAX_SCRIPT_ENGINES];
static int g_script_engine_count = 0;

// 一次性初始化回调，理由同 init_registry_mutex_once（L3）
NL_LANG_ONCE_DEFINE(init_var_mutex_once) {
    NL_LANG_ONCE_BODY(g_var_mutex);
}

static void init_var_mutex(void) {
    NL_LANG_ONCE_RUN(g_var_once, init_var_mutex_once);
}

static nl_lang_variable_t* nl_lang_var_find(const char* name) {
    nl_lang_variable_t* cur = g_var_head;
    while (cur) {
        if (strcmp(cur->name, name) == 0) return cur;
        cur = cur->next;
    }
    return NULL;
}

// ---- Dynamic / external variable resolution (v2.4.1) ----
#if defined(_WIN32)
/* On Windows `_environ` is a macro provided by <stdlib.h>; do not redeclare it. */
#define NL_LANG_ENVIRON _environ
#else
extern char** environ;
#define NL_LANG_ENVIRON environ
#endif

// Create a zero-initialized variable and link it into the list (caller holds lock).
static nl_lang_variable_t* nl_lang_var_create(const char* name) {
    nl_lang_variable_t* v = (nl_lang_variable_t*)calloc(1, sizeof(nl_lang_variable_t));
    if (!v) return NULL;
    v->name = nl_strdup(name);
    if (!v->name) {  // OOM：不把半成品链入链表
        free(v);
        return NULL;
    }
    v->type = NL_VAR_TYPE_STRING;
    v->next = g_var_head;
    g_var_head = v;
    return v;
}

// Resolve the textual value of a variable.
// - Plain string variables are copied into `out` while the lock is held and
//   `out` is returned (never the internal pointer, which a concurrent
//   set/remove may free).
// - Dynamic providers and environment bindings are evaluated OUTSIDE the lock
//   (a provider may not safely call back into the variable API while locked),
//   writing into `out`.
// Returns NULL when the variable does not exist or has no value.
static const char* nl_lang_var_lookup(const char* name, char* out, size_t out_size) {
    nl_var_provider_t prov = NULL;
    void* pdata = NULL;
    char envbuf[128];
    int has_env = 0;
    nl_var_type_t type = NL_VAR_TYPE_NONE;
    const char* sval = NULL;
    long long ival = 0;
    double fval = 0.0;
    int bval = 0;
    int have_out = (out && out_size > 0);
    int str_copied = 0;  // 是否已在锁内把字符串值拷入调用方缓冲
    nl_lang_variable_t* var;

    // 调用前先清空输出缓冲：provider 契约被违反（返回非 0 却未写 out）时
    // 也不会把未初始化的栈内容当作变量值返回
    if (have_out) out[0] = '\0';

    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    var = nl_lang_var_find(name);
    if (var) {
        prov = var->provider;
        pdata = var->provider_data;
        if (var->env_name) {
            has_env = 1;
            strncpy(envbuf, var->env_name, sizeof(envbuf) - 1);
            envbuf[sizeof(envbuf) - 1] = '\0';
        }
        type = var->type;
        sval = var->value_str;
        ival = var->value_int;
        fval = var->value_float;
        bval = var->value_bool;
        // M6：原先 STRING 分支解锁后直接返回内部指针 sval，并发的 set/remove
        // 会释放该内存造成悬垂。必须在持锁期间复制到调用方缓冲，并返回该缓冲。
        if (sval && have_out) {
            size_t n = strlen(sval);
            if (n >= out_size) n = out_size - 1;
            memcpy(out, sval, n);
            out[n] = '\0';
            str_copied = 1;
        }
    }
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);

    if (!var) return NULL;

    if (prov && prov(name, out, out_size, pdata)) return out;

    if (has_env) {
        const char* ev = getenv(envbuf);
        if (ev) { snprintf(out, out_size, "%s", ev); return out; }
    }

    switch (type) {
        case NL_VAR_TYPE_STRING:  return str_copied ? out : NULL;
        case NL_VAR_TYPE_INTEGER: snprintf(out, out_size, "%lld", ival); return out;
        case NL_VAR_TYPE_FLOAT:   snprintf(out, out_size, "%g", fval); return out;
        case NL_VAR_TYPE_BOOL:    snprintf(out, out_size, "%s", bval ? "true" : "false"); return out;
        default:                  return str_copied ? out : NULL;
    }
}

// 说明：原先这里存在 g_nl_var_last_str / g_nl_var_last_str_buf 全局共享缓冲，
// 条件求值左右操作数共用它会导致字符串比较恒真（M3）。现改为由调用方传入
// 独立缓冲，故不再需要该共享存储。

NL_LANG_API int nl_lang_var_set(const char* name, const char* value) {
    if (!name || !value) return -1;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (var) {
        char* dup = nl_strdup(value);
        if (!dup) { NL_LANG_MUTEX_UNLOCK(&g_var_mutex); return -1; }
        free(var->value_str);
        var->value_str = dup;
        var->type = NL_VAR_TYPE_STRING;
        NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
        return 0;
    }
    var = (nl_lang_variable_t*)malloc(sizeof(nl_lang_variable_t));
    if (!var) { NL_LANG_MUTEX_UNLOCK(&g_var_mutex); return -1; }
    var->name = nl_strdup(name);
    var->value_str = nl_strdup(value);
    if (!var->name || !var->value_str) {
        free(var->name);
        free(var->value_str);
        free(var);
        NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
        return -1;
    }
    var->value_int = 0;
    var->value_float = 0.0;
    var->value_bool = 0;
    var->type = NL_VAR_TYPE_STRING;
    var->provider = NULL;
    var->provider_data = NULL;
    var->env_name = NULL;
    var->next = g_var_head;
    g_var_head = var;
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return 0;
}

NL_LANG_API int nl_lang_var_set_int(const char* name, long long value) {
    if (!name) return -1;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (var) {
        free(var->value_str);
        var->value_str = NULL;
        var->value_int = value;
        var->type = NL_VAR_TYPE_INTEGER;
        NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
        return 0;
    }
    var = (nl_lang_variable_t*)malloc(sizeof(nl_lang_variable_t));
    if (!var) { NL_LANG_MUTEX_UNLOCK(&g_var_mutex); return -1; }
    var->name = nl_strdup(name);
    var->value_str = NULL;
    var->value_int = value;
    var->value_float = 0.0;
    var->value_bool = 0;
    var->type = NL_VAR_TYPE_INTEGER;
    if (!var->name) {
        NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
        free(var);
        return -1;
    }
    var->provider = NULL;
    var->provider_data = NULL;
    var->env_name = NULL;
    var->next = g_var_head;
    g_var_head = var;
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return 0;
}

NL_LANG_API int nl_lang_var_set_float(const char* name, double value) {
    if (!name) return -1;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (var) {
        free(var->value_str);
        var->value_str = NULL;
        var->value_float = value;
        var->type = NL_VAR_TYPE_FLOAT;
        NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
        return 0;
    }
    var = (nl_lang_variable_t*)malloc(sizeof(nl_lang_variable_t));
    if (!var) { NL_LANG_MUTEX_UNLOCK(&g_var_mutex); return -1; }
    var->name = nl_strdup(name);
    var->value_str = NULL;
    var->value_int = 0;
    var->value_float = value;
    var->value_bool = 0;
    var->type = NL_VAR_TYPE_FLOAT;
    if (!var->name) {
        NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
        free(var);
        return -1;
    }
    var->provider = NULL;
    var->provider_data = NULL;
    var->env_name = NULL;
    var->next = g_var_head;
    g_var_head = var;
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return 0;
}

NL_LANG_API int nl_lang_var_set_bool(const char* name, int value) {
    if (!name) return -1;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (var) {
        free(var->value_str);
        var->value_str = NULL;
        var->value_bool = value;
        var->type = NL_VAR_TYPE_BOOL;
        NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
        return 0;
    }
    var = (nl_lang_variable_t*)malloc(sizeof(nl_lang_variable_t));
    if (!var) { NL_LANG_MUTEX_UNLOCK(&g_var_mutex); return -1; }
    var->name = nl_strdup(name);
    var->value_str = NULL;
    var->value_int = 0;
    var->value_float = 0.0;
    var->value_bool = value;
    var->type = NL_VAR_TYPE_BOOL;
    if (!var->name) {
        NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
        free(var);
        return -1;
    }
    var->provider = NULL;
    var->provider_data = NULL;
    var->env_name = NULL;
    var->next = g_var_head;
    g_var_head = var;
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return 0;
}

NL_LANG_API const char* nl_lang_var_get(const char* name) {
    static char get_buf[512];
    if (!name) return NULL;
    return nl_lang_var_lookup(name, get_buf, sizeof(get_buf));
}

NL_LANG_API long long nl_lang_var_get_int(const char* name, long long default_value) {
    char dbuf[512];
    long long val = default_value;
    if (!name) return default_value;
    if (nl_lang_var_is_dynamic(name)) {
        const char* s = nl_lang_var_lookup(name, dbuf, sizeof(dbuf));
        return s ? atoll(s) : default_value;
    }
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (var) {
        if (var->type == NL_VAR_TYPE_INTEGER) val = var->value_int;
        else if (var->type == NL_VAR_TYPE_FLOAT) val = (long long)var->value_float;
        else if (var->type == NL_VAR_TYPE_BOOL) val = var->value_bool ? 1 : 0;
        else if (var->type == NL_VAR_TYPE_STRING && var->value_str) val = atoll(var->value_str);
    }
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return val;
}

NL_LANG_API double nl_lang_var_get_float(const char* name, double default_value) {
    char dbuf[512];
    double val = default_value;
    if (!name) return default_value;
    if (nl_lang_var_is_dynamic(name)) {
        const char* s = nl_lang_var_lookup(name, dbuf, sizeof(dbuf));
        return s ? atof(s) : default_value;
    }
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (var) {
        if (var->type == NL_VAR_TYPE_FLOAT) val = var->value_float;
        else if (var->type == NL_VAR_TYPE_INTEGER) val = (double)var->value_int;
        else if (var->type == NL_VAR_TYPE_BOOL) val = var->value_bool ? 1.0 : 0.0;
        else if (var->type == NL_VAR_TYPE_STRING && var->value_str) val = atof(var->value_str);
    }
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return val;
}

NL_LANG_API int nl_lang_var_get_bool(const char* name, int default_value) {
    char dbuf[512];
    int val = default_value;
    if (!name) return default_value;
    if (nl_lang_var_is_dynamic(name)) {
        const char* s = nl_lang_var_lookup(name, dbuf, sizeof(dbuf));
        if (!s) return default_value;
        if (strcmp(s, "true") == 0 || strcmp(s, "1") == 0) return 1;
        if (strcmp(s, "false") == 0 || strcmp(s, "0") == 0) return 0;
        return (atoi(s) != 0) ? 1 : 0;
    }
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (var) {
        if (var->type == NL_VAR_TYPE_BOOL) val = var->value_bool;
        else if (var->type == NL_VAR_TYPE_INTEGER) val = (var->value_int != 0) ? 1 : 0;
        else if (var->type == NL_VAR_TYPE_FLOAT) val = (var->value_float != 0.0) ? 1 : 0;
        else if (var->type == NL_VAR_TYPE_STRING && var->value_str) {
            if (strcmp(var->value_str, "true") == 0 || strcmp(var->value_str, "1") == 0) val = 1;
            else if (strcmp(var->value_str, "false") == 0 || strcmp(var->value_str, "0") == 0) val = 0;
            else val = (atoi(var->value_str) != 0) ? 1 : 0;
        }
    }
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return val;
}

NL_LANG_API int nl_lang_var_exists(const char* name) {
    if (!name) return 0;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    int found = (nl_lang_var_find(name) != NULL);
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return found;
}

NL_LANG_API int nl_lang_var_remove(const char* name) {
    if (!name) return -1;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* prev = NULL;
    nl_lang_variable_t* cur = g_var_head;
    while (cur) {
        if (strcmp(cur->name, name) == 0) {
            if (prev) prev->next = cur->next;
            else g_var_head = cur->next;
            free(cur->name);
            free(cur->value_str);
            free(cur->env_name);
            free(cur);
            NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return -1;
}

NL_LANG_API void nl_lang_var_clear_all(void) {
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* cur = g_var_head;
    while (cur) {
        nl_lang_variable_t* next = cur->next;
        free(cur->name);
        free(cur->value_str);
        free(cur->env_name);
        free(cur);
        cur = next;
    }
    g_var_head = NULL;
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
}

static void nl_lang_var_free_value(nl_lang_variable_t* var) {
    if (var->value_str) { free(var->value_str); var->value_str = NULL; }
}

// ---- Dynamic / external variable management (v2.4.1) ----

NL_LANG_API int nl_lang_var_set_provider(const char* name, nl_var_provider_t provider, void* userdata) {
    if (!name || !provider) return -1;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (!var) var = nl_lang_var_create(name);
    if (!var) { NL_LANG_MUTEX_UNLOCK(&g_var_mutex); return -1; }
    var->provider = provider;
    var->provider_data = userdata;
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return 0;
}

NL_LANG_API int nl_lang_var_is_dynamic(const char* name) {
    int r = 0;
    if (!name) return 0;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (var && (var->provider || var->env_name)) r = 1;
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return r;
}

NL_LANG_API int nl_lang_var_bind_env(const char* name, const char* env_name) {
    if (!name || !env_name) return -1;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_lang_variable_t* var = nl_lang_var_find(name);
    if (!var) var = nl_lang_var_create(name);
    if (!var) { NL_LANG_MUTEX_UNLOCK(&g_var_mutex); return -1; }
    free(var->env_name);
    var->env_name = nl_strdup(env_name);
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return 0;
}

static int nl_lang_var_accept(const char* key, const char* prefix) {
    if (!prefix || prefix[0] == '\0') return 1;
    return strncmp(key, prefix, strlen(prefix)) == 0;
}

NL_LANG_API int nl_lang_var_load_env(const char* prefix) {
    int count = 0;
    char** env = NL_LANG_ENVIRON;
    if (!env) return 0;
    for (; *env; ++env) {
        const char* eq = strchr(*env, '=');
        char key[128];
        size_t klen;
        if (!eq) continue;
        klen = (size_t)(eq - *env);
        if (klen == 0 || klen >= sizeof(key)) continue;
        memcpy(key, *env, klen);
        key[klen] = '\0';
        if (!nl_lang_var_accept(key, prefix)) continue;
        nl_lang_var_set(key, eq + 1);
        count++;
    }
    return count;
}

NL_LANG_API int nl_lang_var_load_file(const char* filepath, const char* prefix) {
    FILE* f;
    char line[1024];
    int count = 0;
    if (!filepath) return -1;
    f = fopen(filepath, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char* key;
        char* val;
        char* eq;
        size_t len;
        char* p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#' || *p == ';') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        key = p;
        val = eq + 1;
        len = strlen(key);
        while (len > 0 && isspace((unsigned char)key[len - 1])) key[--len] = '\0';
        while (*val && isspace((unsigned char)*val)) val++;
        len = strlen(val);
        while (len > 0 && isspace((unsigned char)val[len - 1])) val[--len] = '\0';
        if (len >= 2 && ((val[0] == '"' && val[len - 1] == '"') ||
                         (val[0] == '\'' && val[len - 1] == '\''))) {
            val[len - 1] = '\0';
            val++;
        }
        if (key[0] == '\0') continue;
        if (!nl_lang_var_accept(key, prefix)) continue;
        nl_lang_var_set(key, val);
        count++;
    }
    fclose(f);
    return count;
}

NL_LANG_API const char* nl_lang_var_replace(const char* input, char* output, size_t output_size) {
    if (!input || !output || output_size == 0) return NULL;

    const char* p = input;
    char* out = output;
    const char* end = output + output_size - 1;

    while (*p && out < end) {
        if (p[0] == '{' && p[1] == '{') {
            const char* close = strchr(p + 2, '}');
            if (close && close[1] == '}') {
                char var_name[128];
                size_t len = (size_t)(close - p - 2);
                if (len >= sizeof(var_name)) len = sizeof(var_name) - 1;
                memcpy(var_name, p + 2, len);
                var_name[len] = '\0';

                // Trim whitespace from variable name
                char* start = var_name;
                while (*start && isspace((unsigned char)*start)) start++;
                // 先判空串：全空白时 start 指向 '\0'，若直接 start+strlen-1 会指针回绕
                char* last = start + strlen(start);
                while (last > start && isspace((unsigned char)last[-1])) { last--; *last = '\0'; }

                char vbuf[256];
                const char* vstr = nl_lang_var_lookup(start, vbuf, sizeof(vbuf));
                if (vstr && vstr[0] != '\0') {
                    size_t vl = strlen(vstr);
                    if (out + vl <= end) { memcpy(out, vstr, vl); out += vl; }
                }
                p = close + 2;
            } else {
                *out++ = *p++;
            }
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    return output;
}

// Returns 0=int, 1=float, 2=string, 3=bool
// str_buf/str_buf_size: 调用方提供的独立字符串缓冲。左右操作数各自传入不同的缓冲，
// 从而消除原先共用同一静态缓冲导致字符串比较恒真的问题（M3）。
static int nl_lang_var_parse_expr_value_full(char** expr, long long* out_int, double* out_float,
    int* out_bool, const char** out_str, int* out_str_len, char* str_buf, size_t str_buf_size) {
    while (**expr && isspace((unsigned char)**expr)) (*expr)++;
    if (!**expr) return 0;

    // String literal
    if (**expr == '\'') {
        (*expr)++;
        const char* start = *expr;
        while (**expr && **expr != '\'') (*expr)++;
        // 长度必须在跳过闭合单引号之前计算，否则会把闭合引号也计入内容，
        // 导致字面量 'Alice' 被解析成 Alice'（与变量字符串比较恒不相等）。
        size_t slen = (size_t)(*expr - start);
        if (**expr == '\'') (*expr)++;
        (void)out_int; (void)out_float; (void)out_bool;
        if (str_buf && str_buf_size > 0) {
            if (slen >= str_buf_size) slen = str_buf_size - 1;
            memcpy(str_buf, start, slen);
            str_buf[slen] = '\0';
            if (out_str) *out_str = str_buf;
        } else {
            slen = 0;
            if (out_str) *out_str = "";
        }
        if (out_str_len) *out_str_len = (int)slen;
        return 2;
    }

    // Number
    char* endptr;
    double d = strtod(*expr, &endptr);
    if (endptr != *expr) {
        if (out_int) *out_int = (long long)d;
        if (out_float) *out_float = d;
        if (out_bool) *out_bool = (d != 0.0) ? 1 : 0;
        *expr = endptr;
        while (**expr && isspace((unsigned char)**expr)) (*expr)++;
        // Check if it's an integer or float
        char tmp_d[64]; snprintf(tmp_d, sizeof(tmp_d), "%g", d);
        if (strchr(tmp_d, '.') || strchr(tmp_d, 'e') || strchr(tmp_d, 'E')) return 1;
        return 0;
    }

    // Boolean / NULL
    if (strncmp(*expr, "true", 4) == 0) {
        if (out_bool) *out_bool = 1;
        if (out_int) *out_int = 1;
        *expr += 4;
        return 3;
    }
    if (strncmp(*expr, "false", 5) == 0) {
        if (out_bool) *out_bool = 0;
        if (out_int) *out_int = 0;
        *expr += 5;
        return 3;
    }
    if (strncmp(*expr, "null", 4) == 0 || strncmp(*expr, "nil", 3) == 0) {
        if (out_int) *out_int = 0;
        if (out_bool) *out_bool = 0;
        *expr += (strncmp(*expr, "null", 4)==0) ? 4 : 3;
        return 0;
    }

    // Variable reference：在锁内一次性取出所需字段并按需拷入调用方缓冲，
    // 之后统一在函数出口前解锁。这样任何分支（含 STRING 且 value_str==NULL、
    // NONE/SCRIPT 等无值类型）都不会漏解锁而造成确定性死锁（H4）。
    const char* vstart = *expr;
    while (**expr && (isalnum((unsigned char)**expr) || **expr == '_')) (*expr)++;
    if (*expr > vstart) {
        size_t vlen = (size_t)(*expr - vstart);
        char varname[128];
        if (vlen >= sizeof(varname)) vlen = sizeof(varname) - 1;
        memcpy(varname, vstart, vlen);
        varname[vlen] = '\0';

        int found = 0;
        int have_str = 0;
        size_t str_len = 0;
        nl_var_type_t vtype = NL_VAR_TYPE_NONE;
        long long vint = 0;
        double vfloat = 0.0;
        int vbool = 0;

        init_var_mutex();
        NL_LANG_MUTEX_LOCK(&g_var_mutex);
        nl_lang_variable_t* var = nl_lang_var_find(varname);
        if (var) {
            found = 1;
            vtype = var->type;
            vint = var->value_int;
            vfloat = var->value_float;
            vbool = var->value_bool;
            if (vtype == NL_VAR_TYPE_STRING && var->value_str) {
                have_str = 1;
                str_len = strlen(var->value_str);
                if (str_buf && str_buf_size > 0) {
                    size_t n = str_len;
                    if (n >= str_buf_size) n = str_buf_size - 1;
                    memcpy(str_buf, var->value_str, n);
                    str_buf[n] = '\0';
                    str_len = n;
                } else {
                    str_len = 0;
                }
            }
        }
        NL_LANG_MUTEX_UNLOCK(&g_var_mutex);

        if (!found) {
            // 未定义变量：按整数 0 处理（无值）
            if (out_int) *out_int = 0;
            if (out_bool) *out_bool = 0;
            return 0;
        }

        switch (vtype) {
            case NL_VAR_TYPE_STRING:
                if (!have_str) {
                    // STRING 但 value_str == NULL：视为无值，按整数 0 处理
                    if (out_int) *out_int = 0;
                    if (out_bool) *out_bool = 0;
                    return 0;
                }
                if (out_str) *out_str = (str_buf && str_buf_size > 0) ? str_buf : "";
                if (out_str_len) *out_str_len = (int)str_len;
                return 2;
            case NL_VAR_TYPE_INTEGER:
                if (out_int) *out_int = vint;
                return 0;
            case NL_VAR_TYPE_FLOAT:
                if (out_float) *out_float = vfloat;
                return 1;
            case NL_VAR_TYPE_BOOL:
                if (out_bool) *out_bool = vbool;
                return 3;
            default:
                // NONE / SCRIPT 等无确定数值语义的类型按无值处理
                if (out_int) *out_int = 0;
                if (out_bool) *out_bool = 0;
                return 0;
        }
    }
    return 0;
}

// 把数值类操作数格式化为文本，供「字符串 vs 数值」比较时使用（L7）。
// 参数类型编码与 nl_lang_var_parse_expr_value_full 一致：0=int、1=float、2=string、3=bool。
static void nl_lang_format_operand_text(int type, long long iv, double fv, int bv,
    char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (type == 0) snprintf(buf, buf_size, "%lld", iv);
    else if (type == 3) snprintf(buf, buf_size, "%d", bv ? 1 : 0);
    else snprintf(buf, buf_size, "%g", fv);
}

NL_LANG_API int nl_lang_var_condition_eval(const char* expr) {
    if (!expr) return 0;
    while (*expr && isspace((unsigned char)*expr)) expr++;
    if (!*expr) return 0;

    long long left_int = 0, right_int = 0;
    double left_float = 0.0, right_float = 0.0;
    int left_bool = 0, right_bool = 0;
    const char* left_str = NULL;
    int left_str_len = 0;
    const char* right_str = NULL;
    int right_str_len = 0;

    // Find comparison operator (must handle ==, !=, >=, <= carefully)
    const char* op = NULL;
    const char* p = expr;
    while (*p) {
        // Skip string literals
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') p++;
            if (*p == '\'') p++;
            continue;
        }
        // Check for two-char operators first
        if (p[0] == '=' && p[1] == '=') { op = p; break; }
        if (p[0] == '!' && p[1] == '=') { op = p; break; }
        if (p[0] == '>' && p[1] == '=') { op = p; break; }
        if (p[0] == '<' && p[1] == '=') { op = p; break; }
        // Check single-char operators (but not inside strings - already skipped)
        if (p[0] == '>' || p[0] == '<' || p[0] == '=') {
            // Make sure it's not part of a two-char operator we already checked
            if (p[0] != '=' || p[1] != '=') {
                op = p;
                break;
            }
        }
        p++;
    }
    if (!op) return 0;

    size_t op_len = 1;
    if (op[0] == '!' && op[1] == '=') op_len = 2;
    else if (op[0] == '=' && op[1] == '=') op_len = 2;
    else if ((op[0] == '>' || op[0] == '<') && op[1] == '=') op_len = 2;

    char left_expr[256], right_expr[256];
    size_t llen = (size_t)(op - expr);
    if (llen >= sizeof(left_expr)) llen = sizeof(left_expr) - 1;
    memcpy(left_expr, expr, llen);
    left_expr[llen] = '\0';

    size_t rlen = strlen(op + op_len);
    if (rlen >= sizeof(right_expr)) rlen = sizeof(right_expr) - 1;
    memcpy(right_expr, op + op_len, rlen);
    right_expr[rlen] = '\0';

    // 左右操作数各自使用独立字符串缓冲，避免共用缓冲导致字符串比较恒真（M3）
    char left_str_buf[256];
    char right_str_buf[256];
    char* left_ptr = left_expr;
    char* right_ptr = right_expr;
    int left_type = nl_lang_var_parse_expr_value_full(&left_ptr, &left_int, &left_float, &left_bool,
        &left_str, &left_str_len, left_str_buf, sizeof(left_str_buf));
    int right_type = nl_lang_var_parse_expr_value_full(&right_ptr, &right_int, &right_float, &right_bool,
        &right_str, &right_str_len, right_str_buf, sizeof(right_str_buf));

    int result = 0;
    // String comparison takes priority if either side is a string
    if (left_type == 2 || right_type == 2) {
        // L7：非字符串一侧必须先按数值格式化为文本再比较。原先直接取原始表达式
        // 文本（如 " 18 "、"+18"、布尔字面量 "true"）会得到错误结论。
        char left_num_buf[64];
        char right_num_buf[64];
        const char* ls;
        const char* rs;
        if (left_type == 2) {
            ls = left_str ? left_str : "";
        } else {
            nl_lang_format_operand_text(left_type, left_int, left_float, left_bool,
                left_num_buf, sizeof(left_num_buf));
            ls = left_num_buf;
        }
        if (right_type == 2) {
            rs = right_str ? right_str : "";
        } else {
            nl_lang_format_operand_text(right_type, right_int, right_float, right_bool,
                right_num_buf, sizeof(right_num_buf));
            rs = right_num_buf;
        }
        if (op_len == 2 && op[0] == '!') result = (strcmp(ls, rs) != 0) ? 1 : 0;
        else if (op_len == 2 && op[0] == '=') result = (strcmp(ls, rs) == 0) ? 1 : 0;
        else if (op[0] == '>') result = (strcmp(ls, rs) > 0) ? 1 : 0;
        else if (op[0] == '<') result = (strcmp(ls, rs) < 0) ? 1 : 0;
        else result = (strcmp(ls, rs) == 0) ? 1 : 0;
    } else {
        double lv = left_float;
        double rv = right_float;
        if (left_type == 0) lv = (double)left_int;
        if (right_type == 0) rv = (double)right_int;
        // M1：布尔字面量（true/false）与 NL_VAR_TYPE_BOOL 变量在解析时只写入
        // out_bool/out_int，未写入 out_float。数值分支必须显式取 1.0/0.0，
        // 否则会被当成 0.0（左右一致处理）。
        if (left_type == 3) lv = left_bool ? 1.0 : 0.0;
        if (right_type == 3) rv = right_bool ? 1.0 : 0.0;
        if (op_len == 2 && op[0] == '!') result = (lv != rv) ? 1 : 0;
        else if (op_len == 2 && op[0] == '=') result = (lv == rv) ? 1 : 0;
        else if (op_len == 2 && op[0] == '>') result = (lv >= rv) ? 1 : 0;
        else if (op_len == 2 && op[0] == '<') result = (lv <= rv) ? 1 : 0;
        else if (op[0] == '>') result = (lv > rv) ? 1 : 0;
        else if (op[0] == '<') result = (lv < rv) ? 1 : 0;
        else if (op[0] == '=') result = (lv == rv) ? 1 : 0;
    }
    return result;
}

NL_LANG_API int nl_lang_register_script_engine(const char* engine_id, nl_script_exec_func_t exec_func, void* userdata) {
    if (!engine_id || !exec_func) return -1;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    for (int i = 0; i < g_script_engine_count; i++) {
        if (strcmp(g_script_engines[i].id, engine_id) == 0) {
            g_script_engines[i].exec_func = exec_func;
            g_script_engines[i].userdata = userdata;
            NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
            return 0;
        }
    }
    if (g_script_engine_count >= MAX_SCRIPT_ENGINES) { NL_LANG_MUTEX_UNLOCK(&g_var_mutex); return -1; }
    strncpy(g_script_engines[g_script_engine_count].id, engine_id, sizeof(g_script_engines[0].id) - 1);
    g_script_engines[g_script_engine_count].id[sizeof(g_script_engines[0].id) - 1] = '\0';  // strncpy 不保证终止符
    g_script_engines[g_script_engine_count].exec_func = exec_func;
    g_script_engines[g_script_engine_count].userdata = userdata;
    g_script_engine_count++;
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return 0;
}

NL_LANG_API void nl_lang_unregister_script_engine(const char* engine_id) {
    if (!engine_id) return;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    for (int i = 0; i < g_script_engine_count; i++) {
        if (strcmp(g_script_engines[i].id, engine_id) == 0) {
            for (int j = i; j < g_script_engine_count - 1; j++) g_script_engines[j] = g_script_engines[j + 1];
            g_script_engine_count--;
            break;
        }
    }
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
}

NL_LANG_API int nl_lang_execute_script(const char* engine_id, const char* script, const char* lang_code, int error_code, char* result, size_t result_size) {
    if (!engine_id || !script) return -1;
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    nl_script_exec_func_t func = NULL;
    void* ud = NULL;
    for (int i = 0; i < g_script_engine_count; i++) {
        if (strcmp(g_script_engines[i].id, engine_id) == 0) {
            func = g_script_engines[i].exec_func;
            ud = g_script_engines[i].userdata;
            break;
        }
    }
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    if (!func) return -1;

    nl_script_result_t* res = func(script, lang_code, error_code, ud);
    // L1：result_len 为负时直接转 size_t 会变成极大值导致 memcpy 越界；同时须
    // 保证 res->result 非空。全程用 size_t 比较，避免 (int)result_size 溢出。
    if (res && res->success && result && result_size > 0 &&
        res->result && res->result_len >= 0) {
        size_t max_copy = result_size - 1;
        size_t rlen = (size_t)res->result_len;
        size_t copy_len = (rlen < max_copy) ? rlen : max_copy;
        memcpy(result, res->result, copy_len);
        result[copy_len] = '\0';
        return 0;
    }
    return -1;
}

NL_LANG_API const char** nl_lang_get_script_engines(int* count) {
    init_var_mutex();
    NL_LANG_MUTEX_LOCK(&g_var_mutex);
    static const char* engines[MAX_SCRIPT_ENGINES + 1];
    for (int i = 0; i < g_script_engine_count; i++) engines[i] = g_script_engines[i].id;
    engines[g_script_engine_count] = NULL;
    if (count) *count = g_script_engine_count;
    NL_LANG_MUTEX_UNLOCK(&g_var_mutex);
    return engines;
}

// =========================================
// HTML <var> Tag Variable Substitution (v2.4.0)
// =========================================

// Strip "nl.lang." prefix if present, returns pointer into name or new buffer
static const char* nl_lang_var_strip_prefix(const char* name, char* buf, size_t buf_size) {
    const char* prefix = "nl.lang.";
    if (strncmp(name, prefix, 8) == 0) {
        size_t remaining = strlen(name + 8);
        if (remaining >= buf_size) remaining = buf_size - 1;
        memcpy(buf, name + 8, remaining);
        buf[remaining] = '\0';
        return buf;
    }
    return name;
}

NL_LANG_API const char* nl_lang_var_replace_html(const char* input, char* output, size_t output_size) {
    if (!input || !output || output_size == 0) return NULL;

    const char* p = input;
    char* out = output;
    const char* end = output + output_size - 1;
    char var_buf[256];  // Local buffer for stripped var names

    while (*p && out < end) {
        if (p[0] == '{' && p[1] == '{' && p[2] == '<' && p[3] == 'v' &&
            p[4] == 'a' && p[5] == 'r' && p[6] == '>') {
            const char* tag_start = p + 7;  // skip "{{<var>"
            const char* tag_end = strstr(tag_start, "</var>}}");
            if (tag_end) {
                size_t var_len = (size_t)(tag_end - tag_start);
                if (var_len >= sizeof(var_buf)) var_len = sizeof(var_buf) - 1;
                memcpy(var_buf, tag_start, var_len);
                var_buf[var_len] = '\0';

                // Strip nl.lang. prefix if present
                char stripped[256];
                const char* clean_name = nl_lang_var_strip_prefix(var_buf, stripped, sizeof(stripped));

                char vbuf[256];
                const char* vstr = nl_lang_var_lookup(clean_name, vbuf, sizeof(vbuf));
                if (vstr && vstr[0] != '\0') {
                    size_t vl = strlen(vstr);
                    if (out + vl <= end) { memcpy(out, vstr, vl); out += vl; }
                }
                p = tag_end + 8;  // skip "</var>}}"
            } else {
                *out++ = *p++;
            }
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    return output;
}

NL_LANG_API const char* nl_lang_get_error_with_vars(int lib_id, int error_code, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) return NULL;
    const char* msg = nl_lang_get_error(lib_id, error_code);
    if (!msg) {
        snprintf(buffer, buffer_size, "[Error %d not found]", error_code);
        return buffer;
    }
    nl_lang_var_replace_html(msg, buffer, buffer_size);
    return buffer;
}

// =========================================
// Module Functions for Lazy Loading
// =========================================

const char* nl_lang_version(void) {
    return NL_LANG_VERSION;
}

int nl_lang_is_available(void) {
    return 1;  // Always available on all platforms
}

int nl_lang_init(void) {
    add_language_global("en_us");
    add_language_global("zh_cn");
    init_var_mutex();
    return 0;
}

void nl_lang_shutdown(void) {
    nl_lang_var_clear_all();
    // Cleanup is handled by individual unregister calls
}

// =========================================
// Lang Module Info (NL Extension System)
// =========================================

static nl_module_info_t g_lang_module_info = {
    .type = NL_MODULE_LANG,
    .name = "lang",
    .version = NL_LANG_VERSION,
    .capabilities = NL_CAP_THREAD_SAFE | NL_CAP_ASYNC | NL_CAP_PLATFORM_ALL | NL_CAP_EXT_SYSTEM |
                    NL_LANG_CAP_VARIABLES | NL_LANG_CAP_SCRIPTING | NL_LANG_CAP_CONDITIONALS,
    .status = NL_MODULE_STATUS_UNINITIALIZED,
    .platform_windows = 1,
    .platform_linux = 1,
    .platform_macos = 1,
    .init = nl_lang_init,
    .shutdown = nl_lang_shutdown,
    .is_available = nl_lang_is_available,
    .get_version = nl_lang_version,
    .description = "Multi-language error message translation with variable substitution and scripting (v2.4.0)",
    .author = "508364",
    .next = NULL
};

nl_module_info_t* nl_lang_get_module_info(void) {
    return &g_lang_module_info;
}

// =========================================
// Extension Definition (for dynamic loading)
// =========================================

NL_EXTENSION_DEFINE(lang, "Language Support", NL_LANG_VERSION, "508364",
    "Multi-language error message translation with variable substitution and scripting",
    "Windows,Linux,MacOS",
    NL_CAP_THREAD_SAFE | NL_CAP_ASYNC | NL_LANG_CAP_VARIABLES | NL_LANG_CAP_SCRIPTING | NL_LANG_CAP_CONDITIONALS,
    nl_lang_init, nl_lang_shutdown, nl_lang_is_available, nl_lang_version);

nl_extension_info_t* nl_lang_get_extension_info(void) {
    return &nl_extension_info_lang;
}

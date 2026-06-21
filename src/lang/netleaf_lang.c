#define _POSIX_C_SOURCE 200809L
#include "netleaf_lang.h"
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
#else
    #include <pthread.h>
    #define nl_strdup strdup
    typedef pthread_mutex_t nl_lang_mutex_t;
    #define NL_LANG_MUTEX_INIT(m)   pthread_mutex_init(m, NULL)
    #define NL_LANG_MUTEX_LOCK(m)   pthread_mutex_lock(m)
    #define NL_LANG_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
    #define NL_LANG_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
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
static int g_mutex_initialized = 0;

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

static void init_mutex(void) {
    if (!g_mutex_initialized) {
        NL_LANG_MUTEX_INIT(&g_registry_mutex);
        g_mutex_initialized = 1;
    }
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
        g_all_languages[g_all_lang_count] = nl_strdup(lang_code);
        g_all_lang_count++;
    }
}

// =========================================
// Language Code Validation (xx_xx format, case-insensitive)
// =========================================

int nl_lang_validate_code(const char* code) {
    if (!code) return -1;
    
    size_t len = strlen(code);
    if (len < 2 || len > NL_LANG_CODE_MAX_LEN) return -1;
    
    // Must contain exactly one underscore
    const char* underscore = strchr(code, '_');
    if (!underscore) return -1;
    if (strchr(underscore + 1, '_') != NULL) return -1;  // Multiple underscores
    
    // Underscore must not be at start or end
    if (underscore == code || underscore[1] == '\0') return -1;
    
    // All characters must be letters (case-insensitive) or digits
    for (const char* p = code; *p; p++) {
        if (*p == '_') continue;
        if (!isalpha((unsigned char)*p) && !isdigit((unsigned char)*p)) return -1;
    }
    
    // Language part (before underscore) must be 2-3 letters
    size_t lang_len = underscore - code;
    if (lang_len < 2 || lang_len > 3) return -1;
    
    // Country part (after underscore) must be 2-3 letters
    size_t country_len = strlen(underscore + 1);
    if (country_len < 2 || country_len > 3) return -1;
    
    return 0;
}

// Normalize language code to lowercase (xx_xx)
static void nl_lang_normalize_code(const char* src, char* dest, size_t dest_size) {
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
    if (nl_lang_validate_code(code) != 0) return -1;
    nl_lang_normalize_code(code, g_current_lang, sizeof(g_current_lang));
    return 0;
}

const char* nl_lang_get_default(void) {
    return g_default_lang;
}

int nl_lang_set_default(const char* code) {
    if (nl_lang_validate_code(code) != 0) return -1;
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
        if (nl_lang_validate_code(languages[i]) != 0) return -1;
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
        // Update existing
        for (int i = 0; i < g_lib_registry[idx].lang_count; i++) {
            free(g_lib_registry[idx].languages[i]);
        }
        free(g_lib_registry[idx].languages);
        g_lib_registry[idx].languages = (char**)malloc(lang_count * sizeof(char*));
        g_lib_registry[idx].lang_count = lang_count;
        for (int i = 0; i < lang_count; i++) {
            g_lib_registry[idx].languages[i] = nl_strdup(normalized[i]);
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
    
    g_lib_registry[g_lib_count].lib_id = lib_id;
    g_lib_registry[g_lib_count].languages = (char**)malloc(lang_count * sizeof(char*));
    g_lib_registry[g_lib_count].lang_count = lang_count;
    g_lib_registry[g_lib_count].errors = NULL;
    
    for (int i = 0; i < lang_count; i++) {
        g_lib_registry[g_lib_count].languages[i] = nl_strdup(normalized[i]);
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
    if (nl_lang_validate_code(lang_code) != 0) return -1;
    
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
        g_lib_registry[g_lib_count].languages = (char**)malloc(sizeof(char*));
        g_lib_registry[g_lib_count].languages[0] = nl_strdup(normalized);
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
    
    // Add new language
    char** new_languages = (char**)malloc((reg->lang_count + 1) * sizeof(char*));
    for (int i = 0; i < reg->lang_count; i++) {
        new_languages[i] = reg->languages[i];
    }
    new_languages[reg->lang_count] = nl_strdup(normalized);
    free(reg->languages);
    reg->languages = new_languages;
    reg->lang_count++;
    
    // Expand all error entries to accommodate new language
    nl_error_entry_t* entry = reg->errors;
    while (entry) {
        char** new_messages = (char**)malloc(reg->lang_count * sizeof(char*));
        for (int i = 0; i < entry->lang_count; i++) {
            new_messages[i] = entry->messages[i];
        }
        new_messages[entry->lang_count] = NULL;  // Empty message for new language
        free(entry->messages);
        entry->messages = new_messages;
        entry->lang_count = reg->lang_count;
        entry = entry->next;
    }
    
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
    init_mutex();
    NL_LANG_MUTEX_LOCK(&g_registry_mutex);
    
    int idx = find_lib_index(lib_id);
    if (idx < 0) {
        NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
        return -1;
    }
    
    nl_error_registry_t* reg = &g_lib_registry[idx];
    
    // Check if error already exists
    nl_error_entry_t* existing = find_error_entry(reg, code);
    if (existing) {
        // Update existing
        for (int i = 0; i < existing->lang_count && i < reg->lang_count; i++) {
            free(existing->messages[i]);
            existing->messages[i] = nl_strdup(messages[i]);
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
    
    entry->code = code;
    entry->lang_count = reg->lang_count;
    entry->messages = (char**)malloc(reg->lang_count * sizeof(char*));
    
    for (int i = 0; i < reg->lang_count; i++) {
        entry->messages[i] = nl_strdup(messages[i]);
    }
    
    entry->next = reg->errors;
    reg->errors = entry;
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

int nl_lang_add_error_single(int lib_id, int code, const char* lang_code, const char* message) {
    if (!lang_code || !message) return -1;
    if (nl_lang_validate_code(lang_code) != 0) return -1;
    
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
    
    free(entry->messages[lang_idx]);
    entry->messages[lang_idx] = nl_strdup(message);
    
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return 0;
}

// Set error message for a single language (creates error entry if not exists)
int nl_lang_set_error(int lib_id, int code, const char* lang_code, const char* message) {
    if (!lang_code || !message) return -1;
    if (nl_lang_validate_code(lang_code) != 0) return -1;
    
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
        // Create new error entry
        entry = (nl_error_entry_t*)malloc(sizeof(nl_error_entry_t));
        if (!entry) {
            NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
            return -1;
        }
        
        entry->code = code;
        entry->lang_count = reg->lang_count;
        entry->messages = (char**)malloc(reg->lang_count * sizeof(char*));
        
        // Initialize all messages to NULL
        for (int i = 0; i < reg->lang_count; i++) {
            entry->messages[i] = NULL;
        }
        
        entry->next = reg->errors;
        reg->errors = entry;
    }
    
    // Set message for this language
    if (entry->messages[lang_idx]) {
        free(entry->messages[lang_idx]);
    }
    entry->messages[lang_idx] = nl_strdup(message);
    
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
    
    entry->code = code;
    entry->lang_count = reg->lang_count;
    entry->messages = (char**)malloc(reg->lang_count * sizeof(char*));
    
    for (int i = 0; i < reg->lang_count; i++) {
        entry->messages[i] = NULL;
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
    if (!count) return NULL;
    
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
    
    const char* msg = entry->messages[lang_idx];
    NL_LANG_MUTEX_UNLOCK(&g_registry_mutex);
    return msg;
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
    for (int i = 0; i < g_lib_name_count; i++) {
        if (g_lib_names[i].lib_id == lib_id) {
            return g_lib_names[i].name;
        }
    }
    return NULL;
}

void nl_lang_register_lib_name(int lib_id, const char* name) {
    if (!name) return;
    
    // Check if exists
    for (int i = 0; i < g_lib_name_count; i++) {
        if (g_lib_names[i].lib_id == lib_id) {
            strncpy(g_lib_names[i].name, name, sizeof(g_lib_names[i].name) - 1);
            return;
        }
    }
    
    // Add new
    if (g_lib_name_count < MAX_LIBRARIES) {
        g_lib_names[g_lib_name_count].lib_id = lib_id;
        strncpy(g_lib_names[g_lib_name_count].name, name, sizeof(g_lib_names[g_lib_name_count].name) - 1);
        g_lib_name_count++;
    }
}

// =========================================
// URI Handler Registration
// =========================================

void nl_lang_set_uri_handler(const char* scheme, nl_uri_handler_t handler) {
    if (!scheme || !handler) return;
    
    // Check if exists
    for (int i = 0; i < g_uri_handler_count; i++) {
        if (strcmp(g_uri_handlers[i].scheme, scheme) == 0) {
            g_uri_handlers[i].handler = handler;
            return;
        }
    }
    
    // Add new
    if (g_uri_handler_count < MAX_URI_HANDLERS) {
        strncpy(g_uri_handlers[g_uri_handler_count].scheme, scheme, sizeof(g_uri_handlers[g_uri_handler_count].scheme) - 1);
        g_uri_handlers[g_uri_handler_count].handler = handler;
        g_uri_handler_count++;
    }
}

void nl_lang_unset_uri_handler(const char* scheme) {
    if (!scheme) return;
    
    for (int i = 0; i < g_uri_handler_count; i++) {
        if (strcmp(g_uri_handlers[i].scheme, scheme) == 0) {
            for (int j = i; j < g_uri_handler_count - 1; j++) {
                g_uri_handlers[j] = g_uri_handlers[j + 1];
            }
            g_uri_handler_count--;
            return;
        }
    }
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
// Module Functions for Lazy Loading
// =========================================

const char* nl_lang_version(void) {
    return NL_LANG_VERSION;
}

int nl_lang_is_available(void) {
    return 1;  // Always available on all platforms
}

int nl_lang_init(void) {
    return 0;  // No initialization needed
}

void nl_lang_shutdown(void) {
    // Cleanup is handled by individual unregister calls
}
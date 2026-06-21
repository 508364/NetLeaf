#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "netleaf_autoroute.h"
#include "netleaf_module.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#ifdef _WIN32
#define strdup _strdup
#endif

// =========================================
// Module State
// =========================================

static int g_autoroute_available = 0;
static int g_autoroute_enabled = 0;
static nl_route_matcher_t* g_global_matcher = NULL;

// =========================================
// Module Info
// =========================================

static nl_module_info_t g_autoroute_module_info = {
    .type = NL_MODULE_AUTOROUTE,
    .name = "autoroute",
    .version = NL_AUTOROUTE_VERSION,
    .capabilities = NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL,
    .status = NL_MODULE_STATUS_UNINITIALIZED,
    .platform_windows = 1,
    .platform_linux = 1,
    .platform_macos = 1,
    .init = nl_autoroute_init,
    .shutdown = NULL,
    .is_available = nl_autoroute_is_available,
    .get_version = nl_autoroute_version,
    .description = "Automatic route suggestions and matching",
    .author = "NetLeaf Team",
    .next = NULL
};

nl_module_info_t* nl_autoroute_get_module_info(void) {
    return &g_autoroute_module_info;
}

// =========================================
// Flexible Enable/Disable Helper
// =========================================

static int parse_enable_value(const char* value) {
    if (!value) return 0;
    
    if (strcmp(value, "1") == 0 || strcmp(value, "0") == 0) {
        return atoi(value);
    }
    
    char lower[16];
    size_t i;
    for (i = 0; i < sizeof(lower) - 1 && value[i]; i++) {
        lower[i] = tolower((unsigned char)value[i]);
    }
    lower[i] = '\0';
    
    if (strcmp(lower, "true") == 0 || strcmp(lower, "on") == 0 || 
        strcmp(lower, "yes") == 0) {
        return 1;
    }
    if (strcmp(lower, "false") == 0 || strcmp(lower, "off") == 0 || 
        strcmp(lower, "no") == 0) {
        return 0;
    }
    
    return 0;
}

// =========================================
// Module Info API
// =========================================

int nl_autoroute_is_available(void) {
    return g_autoroute_available;
}

int nl_autoroute_init(void) {
    if (!g_autoroute_available) {
        g_autoroute_available = 1;
        g_autoroute_enabled = 1;
        g_global_matcher = nl_route_matcher_create();
    }
    return g_autoroute_available;
}

const char* nl_autoroute_version(void) {
    return NL_AUTOROUTE_VERSION;
}

// =========================================
// Enable/Disable API
// =========================================

void nl_autoroute_enable_ex(const char* enable_str) {
    g_autoroute_enabled = parse_enable_value(enable_str);
}

void nl_autoroute_enable(int enable) {
    g_autoroute_enabled = enable ? 1 : 0;
}

int nl_autoroute_is_enabled(void) {
    return g_autoroute_enabled;
}

// =========================================
// Levenshtein Distance
// =========================================

// Maximum string length for Levenshtein calculation (stack allocation)
#define NL_LEVEN_MAX_LEN 256

// Comparison function for qsort (descending order by score)
static int nl_route_suggestion_compare(const void* a, const void* b) {
    const nl_route_suggestion_t* sa = (const nl_route_suggestion_t*)a;
    const nl_route_suggestion_t* sb = (const nl_route_suggestion_t*)b;
    // Descending order: higher score first
    if (sb->score > sa->score) return 1;
    if (sb->score < sa->score) return -1;
    return 0;
}

int nl_levenshtein_distance(const char* s1, const char* s2) {
    if (!s1) s1 = "";
    if (!s2) s2 = "";
    
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    
    if (len1 == 0) return (int)len2;
    if (len2 == 0) return (int)len1;
    
    // Use stack allocation for typical path lengths
    // Fall back to heap for very long strings
    int use_stack = (len2 <= NL_LEVEN_MAX_LEN);
    
    int prev_stack[NL_LEVEN_MAX_LEN + 1];
    int curr_stack[NL_LEVEN_MAX_LEN + 1];
    int* prev = use_stack ? prev_stack : (int*)malloc((len2 + 1) * sizeof(int));
    int* curr = use_stack ? curr_stack : (int*)malloc((len2 + 1) * sizeof(int));
    
    if (!prev || !curr) {
        if (!use_stack) {
            free(prev);
            free(curr);
        }
        return -1;
    }
    
    for (size_t j = 0; j <= len2; j++) {
        prev[j] = (int)j;
    }
    
    for (size_t i = 1; i <= len1; i++) {
        curr[0] = (int)i;
        for (size_t j = 1; j <= len2; j++) {
            int cost = (s1[i-1] == s2[j-1]) ? 0 : 1;
            curr[j] = curr[j-1] + 1;
            int insert = prev[j] + 1;
            if (insert < curr[j]) curr[j] = insert;
            int replace = prev[j-1] + cost;
            if (replace < curr[j]) curr[j] = replace;
        }
        int* temp = prev;
        prev = curr;
        curr = temp;
    }
    
    int result = prev[len2];
    
    if (!use_stack) {
        free(prev);
        free(curr);
    }
    
    return result;
}

// =========================================
// Path Similarity Calculation
// =========================================

static double segment_similarity(const char* s1, const char* s2) {
    if (!s1 || !s2) return 0.0;
    if (strcmp(s1, s2) == 0) return 1.0;
    
    int dist = nl_levenshtein_distance(s1, s2);
    size_t max_len = strlen(s1) > strlen(s2) ? strlen(s1) : strlen(s2);
    
    if (max_len == 0) return 1.0;
    
    return 1.0 - ((double)dist / (double)max_len);
}

double nl_route_similarity(const char* path1, const char* path2) {
    if (!path1) path1 = "";
    if (!path2) path2 = "";
    
    if (strcmp(path1, path2) == 0) return 1.0;
    
    int seg1_count = 1;
    int seg2_count = 1;
    
    for (const char* p = path1; *p; p++) {
        if (*p == '/') seg1_count++;
    }
    for (const char* p = path2; *p; p++) {
        if (*p == '/') seg2_count++;
    }
    
    const char* paths1[64];
    const char* paths2[64];
    
    char tmp1[256], tmp2[256];
    strncpy(tmp1, path1, sizeof(tmp1) - 1);
    strncpy(tmp2, path2, sizeof(tmp2) - 1);
    
    paths1[0] = tmp1;
    paths2[0] = tmp2;
    
    int idx1 = 0, idx2 = 0;
    for (char* p = tmp1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            idx1++;
            paths1[idx1] = p + 1;
        }
    }
    seg1_count = idx1 + 1;
    
    for (char* p = tmp2; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            idx2++;
            paths2[idx2] = p + 1;
        }
    }
    seg2_count = idx2 + 1;
    
    double total_sim = 0.0;
    int min_segs = seg1_count < seg2_count ? seg1_count : seg2_count;
    int max_segs = seg1_count > seg2_count ? seg1_count : seg2_count;
    
    double seg_count_bonus = (seg1_count == seg2_count) ? 0.2 : 0.0;
    
    double prefix_bonus = 0.0;
    if (seg1_count > 0 && seg2_count > 0) {
        int same_prefix = 1;
        int min_common = min_segs;
        for (int i = 0; i < min_common; i++) {
            if (strcmp(paths1[i], paths2[i]) != 0) {
                same_prefix = 0;
                break;
            }
        }
        if (same_prefix) {
            prefix_bonus = 0.2 * ((double)min_common / (double)max_segs);
        }
    }
    
    for (int i = 0; i < min_segs; i++) {
        double seg_sim = segment_similarity(paths1[i], paths2[i]);
        total_sim += seg_sim;
    }
    
    double avg_seg_sim = (max_segs > 0) ? (total_sim / (double)max_segs) : 0.0;
    
    double score = avg_seg_sim * 0.6 + seg_count_bonus + prefix_bonus;
    
    if (score > 1.0) score = 1.0;
    
    return score;
}

// =========================================
// Route Matcher Implementation
// =========================================

struct nl_route_node {
    char segment[128];
    int is_leaf;
    struct nl_route_node* children[32];
    int child_count;
};

struct nl_route_matcher {
    struct nl_route_node* root;
    int route_count;
};

static struct nl_route_node* create_node(const char* segment) {
    struct nl_route_node* node = (struct nl_route_node*)calloc(1, sizeof(struct nl_route_node));
    if (node) {
        if (segment) {
            strncpy(node->segment, segment, sizeof(node->segment) - 1);
        }
    }
    return node;
}

static void destroy_node(struct nl_route_node* node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) {
        destroy_node(node->children[i]);
    }
    free(node);
}

static void add_segment_to_tree(struct nl_route_matcher* matcher, const char* path) {
    if (!matcher || !path) return;
    
    struct nl_route_node* current = matcher->root;
    
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    
    char* segment = tmp;
    char* next;
    
    while (segment && *segment) {
        next = strchr(segment, '/');
        if (next) {
            *next = '\0';
            next++;
        }
        
        if (*segment == '\0') {
            segment = next;
            continue;
        }
        
        struct nl_route_node* child = NULL;
        for (int i = 0; i < current->child_count; i++) {
            if (strcmp(current->children[i]->segment, segment) == 0) {
                child = current->children[i];
                break;
            }
        }
        
        if (!child) {
            child = create_node(segment);
            if (child && current->child_count < 32) {
                current->children[current->child_count++] = child;
            } else if (child) {
                free(child);
            }
        }
        
        current = child;
        segment = next;
    }
    
    if (current) {
        current->is_leaf = 1;
    }
}

static void collect_all_routes(struct nl_route_node* node, char* prefix, char** routes, int* count, int max_count) {
    if (!node || !routes || *count >= max_count) return;
    
    char new_prefix[512];
    if (prefix == NULL || prefix[0] == '\0') {
        if (node->segment[0] != '\0') {
            snprintf(new_prefix, sizeof(new_prefix), "/%s", node->segment);
        } else {
            new_prefix[0] = '\0';
        }
    } else {
        if (node->segment[0] != '\0') {
            snprintf(new_prefix, sizeof(new_prefix), "%s/%s", prefix, node->segment);
        } else {
            strncpy(new_prefix, prefix, sizeof(new_prefix) - 1);
        }
    }
    
    if (node->is_leaf && new_prefix[0] != '\0') {
        routes[*count] = strdup(new_prefix);
        (*count)++;
    }
    
    for (int i = 0; i < node->child_count; i++) {
        collect_all_routes(node->children[i], new_prefix, routes, count, max_count);
    }
}

nl_route_matcher_t* nl_route_matcher_create(void) {
    nl_route_matcher_t* matcher = (nl_route_matcher_t*)calloc(1, sizeof(nl_route_matcher_t));
    if (matcher) {
        matcher->root = create_node("");
        matcher->route_count = 0;
    }
    return matcher;
}

void nl_route_matcher_add_route(nl_route_matcher_t* matcher, const char* path) {
    if (!matcher || !path) return;
    add_segment_to_tree(matcher, path);
    matcher->route_count++;
}

void nl_route_matcher_add_routes(nl_route_matcher_t* matcher, const char** paths, int count) {
    if (!matcher || !paths) return;
    for (int i = 0; i < count; i++) {
        if (paths[i]) {
            nl_route_matcher_add_route(matcher, paths[i]);
        }
    }
}

void nl_route_matcher_destroy(nl_route_matcher_t* matcher) {
    if (matcher) {
        destroy_node(matcher->root);
        free(matcher);
    }
}

char* nl_route_matcher_find_similar(nl_route_matcher_t* matcher, const char* path, double min_similarity) {
    if (!matcher || !path) return NULL;
    
    char* routes[256];
    int count = 0;
    collect_all_routes(matcher->root, NULL, routes, &count, 256);
    
    double best_score = 0.0;
    char* best_route = NULL;
    
    for (int i = 0; i < count; i++) {
        double score = nl_route_similarity(path, routes[i]);
        if (score >= min_similarity && score > best_score) {
            best_score = score;
            if (best_route) free(best_route);
            best_route = routes[i];
        } else {
            free(routes[i]);
        }
    }
    
    return best_route;
}

int nl_route_matcher_get_suggestions(nl_route_matcher_t* matcher, const char* path,
                                     nl_route_suggestion_t* suggestions, int max_count) {
    if (!matcher || !path || !suggestions || max_count <= 0) return 0;
    
    char* routes[256];
    int count = 0;
    collect_all_routes(matcher->root, NULL, routes, &count, 256);
    
    for (int i = 0; i < count && i < max_count; i++) {
        suggestions[i].score = nl_route_similarity(path, routes[i]);
        strncpy(suggestions[i].path, routes[i], sizeof(suggestions[i].path) - 1);
        suggestions[i].path[sizeof(suggestions[i].path) - 1] = '\0';
    }
    
    // Use qsort instead of bubble sort for better performance
    int sort_count = (count < max_count) ? count : max_count;
    qsort(suggestions, sort_count, sizeof(nl_route_suggestion_t), nl_route_suggestion_compare);
    
    for (int i = max_count; i < count && i < 256; i++) {
        free(routes[i]);
    }
    for (int i = 0; i < count && i < max_count; i++) {
        free(routes[i]);
    }
    
    return (count < max_count) ? count : max_count;
}

const char* nl_find_best_route(const char* target, const char** routes, int count, double min_similarity) {
    if (!target || !routes || count <= 0) return NULL;
    
    const char* best = NULL;
    double best_score = 0.0;
    
    for (int i = 0; i < count; i++) {
        if (!routes[i]) continue;
        double score = nl_route_similarity(target, routes[i]);
        if (score >= min_similarity && score > best_score) {
            best_score = score;
            best = routes[i];
        }
    }
    
    return best;
}

int nl_route_matches(const char* pattern, const char* path) {
    if (!pattern || !path) return 0;
    if (strcmp(pattern, path) == 0) return 1;
    
    const char* p = pattern;
    const char* q = path;
    
    while (*p && *q) {
        if (*p == '*') {
            p++;
            if (*p == '*') {
                p++;
                if (*p == '\0') return 1;
                while (*q && *q != '/') q++;
            } else {
                while (*q && *q != '/') q++;
            }
        } else if (*p == *q) {
            p++;
            q++;
        } else {
            return 0;
        }
    }
    
    while (*p == '*') p++;
    
    return (*p == '\0' && *q == '\0');
}

// =========================================
// Global Route Matcher
// =========================================

nl_route_matcher_t* nl_autoroute_get_global_matcher(void) {
    if (!g_global_matcher) {
        g_global_matcher = nl_route_matcher_create();
    }
    return g_global_matcher;
}

void nl_autoroute_set_global_matcher(nl_route_matcher_t* matcher) {
    if (g_global_matcher) {
        nl_route_matcher_destroy(g_global_matcher);
    }
    g_global_matcher = matcher;
}

void nl_autoroute_discover_routes(void) {
    // This would be called by the web server to auto-discover routes
    // For now, routes are added manually via nl_route_matcher_add_route
}

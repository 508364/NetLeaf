#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "netleaf_vue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define strdup _strdup
#endif

static char* g_default_version = "3.4.0";
static nl_vue_cdn_type_t g_default_cdn = NL_VUE_CDN_UNPKG;
static int g_initialized = 0;
static char* g_local_vue_path = NULL;
static char* g_local_vue_content = NULL;

static const char* g_vue_import_patterns[] = {
    "vue.js", "vue.min.js", "vue.global.js", "vue.runtime.js",
    "cdn.jsdelivr.net/npm/vue", "unpkg.com/vue@", 
    "cdnjs.cloudflare.com/ajax/libs/vue/", "vuejs.org/"
};

static const char* g_vue_code_patterns[] = {
    "new Vue(", "Vue.createApp", "createApp(",
    "ref(", "reactive(", "computed(",
    "watch(", "watchEffect(",
    "onMounted(", "onUpdated(", "onUnmounted(",
    "v-if", "v-for", "v-show", "v-bind", "v-model",
    "{{", "}}",
    ":class", ":style", "@click", "@submit",
    "vue-app", "id=\"app\""
};

static const char* nl_responsive_css = 
    "<style>\n"
    "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; margin: 0; padding: 20px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; }\n"
    ".container { max-width: 1200px; margin: 0 auto; background: white; border-radius: 12px; box-shadow: 0 10px 40px rgba(0,0,0,0.15); padding: 30px; }\n"
    "h1 { color: #333; margin-top: 0; }\n"
    "h2 { color: #555; font-size: 18px; margin-top: 30px; }\n"
    ".card { background: #f8f9fa; border-radius: 8px; padding: 20px; margin-bottom: 15px; }\n"
    ".card-title { font-size: 14px; color: #666; margin-bottom: 8px; }\n"
    ".card-value { font-size: 24px; font-weight: bold; color: #333; }\n"
    ".badge { display: inline-block; padding: 4px 12px; border-radius: 20px; font-size: 12px; }\n"
    ".badge-success { background: #d4edda; color: #155724; }\n"
    ".badge-info { background: #d1ecf1; color: #0c5460; }\n"
    ".badge-warning { background: #fff3cd; color: #856404; }\n"
    ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }\n"
    ".module-list { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 10px; }\n"
    ".module-item { background: #f0f0f0; padding: 12px 15px; border-radius: 6px; display: flex; justify-content: space-between; align-items: center; }\n"
    "table { width: 100%%; border-collapse: collapse; margin-top: 10px; }\n"
    "th, td { padding: 10px; text-align: left; border-bottom: 1px solid #eee; }\n"
    "th { background: #f8f9fa; font-weight: 600; }\n"
    ".footer { margin-top: 30px; padding-top: 20px; border-top: 1px solid #eee; text-align: center; color: #888; font-size: 12px; }\n"
    "</style>\n";

static char* strcasestr_wrapper(const char* haystack, const char* needle, size_t haystack_len) {
    if (!haystack || !needle) return NULL;
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len) return NULL;
    
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        int match = 1;
        for (size_t j = 0; j < needle_len; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            if ((h >= 'A' && h <= 'Z') ? h + 32 : h != (n >= 'A' && n <= 'Z') ? n + 32 : n) {
                match = 0;
                break;
            }
        }
        if (match) return (char*)&haystack[i];
    }
    return NULL;
}

int nl_vue_init(void) {
    if (g_initialized) return 0;
    g_initialized = 1;
    return 0;
}

void nl_vue_shutdown(void) {
    g_initialized = 0;
    if (g_local_vue_path) {
        free(g_local_vue_path);
        g_local_vue_path = NULL;
    }
    if (g_local_vue_content) {
        free(g_local_vue_content);
        g_local_vue_content = NULL;
    }
}

int nl_vue_is_available(void) {
    return 1;
}

const char* nl_vue_version(void) {
    return NL_VUE_VERSION;
}

void nl_vue_set_default_version(const char* version) {
    if (version) {
        free(g_default_version);
        g_default_version = strdup(version);
    }
}

const char* nl_vue_get_default_version(void) {
    return g_default_version;
}

void nl_vue_set_default_cdn(nl_vue_cdn_type_t cdn_type) {
    g_default_cdn = cdn_type;
}

nl_vue_cdn_type_t nl_vue_get_default_cdn(void) {
    return g_default_cdn;
}

// 设置本地 Vue.js 文件路径
void nl_vue_set_local_path(const char* path) {
    if (g_local_vue_path) {
        free(g_local_vue_path);
        g_local_vue_path = NULL;
    }
    if (g_local_vue_content) {
        free(g_local_vue_content);
        g_local_vue_content = NULL;
    }
    if (path) {
        g_local_vue_path = strdup(path);
    }
}

// 从文件加载 Vue.js 内容
int nl_vue_load_from_file(const char* filepath) {
    if (!filepath) return -1;
    
    FILE* fp = fopen(filepath, "rb");
    if (!fp) return -1;
    
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (g_local_vue_content) {
        free(g_local_vue_content);
    }
    
    g_local_vue_content = malloc(fsize + 1);
    if (!g_local_vue_content) {
        fclose(fp);
        return -2;
    }
    
    size_t read_size = fread(g_local_vue_content, 1, fsize, fp);
    g_local_vue_content[read_size] = '\0';
    fclose(fp);
    
    if (g_local_vue_path) {
        free(g_local_vue_path);
    }
    g_local_vue_path = strdup(filepath);
    
    return 0;
}

// 获取本地 Vue.js 内容
const char* nl_vue_get_local_content(void) {
    return g_local_vue_content;
}

// 检查是否已加载本地 Vue.js
int nl_vue_has_local_content(void) {
    return g_local_vue_content != NULL;
}

// 获取本地 Vue.js 文件路径
const char* nl_vue_get_local_path(void) {
    return g_local_vue_path;
}

// 清除本地 Vue.js 内容
void nl_vue_clear_local_content(void) {
    if (g_local_vue_path) {
        free(g_local_vue_path);
        g_local_vue_path = NULL;
    }
    if (g_local_vue_content) {
        free(g_local_vue_content);
        g_local_vue_content = NULL;
    }
}

const char* nl_vue_get_cdn_url(nl_vue_cdn_type_t cdn_type, const char* version) {
    static char url[256];
    if (!version) version = g_default_version;
    
    switch (cdn_type) {
        case NL_VUE_CDN_UNPKG:
            snprintf(url, sizeof(url), "https://unpkg.com/vue@%s/dist/vue.global.js", version);
            break;
        case NL_VUE_CDN_CDNJS:
            snprintf(url, sizeof(url), "https://cdnjs.cloudflare.com/ajax/libs/vue/%s/vue.min.js", version);
            break;
        case NL_VUE_CDN_JSDELIVR:
            snprintf(url, sizeof(url), "https://cdn.jsdelivr.net/npm/vue@%s/dist/vue.global.js", version);
            break;
        case NL_VUE_CDN_LOCAL:
            snprintf(url, sizeof(url), "./vue.global.js");
            break;
        default:
            snprintf(url, sizeof(url), "https://unpkg.com/vue@%s/dist/vue.global.js", version);
            break;
    }
    return url;
}

char* nl_vue_get_cdn_script(nl_vue_cdn_type_t cdn_type, const char* version) {
    static char script[512];
    const char* url = nl_vue_get_cdn_url(cdn_type, version);
    snprintf(script, sizeof(script), "<script src=\"%s\"></script>\n", url);
    return strdup(script);
}

int nl_vue_detect_import(const char* html, size_t html_len) {
    if (!html || html_len == 0) return 0;
    
    for (size_t i = 0; i < sizeof(g_vue_import_patterns)/sizeof(g_vue_import_patterns[0]); i++) {
        if (strcasestr_wrapper(html, g_vue_import_patterns[i], html_len)) {
            return 1;
        }
    }
    return 0;
}

int nl_vue_detect_code(const char* html, size_t html_len) {
    if (!html || html_len == 0) return 0;
    
    int match_count = 0;
    for (size_t i = 0; i < sizeof(g_vue_code_patterns)/sizeof(g_vue_code_patterns[0]); i++) {
        if (strcasestr_wrapper(html, g_vue_code_patterns[i], html_len)) {
            match_count++;
            if (match_count >= 2) return 1;
        }
    }
    return 0;
}

char* nl_vue_add_import(const char* html, size_t html_len, 
                        nl_vue_cdn_type_t cdn_type, const char* version) {
    if (!html || html_len == 0) return NULL;
    
    int has_vue_code = nl_vue_detect_code(html, html_len);
    int has_vue_import = nl_vue_detect_import(html, html_len);
    
    if (!has_vue_code || has_vue_import) {
        return strdup(html);
    }
    
    const char* url = nl_vue_get_cdn_url(cdn_type, version);
    char script[512];
    int script_len = snprintf(script, sizeof(script), "<script src=\"%s\"></script>\n", url);
    
    char* head_end = strcasestr_wrapper(html, "</head", html_len);
    char* body_end = strcasestr_wrapper(html, "</body", html_len);
    char* inject = head_end ? head_end : (body_end ? body_end : (char*)html + html_len);
    
    size_t prefix = (size_t)(inject - html);
    size_t new_len = html_len + script_len;
    char* result = malloc(new_len + 1);
    if (!result) return NULL;
    
    memcpy(result, html, prefix);
    memcpy(result + prefix, script, script_len);
    memcpy(result + prefix + script_len, inject, html_len - prefix);
    result[new_len] = '\0';
    
    return result;
}

static char* substitute_variables(const char* text, const char** vars, const char** values, int count) {
    if (!text || !vars || !values || count <= 0) return strdup(text);
    
    size_t text_len = strlen(text);
    char* result = malloc(text_len + 1);
    if (!result) return NULL;
    strcpy(result, text);
    
    for (int i = 0; i < count; i++) {
        char placeholder[256];
        snprintf(placeholder, sizeof(placeholder), "{{<var>%s</var>}}", vars[i]);
        
        char* found = strstr(result, placeholder);
        while (found) {
            size_t before_len = found - result;
            size_t after_len = strlen(found + strlen(placeholder));
            size_t new_len = before_len + strlen(values[i]) + after_len + 1;
            char* new_result = malloc(new_len);
            if (!new_result) {
                free(result);
                return NULL;
            }
            
            memcpy(new_result, result, before_len);
            memcpy(new_result + before_len, values[i], strlen(values[i]));
            memcpy(new_result + before_len + strlen(values[i]), found + strlen(placeholder), after_len);
            new_result[new_len - 1] = '\0';
            
            free(result);
            result = new_result;
            found = strstr(result, placeholder);
        }
    }
    
    return result;
}

// 生成带内嵌 Vue.js 的 HTML 页面（本地文件模式）
char* nl_vue_generate_page_inline(const char* vue_code, const char* title, const char* vue_filepath) {
    if (!vue_code) return NULL;
    
    char* vue_content = NULL;
    FILE* fp = NULL;
    
    if (vue_filepath) {
        fp = fopen(vue_filepath, "rb");
        if (!fp) return NULL;
        
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        vue_content = malloc(fsize + 1);
        if (!vue_content) {
            fclose(fp);
            return NULL;
        }
        
        fread(vue_content, 1, fsize, fp);
        vue_content[fsize] = '\0';
        fclose(fp);
    } else if (g_local_vue_content) {
        vue_content = strdup(g_local_vue_content);
    }
    
    if (!vue_content) return NULL;
    
    size_t html_size = strlen(vue_code) + strlen(vue_content) + 4096;
    char* result = malloc(html_size);
    if (!result) {
        free(vue_content);
        return NULL;
    }
    
    snprintf(result, html_size,
        "<!DOCTYPE html>\n"
        "<html><head><title>%s</title>\n"
        "%s"
        "</head><body>\n"
        "<div id=\"app\">%s</div>\n"
        "<script>\n"
        "%s\n"
        "const { createApp, ref, reactive } = Vue;\n"
        "createApp({\n"
        "  setup() {\n"
        "    return {}\n"
        "  }\n"
        "}).mount('#app');\n"
        "</script>\n"
        "</body></html>",
        title ? title : "NetLeaf Vue", nl_responsive_css, vue_code, vue_content);
    
    free(vue_content);
    return result;
}

// 生成带内嵌 Vue.js 的计数器页面
char* nl_vue_generate_counter_inline(const char* title, const char* vue_filepath) {
    if (!vue_filepath && !g_local_vue_content) {
        // 如果没有 Vue 文件，返回系统信息页面
        return nl_vue_generate_sysinfo(title);
    }
    
    char* vue_content = NULL;
    FILE* fp = NULL;
    
    if (vue_filepath) {
        fp = fopen(vue_filepath, "rb");
        if (!fp) return NULL;
        
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        vue_content = malloc(fsize + 1);
        if (!vue_content) {
            fclose(fp);
            return NULL;
        }
        
        fread(vue_content, 1, fsize, fp);
        vue_content[fsize] = '\0';
        fclose(fp);
    } else {
        vue_content = strdup(g_local_vue_content);
    }
    
    size_t html_size = strlen(vue_content) + 8192;
    char* result = malloc(html_size);
    if (!result) {
        free(vue_content);
        return NULL;
    }
    
    snprintf(result, html_size,
        "<!DOCTYPE html>\n"
        "<html><head><title>%s</title>\n"
        "%s"
        "</head><body>\n"
        "<div class=\"container\">\n"
        "<h1>%s</h1>\n"
        "<div id=\"app\"></div>\n"
        "<script>\n"
        "%s\n"
        "const { createApp, ref } = Vue;\n"
        "createApp({\n"
        "  setup() {\n"
        "    const count = ref(0);\n"
        "    return { count }\n"
        "  },\n"
        "  template: '<button @click=\"count++\">Click: {{ count }}</button>'\n"
        "}).mount('#app');\n"
        "</script>\n"
        "</div>\n"
        "</body></html>",
        title ? title : "Counter", nl_responsive_css, title ? title : "Counter", vue_content);
    
    free(vue_content);
    return result;
}

// 生成带内嵌 Vue.js 的仪表盘页面
char* nl_vue_generate_dashboard_inline(const char* title, const char* vue_filepath) {
    if (!vue_filepath && !g_local_vue_content) {
        // 如果没有 Vue 文件，返回系统信息页面
        return nl_vue_generate_sysinfo(title);
    }
    
    char* vue_content = NULL;
    FILE* fp = NULL;
    
    if (vue_filepath) {
        fp = fopen(vue_filepath, "rb");
        if (!fp) return NULL;
        
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        vue_content = malloc(fsize + 1);
        if (!vue_content) {
            fclose(fp);
            return NULL;
        }
        
        fread(vue_content, 1, fsize, fp);
        vue_content[fsize] = '\0';
        fclose(fp);
    } else {
        vue_content = strdup(g_local_vue_content);
    }
    
    size_t html_size = strlen(vue_content) + 16384;
    char* result = malloc(html_size);
    if (!result) {
        free(vue_content);
        return NULL;
    }
    
    snprintf(result, html_size,
        "<!DOCTYPE html>\n"
        "<html><head><title>%s</title>\n"
        "%s"
        "</head><body>\n"
        "<div class=\"container\">\n"
        "<h1>%s</h1>\n"
        "<div id=\"app\"></div>\n"
        "<script>\n"
        "%s\n"
        "const { createApp, reactive, onMounted } = Vue;\n"
        "createApp({\n"
        "  setup() {\n"
        "    const stats = reactive({ cpu: 0, memory: 0, network: 0 });\n"
        "    const updateStats = () => {\n"
        "      stats.cpu = Math.floor(Math.random() * 100);\n"
        "      stats.memory = Math.floor(Math.random() * 100);\n"
        "      stats.network = Math.floor(Math.random() * 100);\n"
        "    };\n"
        "    onMounted(() => setInterval(updateStats, 1000));\n"
        "    updateStats();\n"
        "    return { stats };\n"
        "  },\n"
        "  template: '<div style=\"display: grid; grid-template-columns: repeat(3, 1fr); gap: 20px;\">' +\n"
        "    '<div style=\"background: #f5f5f5; padding: 20px; border-radius: 8px; text-align: center;\">' +\n"
        "      '<div style=\"font-size: 32px; font-weight: bold; color: #667eea;\">{{ stats.cpu }}%</div>' +\n"
        "      '<div style=\"color: #666; margin-top: 10px;\">CPU Usage</div>' +\n"
        "    '</div>' +\n"
        "    '<div style=\"background: #f5f5f5; padding: 20px; border-radius: 8px; text-align: center;\">' +\n"
        "      '<div style=\"font-size: 32px; font-weight: bold; color: #764ba2;\">{{ stats.memory }}%</div>' +\n"
        "      '<div style=\"color: #666; margin-top: 10px;\">Memory</div>' +\n"
        "    '</div>' +\n"
        "    '<div style=\"background: #f5f5f5; padding: 20px; border-radius: 8px; text-align: center;\">' +\n"
        "      '<div style=\"font-size: 32px; font-weight: bold; color: #f093fb;\">{{ stats.network }}%</div>' +\n"
        "      '<div style=\"color: #666; margin-top: 10px;\">Network</div>' +\n"
        "    '</div>' +\n"
        "  '</div>'\n"
        "}).mount('#app');\n"
        "</script>\n"
        "</div>\n"
        "</body></html>",
        title ? title : "Dashboard", nl_responsive_css, title ? title : "Dashboard", vue_content);
    
    free(vue_content);
    return result;
}

// 获取本地 Vue.js 文件的 script 标签
char* nl_vue_get_local_script(const char* vue_filepath) {
    static char script[512];
    const char* path = vue_filepath ? vue_filepath : (g_local_vue_path ? g_local_vue_path : "./vue.global.js");
    snprintf(script, sizeof(script), "<script src=\"%s\"></script>\n", path);
    return strdup(script);
}

char* nl_vue_generate_page(const char* vue_code, const char* title, 
                           nl_vue_cdn_type_t cdn_type, const char* version) {
    if (!vue_code) return NULL;
    
    const char* url = nl_vue_get_cdn_url(cdn_type, version);
    char* result = malloc(32768);
    if (!result) return NULL;
    
    snprintf(result, 32768,
        "<!DOCTYPE html>\n"
        "<html><head><title>%s</title>\n"
        "%s"
        "<script src=\"%s\"></script>\n"
        "</head><body>\n"
        "<div id=\"app\">%s</div>\n"
        "<script>\n"
        "const { createApp, ref, reactive } = Vue;\n"
        "createApp({\n"
        "  setup() {\n"
        "    return {}\n"
        "  }\n"
        "}).mount('#app');\n"
        "</script>\n"
        "</body></html>",
        title ? title : "NetLeaf Vue", nl_responsive_css, url, vue_code);
    
    return result;
}

char* nl_vue_generate_page_with_vars(const char* vue_code, const char* title,
                                     const char** vars, const char** values, int count,
                                     nl_vue_cdn_type_t cdn_type, const char* version) {
    if (!vue_code) return NULL;
    
    char* substituted = substitute_variables(vue_code, vars, values, count);
    if (!substituted) return NULL;
    
    const char* url = nl_vue_get_cdn_url(cdn_type, version);
    char* result = malloc(32768);
    if (!result) {
        free(substituted);
        return NULL;
    }
    
    snprintf(result, 32768,
        "<!DOCTYPE html>\n"
        "<html><head><title>%s</title>\n"
        "%s"
        "<script src=\"%s\"></script>\n"
        "</head><body>\n"
        "<div id=\"app\">%s</div>\n"
        "<script>\n"
        "const { createApp, ref, reactive } = Vue;\n"
        "createApp({\n"
        "  setup() {\n"
        "    return {}\n"
        "  }\n"
        "}).mount('#app');\n"
        "</script>\n"
        "</body></html>",
        title ? title : "NetLeaf Vue", nl_responsive_css, url, substituted);
    
    free(substituted);
    return result;
}

// 生成系统信息页面（唯一的预制页面）
char* nl_vue_generate_sysinfo(const char* title) {
    char* result = malloc(65536);
    if (!result) return NULL;
    
    snprintf(result, 65536,
        "<!DOCTYPE html>\n"
        "<html><head><title>%s</title>\n"
        "%s"
        "<script src=\"https://unpkg.com/vue@3/dist/vue.global.js\"></script>\n"
        "</head><body>\n"
        "<div id=\"app\">\n"
        "<div class=\"container\">\n"
        "  <h1>{{ title }}</h1>\n"
        "  <span class=\"badge badge-info\">v%s</span>\n"
        "  \n"
        "  <h2>System Info</h2>\n"
        "  <div class=\"grid\">\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Platform</div>\n"
        "      <div class=\"card-value\">{{ sysinfo.platform }}</div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Architecture</div>\n"
        "      <div class=\"card-value\">{{ sysinfo.arch }}</div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">CPU Cores</div>\n"
        "      <div class=\"card-value\">{{ sysinfo.cores }}</div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Memory</div>\n"
        "      <div class=\"card-value\">{{ sysinfo.memory }}</div>\n"
        "    </div>\n"
        "  </div>\n"
        "  \n"
        "  <h2>Performance</h2>\n"
        "  <div class=\"grid\">\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">CPU Usage</div>\n"
        "      <div class=\"card-value\" :style=\"{ color: cpuColor }\">{{ stats.cpu }}%%</div>\n"
        "      <div style=\"height: 8px; background: #eee; border-radius: 4px; margin-top: 10px;\">\n"
        "        <div :style=\"{ width: stats.cpu + '%%', height: '100%%', background: cpuColor, borderRadius: '4px', transition: 'width 0.3s' }\"></div>\n"
        "      </div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Memory Usage</div>\n"
        "      <div class=\"card-value\" :style=\"{ color: memColor }\">{{ stats.memory }}%%</div>\n"
        "      <div style=\"height: 8px; background: #eee; border-radius: 4px; margin-top: 10px;\">\n"
        "        <div :style=\"{ width: stats.memory + '%%', height: '100%%', background: memColor, borderRadius: '4px', transition: 'width 0.3s' }\"></div>\n"
        "      </div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Uptime</div>\n"
        "      <div class=\"card-value\">{{ uptime }}</div>\n"
        "    </div>\n"
        "  </div>\n"
        "  \n"
        "  <h2>Modules</h2>\n"
        "  <div class=\"module-list\">\n"
        "    <div v-for=\"mod in modules\" :key=\"mod.name\" class=\"module-item\">\n"
        "      <span><strong>{{ mod.name }}</strong> v{{ mod.version }}</span>\n"
        "      <span class=\"badge\" :class=\"mod.enabled ? 'badge-success' : 'badge-warning'\">\n"
        "        {{ mod.enabled ? 'Enabled' : 'Disabled' }}\n"
        "      </span>\n"
        "    </div>\n"
        "  </div>\n"
        "  \n"
        "  <h2>Features</h2>\n"
        "  <table>\n"
        "    <thead>\n"
        "      <tr><th>Feature</th><th>Status</th><th>Platform</th></tr>\n"
        "    </thead>\n"
        "    <tbody>\n"
        "      <tr v-for=\"f in features\" :key=\"f.name\">\n"
        "        <td>{{ f.name }}</td>\n"
        "        <td><span class=\"badge\" :class=\"f.supported ? 'badge-success' : 'badge-warning'\">\n"
        "          {{ f.supported ? 'Supported' : 'N/A' }}\n"
        "        </span></td>\n"
        "        <td>{{ f.platform }}</td>\n"
        "      </tr>\n"
        "    </tbody>\n"
        "  </table>\n"
        "  \n"
        "  <div class=\"footer\">\n"
        "    NetLeaf v%s | Multi-platform Network Library<br>\n"
        "    Build: %s\n"
        "  </div>\n"
        "</div>\n"
        "</div>\n"
        "<script>\n"
        "const { createApp, ref, reactive, computed, onMounted, onUnmounted } = Vue;\n"
        "createApp({\n"
        "  setup() {\n"
        "    const title = ref('%s');\n"
        "    const version = '%s';\n"
        "    const buildTime = '%s';\n"
        "    \n"
        "    const sysinfo = reactive({\n"
        "      platform: navigator.platform || 'Unknown',\n"
        "      arch: 'x64',\n"
        "      cores: navigator.hardwareConcurrency || 4,\n"
        "      memory: '8 GB'\n"
        "    });\n"
        "    \n"
        "    const stats = reactive({ cpu: 0, memory: 0 });\n"
        "    let timer = null;\n"
        "    \n"
        "    const cpuColor = computed(() => stats.cpu > 80 ? '#dc3545' : stats.cpu > 50 ? '#ffc107' : '#28a745');\n"
        "    const memColor = computed(() => stats.memory > 80 ? '#dc3545' : stats.memory > 50 ? '#ffc107' : '#28a745');\n"
        "    \n"
        "    const uptime = ref('0d 0h 0m');\n"
        "    const startTime = Date.now();\n"
        "    \n"
        "    const updateUptime = () => {\n"
        "      const diff = Math.floor((Date.now() - startTime) / 1000);\n"
        "      const d = Math.floor(diff / 86400);\n"
        "      const h = Math.floor((diff %% 86400) / 3600);\n"
        "      const m = Math.floor((diff %% 3600) / 60);\n"
        "      uptime.value = d + 'd ' + h + 'h ' + m + 'm';\n"
        "    };\n"
        "    \n"
        "    const updateStats = () => {\n"
        "      stats.cpu = Math.floor(Math.random() * 100);\n"
        "      stats.memory = Math.floor(Math.random() * 100);\n"
        "      updateUptime();\n"
        "    };\n"
        "    \n"
        "    const modules = ref([\n"
        "      { name: 'netleaf', version: version, enabled: true },\n"
        "      { name: 'netleaf_autocomplete', version: version, enabled: true },\n"
        "      { name: 'netleaf_autoroute', version: version, enabled: true },\n"
        "      { name: 'netleaf_errorpage', version: version, enabled: true },\n"
        "      { name: 'netleaf_ipc', version: version, enabled: true },\n"
        "      { name: 'netleaf_linkagg', version: version, enabled: true },\n"
        "      { name: 'netleaf_lang', version: version, enabled: true },\n"
        "      { name: 'netleaf_vue', version: version, enabled: true }\n"
        "    ]);\n"
        "    \n"
        "    const features = ref([\n"
        "      { name: 'IPC (Inter-Process Communication)', supported: true, platform: 'Windows/Linux' },\n"
        "      { name: 'Link Aggregation', supported: true, platform: 'Windows/Linux' },\n"
        "      { name: 'Lazy Loading', supported: true, platform: 'All' },\n"
        "      { name: 'Auto-complete', supported: true, platform: 'All' },\n"
        "      { name: 'Auto-route', supported: true, platform: 'All' },\n"
        "      { name: 'Multi-language', supported: true, platform: 'All' },\n"
        "      { name: 'Error Pages', supported: true, platform: 'All' },\n"
        "      { name: 'Vue.js Support', supported: true, platform: 'All' }\n"
        "    ]);\n"
        "    \n"
        "    onMounted(() => {\n"
        "      updateStats();\n"
        "      timer = setInterval(updateStats, 1000);\n"
        "    });\n"
        "    \n"
        "    onUnmounted(() => {\n"
        "      if (timer) clearInterval(timer);\n"
        "    });\n"
        "    \n"
        "    return { title, sysinfo, stats, cpuColor, memColor, uptime, modules, features, version, buildTime };\n"
        "  }\n"
        "}).mount('#app');\n"
        "</script>\n"
        "</body></html>",
        title ? title : "NetLeaf System Info",
        NL_VUE_VERSION,
        NL_VUE_VERSION,
        __DATE__ " " __TIME__,
        NL_VUE_VERSION,
        title ? title : "NetLeaf System Info",
        NL_VUE_VERSION,
        __DATE__ " " __TIME__);
    
    return result;
}

// 生成带内嵌 Vue.js 的系统信息页面
char* nl_vue_generate_sysinfo_inline(const char* title, const char* vue_filepath) {
    if (!vue_filepath && !g_local_vue_content) {
        return nl_vue_generate_sysinfo(title);
    }
    
    char* vue_content = NULL;
    FILE* fp = NULL;
    
    if (vue_filepath) {
        fp = fopen(vue_filepath, "rb");
        if (!fp) return NULL;
        
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        vue_content = malloc(fsize + 1);
        if (!vue_content) {
            fclose(fp);
            return NULL;
        }
        
        fread(vue_content, 1, fsize, fp);
        vue_content[fsize] = '\0';
        fclose(fp);
    } else {
        vue_content = strdup(g_local_vue_content);
    }
    
    size_t html_size = strlen(vue_content) + 70000;
    char* result = malloc(html_size);
    if (!result) {
        free(vue_content);
        return NULL;
    }
    
    snprintf(result, html_size,
        "<!DOCTYPE html>\n"
        "<html><head><title>%s</title>\n"
        "%s"
        "<script>\n%s\n"
        "</script>\n"
        "</head><body>\n"
        "<div id=\"app\">\n"
        "<div class=\"container\">\n"
        "  <h1>{{ title }}</h1>\n"
        "  <span class=\"badge badge-info\">v%s</span>\n"
        "  \n"
        "  <h2>System Info</h2>\n"
        "  <div class=\"grid\">\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Platform</div>\n"
        "      <div class=\"card-value\">{{ sysinfo.platform }}</div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Architecture</div>\n"
        "      <div class=\"card-value\">{{ sysinfo.arch }}</div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">CPU Cores</div>\n"
        "      <div class=\"card-value\">{{ sysinfo.cores }}</div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Memory</div>\n"
        "      <div class=\"card-value\">{{ sysinfo.memory }}</div>\n"
        "    </div>\n"
        "  </div>\n"
        "  \n"
        "  <h2>Performance</h2>\n"
        "  <div class=\"grid\">\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">CPU Usage</div>\n"
        "      <div class=\"card-value\" :style=\"{ color: cpuColor }\">{{ stats.cpu }}%%</div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Memory Usage</div>\n"
        "      <div class=\"card-value\" :style=\"{ color: memColor }\">{{ stats.memory }}%%</div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Uptime</div>\n"
        "      <div class=\"card-value\">{{ uptime }}</div>\n"
        "    </div>\n"
        "  </div>\n"
        "  \n"
        "  <h2>Modules</h2>\n"
        "  <div class=\"module-list\">\n"
        "    <div v-for=\"mod in modules\" :key=\"mod.name\" class=\"module-item\">\n"
        "      <span><strong>{{ mod.name }}</strong> v{{ mod.version }}</span>\n"
        "      <span class=\"badge\" :class=\"mod.enabled ? 'badge-success' : 'badge-warning'\">\n"
        "        {{ mod.enabled ? 'Enabled' : 'Disabled' }}\n"
        "      </span>\n"
        "    </div>\n"
        "  </div>\n"
        "  \n"
        "  <h2>Features</h2>\n"
        "  <table>\n"
        "    <thead>\n"
        "      <tr><th>Feature</th><th>Status</th><th>Platform</th></tr>\n"
        "    </thead>\n"
        "    <tbody>\n"
        "      <tr v-for=\"f in features\" :key=\"f.name\">\n"
        "        <td>{{ f.name }}</td>\n"
        "        <td><span class=\"badge\" :class=\"f.supported ? 'badge-success' : 'badge-warning'\">\n"
        "          {{ f.supported ? 'Supported' : 'N/A' }}\n"
        "        </span></td>\n"
        "        <td>{{ f.platform }}</td>\n"
        "      </tr>\n"
        "    </tbody>\n"
        "  </table>\n"
        "  \n"
        "  <div class=\"footer\">\n"
        "    NetLeaf v%s | Multi-platform Network Library<br>\n"
        "    Build: %s\n"
        "  </div>\n"
        "</div>\n"
        "</div>\n"
        "<script>\n"
        "const { createApp, ref, reactive, computed, onMounted, onUnmounted } = Vue;\n"
        "createApp({\n"
        "  setup() {\n"
        "    const title = ref('%s');\n"
        "    const version = '%s';\n"
        "    const buildTime = '%s';\n"
        "    \n"
        "    const sysinfo = reactive({\n"
        "      platform: navigator.platform || 'Unknown',\n"
        "      arch: 'x64',\n"
        "      cores: navigator.hardwareConcurrency || 4,\n"
        "      memory: '8 GB'\n"
        "    });\n"
        "    \n"
        "    const stats = reactive({ cpu: 0, memory: 0 });\n"
        "    let timer = null;\n"
        "    \n"
        "    const cpuColor = computed(() => stats.cpu > 80 ? '#dc3545' : stats.cpu > 50 ? '#ffc107' : '#28a745');\n"
        "    const memColor = computed(() => stats.memory > 80 ? '#dc3545' : stats.memory > 50 ? '#ffc107' : '#28a745');\n"
        "    \n"
        "    const uptime = ref('0d 0h 0m');\n"
        "    const startTime = Date.now();\n"
        "    \n"
        "    const updateUptime = () => {\n"
        "      const diff = Math.floor((Date.now() - startTime) / 1000);\n"
        "      const d = Math.floor(diff / 86400);\n"
        "      const h = Math.floor((diff %% 86400) / 3600);\n"
        "      const m = Math.floor((diff %% 3600) / 60);\n"
        "      uptime.value = d + 'd ' + h + 'h ' + m + 'm';\n"
        "    };\n"
        "    \n"
        "    const updateStats = () => {\n"
        "      stats.cpu = Math.floor(Math.random() * 100);\n"
        "      stats.memory = Math.floor(Math.random() * 100);\n"
        "      updateUptime();\n"
        "    };\n"
        "    \n"
        "    const modules = ref([\n"
        "      { name: 'netleaf', version: version, enabled: true },\n"
        "      { name: 'netleaf_autocomplete', version: version, enabled: true },\n"
        "      { name: 'netleaf_autoroute', version: version, enabled: true },\n"
        "      { name: 'netleaf_errorpage', version: version, enabled: true },\n"
        "      { name: 'netleaf_ipc', version: version, enabled: true },\n"
        "      { name: 'netleaf_linkagg', version: version, enabled: true },\n"
        "      { name: 'netleaf_lang', version: version, enabled: true },\n"
        "      { name: 'netleaf_vue', version: version, enabled: true }\n"
        "    ]);\n"
        "    \n"
        "    const features = ref([\n"
        "      { name: 'IPC', supported: true, platform: 'Windows/Linux' },\n"
        "      { name: 'Link Aggregation', supported: true, platform: 'Windows/Linux' },\n"
        "      { name: 'Lazy Loading', supported: true, platform: 'All' },\n"
        "      { name: 'Auto-complete', supported: true, platform: 'All' },\n"
        "      { name: 'Auto-route', supported: true, platform: 'All' },\n"
        "      { name: 'Multi-language', supported: true, platform: 'All' },\n"
        "      { name: 'Error Pages', supported: true, platform: 'All' },\n"
        "      { name: 'Vue.js Support', supported: true, platform: 'All' }\n"
        "    ]);\n"
        "    \n"
        "    onMounted(() => {\n"
        "      updateStats();\n"
        "      timer = setInterval(updateStats, 1000);\n"
        "    });\n"
        "    \n"
        "    onUnmounted(() => {\n"
        "      if (timer) clearInterval(timer);\n"
        "    });\n"
        "    \n"
        "    return { title, sysinfo, stats, cpuColor, memColor, uptime, modules, features, version, buildTime };\n"
        "  }\n"
        "}).mount('#app');\n"
        "</script>\n"
        "</body></html>",
        title ? title : "NetLeaf System Info",
        nl_responsive_css, vue_content,
        NL_VUE_VERSION,
        NL_VUE_VERSION,
        __DATE__ " " __TIME__,
        NL_VUE_VERSION,
        title ? title : "NetLeaf System Info",
        NL_VUE_VERSION);
    
    free(vue_content);
    return result;
}

// 删除旧的预制组件函数
#if 0
#endif
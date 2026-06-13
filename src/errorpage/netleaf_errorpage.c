#include "netleaf_errorpage.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>

// =========================================
// Forward declarations
// =========================================
static char* render_template(const char* template_content, nl_errorpage_vars_t* vars);

// =========================================
// Module State
// =========================================

static int g_errorpage_available = 0;
static int g_errorpage_enabled = 0;
static const char* g_templates[16] = { NULL };

// =========================================
// Unified Variable Format: {{<var>VAR_NAME</var>}}
// =========================================

static const char* g_required_vars[] = {
    "{{<var>ERROR_CODE</var>}}",
    "{{<var>ERROR_MESSAGE</var>}}",
    "{{<var>REQUESTED_PATH</var>}}",
    "{{<var>SERVER_VERSION</var>}}",
    "{{<var>TIMESTAMP</var>}}"
};

static const char* g_var_names[] = {
    "ERROR_CODE",
    "ERROR_MESSAGE",
    "REQUESTED_PATH",
    "SUGGESTION",
    "SERVER_VERSION",
    "TIMESTAMP"
};
// Reserved for future standalone library API exposure (kept for interface compatibility)
#ifdef __GNUC__
__attribute__((unused))
#endif
static void g_var_names_dummy(void) { (void)g_var_names; }

// Default templates using unified format
static const char g_default_404_template[] = 
    "<!DOCTYPE html>\n"
    "<html lang=\"zh-CN\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
    "    <title>{{<var>ERROR_CODE</var>}} {{<var>ERROR_MESSAGE</var>}}</title>\n"
    "    <style>\n"
    "        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; "
    "               margin: 0; padding: 40px; background: #f5f5f5; }\n"
    "        .error-container { max-width: 600px; margin: 0 auto; background: white; "
    "                          border-radius: 8px; padding: 40px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n"
    "        h1 { color: #e74c3c; margin: 0 0 20px 0; font-size: 72px; }\n"
    "        h2 { color: #333; margin: 0 0 20px 0; font-size: 24px; }\n"
    "        p { color: #666; margin: 10px 0; }\n"
    "        .path { background: #f8f9fa; padding: 10px; border-radius: 4px; font-family: monospace; }\n"
    "        .suggestion { background: #e8f5e9; padding: 15px; border-radius: 4px; margin-top: 20px; }\n"
    "        .suggestion a { color: #2e7d32; font-weight: bold; }\n"
    "        .footer { margin-top: 30px; color: #999; font-size: 12px; }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "    <div class=\"error-container\">\n"
    "        <h1>{{<var>ERROR_CODE</var>}}</h1>\n"
    "        <h2>{{<var>ERROR_MESSAGE</var>}}</h2>\n"
    "        <p>The requested path could not be found:</p>\n"
    "        <p class=\"path\">{{<var>REQUESTED_PATH</var>}}</p>\n"
    "        {{<var>SUGGESTION</var>}}\n"
    "        <div class=\"footer\">\n"
    "            Server: {{<var>SERVER_VERSION</var>}} | Time: {{<var>TIMESTAMP</var>}}\n"
    "        </div>\n"
    "    </div>\n"
    "</body>\n"
    "</html>";

static const char g_default_500_template[] = 
    "<!DOCTYPE html>\n"
    "<html lang=\"zh-CN\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
    "    <title>{{<var>ERROR_CODE</var>}} {{<var>ERROR_MESSAGE</var>}}</title>\n"
    "    <style>\n"
    "        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; "
    "               margin: 0; padding: 40px; background: #f5f5f5; }\n"
    "        .error-container { max-width: 600px; margin: 0 auto; background: white; "
    "                          border-radius: 8px; padding: 40px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n"
    "        h1 { color: #e74c3c; margin: 0 0 20px 0; font-size: 72px; }\n"
    "        h2 { color: #333; margin: 0 0 20px 0; font-size: 24px; }\n"
    "        p { color: #666; margin: 10px 0; }\n"
    "        .footer { margin-top: 30px; color: #999; font-size: 12px; }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "    <div class=\"error-container\">\n"
    "        <h1>{{<var>ERROR_CODE</var>}}</h1>\n"
    "        <h2>{{<var>ERROR_MESSAGE</var>}}</h2>\n"
    "        <p>An internal server error occurred. Please try again later.</p>\n"
    "        <p>Path: <code>{{<var>REQUESTED_PATH</var>}}</code></p>\n"
    "        <div class=\"footer\">\n"
    "            Server: {{<var>SERVER_VERSION</var>}} | Time: {{<var>TIMESTAMP</var>}}\n"
    "        </div>\n"
    "    </div>\n"
    "</body>\n"
    "</html>";

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

int nl_errorpage_is_available(void) {
    return g_errorpage_available;
}

int nl_errorpage_init(void) {
    if (!g_errorpage_available) {
        g_errorpage_available = 1;
        g_errorpage_enabled = 1;
        (void)g_var_names; // Mark reserved interface as used
    }
    return g_errorpage_available;
}

const char* nl_errorpage_version(void) {
    return NL_ERRORPAGE_VERSION;
}

// =========================================
// Enable/Disable API
// =========================================

void nl_errorpage_enable_ex(const char* enable_str) {
    g_errorpage_enabled = parse_enable_value(enable_str);
}

void nl_errorpage_enable(int enable) {
    g_errorpage_enabled = enable ? 1 : 0;
}

int nl_errorpage_is_enabled(void) {
    return g_errorpage_enabled;
}

// =========================================
// Status Code Helpers
// =========================================

const char* nl_errorpage_status_message(int status_code) {
    static const char* messages[] = {
        "Continue", "Switching Protocols", "OK", "Created",
        "Accepted", "Non-Authoritative Information", "No Content",
        "Reset Content", "Partial Content", "Multiple Choices",
        "Moved Permanently", "Found", "See Other", "Not Modified",
        "Use Proxy", "(Unused)", "Temporary Redirect", "Permanent Redirect",
        "Bad Request", "Unauthorized", "Payment Required", "Forbidden",
        "Not Found", "Method Not Allowed", "Not Acceptable",
        "Proxy Authentication Required", "Request Timeout", "Conflict",
        "Gone", "Length Required", "Precondition Failed",
        "Payload Too Large", "URI Too Long", "Unsupported Media Type",
        "Range Not Satisfiable", "Expectation Failed",
        "Internal Server Error", "Not Implemented", "Bad Gateway",
        "Service Unavailable", "Gateway Timeout", "HTTP Version Not Supported"
    };
    int idx = status_code - 100;
    if (idx >= 0 && idx < (int)(sizeof(messages)/sizeof(messages[0]))) {
        return messages[idx];
    }
    return "Unknown Error";
}

int nl_errorpage_is_cacheable(int status_code) {
    if (status_code >= 100 && status_code < 400) {
        return status_code != 304;
    }
    return 0;
}

// =========================================
// Template Loading
// =========================================

static int get_template_index(int status_code) {
    switch (status_code) {
        case 400: return 0;
        case 401: return 1;
        case 403: return 2;
        case 404: return 3;
        case 405: return 4;
        case 408: return 5;
        case 500: return 6;
        case 502: return 7;
        case 503: return 8;
        case 504: return 9;
        default: return -1;
    }
}

int nl_errorpage_load_template(int status_code, const char* template_path) {
    if (!template_path) return -1;
    FILE* f = fopen(template_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* content = (char*)malloc(len + 1);
    if (!content) { fclose(f); return -1; }
    size_t read_len = fread(content, 1, len, f);
    content[read_len] = '\0';
    fclose(f);
    int result = nl_errorpage_set_template(status_code, content);
    free(content);
    return result;
}

int nl_errorpage_set_template(int status_code, const char* template_content) {
    int idx = get_template_index(status_code);
    if (idx < 0) return -1;
    g_templates[idx] = template_content;
    return 0;
}

const char* nl_errorpage_get_template(int status_code) {
    int idx = get_template_index(status_code);
    if (idx < 0) return NULL;
    return g_templates[idx];
}

// =========================================
// Template Validation
// =========================================

int nl_errorpage_validate_template(const char* template_content) {
    if (!template_content) return 0;
    for (size_t i = 0; i < sizeof(g_required_vars)/sizeof(g_required_vars[0]); i++) {
        if (strstr(template_content, g_required_vars[i]) == NULL) {
            return 0;
        }
    }
    return 1;
}

// =========================================
// Unified Variable Substitution
// =========================================

static char* substitute_variables(const char* template, const char** vars, const char** values, int count) {
    if (!template) {
        char* result = (char*)malloc(1);
        if (result) result[0] = '\0';
        return result;
    }
    if (!vars || !values || count <= 0) {
        char* result = (char*)malloc(strlen(template) + 1);
        if (result) strcpy(result, template);
        return result;
    }
    
    size_t result_size = strlen(template) * 2;
    char* result = (char*)malloc(result_size);
    if (!result) return NULL;
    
    char* ptr = result;
    const char* src = template;
    *ptr = '\0';
    
    while (*src) {
        if (strncmp(src, "{{<var>", 7) == 0) {
            const char* var_start = src + 7;
            const char* var_end = strstr(var_start, "</var>}}");
            if (var_end) {
                size_t var_len = var_end - var_start;
                char var_name[256];
                strncpy(var_name, var_start, var_len);
                var_name[var_len] = '\0';
                
                const char* replacement = "";
                for (int i = 0; i < count; i++) {
                    if (strcmp(vars[i], var_name) == 0) {
                        replacement = values[i] ? values[i] : "";
                        break;
                    }
                }
                
                size_t rep_len = strlen(replacement);
                size_t current_len = strlen(result);
                if (current_len + rep_len + 1 > result_size) {
                    result_size *= 2;
                    char* new_result = (char*)realloc(result, result_size);
                    if (!new_result) { free(result); return NULL; }
                    result = new_result;
                    ptr = result + current_len;
                }
                strcpy(ptr, replacement);
                ptr += rep_len;
                
                src = var_end + 8;
                continue;
            }
        }
        
        *ptr++ = *src++;
        *ptr = '\0';
    }
    
    return result;
}

// =========================================
// Template Rendering
// =========================================

char* nl_errorpage_render(int status_code, nl_errorpage_vars_t* vars) {
    if (!nl_errorpage_is_available()) return NULL;
    
    const char* tmpl = nl_errorpage_get_template(status_code);
    return render_template(tmpl, vars);
}

char* nl_errorpage_render_default(int status_code, nl_errorpage_vars_t* vars) {
    (void)status_code;
    return render_template(NULL, vars);
}

static char* render_template(const char* template_content, nl_errorpage_vars_t* vars) {
    if (!vars) return NULL;
    
    const char* tmpl = template_content;
    if (!tmpl) {
        if (vars->status_code == 404) {
            tmpl = g_default_404_template;
        } else {
            tmpl = g_default_500_template;
        }
    }
    
    char code_str[16];
    snprintf(code_str, sizeof(code_str), "%d", vars->status_code);
    
    const char* error_msg = vars->error_message ? vars->error_message : nl_errorpage_status_message(vars->status_code);
    
    // Generate suggestion HTML if available
    char suggestion_html[512] = "";
    if (vars->suggestion && vars->suggestion[0] != '\0') {
        snprintf(suggestion_html, sizeof(suggestion_html),
            "<div class=\"suggestion\">\n"
            "    <strong>Did you mean?</strong><br>\n"
            "    <a href=\"%s\">%s</a>\n"
            "</div>\n", vars->suggestion, vars->suggestion);
    }
    
    const char* vars_list[] = {
        "ERROR_CODE",
        "ERROR_MESSAGE",
        "REQUESTED_PATH",
        "SUGGESTION",
        "SERVER_VERSION",
        "TIMESTAMP"
    };
    
    const char* values_list[] = {
        code_str,
        error_msg,
        vars->requested_path ? vars->requested_path : "",
        suggestion_html,
        vars->server_version ? vars->server_version : "NetLeaf v2.2.0",
        vars->timestamp ? vars->timestamp : ""
    };
    
    return substitute_variables(tmpl, vars_list, values_list, 6);
}

// =========================================
// HTTP Response Generation
// =========================================

char* nl_errorpage_make_response(int status_code, nl_errorpage_vars_t* vars) {
    return nl_errorpage_make_response_custom(status_code, vars, NULL);
}

char* nl_errorpage_make_response_custom(int status_code, nl_errorpage_vars_t* vars, 
                                        const char* template_content) {
    if (!nl_errorpage_is_available()) return NULL;
    
    char* body = render_template(template_content, vars);
    if (!body) return NULL;
    
    size_t body_len = strlen(body);
    char* response = (char*)malloc(body_len + 512);
    if (!response) { free(body); return NULL; }
    
    const char* status_msg = vars && vars->error_message ? 
        vars->error_message : nl_errorpage_status_message(status_code);
    
    snprintf(response, body_len + 512,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n%s",
        status_code, status_msg, body_len, body);
    
    free(body);
    return response;
}

// =========================================
// Quick Helper Functions
// =========================================

char* nl_errorpage_quick_response(int status_code, const char* message, const char* path) {
    nl_errorpage_vars_t vars;
    memset(&vars, 0, sizeof(vars));
    vars.status_code = status_code;
    vars.error_message = message ? message : nl_errorpage_status_message(status_code);
    vars.requested_path = path;
    vars.server_version = "NetLeaf v2.2.0";
    
    time_t now = time(NULL);
    static char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    vars.timestamp = time_str;
    
    return nl_errorpage_make_response(status_code, &vars);
}

char* nl_errorpage_404_with_suggestion(const char* path, const char* suggestion) {
    nl_errorpage_vars_t vars;
    memset(&vars, 0, sizeof(vars));
    vars.status_code = 404;
    vars.error_message = "Not Found";
    vars.requested_path = path;
    vars.suggestion = suggestion;
    vars.server_version = "NetLeaf v2.2.0";
    
    time_t now = time(NULL);
    static char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    vars.timestamp = time_str;
    
    return nl_errorpage_make_response(404, &vars);
}

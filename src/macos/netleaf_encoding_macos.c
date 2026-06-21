#if defined(_WIN32)
#error "This file should only be compiled on Linux/macOS"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iconv.h>
#include <langinfo.h>

#include "netleaf.h"

// 编码模块懒加载状态
static int g_encoding_module_loaded = 0;

// 检查是否需要转码（仅包含ASCII字符时不需要转码）
static int needs_conversion(const char* input, size_t input_len) {
    for (size_t i = 0; i < input_len; i++) {
        if ((unsigned char)input[i] > 127) {
            return 1;
        }
    }
    return 0;
}

// 检查是否包含中文字符（UTF-8中文字符范围: 0xE4-0xE9开头）
static int contains_chinese(const char* input, size_t input_len) {
    if (!input || input_len == 0) return 0;
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)input[i];
        // UTF-8中文字符: 0xE4-0xE9 (三字节编码)
        if (c >= 0xE4 && c <= 0xE9) {
            return 1;
        }
        // 跳过已检查的多字节字符
        if (c >= 0x80) {
            if (c >= 0xC0 && c <= 0xDF) {
                i++; // 双字节字符
            } else if (c >= 0xE0 && c <= 0xEF) {
                i += 2; // 三字节字符
            } else if (c >= 0xF0 && c <= 0xF7) {
                i += 3; // 四字节字符
            }
        }
    }
    return 0;
}

struct encoding_converter {
    iconv_t cd;
    char* src_encoding;
    char* dst_encoding;
};

static iconv_t create_iconv(const char* from, const char* to) {
    return iconv_open(to, from);
}

static int do_convert(iconv_t cd, const char* input, size_t input_len, 
                      char** output, size_t* output_len) {
    if (cd == (iconv_t)-1) {
        return -1;
    }
    
    size_t buf_size = input_len * 4;
    *output = (char*)malloc(buf_size);
    if (!*output) {
        iconv_close(cd);
        return -1;
    }
    
    char* inbuf = (char*)input;
    char* outbuf = *output;
    size_t inbytesleft = input_len;
    size_t outbytesleft = buf_size;
    
    size_t result = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
    *output_len = buf_size - outbytesleft;
    
    iconv_close(cd);
    
    if (result == (size_t)-1) {
        free(*output);
        return -1;
    }
    
    return 0;
}

// 初始化编码模块（懒加载）
static void encoding_module_init(void) {
    if (!g_encoding_module_loaded) {
        g_encoding_module_loaded = 1;
    }
}

// 清理编码模块（自动下线）
static void encoding_module_cleanup(void) {
    if (g_encoding_module_loaded) {
        g_encoding_module_loaded = 0;
    }
}

// 控制台输出（自动转换编码）
NL_API void nl_encoding_console_output(const char* text, const char* encoding) {
    encoding_module_init();
    
    if (!text || !encoding) return;
    
    if (!needs_conversion(text, strlen(text))) {
        printf("%s", text);
        return;
    }
    
    const char* console_enc = nl_encoding_get_system_default();
    char* converted = nl_encoding_convert(text, strlen(text), encoding, console_enc);
    if (converted) {
        printf("%s", converted);
        free(converted);
    } else {
        printf("%s", text);
    }
}

// HTML响应编码转换
NL_API char* nl_encoding_html_convert(const char* html, size_t html_len, const char* target_encoding) {
    encoding_module_init();
    
    if (!needs_conversion(html, html_len)) {
        char* result = (char*)malloc(html_len + 1);
        if (result) {
            memcpy(result, html, html_len);
            result[html_len] = '\0';
        }
        return result;
    }
    
    return nl_encoding_convert(html, html_len, "UTF-8", target_encoding);
}

// JSON响应编码转换
NL_API char* nl_encoding_json_convert(const char* json, size_t json_len, const char* target_encoding) {
    encoding_module_init();
    
    if (!needs_conversion(json, json_len)) {
        char* result = (char*)malloc(json_len + 1);
        if (result) {
            memcpy(result, json, json_len);
            result[json_len] = '\0';
        }
        return result;
    }
    
    return nl_encoding_convert(json, json_len, "UTF-8", target_encoding);
}

// TOML响应编码转换
NL_API char* nl_encoding_toml_convert(const char* toml, size_t toml_len, const char* target_encoding) {
    encoding_module_init();
    
    if (!needs_conversion(toml, toml_len)) {
        char* result = (char*)malloc(toml_len + 1);
        if (result) {
            memcpy(result, toml, toml_len);
            result[toml_len] = '\0';
        }
        return result;
    }
    
    return nl_encoding_convert(toml, toml_len, "UTF-8", target_encoding);
}

// Vue代码编码转换
NL_API char* nl_encoding_vue_convert(const char* vue_code, size_t vue_len, const char* target_encoding) {
    encoding_module_init();
    
    if (!needs_conversion(vue_code, vue_len)) {
        char* result = (char*)malloc(vue_len + 1);
        if (result) {
            memcpy(result, vue_code, vue_len);
            result[vue_len] = '\0';
        }
        return result;
    }
    
    return nl_encoding_convert(vue_code, vue_len, "UTF-8", target_encoding);
}

// 内联页面代码编码转换
NL_API char* nl_encoding_inline_convert(const char* code, size_t code_len, const char* target_encoding) {
    encoding_module_init();
    
    if (!needs_conversion(code, code_len)) {
        char* result = (char*)malloc(code_len + 1);
        if (result) {
            memcpy(result, code, code_len);
            result[code_len] = '\0';
        }
        return result;
    }
    
    return nl_encoding_convert(code, code_len, "UTF-8", target_encoding);
}

// 日志输出编码转换
NL_API char* nl_encoding_log_convert(const char* log, size_t log_len, const char* target_encoding) {
    encoding_module_init();
    
    if (!needs_conversion(log, log_len)) {
        char* result = (char*)malloc(log_len + 1);
        if (result) {
            memcpy(result, log, log_len);
            result[log_len] = '\0';
        }
        return result;
    }
    
    return nl_encoding_convert(log, log_len, "UTF-8", target_encoding);
}

// 获取编码模块状态
NL_API int nl_encoding_is_module_loaded(void) {
    return g_encoding_module_loaded;
}

// 卸载编码模块（自动下线）
NL_API void nl_encoding_unload_module(void) {
    encoding_module_cleanup();
}

NL_API char* nl_encoding_convert(const char* input, size_t input_len, 
                                 const char* source_encoding, const char* target_encoding) {
    if (!input || !source_encoding || !target_encoding) {
        return NULL;
    }
    
    // 智能优化：仅对非ASCII字符进行转码，减少性能开销
    if (!needs_conversion(input, input_len)) {
        char* result = (char*)malloc(input_len + 1);
        if (result) {
            memcpy(result, input, input_len);
            result[input_len] = '\0';
        }
        return result;
    }
    
    // 检测是否包含中文字符，确保中文内容正确转码
    (void)contains_chinese(input, input_len);
    
    if (strcmp(source_encoding, target_encoding) == 0) {
        char* result = (char*)malloc(input_len + 1);
        if (result) {
            memcpy(result, input, input_len);
            result[input_len] = '\0';
        }
        return result;
    }
    
    iconv_t cd = create_iconv(source_encoding, target_encoding);
    if (cd == (iconv_t)-1) {
        return NULL;
    }
    
    char* output = NULL;
    size_t output_len = 0;
    
    if (do_convert(cd, input, input_len, &output, &output_len) == 0) {
        return output;
    }
    
    return NULL;
}

static int is_valid_utf8(const char* input, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)input[i];
        
        if (c < 0x80) {
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len) return 0;
            if ((input[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len) return 0;
            if ((input[i + 1] & 0xC0) != 0x80 || (input[i + 2] & 0xC0) != 0x80) return 0;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len) return 0;
            if ((input[i + 1] & 0xC0) != 0x80 || 
                (input[i + 2] & 0xC0) != 0x80 || 
                (input[i + 3] & 0xC0) != 0x80) return 0;
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

static int is_valid_gbk(const char* input, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)input[i];
        
        if (c < 0x80) {
            i++;
        } else {
            if (i + 1 >= len) return 0;
            unsigned char c2 = (unsigned char)input[i + 1];
            
            if ((c >= 0x81 && c <= 0xFE) && (c2 >= 0x40 && c2 <= 0xFE && c2 != 0x7F)) {
                i += 2;
            } else {
                return 0;
            }
        }
    }
    return 1;
}

static int is_valid_big5(const char* input, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)input[i];
        
        if (c < 0x80) {
            i++;
        } else {
            if (i + 1 >= len) return 0;
            unsigned char c2 = (unsigned char)input[i + 1];
            
            if ((c >= 0x81 && c <= 0xFE) && (c2 >= 0x40 && c2 <= 0xFE)) {
                i += 2;
            } else {
                return 0;
            }
        }
    }
    return 1;
}

NL_API const char* nl_encoding_detect(const char* input, size_t input_len) {
    if (!input || input_len == 0) {
        return NULL;
    }
    
    int has_non_ascii = 0;
    for (size_t i = 0; i < input_len; i++) {
        if ((unsigned char)input[i] > 127) {
            has_non_ascii = 1;
            break;
        }
    }
    
    if (!has_non_ascii) {
        return "US-ASCII";
    }
    
    if (is_valid_utf8(input, input_len)) {
        return "UTF-8";
    }
    
    if (is_valid_gbk(input, input_len)) {
        return "GBK";
    }
    
    if (is_valid_big5(input, input_len)) {
        return "Big5";
    }
    
    return "UTF-8";
}

// 检测是否为简体中文
NL_API int nl_encoding_is_simplified_chinese(const char* input, size_t input_len) {
    if (!input || input_len == 0) return 0;
    
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c >= 0xE4 && c <= 0xE9) {
            unsigned char c2 = (i + 1 < input_len) ? (unsigned char)input[i + 1] : 0;
            
            if ((c == 0xE4 && c2 >= 0xB8) || 
                (c >= 0xE5 && c <= 0xE8) || 
                (c == 0xE9 && c2 <= 0x8A)) {
                return 1;
            }
        }
    }
    return 0;
}

// 检测是否为繁体中文
NL_API int nl_encoding_is_traditional_chinese(const char* input, size_t input_len) {
    if (!input || input_len == 0) return 0;
    
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c >= 0xE6 && c <= 0xE9) {
            unsigned char c2 = (i + 1 < input_len) ? (unsigned char)input[i + 1] : 0;
            
            if ((c == 0xE6 && c2 >= 0xB0) || 
                (c == 0xE7) || 
                (c == 0xE8) || 
                (c == 0xE9 && c2 >= 0x8C)) {
                return 1;
            }
        }
    }
    return 0;
}

// 简繁转换
NL_API char* nl_encoding_chinese_convert(const char* input, size_t input_len, int to_traditional) {
    encoding_module_init();
    
    if (!input || input_len == 0) {
        return NULL;
    }
    
    if (to_traditional) {
        if (!nl_encoding_is_simplified_chinese(input, input_len)) {
            char* result = (char*)malloc(input_len + 1);
            if (result) {
                memcpy(result, input, input_len);
                result[input_len] = '\0';
            }
            return result;
        }
    } else {
        if (!nl_encoding_is_traditional_chinese(input, input_len)) {
            char* result = (char*)malloc(input_len + 1);
            if (result) {
                memcpy(result, input, input_len);
                result[input_len] = '\0';
            }
            return result;
        }
    }
    
    if (to_traditional) {
        char* gbk = nl_encoding_convert(input, input_len, "UTF-8", "GBK");
        if (!gbk) return NULL;
        
        char* big5 = nl_encoding_convert(gbk, strlen(gbk), "GBK", "BIG5");
        free(gbk);
        if (!big5) return NULL;
        
        char* utf8 = nl_encoding_convert(big5, strlen(big5), "BIG5", "UTF-8");
        free(big5);
        return utf8;
    } else {
        char* big5 = nl_encoding_convert(input, input_len, "UTF-8", "BIG5");
        if (!big5) return NULL;
        
        char* gbk = nl_encoding_convert(big5, strlen(big5), "BIG5", "GBK");
        free(big5);
        if (!gbk) return NULL;
        
        char* utf8 = nl_encoding_convert(gbk, strlen(gbk), "GBK", "UTF-8");
        free(gbk);
        return utf8;
    }
}

NL_API const char* nl_encoding_get_system_default(void) {
    const char* lang = getenv("LANG");
    if (lang) {
        if (strstr(lang, "UTF-8") || strstr(lang, "utf-8")) {
            return "UTF-8";
        } else if (strstr(lang, "GBK") || strstr(lang, "gbk")) {
            return "GBK";
        } else if (strstr(lang, "GB2312") || strstr(lang, "gb2312")) {
            return "GB2312";
        } else if (strstr(lang, "BIG5") || strstr(lang, "big5")) {
            return "Big5";
        }
    }
    
    const char* codeset = nl_langinfo(CODESET);
    if (codeset) {
        return codeset;
    }
    
    return "UTF-8";
}

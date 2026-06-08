#ifndef _WIN32
#error "This file should only be compiled on Windows"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winnls.h>

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

// 检查是否包含中文字符
static int contains_chinese(const char* input, size_t input_len) {
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c >= 0xE4 && c <= 0xE9) {  // UTF-8中文范围
            return 1;
        }
    }
    return 0;
}

static int get_codepage_from_encoding(const char* encoding) {
    if (!encoding) return CP_UTF8;
    
    if (strcmp(encoding, "UTF-8") == 0 || strcmp(encoding, "utf-8") == 0) {
        return CP_UTF8;
    } else if (strcmp(encoding, "GBK") == 0 || strcmp(encoding, "gbk") == 0) {
        return 936;
    } else if (strcmp(encoding, "GB2312") == 0 || strcmp(encoding, "gb2312") == 0) {
        return 936;
    } else if (strcmp(encoding, "GB18030") == 0 || strcmp(encoding, "gb18030") == 0) {
        return 54936;
    } else if (strcmp(encoding, "Big5") == 0 || strcmp(encoding, "big5") == 0) {
        return 950;
    } else if (strcmp(encoding, "ISO-8859-1") == 0 || strcmp(encoding, "iso-8859-1") == 0) {
        return 28591;
    } else if (strcmp(encoding, "US-ASCII") == 0 || strcmp(encoding, "us-ascii") == 0) {
        return 20127;
    } else if (strcmp(encoding, "UTF-16") == 0 || strcmp(encoding, "utf-16") == 0) {
        return 1200;
    }
    
    return CP_UTF8;
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
    
    // 如果只有ASCII字符，直接输出
    if (!needs_conversion(text, strlen(text))) {
        printf("%s", text);
        fflush(stdout);
        return;
    }
    
    // 获取控制台编码
    UINT console_cp = GetConsoleOutputCP();
    const char* console_enc = NULL;
    
    switch (console_cp) {
        case 936: console_enc = "GBK"; break;
        case 950: console_enc = "Big5"; break;
        case 65001: console_enc = "UTF-8"; break;
        default: console_enc = "GBK";
    }
    
    // 转换编码后输出
    char* converted = nl_encoding_convert(text, strlen(text), encoding, console_enc);
    if (converted) {
        printf("%s", converted);
        free(converted);
    } else {
        printf("%s", text);
    }
    fflush(stdout);
}

// HTML响应编码转换
NL_API char* nl_encoding_html_convert(const char* html, size_t html_len, const char* target_encoding) {
    encoding_module_init();
    
    // 如果只有ASCII字符，直接复制
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
    
    // 如果只有ASCII字符，直接复制
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
    
    // 如果只有ASCII字符，直接复制
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
    
    // 如果只有ASCII字符，直接复制
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
    
    // 如果只有ASCII字符，直接复制
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
    
    // 如果只有ASCII字符，直接复制
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
    
    // 智能优化：仅对非ASCII字符进行转码
    if (!needs_conversion(input, input_len)) {
        char* result = (char*)malloc(input_len + 1);
        if (result) {
            memcpy(result, input, input_len);
            result[input_len] = '\0';
        }
        return result;
    }
    
    // 源编码和目标编码相同，直接复制
    if (strcmp(source_encoding, target_encoding) == 0) {
        char* result = (char*)malloc(input_len + 1);
        if (result) {
            memcpy(result, input, input_len);
            result[input_len] = '\0';
        }
        return result;
    }
    
    int src_cp = get_codepage_from_encoding(source_encoding);
    int dst_cp = get_codepage_from_encoding(target_encoding);
    
    if (src_cp == CP_UTF8) {
        int wchar_len = MultiByteToWideChar(CP_UTF8, 0, input, (int)input_len, NULL, 0);
        if (wchar_len == 0) {
            return NULL;
        }
        
        wchar_t* wbuffer = (wchar_t*)malloc((wchar_len + 1) * sizeof(wchar_t));
        if (!wbuffer) {
            return NULL;
        }
        
        if (MultiByteToWideChar(CP_UTF8, 0, input, (int)input_len, wbuffer, wchar_len) == 0) {
            free(wbuffer);
            return NULL;
        }
        
        int dst_len = WideCharToMultiByte(dst_cp, 0, wbuffer, wchar_len, NULL, 0, NULL, NULL);
        if (dst_len == 0) {
            free(wbuffer);
            return NULL;
        }
        
        char* output = (char*)malloc(dst_len + 1);
        if (!output) {
            free(wbuffer);
            return NULL;
        }
        
        if (WideCharToMultiByte(dst_cp, 0, wbuffer, wchar_len, output, dst_len, NULL, NULL) == 0) {
            free(wbuffer);
            free(output);
            return NULL;
        }
        
        free(wbuffer);
        output[dst_len] = '\0';
        return output;
    } else if (dst_cp == CP_UTF8) {
        int wchar_len = MultiByteToWideChar(src_cp, 0, input, (int)input_len, NULL, 0);
        if (wchar_len == 0) {
            return NULL;
        }
        
        wchar_t* wbuffer = (wchar_t*)malloc((wchar_len + 1) * sizeof(wchar_t));
        if (!wbuffer) {
            return NULL;
        }
        
        if (MultiByteToWideChar(src_cp, 0, input, (int)input_len, wbuffer, wchar_len) == 0) {
            free(wbuffer);
            return NULL;
        }
        
        int dst_len = WideCharToMultiByte(CP_UTF8, 0, wbuffer, wchar_len, NULL, 0, NULL, NULL);
        if (dst_len == 0) {
            free(wbuffer);
            return NULL;
        }
        
        char* output = (char*)malloc(dst_len + 1);
        if (!output) {
            free(wbuffer);
            return NULL;
        }
        
        if (WideCharToMultiByte(CP_UTF8, 0, wbuffer, wchar_len, output, dst_len, NULL, NULL) == 0) {
            free(wbuffer);
            free(output);
            return NULL;
        }
        
        free(wbuffer);
        output[dst_len] = '\0';
        return output;
    } else {
        int wchar_len = MultiByteToWideChar(src_cp, 0, input, (int)input_len, NULL, 0);
        if (wchar_len == 0) {
            return NULL;
        }
        
        wchar_t* wbuffer = (wchar_t*)malloc((wchar_len + 1) * sizeof(wchar_t));
        if (!wbuffer) {
            return NULL;
        }
        
        if (MultiByteToWideChar(src_cp, 0, input, (int)input_len, wbuffer, wchar_len) == 0) {
            free(wbuffer);
            return NULL;
        }
        
        int dst_len = WideCharToMultiByte(dst_cp, 0, wbuffer, wchar_len, NULL, 0, NULL, NULL);
        if (dst_len == 0) {
            free(wbuffer);
            return NULL;
        }
        
        char* output = (char*)malloc(dst_len + 1);
        if (!output) {
            free(wbuffer);
            return NULL;
        }
        
        if (WideCharToMultiByte(dst_cp, 0, wbuffer, wchar_len, output, dst_len, NULL, NULL) == 0) {
            free(wbuffer);
            free(output);
            return NULL;
        }
        
        free(wbuffer);
        output[dst_len] = '\0';
        return output;
    }
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
    
    // 检查是否只有ASCII字符
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
    
    // 检测UTF-8
    if (is_valid_utf8(input, input_len)) {
        return "UTF-8";
    }
    
    // 检测GBK（简体中文）
    if (is_valid_gbk(input, input_len)) {
        return "GBK";
    }
    
    // 检测Big5（繁体中文）
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
        // UTF-8简体中文范围
        if (c >= 0xE4 && c <= 0xE9) {
            unsigned char c2 = (i + 1 < input_len) ? (unsigned char)input[i + 1] : 0;
            unsigned char c3 = (i + 2 < input_len) ? (unsigned char)input[i + 2] : 0;
            
            // 判断是否在简体中文范围内
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
        // UTF-8繁体中文常用范围
        if (c >= 0xE6 && c <= 0xE9) {
            unsigned char c2 = (i + 1 < input_len) ? (unsigned char)input[i + 1] : 0;
            
            // 繁体中文常用字范围
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
    
    // 检查是否需要转换
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
    
    // 使用GBK/Big5转换实现简繁转换
    if (to_traditional) {
        // 简体转繁体：UTF-8 -> GBK -> Big5 -> UTF-8
        char* gbk = nl_encoding_convert(input, input_len, "UTF-8", "GBK");
        if (!gbk) return NULL;
        
        char* big5 = nl_encoding_convert(gbk, strlen(gbk), "GBK", "Big5");
        free(gbk);
        if (!big5) return NULL;
        
        char* utf8 = nl_encoding_convert(big5, strlen(big5), "Big5", "UTF-8");
        free(big5);
        return utf8;
    } else {
        // 繁体转简体：UTF-8 -> Big5 -> GBK -> UTF-8
        char* big5 = nl_encoding_convert(input, input_len, "UTF-8", "Big5");
        if (!big5) return NULL;
        
        char* gbk = nl_encoding_convert(big5, strlen(big5), "Big5", "GBK");
        free(big5);
        if (!gbk) return NULL;
        
        char* utf8 = nl_encoding_convert(gbk, strlen(gbk), "GBK", "UTF-8");
        free(gbk);
        return utf8;
    }
}

NL_API const char* nl_encoding_get_system_default(void) {
    UINT cp = GetACP();
    
    switch (cp) {
        case 936:
            return "GBK";
        case 950:
            return "Big5";
        case 65001:
            return "UTF-8";
        case 20127:
            return "US-ASCII";
        case 28591:
            return "ISO-8859-1";
        case 54936:
            return "GB18030";
        default:
            return "GBK";
    }
}
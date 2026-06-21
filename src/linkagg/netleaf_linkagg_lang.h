#ifndef NETLEAF_LINKAGG_LANG_H
#define NETLEAF_LINKAGG_LANG_H

#include "netleaf_lang.h"

// Library ID for LinkAgg
#define NL_LIB_LINKAGG 0x0002

// Supported languages for LinkAgg
static const char* nl_linkagg_languages[] = NL_LANG_CODES("en_us", "zh_cn");

// Error messages for each language
// Index 0: en_us, Index 1: zh_cn

static const char* nl_linkagg_msg_0[] = NL_ERROR_MSGS("Success", "成功");
static const char* nl_linkagg_msg_1[] = NL_ERROR_MSGS("Unknown error", "未知错误");
static const char* nl_linkagg_msg_2[] = NL_ERROR_MSGS("Platform not supported", "平台不支持（依赖IPC不可用）");
static const char* nl_linkagg_msg_3[] = NL_ERROR_MSGS("Memory allocation failed", "内存分配失败");
static const char* nl_linkagg_msg_4[] = NL_ERROR_MSGS("Invalid parameter", "无效参数");
static const char* nl_linkagg_msg_5[] = NL_ERROR_MSGS("Port bind failed", "端口绑定失败");
static const char* nl_linkagg_msg_6[] = NL_ERROR_MSGS("Backend add failed", "后端添加失败");
static const char* nl_linkagg_msg_7[] = NL_ERROR_MSGS("Service not started", "服务未启动");
static const char* nl_linkagg_msg_8[] = NL_ERROR_MSGS("Exceeded maximum backends (512)", "超过最大后端数量（512个）");
static const char* nl_linkagg_msg_9[] = NL_ERROR_MSGS("ID already in use, not connected", "不接入，ID被占用");
static const char* nl_linkagg_msg_10[] = NL_ERROR_MSGS("Invalid ID format (expected xxx.xxx)", "ID格式错误（应为xxx.xxx）");
static const char* nl_linkagg_msg_11[] = NL_ERROR_MSGS("Backend not found", "后端未找到");
static const char* nl_linkagg_msg_12[] = NL_ERROR_MSGS("Backend connection failed", "后端连接失败");
static const char* nl_linkagg_msg_13[] = NL_ERROR_MSGS("Backend not responding", "后端无响应");
static const char* nl_linkagg_msg_14[] = NL_ERROR_MSGS("No backends available", "没有可用的后端");
static const char* nl_linkagg_msg_15[] = NL_ERROR_MSGS("Invalid policy", "无效的负载均衡策略");

// Helper function to register all errors
static int nl_linkagg_register_lang(void) {
    // Register library name
    nl_lang_register_lib_name(NL_LIB_LINKAGG, "linkagg");
    
    // Register languages
    int result = nl_lang_register_lib(NL_LIB_LINKAGG, nl_linkagg_languages, 2);
    if (result != 0) return result;
    
    // Register error messages
    nl_lang_add_error(NL_LIB_LINKAGG, 0, nl_linkagg_msg_0);
    nl_lang_add_error(NL_LIB_LINKAGG, -1, nl_linkagg_msg_1);
    nl_lang_add_error(NL_LIB_LINKAGG, -2, nl_linkagg_msg_2);
    nl_lang_add_error(NL_LIB_LINKAGG, -3, nl_linkagg_msg_3);
    nl_lang_add_error(NL_LIB_LINKAGG, -4, nl_linkagg_msg_4);
    nl_lang_add_error(NL_LIB_LINKAGG, -5, nl_linkagg_msg_5);
    nl_lang_add_error(NL_LIB_LINKAGG, -6, nl_linkagg_msg_6);
    nl_lang_add_error(NL_LIB_LINKAGG, -7, nl_linkagg_msg_7);
    nl_lang_add_error(NL_LIB_LINKAGG, -8, nl_linkagg_msg_8);
    nl_lang_add_error(NL_LIB_LINKAGG, -9, nl_linkagg_msg_9);
    nl_lang_add_error(NL_LIB_LINKAGG, -10, nl_linkagg_msg_10);
    nl_lang_add_error(NL_LIB_LINKAGG, -11, nl_linkagg_msg_11);
    nl_lang_add_error(NL_LIB_LINKAGG, -12, nl_linkagg_msg_12);
    nl_lang_add_error(NL_LIB_LINKAGG, -13, nl_linkagg_msg_13);
    nl_lang_add_error(NL_LIB_LINKAGG, -14, nl_linkagg_msg_14);
    nl_lang_add_error(NL_LIB_LINKAGG, -15, nl_linkagg_msg_15);
    
    return 0;
}

// Helper macro for registration
#define NL_LINKAGG_REGISTER_LANG() nl_linkagg_register_lang()

#endif // NETLEAF_LINKAGG_LANG_H
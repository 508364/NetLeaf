#ifndef NETLEAF_ERRORPAGE_LANG_H
#define NETLEAF_ERRORPAGE_LANG_H

#include "netleaf_lang.h"

// Library ID for ErrorPage
#define NL_LIB_ERRORPAGE 0x0005

// ErrorPage error messages
NL_ERROR_BEGIN(nl_errorpage_errors)
    NL_ERROR(0,    "Success",                               "成功")
    NL_ERROR(-1,   "Unknown error",                         "未知错误")
    NL_ERROR(-3,   "Memory allocation failed",               "内存分配失败")
    NL_ERROR(-4,   "Invalid parameter",                     "无效参数")
    NL_ERROR(-5,   "Template error",                         "模板错误")
    NL_ERROR(-6,   "Missing required variable",              "缺少必需的变量")
    NL_ERROR(-7,   "Template parse error",                   "模板解析错误")
    NL_ERROR(-8,   "Variable buffer too small",               "变量缓冲区过小")
    NL_ERROR(-9,   "Invalid variable name",                  "无效的变量名")
    NL_ERROR(-10,  "Template too large",                     "模板过大")
NL_ERROR_END

// Helper macro to register errors
#define NL_ERRORPAGE_REGISTER_LANG() nl_lang_register_errors(NL_LIB_ERRORPAGE, nl_errorpage_errors)

#endif // NETLEAF_ERRORPAGE_LANG_H
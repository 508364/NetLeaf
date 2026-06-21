#ifndef NETLEAF_AUTOCOMPLETE_LANG_H
#define NETLEAF_AUTOCOMPLETE_LANG_H

#include "netleaf_lang.h"

// Library ID for AutoComplete
#define NL_LIB_AUTOCOMPLETE 0x0004

// AutoComplete error messages
NL_ERROR_BEGIN(nl_autocomplete_errors)
    NL_ERROR(0,    "Success",                               "成功")
    NL_ERROR(-1,   "Unknown error",                         "未知错误")
    NL_ERROR(-3,   "Memory allocation failed",               "内存分配失败")
    NL_ERROR(-4,   "Invalid parameter",                     "无效参数")
    NL_ERROR(-5,   "Template too large",                     "模板过大")
    NL_ERROR(-6,   "Charset detection failed",               "字符集检测失败")
NL_ERROR_END

// Helper macro to register errors
#define NL_AUTOCOMPLETE_REGISTER_LANG() nl_lang_register_errors(NL_LIB_AUTOCOMPLETE, nl_autocomplete_errors)

#endif // NETLEAF_AUTOCOMPLETE_LANG_H
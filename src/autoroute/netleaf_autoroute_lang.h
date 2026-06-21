#ifndef NETLEAF_AUTOROUTE_LANG_H
#define NETLEAF_AUTOROUTE_LANG_H

#include "netleaf_lang.h"

// Library ID for AutoRoute
#define NL_LIB_AUTOROUTE 0x0003

// AutoRoute error messages
NL_ERROR_BEGIN(nl_autoroute_errors)
    NL_ERROR(0,    "Success",                               "成功")
    NL_ERROR(-1,   "Unknown error",                         "未知错误")
    NL_ERROR(-3,   "Memory allocation failed",               "内存分配失败")
    NL_ERROR(-4,   "Invalid parameter",                     "无效参数")
    NL_ERROR(-6,   "Route registration failed",              "路由注册失败")
    NL_ERROR(-7,   "Route not found",                        "路由未找到")
    NL_ERROR(-8,   "Too many routes",                        "路由数量过多")
    NL_ERROR(-9,   "Invalid route pattern",                  "无效的路由模式")
    NL_ERROR(-10,  "Matcher initialization failed",           "匹配器初始化失败")
NL_ERROR_END

// Helper macro to register errors
#define NL_AUTOROUTE_REGISTER_LANG() nl_lang_register_errors(NL_LIB_AUTOROUTE, nl_autoroute_errors)

#endif // NETLEAF_AUTOROUTE_LANG_H
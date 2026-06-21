#ifndef NETLEAF_IPC_LANG_H
#define NETLEAF_IPC_LANG_H

#include "netleaf_lang.h"

// Library ID for IPC
#define NL_LIB_IPC 0x0001

// IPC error messages
NL_ERROR_BEGIN(nl_ipc_errors)
    NL_ERROR(0,    "Success",                               "成功")
    NL_ERROR(-1,   "Unknown error",                         "未知错误")
    NL_ERROR(-2,   "Platform not supported",                 "平台不支持")
    NL_ERROR(-3,   "Memory allocation failed",               "内存分配失败")
    NL_ERROR(-4,   "Invalid parameter",                     "无效参数")
    NL_ERROR(-5,   "Connection failed",                     "连接失败")
    NL_ERROR(-6,   "Send failed",                           "发送失败")
    NL_ERROR(-7,   "Server not started",                    "服务器未启动")
    NL_ERROR(-8,   "Server already started",                 "服务器已启动")
    NL_ERROR(-9,   "Client not connected",                   "客户端未连接")
    NL_ERROR(-10,  "Path too long",                         "路径过长")
    NL_ERROR(-11,  "Invalid path format",                   "无效的路径格式")
    NL_ERROR(-12,  "Socket creation failed",                "套接字创建失败")
    NL_ERROR(-13,  "Bind failed",                           "绑定失败")
    NL_ERROR(-14,  "Listen failed",                         "监听失败")
    NL_ERROR(-15,  "Accept failed",                         "接受连接失败")
NL_ERROR_END

// Helper macro to register errors
#define NL_IPC_REGISTER_LANG() nl_lang_register_errors(NL_LIB_IPC, nl_ipc_errors)

#endif // NETLEAF_IPC_LANG_H
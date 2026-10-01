#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * HTTPS 扩展库注册入口。
 *
 * 本文件提供 libnetleaf_https 的扩展系统入口点：
 *   - nl_https_get_extension_info()：导出 nl_extension_info_t 静态结构体，
 *     供宿主 nl_extension_auto_load 自动发现（g_ext_symbol_patterns + 通用回退）。
 *   - 可选的 nl_get_extension_info() 通用入口（见 CMake 自动发现逻辑）。
 *
 * 加载后，宿主可通过 nl_extension_register 将本扩展登记到注册表；
 * 扩展内部的 4 个 HTTPS API（nl_http_server_enable_tls /
 * nl_http_client_connect / nl_http_client_close / nl_http_client_request）
 * 由 netleaf_http_tls_<plat>.c 提供，与本文件同属一个 DLL。
 *
 * 设计原则：HTTP→TLS 的"中间层扩展"——纯 HTTP API 已由核心库
 * （netleaf_core）承载，本扩展只负责 TLS 握手 + 加密通道的接入。
 *
 * 注意：本文件仅 include 扩展系统与 TLS 后端所需的头，不直接依赖
 * netleaf.h / netleaf_http.h 的 HTTP 类型定义。
 * （v2.4.2 起 netleaf_http.h 的方法枚举更名为 nlh_http_method_t，
 *  与 netleaf.h 的 nl_http_method_t 不再重定义，两个头可安全同包包含。）
 */

#include "netleaf_module.h"

#if defined(NL_HTTPS_USE_TLS3)
    #include "netleaf_tls3.h"
    #define NL_HTTPS_BACKEND  "tls3"
    #define NL_HTTPS_BACKEND_VER NL_TLS3_VERSION
#else
    #include "netleaf_tls2.h"
    #define NL_HTTPS_BACKEND  "tls2"
    #define NL_HTTPS_BACKEND_VER NL_TLS2_VERSION
#endif

#define NL_HTTPS_VERSION "2.4.2"
#define NL_HTTPS_VERSION_MAJOR 2
#define NL_HTTPS_VERSION_MINOR 4
#define NL_HTTPS_VERSION_PATCH 2

/* ============================================================
 * 扩展生命周期回调
 * ============================================================ */

static int nl_https_ext_init(void) {
    /* 确保 TLS 后端已初始化（幂等：已初始化则返回 0） */
#if defined(NL_HTTPS_USE_TLS3)
    int rc = nl_tls3_init();
#else
    int rc = nl_tls2_init();
#endif
    return rc >= 0 ? 0 : -1;
}

static void nl_https_ext_shutdown(void) {
}

static int nl_https_ext_is_available(void) {
    return 1;
}

static const char* nl_https_ext_get_version(void) {
    return NL_HTTPS_VERSION;
}

/* ============================================================
 * 扩展信息结构体
 * ============================================================ */

NL_EXTENSION_DEFINE(https,
    "HTTPS (TLS) Extension",
    NL_HTTPS_VERSION,
    "NetLeaf",
    "HTTP-to-TLS bridge extension: provides encrypted HTTP (HTTPS) "
    "server/client APIs on top of the core HTTP stack. "
    "Bridges plain HTTP servers/clients to TLS-encrypted connections "
    "using mbedTLS " NL_HTTPS_BACKEND ".",
    "all",
    NL_CAP_TLS,
    nl_https_ext_init,
    nl_https_ext_shutdown,
    nl_https_ext_is_available,
    nl_https_ext_get_version
);

/* 标准扩展入口：宿主 auto_load 通过此符号发现 HTTPS 扩展。
 * 与 netleaf_lang / netleaf_mqtt 等其它扩展一致——*不带* 任何宏前缀，
 * 由编译器默认导出（MinGW / MSVC 下 .c 中的非 static 全局函数默认进入
 * DLL 导出表）。此前使用 NL_EXT_API 在 MinGW 下会展开为
 * __attribute__((dllexport))，函数定义处放置该属性会引发语法错误
 * （"expected ',' or ';' before '__attribute__'"），故移除。
 */
nl_extension_info_t* nl_https_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(https);
}

/* 通用回退入口：宿主 auto_load 在文件名派生失败时可尝试此符号 */
nl_extension_info_t* nl_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(https);
}

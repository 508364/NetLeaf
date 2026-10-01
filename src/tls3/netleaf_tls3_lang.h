#ifndef NETLEAF_TLS3_LANG_H
#define NETLEAF_TLS3_LANG_H

#include "netleaf_lang.h"

// Library ID for TLS3 (mbedTLS 3.x)
#define NL_LIB_TLS3 0x0007

// Supported languages for TLS3
static const char* nl_tls3_languages[] = NL_LANG_CODES("en_us", "zh_cn");

// Error messages for each language
// Index 0: en_us, Index 1: zh_cn

static const char* nl_tls3_msg_0[] = NL_ERROR_MSGS("Success", "成功");
static const char* nl_tls3_msg_1[] = NL_ERROR_MSGS("TLS initialization failed", "TLS初始化失败");
static const char* nl_tls3_msg_2[] = NL_ERROR_MSGS("CA certificate invalid or not found", "CA证书无效或不存在");
static const char* nl_tls3_msg_3[] = NL_ERROR_MSGS("Client certificate invalid or not found", "客户端证书无效或不存在");
static const char* nl_tls3_msg_4[] = NL_ERROR_MSGS("Private key invalid or not found", "私钥无效或不存在");
static const char* nl_tls3_msg_5[] = NL_ERROR_MSGS("Memory allocation failed", "内存分配失败");
static const char* nl_tls3_msg_6[] = NL_ERROR_MSGS("TLS handshake failed", "TLS握手失败");
static const char* nl_tls3_msg_7[] = NL_ERROR_MSGS("TLS read error", "TLS读取错误");
static const char* nl_tls3_msg_8[] = NL_ERROR_MSGS("TLS write error", "TLS写入错误");
static const char* nl_tls3_msg_9[] = NL_ERROR_MSGS("TLS not initialized", "TLS未初始化");
static const char* nl_tls3_msg_10[] = NL_ERROR_MSGS("Invalid TLS state", "TLS状态无效");
static const char* nl_tls3_msg_11[] = NL_ERROR_MSGS("Certificate verification failed", "证书验证失败");
static const char* nl_tls3_msg_12[] = NL_ERROR_MSGS("Protocol version not supported", "协议版本不受支持");
static const char* nl_tls3_msg_13[] = NL_ERROR_MSGS("Cipher suite negotiation failed", "密码套件协商失败");
static const char* nl_tls3_msg_14[] = NL_ERROR_MSGS("Entropy source unavailable", "熵源不可用");

// Helper function to register all errors
static int nl_tls3_register_lang(void) {
    nl_lang_register_lib_name(NL_LIB_TLS3, "tls3");

    int result = nl_lang_register_lib(NL_LIB_TLS3, nl_tls3_languages, 2);
    if (result != 0) return result;

    nl_lang_add_error(NL_LIB_TLS3, 0, nl_tls3_msg_0);
    nl_lang_add_error(NL_LIB_TLS3, -1, nl_tls3_msg_1);
    nl_lang_add_error(NL_LIB_TLS3, -2, nl_tls3_msg_2);
    nl_lang_add_error(NL_LIB_TLS3, -3, nl_tls3_msg_3);
    nl_lang_add_error(NL_LIB_TLS3, -4, nl_tls3_msg_4);
    nl_lang_add_error(NL_LIB_TLS3, -5, nl_tls3_msg_5);
    nl_lang_add_error(NL_LIB_TLS3, -6, nl_tls3_msg_6);
    nl_lang_add_error(NL_LIB_TLS3, -7, nl_tls3_msg_7);
    nl_lang_add_error(NL_LIB_TLS3, -8, nl_tls3_msg_8);
    nl_lang_add_error(NL_LIB_TLS3, -9, nl_tls3_msg_9);
    nl_lang_add_error(NL_LIB_TLS3, -10, nl_tls3_msg_10);
    nl_lang_add_error(NL_LIB_TLS3, -11, nl_tls3_msg_11);
    nl_lang_add_error(NL_LIB_TLS3, -12, nl_tls3_msg_12);
    nl_lang_add_error(NL_LIB_TLS3, -13, nl_tls3_msg_13);
    nl_lang_add_error(NL_LIB_TLS3, -14, nl_tls3_msg_14);

    return 0;
}

#define NL_TLS3_REGISTER_LANG() nl_tls3_register_lang()

#endif // NETLEAF_TLS3_LANG_H

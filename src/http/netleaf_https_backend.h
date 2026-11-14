/*
 * netleaf_https_backend.h - HTTPS 扩展的 TLS 后端选择（平台无关）
 *
 * 仅收敛 NL_HTTPS_USE_TLS3 / tls2 的符号映射，供 netleaf_https 内部
 * 多个编译单元共用，避免同一套宏在多个 .c 里各写一遍。
 */
#ifndef NL_HTTPS_BACKEND_H
#define NL_HTTPS_BACKEND_H

#if defined(NL_HTTPS_USE_TLS3)
    #include "netleaf_tls3.h"
    #define NL_HTTPS_BACKEND     "tls3"
    #define NL_HTTPS_TLS_CTX       nl_tls3_ctx_t
    #define NL_HTTPS_TLS_CONFIG    nl_tls3_config_t
    #define NL_HTTPS_TLS_OK        NL_TLS3_OK
    #define NL_HTTPS_TLS_create    nl_tls3_create
    #define NL_HTTPS_TLS_destroy   nl_tls3_destroy
    #define NL_HTTPS_TLS_configure nl_tls3_configure
    #define NL_HTTPS_TLS_set_hostname nl_tls3_set_hostname
    #define NL_HTTPS_TLS_handshake_ex nl_tls3_handshake_ex
    #define NL_HTTPS_TLS_send      nl_tls3_send
    #define NL_HTTPS_TLS_recv      nl_tls3_recv
    #define NL_HTTPS_TLS_is_available nl_tls3_is_available
    #define NL_HTTPS_TLS_get_alpn  nl_tls3_get_alpn
    #define NL_HTTPS_TLS_set_nonblocking nl_tls3_set_nonblocking
    #define NL_HTTPS_TLS_WANT_READ  NL_TLS3_WANT_READ
    #define NL_HTTPS_TLS_WANT_WRITE NL_TLS3_WANT_WRITE
#else
    #include "netleaf_tls2.h"
    #define NL_HTTPS_BACKEND     "tls2"
    #define NL_HTTPS_TLS_CTX       nl_tls2_ctx_t
    #define NL_HTTPS_TLS_CONFIG    nl_tls2_config_t
    #define NL_HTTPS_TLS_OK        NL_TLS2_OK
    #define NL_HTTPS_TLS_create    nl_tls2_create
    #define NL_HTTPS_TLS_destroy   nl_tls2_destroy
    #define NL_HTTPS_TLS_configure nl_tls2_configure
    #define NL_HTTPS_TLS_set_hostname nl_tls2_set_hostname
    #define NL_HTTPS_TLS_handshake_ex nl_tls2_handshake_ex
    #define NL_HTTPS_TLS_send      nl_tls2_send
    #define NL_HTTPS_TLS_recv      nl_tls2_recv
    #define NL_HTTPS_TLS_is_available nl_tls2_is_available
    #define NL_HTTPS_TLS_get_alpn  nl_tls2_get_alpn
    #define NL_HTTPS_TLS_set_nonblocking nl_tls2_set_nonblocking
    #define NL_HTTPS_TLS_WANT_READ  NL_TLS2_WANT_READ
    #define NL_HTTPS_TLS_WANT_WRITE NL_TLS2_WANT_WRITE
#endif

#endif /* NL_HTTPS_BACKEND_H */

/*
 * netleaf_https_session.h - https 扩展 TLS 会话对象（反代上游契约）
 *
 * 宿主通过 nl_extension_get_func_by_id("https", "nlh_tls_conn_wrap") 动态查找，
 * 因此这些符号必须显式导出（NL_API：Windows 下为 dllexport）。
 */
#ifndef NL_HTTPS_SESSION_H
#define NL_HTTPS_SESSION_H

#include "netleaf_http.h"   /* NL_API 导出宏 */

#ifdef __cplusplus
extern "C" {
#endif

/* 在既有 TCP 连接上建立客户端 TLS 会话；失败返回 NULL（绝不降级明文）。
 * 成功后底层 socket 被置为非阻塞，并返回会话句柄。 */
NL_API void* nlh_tls_conn_wrap(int sock, const char* host, const char** out_alpn);

/* >0 字节数；0 暂时不可读/写（WANT_*）；<0 EOF/错误。 */
NL_API int nlh_tls_conn_read(void* conn, void* buf, int len);
NL_API int nlh_tls_conn_write(void* conn, const void* buf, int len);

/* 释放会话（不关闭 socket）。 */
NL_API void nlh_tls_conn_free(void* conn);

#ifdef __cplusplus
}
#endif

#endif /* NL_HTTPS_SESSION_H */

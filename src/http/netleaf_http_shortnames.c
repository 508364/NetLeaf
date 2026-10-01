/*
 * NetLeaf HTTP 短名转接层（nlh_*）
 *
 * 原长名接口（nl_http_* / nl_http2_* / nl_http3_*）已标记 deprecated，
 * 直接调用仍可用但产生告警；本文件提供一一对应的短名 nlh_*，签名
 * 完全一致、无告警，为推荐用法。本文件随核心库 netleaf_core 编译导出，
 * 仅在内部转发到原长名实现，不改变 ABI。
 *
 * TLS-gated 短名（nlh_server_enable_tls / nlh_client_*）仅在
 * NL_HTTPS_ENABLE 构建（netleaf_https 目标）中实现：原长名声明同样
 * 在该门控下可用，故此处转发不会因符号缺失而链接失败。
 *
 * 本文件为库自身源码，调用原长名实现体不应产生 deprecated 告警：
 * 在 include 公共头前定义 NL_HTTP_INTERNAL_BUILD 关闭告警。
 */
#define NL_HTTP_INTERNAL_BUILD
#include "netleaf_http.h"

/* ------------------------------------------------------------
 * H1 / H2 / H3 服务端生命周期与访问器（随核心库导出，始终可用）
 * 全部转发到原长名，签名一致。
 * ------------------------------------------------------------ */

nl_http_server_t* nlh_server_create(int port) {
    return nl_http_server_create(port);
}
void nlh_server_destroy(nl_http_server_t* server) {
    nl_http_server_destroy(server);
}
int nlh_server_start(nl_http_server_t* server) {
    return nl_http_server_start(server);
}
void nlh_server_stop(nl_http_server_t* server) {
    nl_http_server_stop(server);
}
void nlh_server_set_handler(nl_http_server_t* server, nl_http_handler handler, void* user_data) {
    nl_http_server_set_handler(server, handler, user_data);
}
void nlh_server_enable_http2(nl_http_server_t* server, int enable) {
    nl_http_server_enable_http2(server, enable);
}
void nlh_server_enable_http3(nl_http_server_t* server, int enable) {
    nl_http_server_enable_http3(server, enable);
}

nl_http2_server_t* nlh_h2_create(int port) {
    return nl_http2_server_create(port);
}
void nlh_h2_destroy(nl_http2_server_t* server) {
    nl_http2_server_destroy(server);
}
int nlh_h2_start(nl_http2_server_t* server) {
    return nl_http2_server_start(server);
}
void nlh_h2_stop(nl_http2_server_t* server) {
    nl_http2_server_stop(server);
}
void nlh_h2_set_handler(nl_http2_server_t* server, nl_http_handler handler, void* user_data) {
    nl_http2_server_set_handler(server, handler, user_data);
}

nl_http3_server_t* nlh_h3_create(int port) {
    return nl_http3_server_create(port);
}
void nlh_h3_destroy(nl_http3_server_t* server) {
    nl_http3_server_destroy(server);
}
int nlh_h3_start(nl_http3_server_t* server) {
    return nl_http3_server_start(server);
}
void nlh_h3_stop(nl_http3_server_t* server) {
    nl_http3_server_stop(server);
}
void nlh_h3_set_handler(nl_http3_server_t* server, nl_http_handler handler, void* user_data) {
    nl_http3_server_set_handler(server, handler, user_data);
}

nlh_http_method_t nlh_req_method(const nl_http_request_t* req) {
    return nl_http_request_get_method(req);
}
nl_http_version_t nlh_req_version(const nl_http_request_t* req) {
    return nl_http_request_get_version(req);
}
const char* nlh_req_path(const nl_http_request_t* req) {
    return nl_http_request_get_path(req);
}
const char* nlh_req_header(const nl_http_request_t* req, const char* name) {
    return nl_http_request_get_header(req, name);
}
const char* nlh_req_body(const nl_http_request_t* req) {
    return nl_http_request_get_body(req);
}
size_t nlh_req_body_size(const nl_http_request_t* req) {
    return nl_http_request_get_body_size(req);
}

void nlh_resp_status(nl_http_response_t* resp, int status) {
    nl_http_response_set_status(resp, status);
}
void nlh_resp_header(nl_http_response_t* resp, const char* name, const char* value) {
    nl_http_response_set_header(resp, name, value);
}
void nlh_resp_body(nl_http_response_t* resp, const char* body, size_t len) {
    nl_http_response_set_body(resp, body, len);
}

/* ------------------------------------------------------------
 * TLS-gated 短名（仅 NL_HTTPS_ENABLE 构建）：原长名符号由 netleaf_https
 * 目标导出，此处转发不会因符号缺失而链接失败。
 * ------------------------------------------------------------ */
#ifdef NL_HTTPS_ENABLE

int nlh_server_enable_tls(nl_http_server_t* server, const nl_http_tls_server_cfg_t* cfg) {
    return nl_http_server_enable_tls(server, cfg);
}
nl_http_client_t* nlh_client_connect(const char* host, int port, const nl_http_tls_client_cfg_t* cfg) {
    return nl_http_client_connect(host, port, cfg);
}
void nlh_client_close(nl_http_client_t* client) {
    nl_http_client_close(client);
}
int nlh_client_request(nl_http_client_t* client,
                       const char* method,
                       const char* path,
                       const char* body, size_t body_len,
                       char* out_buf, size_t out_buf_size,
                       size_t* out_body_len) {
    return nl_http_client_request(client, method, path, body, body_len, out_buf, out_buf_size, out_body_len);
}

#endif /* NL_HTTPS_ENABLE */

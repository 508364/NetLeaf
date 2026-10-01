#ifndef NETLEAF_WEBSOCKET_H
#define NETLEAF_WEBSOCKET_H

#include <stddef.h>

// DLL export/import macros
#ifdef _WIN32
    #ifdef NL_EXPORTS
        #define NL_API __declspec(dllexport)
    #else
        #define NL_API __declspec(dllimport)
    #endif
#else
    #define NL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nl_websocket_server nl_websocket_server_t;
typedef struct nl_websocket_frame nl_websocket_frame_t;

typedef enum {
    NL_WS_CONTINUATION = 0x0,
    NL_WS_TEXT = 0x1,
    NL_WS_BINARY = 0x2,
    NL_WS_CLOSE = 0x8,
    NL_WS_PING = 0x9,
    NL_WS_PONG = 0xA
} nl_ws_opcode_t;

typedef void (*nl_ws_connect_handler)(void* user_data);
typedef void (*nl_ws_message_handler)(const char* data, size_t len, nl_ws_opcode_t opcode, void* user_data);
typedef void (*nl_ws_close_handler)(void* user_data);

/**
 * WebSocket 消息回调签名说明（重要）。
 *
 * `nl_ws_message_handler` 是 4 参数回调，**不是** 3 参数：
 *   void handler(const char* data, size_t len, nl_ws_opcode_t opcode, void* user_data);
 *
 * 第 3 个参数 `opcode` 标识消息类型：
 *   - NL_WS_TEXT / NL_WS_BINARY ：文本 / 二进制载荷，`data` 指向消息体，`len` 为长度
 *   - NL_WS_CLOSE               ：关闭帧，`data` 可能含关闭码+原因（2+ 字节）
 *   - NL_WS_PING / NL_WS_PONG   ：控制帧，通常 `len == 0` 或为 echo 数据
 *
 * 用户常误写成 3 参数（漏掉 `opcode`），编译器会报
 * "incompatible pointer to 'nl_ws_message_handler'"。请按 4 参数签名注册：
 *
 * @code
 *   void on_msg(const char* data, size_t len, nl_ws_opcode_t opcode, void* user_data) {
 *       if (opcode == NL_WS_TEXT) {
 *           printf("recv %.*s\n", (int)len, data);
 *       } else if (opcode == NL_WS_CLOSE) {
 *           // 处理关闭帧
 *       }
 *   }
 *   nl_ws_server_set_on_message(server, on_msg, my_userdata);
 * @endcode
 */

NL_API nl_websocket_server_t* nl_ws_server_create(int port);
NL_API void nl_ws_server_destroy(nl_websocket_server_t* server);
NL_API int nl_ws_server_start(nl_websocket_server_t* server);
NL_API void nl_ws_server_stop(nl_websocket_server_t* server);

NL_API void nl_ws_server_set_on_connect(nl_websocket_server_t* server, nl_ws_connect_handler handler, void* user_data);
NL_API void nl_ws_server_set_on_message(nl_websocket_server_t* server, nl_ws_message_handler handler, void* user_data);
NL_API void nl_ws_server_set_on_close(nl_websocket_server_t* server, nl_ws_close_handler handler, void* user_data);

NL_API int nl_ws_server_broadcast(nl_websocket_server_t* server, const char* data, size_t len, nl_ws_opcode_t opcode);
NL_API int nl_ws_server_send_text(nl_websocket_server_t* server, const char* data, size_t len);
NL_API int nl_ws_server_send_binary(nl_websocket_server_t* server, const void* data, size_t len);
NL_API int nl_ws_server_send_ping(nl_websocket_server_t* server);
NL_API int nl_ws_server_send_pong(nl_websocket_server_t* server, const char* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif

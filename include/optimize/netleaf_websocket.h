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

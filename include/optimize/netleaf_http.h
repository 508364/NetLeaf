#ifndef NETLEAF_HTTP_H
#define NETLEAF_HTTP_H

#include <stddef.h>
#include <stdint.h>

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

typedef struct nl_http_request nl_http_request_t;
typedef struct nl_http_response nl_http_response_t;
typedef struct nl_http_server nl_http_server_t;
typedef struct nl_http2_server nl_http2_server_t;
typedef struct nl_http3_server nl_http3_server_t;
typedef struct nl_http2_stream nl_http2_stream_t;

typedef enum {
    NL_HTTP_GET,
    NL_HTTP_POST,
    NL_HTTP_PUT,
    NL_HTTP_DELETE,
    NL_HTTP_HEAD,
    NL_HTTP_OPTIONS,
    NL_HTTP_PATCH,
    NL_HTTP_UNKNOWN
} nl_http_method_t;

typedef enum {
    NL_HTTP_VERSION_1_0,
    NL_HTTP_VERSION_1_1,
    NL_HTTP_VERSION_2,
    NL_HTTP_VERSION_3
} nl_http_version_t;

typedef enum {
    NL_H2_FRAME_DATA = 0x0,
    NL_H2_FRAME_HEADERS = 0x1,
    NL_H2_FRAME_PRIORITY = 0x2,
    NL_H2_FRAME_RST_STREAM = 0x3,
    NL_H2_FRAME_SETTINGS = 0x4,
    NL_H2_FRAME_PUSH_PROMISE = 0x5,
    NL_H2_FRAME_PING = 0x6,
    NL_H2_FRAME_GOAWAY = 0x7,
    NL_H2_FRAME_WINDOW_UPDATE = 0x8,
    NL_H2_FRAME_CONTINUATION = 0x9
} nl_h2_frame_type_t;

// HTTP/3 (QUIC) related definitions
typedef enum {
    NL_H3_FRAME_DATA = 0x0,
    NL_H3_FRAME_HEADERS = 0x1,
    NL_H3_FRAME_CANCEL_PUSH = 0x3,
    NL_H3_FRAME_SETTINGS = 0x4,
    NL_H3_FRAME_PUSH_PROMISE = 0x5,
    NL_H3_FRAME_GOAWAY = 0x7,
    NL_H3_FRAME_MAX_PUSH_ID = 0xD
} nl_h3_frame_type_t;

// QUIC packet types
typedef enum {
    NL_QUIC_PACKET_INITIAL = 0x0,
    NL_QUIC_PACKET_0RTT = 0x1,
    NL_QUIC_PACKET_HANDSHAKE = 0x2,
    NL_QUIC_PACKET_RETRY = 0x3,
    NL_QUIC_PACKET_VERSION_NEGOTIATION = 0x4,
    NL_QUIC_PACKET_SHORT = 0x5
} nl_quic_packet_type_t;

// QUIC error codes
typedef enum {
    NL_QUIC_NO_ERROR = 0x0,
    NL_QUIC_INTERNAL_ERROR = 0x1,
    NL_QUIC_CONNECTION_REFUSED = 0x2,
    NL_QUIC_FLOW_CONTROL_ERROR = 0x3,
    NL_QUIC_STREAM_LIMIT_ERROR = 0x4,
    NL_QUIC_STREAM_STATE_ERROR = 0x5,
    NL_QUIC_FINAL_SIZE_ERROR = 0x6,
    NL_QUIC_FRAME_ENCODING_ERROR = 0x7,
    NL_QUIC_TRANSPORT_PARAMETER_ERROR = 0x8,
    NL_QUIC_CONNECTION_ID_LIMIT_ERROR = 0x9,
    NL_QUIC_PROTOCOL_VIOLATION = 0xA,
    NL_QUIC_INVALID_TOKEN = 0xB,
    NL_QUIC_APPLICATION_ERROR = 0xC,
    NL_QUIC_CRYPTO_BUFFER_EXCEEDED = 0xD,
    NL_QUIC_KEY_UPDATE_ERROR = 0xE,
    NL_QUIC_AEAD_LIMIT_REACHED = 0xF
} nl_quic_error_code_t;

// QUIC transport parameter IDs
typedef enum {
    NL_QUIC_PARAM_ORIGINAL_DESTINATION_CONNECTION_ID = 0x0,
    NL_QUIC_PARAM_MAX_IDLE_TIMEOUT = 0x1,
    NL_QUIC_PARAM_STATELESS_RESET_TOKEN = 0x2,
    NL_QUIC_PARAM_MAX_UDP_PAYLOAD_SIZE = 0x3,
    NL_QUIC_PARAM_INITIAL_MAX_DATA = 0x4,
    NL_QUIC_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL = 0x5,
    NL_QUIC_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE = 0x6,
    NL_QUIC_PARAM_INITIAL_MAX_STREAM_DATA_UNI = 0x7,
    NL_QUIC_PARAM_INITIAL_MAX_STREAMS_BIDI = 0x8,
    NL_QUIC_PARAM_INITIAL_MAX_STREAMS_UNI = 0x9,
    NL_QUIC_PARAM_ACK_DELAY_EXPONENT = 0xA,
    NL_QUIC_PARAM_MAX_ACK_DELAY = 0xB,
    NL_QUIC_PARAM_DISABLE_ACTIVE_MIGRATION = 0xC,
    NL_QUIC_PARAM_PREFERRED_ADDRESS = 0xD,
    NL_QUIC_PARAM_ACTIVE_CONNECTION_ID_LIMIT = 0xE,
    NL_QUIC_PARAM_INITIAL_SOURCE_CONNECTION_ID = 0xF,
    NL_QUIC_PARAM_RETRY_SOURCE_CONNECTION_ID = 0x10
} nl_quic_transport_param_t;

// QUIC frame types
typedef enum {
    NL_QUIC_FRAME_PADDING = 0x0,
    NL_QUIC_FRAME_PING = 0x1,
    NL_QUIC_FRAME_ACK = 0x2,
    NL_QUIC_FRAME_ACK_ECN = 0x3,
    NL_QUIC_FRAME_RESET_STREAM = 0x4,
    NL_QUIC_FRAME_STOP_SENDING = 0x5,
    NL_QUIC_FRAME_CRYPTO = 0x6,
    NL_QUIC_FRAME_NEW_TOKEN = 0x7,
    NL_QUIC_FRAME_STREAM = 0x8,
    NL_QUIC_FRAME_STREAM_FIN = 0x9,
    NL_QUIC_FRAME_MAX_DATA = 0x10,
    NL_QUIC_FRAME_MAX_STREAM_DATA = 0x11,
    NL_QUIC_FRAME_MAX_STREAMS_BIDI = 0x12,
    NL_QUIC_FRAME_MAX_STREAMS_UNI = 0x13,
    NL_QUIC_FRAME_DATA_BLOCKED = 0x14,
    NL_QUIC_FRAME_STREAM_DATA_BLOCKED = 0x15,
    NL_QUIC_FRAME_STREAMS_BLOCKED_BIDI = 0x16,
    NL_QUIC_FRAME_STREAMS_BLOCKED_UNI = 0x17,
    NL_QUIC_FRAME_NEW_CONNECTION_ID = 0x18,
    NL_QUIC_FRAME_RETIRE_CONNECTION_ID = 0x19,
    NL_QUIC_FRAME_PATH_CHALLENGE = 0x1A,
    NL_QUIC_FRAME_PATH_RESPONSE = 0x1B,
    NL_QUIC_FRAME_CONNECTION_CLOSE = 0x1C,
    NL_QUIC_FRAME_HANDSHAKE_DONE = 0x1E
} nl_quic_frame_type_t;

typedef enum {
    NL_H2_SETTINGS_HEADER_TABLE_SIZE = 0x1,
    NL_H2_SETTINGS_ENABLE_PUSH = 0x2,
    NL_H2_SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    NL_H2_SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
    NL_H2_SETTINGS_MAX_FRAME_SIZE = 0x5,
    NL_H2_SETTINGS_MAX_HEADER_LIST_SIZE = 0x6
} nl_h2_settings_id_t;

typedef enum {
    NL_H2_NO_ERROR = 0x0,
    NL_H2_PROTOCOL_ERROR = 0x1,
    NL_H2_INTERNAL_ERROR = 0x2,
    NL_H2_FLOW_CONTROL_ERROR = 0x3,
    NL_H2_SETTINGS_TIMEOUT = 0x4,
    NL_H2_STREAM_CLOSED = 0x5,
    NL_H2_FRAME_SIZE_ERROR = 0x6,
    NL_H2_REFUSED_STREAM = 0x7,
    NL_H2_CANCEL = 0x8,
    NL_H2_COMPRESSION_ERROR = 0x9,
    NL_H2_CONNECT_ERROR = 0xa,
    NL_H2_ENHANCE_YOUR_CALM = 0xb,
    NL_H2_INADEQUATE_SECURITY = 0xc,
    NL_H2_HTTP_1_1_REQUIRED = 0xd
} nl_h2_error_code_t;

typedef void (*nl_http_handler)(const nl_http_request_t* req, nl_http_response_t* resp, void* user_data);

NL_API nl_http_server_t* nl_http_server_create(int port);
NL_API void nl_http_server_destroy(nl_http_server_t* server);
NL_API int nl_http_server_start(nl_http_server_t* server);
NL_API void nl_http_server_stop(nl_http_server_t* server);
NL_API void nl_http_server_set_handler(nl_http_server_t* server, nl_http_handler handler, void* user_data);
NL_API void nl_http_server_enable_http2(nl_http_server_t* server, int enable);
NL_API void nl_http_server_enable_http3(nl_http_server_t* server, int enable);

NL_API nl_http2_server_t* nl_http2_server_create(int port);
NL_API void nl_http2_server_destroy(nl_http2_server_t* server);
NL_API int nl_http2_server_start(nl_http2_server_t* server);
NL_API void nl_http2_server_stop(nl_http2_server_t* server);
NL_API void nl_http2_server_set_handler(nl_http2_server_t* server, nl_http_handler handler, void* user_data);

NL_API nl_http3_server_t* nl_http3_server_create(int port);
NL_API void nl_http3_server_destroy(nl_http3_server_t* server);
NL_API int nl_http3_server_start(nl_http3_server_t* server);
NL_API void nl_http3_server_stop(nl_http3_server_t* server);
NL_API void nl_http3_server_set_handler(nl_http3_server_t* server, nl_http_handler handler, void* user_data);

NL_API nl_http_method_t nl_http_request_get_method(const nl_http_request_t* req);
NL_API nl_http_version_t nl_http_request_get_version(const nl_http_request_t* req);
NL_API const char* nl_http_request_get_path(const nl_http_request_t* req);
NL_API const char* nl_http_request_get_header(const nl_http_request_t* req, const char* name);
NL_API const char* nl_http_request_get_body(const nl_http_request_t* req);
NL_API size_t nl_http_request_get_body_size(const nl_http_request_t* req);

NL_API void nl_http_response_set_status(nl_http_response_t* resp, int status);
NL_API void nl_http_response_set_header(nl_http_response_t* resp, const char* name, const char* value);
NL_API void nl_http_response_set_body(nl_http_response_t* resp, const char* body, size_t len);

#ifdef __cplusplus
}
#endif

#endif

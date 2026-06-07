#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>

#include "netleaf_http.h"
#include "netleaf_optimize.h"

#define MAX_HEADERS 64
#define MAX_HEADER_NAME 128
#define MAX_HEADER_VALUE 1024
#define MAX_PATH 4096
#define BUFFER_SIZE 16384
#define H2_DEFAULT_WINDOW_SIZE 65535
#define H2_MAX_FRAME_SIZE 16384
#define H2_INITIAL_SETTINGS_COUNT 6

struct nl_http_header {
    char name[MAX_HEADER_NAME];
    char value[MAX_HEADER_VALUE];
};

struct nl_http_request {
    nl_http_method_t method;
    nl_http_version_t version;
    char path[MAX_PATH];
    struct nl_http_header headers[MAX_HEADERS];
    int header_count;
    char* body;
    size_t body_size;
};

struct nl_http_response {
    int status;
    struct nl_http_header headers[MAX_HEADERS];
    int header_count;
    char* body;
    size_t body_size;
};

struct nl_http_server {
    int fd;
    int port;
    int running;
    int enable_http2;
    int enable_http3;
    pthread_t thread;
    nl_http_handler handler;
    void* user_data;
};

struct hpack_huffman_node {
    int symbol;
    int bits;
};

struct hpack_dynamic_entry {
    char name[MAX_HEADER_NAME];
    char value[MAX_HEADER_VALUE];
};

struct hpack_context {
    struct hpack_dynamic_entry dynamic_table[4096];
    int dynamic_table_size;
    int max_dynamic_table_size;
};

struct nl_h2_frame_header {
    uint32_t length : 24;
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id : 31;
    uint8_t reserved : 1;
};

struct nl_h2_settings {
    uint32_t header_table_size;
    uint32_t enable_push;
    uint32_t max_concurrent_streams;
    uint32_t initial_window_size;
    uint32_t max_frame_size;
    uint32_t max_header_list_size;
};

struct nl_http2_stream {
    uint32_t id;
    int state;
    int window_size;
    nl_http_request_t request;
    nl_http_response_t response;
    struct nl_http2_stream* next;
};

struct nl_http2_connection {
    int fd;
    struct hpack_context hpack;
    struct nl_h2_settings local_settings;
    struct nl_h2_settings remote_settings;
    int connection_window_size;
    int preface_sent;
    int settings_ack_received;
    struct nl_http2_stream* streams;
    pthread_mutex_t mutex;
};

struct nl_http2_server {
    int fd;
    int port;
    int running;
    pthread_t thread;
    nl_http_handler handler;
    void* user_data;
    struct nl_http2_connection* connections;
};

// QUIC connection state
typedef enum {
    NL_QUIC_STATE_INIT,
    NL_QUIC_STATE_HANDSHAKE,
    NL_QUIC_STATE_ESTABLISHED,
    NL_QUIC_STATE_CLOSING,
    NL_QUIC_STATE_DRAINING,
    NL_QUIC_STATE_CLOSED
} nl_quic_state_t;

// QUIC stream state
typedef enum {
    NL_QUIC_STREAM_STATE_IDLE,
    NL_QUIC_STREAM_STATE_OPEN,
    NL_QUIC_STREAM_STATE_HALF_CLOSED_LOCAL,
    NL_QUIC_STREAM_STATE_HALF_CLOSED_REMOTE,
    NL_QUIC_STREAM_STATE_CLOSED
} nl_quic_stream_state_t;

// QUIC connection ID structure
typedef struct {
    uint8_t data[20];
    uint8_t len;
} nl_quic_conn_id_t;

// QUIC transport parameters
typedef struct {
    uint64_t original_destination_connection_id;
    uint64_t max_idle_timeout;
    uint64_t stateless_reset_token;
    uint64_t max_udp_payload_size;
    uint64_t initial_max_data;
    uint64_t initial_max_stream_data_bidi_local;
    uint64_t initial_max_stream_data_bidi_remote;
    uint64_t initial_max_stream_data_uni;
    uint64_t initial_max_streams_bidi;
    uint64_t initial_max_streams_uni;
    uint64_t ack_delay_exponent;
    uint64_t max_ack_delay;
    uint64_t disable_active_migration;
    uint64_t active_connection_id_limit;
    uint64_t initial_source_connection_id;
    uint64_t retry_source_connection_id;
} nl_quic_transport_params_t;

struct nl_http3_stream {
    uint64_t id;
    nl_quic_stream_state_t state;
    uint64_t recv_offset;
    uint64_t send_offset;
    uint64_t recv_window;
    uint64_t send_window;
    nl_http_request_t request;
    nl_http_response_t response;
    uint8_t* recv_buffer;
    size_t recv_buffer_len;
    size_t recv_buffer_size;
    struct nl_http3_stream* next;
};

struct nl_http3_connection {
    int fd;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;
    nl_quic_conn_id_t dest_conn_id;
    nl_quic_conn_id_t src_conn_id;
    nl_quic_conn_id_t original_conn_id;
    uint64_t packet_number;
    nl_quic_state_t state;
    nl_quic_transport_params_t local_params;
    nl_quic_transport_params_t remote_params;
    uint64_t max_data;
    uint64_t max_streams_bidi;
    uint64_t max_streams_uni;
    uint64_t streams_bidi_count;
    uint64_t streams_uni_count;
    struct hpack_context hpack;
    struct nl_http3_stream* streams;
    uint8_t* send_buffer;
    size_t send_buffer_len;
    size_t send_buffer_size;
    pthread_mutex_t mutex;
    struct nl_http3_connection* next;
};

struct nl_http3_server {
    int fd;
    int port;
    int running;
    pthread_t thread;
    nl_http_handler handler;
    void* user_data;
    struct nl_http3_connection* connections;
    nl_quic_conn_id_t server_conn_id;
    nl_quic_transport_params_t default_params;
};

#define H3_DEFAULT_MAX_STREAMS 100
#define H3_BUFFER_SIZE 65536
#define QUIC_VERSION_1 0x00000001
#define QUIC_MAX_CONN_ID_LEN 20
#define QUIC_INITIAL_MAX_DATA 1048576
#define QUIC_INITIAL_MAX_STREAM_DATA 131072
#define QUIC_DEFAULT_MAX_STREAMS_BIDI 100
#define QUIC_DEFAULT_MAX_STREAMS_UNI 100
#define QUIC_DEFAULT_MAX_IDLE_TIMEOUT 60000

static const char* h2_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

static const char* hpack_static_table[][2] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""}
};

static nl_http_method_t parse_method(const char* method) {
    if (strcmp(method, "GET") == 0) return NL_HTTP_GET;
    if (strcmp(method, "POST") == 0) return NL_HTTP_POST;
    if (strcmp(method, "PUT") == 0) return NL_HTTP_PUT;
    if (strcmp(method, "DELETE") == 0) return NL_HTTP_DELETE;
    if (strcmp(method, "HEAD") == 0) return NL_HTTP_HEAD;
    if (strcmp(method, "OPTIONS") == 0) return NL_HTTP_OPTIONS;
    if (strcmp(method, "PATCH") == 0) return NL_HTTP_PATCH;
    return NL_HTTP_UNKNOWN;
}

static int parse_request(nl_http_request_t* req, const char* data, size_t len) {
    memset(req, 0, sizeof(*req));
    req->version = NL_HTTP_VERSION_1_1;
    
    char* buffer = malloc(len + 1);
    if (!buffer) return -1;
    memcpy(buffer, data, len);
    buffer[len] = '\0';
    
    char* line = buffer;
    char* end = strstr(line, "\r\n");
    if (!end) {
        free(buffer);
        return -1;
    }
    *end = '\0';
    
    char method[16], path[MAX_PATH], version[16];
    if (sscanf(line, "%15s %4095s %15s", method, path, version) != 3) {
        free(buffer);
        return -1;
    }
    
    req->method = parse_method(method);
    strncpy(req->path, path, MAX_PATH - 1);
    
    if (strstr(version, "2.0") != NULL) {
        req->version = NL_HTTP_VERSION_2;
    } else if (strstr(version, "1.1") != NULL) {
        req->version = NL_HTTP_VERSION_1_1;
    } else {
        req->version = NL_HTTP_VERSION_1_0;
    }
    
    line = end + 2;
    while (line < buffer + len && *line) {
        end = strstr(line, "\r\n");
        if (!end) break;
        *end = '\0';
        
        if (strlen(line) == 0) {
            line = end + 2;
            break;
        }
        
        char* colon = strchr(line, ':');
        if (colon && req->header_count < MAX_HEADERS) {
            *colon = '\0';
            strncpy(req->headers[req->header_count].name, line, MAX_HEADER_NAME - 1);
            char* value = colon + 1;
            while (*value == ' ') value++;
            strncpy(req->headers[req->header_count].value, value, MAX_HEADER_VALUE - 1);
            req->header_count++;
        }
        
        line = end + 2;
    }
    
    if (line < buffer + len) {
        size_t body_len = buffer + len - line;
        req->body = malloc(body_len + 1);
        if (req->body) {
            memcpy(req->body, line, body_len);
            req->body[body_len] = '\0';
            req->body_size = body_len;
        }
    }
    
    free(buffer);
    return 0;
}

static void generate_response_http1(nl_http_response_t* resp, char** out, size_t* out_len) {
    size_t initial_size = BUFFER_SIZE + (resp->body ? resp->body_size : 0);
    char* buffer = malloc(initial_size);
    if (!buffer) {
        *out = NULL;
        *out_len = 0;
        return;
    }
    
    int len = snprintf(buffer, initial_size, "HTTP/1.1 %d OK\r\n", resp->status);
    if (len < 0) {
        free(buffer);
        *out = NULL;
        *out_len = 0;
        return;
    }
    
    int content_len_added = 0;
    for (int i = 0; i < resp->header_count; i++) {
        if (strcmp(resp->headers[i].name, "Content-Length") == 0) {
            content_len_added = 1;
        }
        len += snprintf(buffer + len, initial_size - len, "%s: %s\r\n", 
                        resp->headers[i].name, resp->headers[i].value);
        if ((size_t)len >= initial_size) break;
    }
    
    if (!content_len_added && resp->body) {
        len += snprintf(buffer + len, initial_size - len, "Content-Length: %zu\r\n", 
                        resp->body_size);
    }
    
    len += snprintf(buffer + len, initial_size - len, "\r\n");
    
    if (resp->body && resp->body_size > 0) {
        if ((size_t)len + resp->body_size < initial_size) {
            memcpy(buffer + len, resp->body, resp->body_size);
            len += resp->body_size;
        }
    }
    
    *out = buffer;
    *out_len = len;
}

static int hpack_read_varint(const uint8_t* data, size_t len, uint8_t prefix_bits, uint64_t* value, size_t* consumed) {
    if (len == 0) return -1;
    
    uint8_t prefix_mask = (1 << prefix_bits) - 1;
    *value = data[0] & prefix_mask;
    
    if (*value < prefix_mask) {
        *consumed = 1;
        return 0;
    }
    
    *consumed = 1;
    uint8_t shift = 0;
    
    while (*consumed < len) {
        uint8_t byte = data[*consumed];
        *value |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
        (*consumed)++;
        
        if (!(byte & 0x80)) {
            return 0;
        }
        
        if (shift >= 64) {
            return -1;
        }
    }
    
    return -1;
}

static int hpack_write_varint(uint8_t* data, size_t len, uint8_t prefix_bits, uint64_t value, uint8_t prefix) {
    if (len == 0) return 0;
    
    uint8_t prefix_mask = (1 << prefix_bits) - 1;
    
    if (value < prefix_mask) {
        data[0] = (prefix & ~prefix_mask) | (uint8_t)value;
        return 1;
    }
    
    size_t offset = 0;
    data[offset++] = (prefix & ~prefix_mask) | prefix_mask;
    
    value -= prefix_mask;
    
    while (value >= 0x80) {
        if (offset >= len) return 0;
        data[offset++] = (uint8_t)(value & 0x7F) | 0x80;
        value >>= 7;
    }
    
    if (offset >= len) return 0;
    data[offset++] = (uint8_t)value;
    
    return offset;
}

static void hpack_init(struct hpack_context* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->max_dynamic_table_size = 4096;
}

static int hpack_find_static(const char* name, const char* value) {
    for (size_t i = 0; i < sizeof(hpack_static_table) / sizeof(hpack_static_table[0]); i++) {
        if (strcmp(hpack_static_table[i][0], name) == 0) {
            if (strcmp(hpack_static_table[i][1], value) == 0) {
                return i + 1;
            }
        }
    }
    for (size_t i = 0; i < sizeof(hpack_static_table) / sizeof(hpack_static_table[0]); i++) {
        if (strcmp(hpack_static_table[i][0], name) == 0) {
            return -(i + 1);
        }
    }
    return 0;
}

static int hpack_decode_header(struct hpack_context* ctx, const uint8_t* data, size_t len, char* name, char* value, size_t* consumed) {
    (void)ctx;
    if (len == 0) return -1;
    
    *consumed = 0;
    
    if (data[0] & 0x80) {
        uint64_t index;
        size_t c;
        if (hpack_read_varint(data, len, 7, &index, &c) != 0) return -1;
        *consumed = c;
        
        if (index > 0 && index <= 61) {
            strncpy(name, hpack_static_table[index - 1][0], MAX_HEADER_NAME - 1);
            strncpy(value, hpack_static_table[index - 1][1], MAX_HEADER_VALUE - 1);
            return 0;
        }
    } else if ((data[0] & 0xC0) == 0x40) {
        return -1;
    } else if ((data[0] & 0xF0) == 0) {
        uint64_t name_index = 0;
        size_t c = 0;
        
        if (data[0] & 0x0F) {
            if (hpack_read_varint(data, len, 4, &name_index, &c) != 0) return -1;
            *consumed = c;
        } else {
            *consumed = 1;
        }
        
        uint64_t value_len;
        if (hpack_read_varint(data + *consumed, len - *consumed, 7, &value_len, &c) != 0) return -1;
        *consumed += c;
        
        if (*consumed + value_len > len) return -1;
        
        if (name_index > 0 && name_index <= 61) {
            strncpy(name, hpack_static_table[name_index - 1][0], MAX_HEADER_NAME - 1);
        } else {
            strncpy(name, (const char*)(data + *consumed), value_len < MAX_HEADER_NAME ? value_len : MAX_HEADER_NAME - 1);
        }
        
        strncpy(value, (const char*)(data + *consumed), value_len < MAX_HEADER_VALUE ? value_len : MAX_HEADER_VALUE - 1);
        name[MAX_HEADER_NAME - 1] = '\0';
        value[MAX_HEADER_VALUE - 1] = '\0';
        *consumed += value_len;
        
        return 0;
    } else if ((data[0] & 0xF0) == 0x10) {
        uint64_t name_index = 0;
        size_t c = 0;
        
        if (hpack_read_varint(data, len, 4, &name_index, &c) != 0) return -1;
        *consumed = c;
        
        uint64_t value_len;
        if (hpack_read_varint(data + *consumed, len - *consumed, 7, &value_len, &c) != 0) return -1;
        *consumed += c;
        
        if (*consumed + value_len > len) return -1;
        
        if (name_index > 0 && name_index <= 61) {
            strncpy(name, hpack_static_table[name_index - 1][0], MAX_HEADER_NAME - 1);
        } else {
            strncpy(name, (const char*)(data + *consumed), value_len < MAX_HEADER_NAME ? value_len : MAX_HEADER_NAME - 1);
        }
        
        strncpy(value, (const char*)(data + *consumed), value_len < MAX_HEADER_VALUE ? value_len : MAX_HEADER_VALUE - 1);
        name[MAX_HEADER_NAME - 1] = '\0';
        value[MAX_HEADER_VALUE - 1] = '\0';
        *consumed += value_len;
        
        return 0;
    }
    
    return -1;
}

static int hpack_encode_header(struct hpack_context* ctx, uint8_t* data, size_t len, const char* name, const char* value) {
    (void)ctx;
    int idx = hpack_find_static(name, value);
    if (idx > 0) {
        return hpack_write_varint(data, len, 7, idx, 0x80);
    }
    
    size_t offset = 0;
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    
    if (idx < 0) {
        idx = -idx;
        offset += hpack_write_varint(data + offset, len - offset, 6, idx, 0x00);
    } else {
        data[offset++] = 0x00;
        offset += hpack_write_varint(data + offset, len - offset, 7, name_len, 0x00);
        if (offset + name_len > len) return 0;
        memcpy(data + offset, name, name_len);
        offset += name_len;
    }
    
    offset += hpack_write_varint(data + offset, len - offset, 7, value_len, 0x00);
    if (offset + value_len > len) return 0;
    memcpy(data + offset, value, value_len);
    offset += value_len;
    
    return offset;
}

static void h2_frame_header_write(uint8_t* data, const struct nl_h2_frame_header* header) {
    data[0] = (header->length >> 16) & 0xFF;
    data[1] = (header->length >> 8) & 0xFF;
    data[2] = header->length & 0xFF;
    data[3] = header->type;
    data[4] = header->flags;
    data[5] = (header->stream_id >> 24) & 0x7F;
    data[6] = (header->stream_id >> 16) & 0xFF;
    data[7] = (header->stream_id >> 8) & 0xFF;
    data[8] = header->stream_id & 0xFF;
}

static void h2_frame_header_read(const uint8_t* data, struct nl_h2_frame_header* header) {
    header->length = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | (uint32_t)data[2];
    header->type = data[3];
    header->flags = data[4];
    header->stream_id = ((uint32_t)(data[5] & 0x7F) << 24) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 8) | (uint32_t)data[8];
}

static int h2_send_settings_ack(struct nl_http2_connection* conn) {
    uint8_t frame[9];
    struct nl_h2_frame_header header = {0};
    header.type = NL_H2_FRAME_SETTINGS;
    header.flags = 0x01;
    header.stream_id = 0;
    h2_frame_header_write(frame, &header);
    return write(conn->fd, frame, 9);
}

static int h2_send_settings(struct nl_http2_connection* conn) {
    uint8_t payload[6 * 6];
    size_t offset = 0;
    
    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_HEADER_TABLE_SIZE;
    payload[offset++] = (conn->local_settings.header_table_size >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.header_table_size >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.header_table_size >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.header_table_size & 0xFF;
    
    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_ENABLE_PUSH;
    payload[offset++] = 0x00;
    payload[offset++] = 0x00;
    payload[offset++] = 0x00;
    payload[offset++] = conn->local_settings.enable_push & 0xFF;
    
    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_MAX_CONCURRENT_STREAMS;
    payload[offset++] = (conn->local_settings.max_concurrent_streams >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.max_concurrent_streams >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.max_concurrent_streams >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.max_concurrent_streams & 0xFF;
    
    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_INITIAL_WINDOW_SIZE;
    payload[offset++] = (conn->local_settings.initial_window_size >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.initial_window_size >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.initial_window_size >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.initial_window_size & 0xFF;
    
    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_MAX_FRAME_SIZE;
    payload[offset++] = (conn->local_settings.max_frame_size >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.max_frame_size >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.max_frame_size >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.max_frame_size & 0xFF;
    
    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_MAX_HEADER_LIST_SIZE;
    payload[offset++] = (conn->local_settings.max_header_list_size >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.max_header_list_size >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.max_header_list_size >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.max_header_list_size & 0xFF;
    
    uint8_t frame[9 + sizeof(payload)];
    struct nl_h2_frame_header header = {0};
    header.length = sizeof(payload);
    header.type = NL_H2_FRAME_SETTINGS;
    header.flags = 0;
    header.stream_id = 0;
    h2_frame_header_write(frame, &header);
    memcpy(frame + 9, payload, sizeof(payload));
    
    return write(conn->fd, frame, 9 + sizeof(payload));
}

static int h2_send_window_update(struct nl_http2_connection* conn, uint32_t stream_id, uint32_t increment) {
    uint8_t frame[13];
    struct nl_h2_frame_header header = {0};
    header.length = 4;
    header.type = NL_H2_FRAME_WINDOW_UPDATE;
    header.flags = 0;
    header.stream_id = stream_id;
    h2_frame_header_write(frame, &header);
    frame[9] = (increment >> 24) & 0x7F;
    frame[10] = (increment >> 16) & 0xFF;
    frame[11] = (increment >> 8) & 0xFF;
    frame[12] = increment & 0xFF;
    return write(conn->fd, frame, 13);
}

static int h2_send_headers(struct nl_http2_connection* conn, struct nl_http2_stream* stream, nl_http_response_t* resp) {
    uint8_t header_block[BUFFER_SIZE];
    size_t offset = 0;
    
    char status[8];
    snprintf(status, sizeof(status), "%d", resp->status);
    offset += hpack_encode_header(&conn->hpack, header_block + offset, sizeof(header_block) - offset, ":status", status);
    
    for (int i = 0; i < resp->header_count; i++) {
        offset += hpack_encode_header(&conn->hpack, header_block + offset, sizeof(header_block) - offset,
                                     resp->headers[i].name, resp->headers[i].value);
    }
    
    struct nl_h2_frame_header frame_header = {0};
    frame_header.length = offset;
    frame_header.type = NL_H2_FRAME_HEADERS;
    frame_header.flags = 0x04;
    frame_header.stream_id = stream->id;
    
    uint8_t frame[9 + offset];
    h2_frame_header_write(frame, &frame_header);
    memcpy(frame + 9, header_block, offset);
    
    ssize_t result = write(conn->fd, frame, 9 + offset);
    if (result < 0) return -1;
    
    if (resp->body && resp->body_size > 0) {
        size_t remaining = resp->body_size;
        const char* body_ptr = resp->body;
        
        while (remaining > 0) {
            size_t chunk_size = remaining < H2_MAX_FRAME_SIZE ? remaining : H2_MAX_FRAME_SIZE;
            uint8_t flags = (remaining == chunk_size) ? 0x01 : 0x00;
            
            frame_header.length = chunk_size;
            frame_header.type = NL_H2_FRAME_DATA;
            frame_header.flags = flags;
            frame_header.stream_id = stream->id;
            
            h2_frame_header_write(frame, &frame_header);
            memcpy(frame + 9, body_ptr, chunk_size);
            
            result = write(conn->fd, frame, 9 + chunk_size);
            if (result < 0) return -1;
            
            body_ptr += chunk_size;
            remaining -= chunk_size;
        }
    }
    
    return 0;
}

static struct nl_http2_stream* h2_find_or_create_stream(struct nl_http2_connection* conn, uint32_t stream_id) {
    struct nl_http2_stream* stream = conn->streams;
    while (stream) {
        if (stream->id == stream_id) return stream;
        stream = stream->next;
    }
    
    stream = calloc(1, sizeof(*stream));
    if (!stream) return NULL;
    stream->id = stream_id;
    stream->state = 0;
    stream->window_size = H2_DEFAULT_WINDOW_SIZE;
    stream->next = conn->streams;
    conn->streams = stream;
    
    return stream;
}

static int h2_process_frame(struct nl_http2_connection* conn, const uint8_t* data, size_t len, nl_http_handler handler, void* user_data) {
    if (len < 9) return -1;
    
    struct nl_h2_frame_header header;
    h2_frame_header_read(data, &header);
    
    if (header.length > len - 9) return -1;
    
    const uint8_t* payload = data + 9;
    
    switch (header.type) {
        case NL_H2_FRAME_SETTINGS:
            if (header.flags & 0x01) {
                conn->settings_ack_received = 1;
            } else {
                for (size_t i = 0; i < header.length; i += 6) {
                    uint16_t id = ((uint16_t)payload[i] << 8) | payload[i + 1];
                    uint32_t value = ((uint32_t)payload[i + 2] << 24) | ((uint32_t)payload[i + 3] << 16) | 
                                     ((uint32_t)payload[i + 4] << 8) | payload[i + 5];
                    
                    switch (id) {
                        case NL_H2_SETTINGS_HEADER_TABLE_SIZE:
                            conn->remote_settings.header_table_size = value;
                            break;
                        case NL_H2_SETTINGS_ENABLE_PUSH:
                            conn->remote_settings.enable_push = value;
                            break;
                        case NL_H2_SETTINGS_MAX_CONCURRENT_STREAMS:
                            conn->remote_settings.max_concurrent_streams = value;
                            break;
                        case NL_H2_SETTINGS_INITIAL_WINDOW_SIZE:
                            conn->remote_settings.initial_window_size = value;
                            break;
                        case NL_H2_SETTINGS_MAX_FRAME_SIZE:
                            conn->remote_settings.max_frame_size = value;
                            break;
                        case NL_H2_SETTINGS_MAX_HEADER_LIST_SIZE:
                            conn->remote_settings.max_header_list_size = value;
                            break;
                    }
                }
                h2_send_settings_ack(conn);
            }
            break;
            
        case NL_H2_FRAME_HEADERS: {
            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
            if (!stream) return -1;
            
            stream->request.version = NL_HTTP_VERSION_2;
            
            size_t offset = 0;
            while (offset < header.length) {
                char name[MAX_HEADER_NAME];
                char value[MAX_HEADER_VALUE];
                size_t consumed;
                
                if (hpack_decode_header(&conn->hpack, payload + offset, header.length - offset, name, value, &consumed) == 0) {
                    if (strcmp(name, ":method") == 0) {
                        stream->request.method = parse_method(value);
                    } else if (strcmp(name, ":path") == 0) {
                        strncpy(stream->request.path, value, MAX_PATH - 1);
                    } else if (name[0] != ':') {
                        if (stream->request.header_count < MAX_HEADERS) {
                            strncpy(stream->request.headers[stream->request.header_count].name, name, MAX_HEADER_NAME - 1);
                            strncpy(stream->request.headers[stream->request.header_count].value, value, MAX_HEADER_VALUE - 1);
                            stream->request.header_count++;
                        }
                    }
                }
                offset += consumed;
            }
            
            if (handler) {
                nl_http_response_t resp = {0};
                resp.status = 200;
                handler(&stream->request, &resp, user_data);
                h2_send_headers(conn, stream, &resp);
                
                if (resp.body) free(resp.body);
            }
            break;
        }
            
        case NL_H2_FRAME_DATA: {
            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
            if (!stream) return -1;
            
            if (header.length > 0) {
                stream->request.body = realloc(stream->request.body, stream->request.body_size + header.length);
                if (stream->request.body) {
                    memcpy(stream->request.body + stream->request.body_size, payload, header.length);
                    stream->request.body_size += header.length;
                }
            }
            
            h2_send_window_update(conn, header.stream_id, header.length);
            h2_send_window_update(conn, 0, header.length);
            break;
        }
            
        case NL_H2_FRAME_WINDOW_UPDATE:
            if (header.stream_id == 0) {
                conn->connection_window_size += ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) | 
                                                ((uint32_t)payload[2] << 8) | payload[3];
            } else {
                struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
                if (stream) {
                    stream->window_size += ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) | 
                                           ((uint32_t)payload[2] << 8) | payload[3];
                }
            }
            break;
            
        case NL_H2_FRAME_PING:
            if (!(header.flags & 0x01)) {
                struct nl_h2_frame_header ack_header = header;
                ack_header.flags = 0x01;
                uint8_t frame[17];
                h2_frame_header_write(frame, &ack_header);
                memcpy(frame + 9, payload, 8);
                write(conn->fd, frame, 17);
            }
            break;
            
        case NL_H2_FRAME_RST_STREAM:
            break;
            
        case NL_H2_FRAME_GOAWAY:
            break;
    }
    
    return 0;
}

static void h2_connection_init(struct nl_http2_connection* conn, int fd) {
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;
    conn->connection_window_size = H2_DEFAULT_WINDOW_SIZE;
    conn->local_settings.header_table_size = 4096;
    conn->local_settings.enable_push = 0;
    conn->local_settings.max_concurrent_streams = 100;
    conn->local_settings.initial_window_size = H2_DEFAULT_WINDOW_SIZE;
    conn->local_settings.max_frame_size = H2_MAX_FRAME_SIZE;
    conn->local_settings.max_header_list_size = 65536;
    hpack_init(&conn->hpack);
    pthread_mutex_init(&conn->mutex, NULL);
}

typedef struct {
    int client_fd;
    nl_http_handler handler;
    void* user_data;
} http2_client_args_t;

static void* http2_client_thread(void* arg) {
    http2_client_args_t* args = (http2_client_args_t*)arg;
    int client_fd = args->client_fd;
    nl_http_handler handler = args->handler;
    void* user_data = args->user_data;
    free(args);
    
    struct nl_http2_connection conn;
    h2_connection_init(&conn, client_fd);
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
    
    if (bytes_read >= 24 && memcmp(buffer, h2_preface, 24) == 0) {
        conn.preface_sent = 1;
        h2_send_settings(&conn);
        
        size_t remaining = bytes_read - 24;
        if (remaining > 0) {
            h2_process_frame(&conn, (uint8_t*)buffer + 24, remaining, handler, user_data);
        }
        
        while (1) {
            bytes_read = read(client_fd, buffer, sizeof(buffer));
            if (bytes_read <= 0) break;
            
            size_t offset = 0;
            while (offset < (size_t)bytes_read) {
                if (offset + 9 > (size_t)bytes_read) break;
                
                struct nl_h2_frame_header header;
                h2_frame_header_read((uint8_t*)buffer + offset, &header);
                
                if (offset + 9 + header.length > (size_t)bytes_read) break;
                
                h2_process_frame(&conn, (uint8_t*)buffer + offset, 9 + header.length, handler, user_data);
                offset += 9 + header.length;
            }
        }
    }
    
    close(client_fd);
    return NULL;
}

typedef struct {
    int client_fd;
    nl_http_server_t* server;
} http1_client_args_t;

static void* http1_client_thread(void* arg) {
    http1_client_args_t* args = (http1_client_args_t*)arg;
    int client_fd = args->client_fd;
    nl_http_server_t* server = args->server;
    free(args);
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        
        nl_http_request_t req;
        nl_http_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = 200;
        
        if (parse_request(&req, buffer, bytes_read) == 0) {
            if (server && server->handler) {
                server->handler(&req, &resp, server->user_data);
            } else {
                nl_http_response_set_body(&resp, "<html><body><h1>Hello, NetLeaf HTTP/1.1!</h1></body></html>", 53);
            }
        }
        
        char* response_data = NULL;
        size_t response_len = 0;
        generate_response_http1(&resp, &response_data, &response_len);
        
        if (response_data) {
            write(client_fd, response_data, response_len);
            free(response_data);
        }
        
        if (req.body) free(req.body);
    }
    
    close(client_fd);
    return NULL;
}

static void* server_thread(void* arg) {
    nl_http_server_t* server = (nl_http_server_t*)arg;
    
    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd >= 0) {
            if (server->enable_http2) {
                http2_client_args_t* args = malloc(sizeof(http2_client_args_t));
                args->client_fd = client_fd;
                args->handler = server->handler;
                args->user_data = server->user_data;
                pthread_t thread;
                pthread_create(&thread, NULL, http2_client_thread, args);
                pthread_detach(thread);
            } else {
                http1_client_args_t* args = malloc(sizeof(http1_client_args_t));
                args->client_fd = client_fd;
                args->server = server;
                pthread_t thread;
                pthread_create(&thread, NULL, http1_client_thread, args);
                pthread_detach(thread);
            }
        }
    }
    
    return NULL;
}

static void* http2_server_thread(void* arg) {
    nl_http2_server_t* server = (nl_http2_server_t*)arg;
    
    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd >= 0) {
            http2_client_args_t* args = malloc(sizeof(http2_client_args_t));
            args->client_fd = client_fd;
            args->handler = server->handler;
            args->user_data = server->user_data;
            
            pthread_t thread;
            pthread_create(&thread, NULL, http2_client_thread, args);
            pthread_detach(thread);
        }
    }
    
    return NULL;
}

nl_http_server_t* nl_http_server_create(int port) {
    nl_http_server_t* server = calloc(1, sizeof(nl_http_server_t));
    if (!server) return NULL;
    
    server->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->fd < 0) {
        free(server);
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }
    
    if (listen(server->fd, SOMAXCONN) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }
    
    server->port = port;
    return server;
}

void nl_http_server_destroy(nl_http_server_t* server) {
    if (!server) return;
    
    if (server->fd >= 0) close(server->fd);
    free(server);
}

int nl_http_server_start(nl_http_server_t* server) {
    if (!server || server->running) return -1;
    
    server->running = 1;
    return pthread_create(&server->thread, NULL, server_thread, server);
}

void nl_http_server_stop(nl_http_server_t* server) {
    if (!server || !server->running) return;
    
    server->running = 0;
    close(server->fd);
    pthread_join(server->thread, NULL);
}

void nl_http_server_set_handler(nl_http_server_t* server, nl_http_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}

void nl_http_server_enable_http2(nl_http_server_t* server, int enable) {
    if (server) server->enable_http2 = enable;
}

void nl_http_server_enable_http3(nl_http_server_t* server, int enable) {
    if (server) server->enable_http3 = enable;
}

nl_http2_server_t* nl_http2_server_create(int port) {
    nl_http2_server_t* server = calloc(1, sizeof(nl_http2_server_t));
    if (!server) return NULL;
    
    server->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->fd < 0) {
        free(server);
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }
    
    if (listen(server->fd, SOMAXCONN) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }
    
    server->port = port;
    return server;
}

void nl_http2_server_destroy(nl_http2_server_t* server) {
    if (!server) return;
    
    if (server->fd >= 0) close(server->fd);
    free(server);
}

int nl_http2_server_start(nl_http2_server_t* server) {
    if (!server || server->running) return -1;
    
    server->running = 1;
    return pthread_create(&server->thread, NULL, http2_server_thread, server);
}

void nl_http2_server_stop(nl_http2_server_t* server) {
    if (!server || !server->running) return;
    
    server->running = 0;
    close(server->fd);
    pthread_join(server->thread, NULL);
}

void nl_http2_server_set_handler(nl_http2_server_t* server, nl_http_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}

// Helper functions for variable-length integers
static size_t quic_read_varint(const uint8_t* data, size_t len, uint64_t* value) {
    if (len < 1) return 0;
    
    uint8_t first_byte = data[0];
    uint8_t prefix = (first_byte >> 6) & 0x03;
    
    if (prefix == 0 && len >= 1) {
        *value = first_byte & 0x3F;
        return 1;
    } else if (prefix == 1 && len >= 2) {
        *value = ((uint64_t)(first_byte & 0x3F) << 8) | data[1];
        return 2;
    } else if (prefix == 2 && len >= 4) {
        *value = ((uint64_t)(first_byte & 0x3F) << 24) | 
                 ((uint64_t)data[1] << 16) | 
                 ((uint64_t)data[2] << 8) | 
                 data[3];
        return 4;
    } else if (prefix == 3 && len >= 8) {
        *value = ((uint64_t)(first_byte & 0x3F) << 56) | 
                 ((uint64_t)data[1] << 48) | 
                 ((uint64_t)data[2] << 40) | 
                 ((uint64_t)data[3] << 32) | 
                 ((uint64_t)data[4] << 24) | 
                 ((uint64_t)data[5] << 16) | 
                 ((uint64_t)data[6] << 8) | 
                 data[7];
        return 8;
    }
    
    return 0;
}

static size_t quic_write_varint(uint8_t* data, size_t len, uint64_t value) {
    if (value < 64 && len >= 1) {
        data[0] = (uint8_t)value;
        return 1;
    } else if (value < 16384 && len >= 2) {
        data[0] = 0x40 | (uint8_t)(value >> 8);
        data[1] = (uint8_t)(value & 0xFF);
        return 2;
    } else if (value < 1073741824 && len >= 4) {
        data[0] = 0x80 | (uint8_t)(value >> 24);
        data[1] = (uint8_t)((value >> 16) & 0xFF);
        data[2] = (uint8_t)((value >> 8) & 0xFF);
        data[3] = (uint8_t)(value & 0xFF);
        return 4;
    } else if (len >= 8) {
        data[0] = 0xC0 | (uint8_t)(value >> 56);
        data[1] = (uint8_t)((value >> 48) & 0xFF);
        data[2] = (uint8_t)((value >> 40) & 0xFF);
        data[3] = (uint8_t)((value >> 32) & 0xFF);
        data[4] = (uint8_t)((value >> 24) & 0xFF);
        data[5] = (uint8_t)((value >> 16) & 0xFF);
        data[6] = (uint8_t)((value >> 8) & 0xFF);
        data[7] = (uint8_t)(value & 0xFF);
        return 8;
    }
    return 0;
}

static void quic_generate_conn_id(nl_quic_conn_id_t* conn_id, uint8_t len) {
    if (len > QUIC_MAX_CONN_ID_LEN) len = QUIC_MAX_CONN_ID_LEN;
    conn_id->len = len;
    
    // Generate random connection ID
    for (uint8_t i = 0; i < len; i++) {
        conn_id->data[i] = (uint8_t)rand();
    }
}

static struct nl_http3_stream* h3_find_or_create_stream(struct nl_http3_connection* conn, uint64_t stream_id) {
    struct nl_http3_stream* stream = conn->streams;
    while (stream) {
        if (stream->id == stream_id) return stream;
        stream = stream->next;
    }
    
    stream = calloc(1, sizeof(*stream));
    if (!stream) return NULL;
    stream->id = stream_id;
    stream->state = NL_QUIC_STREAM_STATE_IDLE;
    stream->recv_window = QUIC_INITIAL_MAX_STREAM_DATA;
    stream->send_window = QUIC_INITIAL_MAX_STREAM_DATA;
    stream->next = conn->streams;
    conn->streams = stream;
    
    return stream;
}

static void h3_stream_free(struct nl_http3_stream* stream) {
    if (stream) {
        if (stream->recv_buffer) free(stream->recv_buffer);
        if (stream->request.body) free(stream->request.body);
        if (stream->response.body) free(stream->response.body);
        free(stream);
    }
}

static int h3_process_stream_frame(struct nl_http3_connection* conn, struct nl_http3_stream* stream,
                                   const uint8_t* data, size_t len, nl_http_handler handler, void* user_data) {
    if (len < 1) return -1;
    
    uint8_t frame_type = data[0];
    size_t offset = 1;
    
    uint64_t frame_length = 0;
    size_t varint_len = quic_read_varint(data + offset, len - offset, &frame_length);
    if (varint_len == 0) return -1;
    offset += varint_len;
    
    if (offset + frame_length > len) return -1;
    
    switch (frame_type) {
        case NL_H3_FRAME_DATA: {
            // Handle data frame
            if (frame_length > 0) {
                // Ensure we have buffer space
                if (!stream->recv_buffer) {
                    stream->recv_buffer_size = 8192;
                    stream->recv_buffer = calloc(1, stream->recv_buffer_size);
                }
                
                if (stream->recv_buffer_len + frame_length > stream->recv_buffer_size) {
                    size_t new_size = stream->recv_buffer_size * 2;
                    while (new_size < stream->recv_buffer_len + frame_length) new_size *= 2;
                    uint8_t* new_buffer = realloc(stream->recv_buffer, new_size);
                    if (new_buffer) {
                        stream->recv_buffer = new_buffer;
                        stream->recv_buffer_size = new_size;
                    }
                }
                
                if (stream->recv_buffer) {
                    memcpy(stream->recv_buffer + stream->recv_buffer_len, data + offset, frame_length);
                    stream->recv_buffer_len += frame_length;
                    stream->recv_offset += frame_length;
                }
            }
            break;
        }
        
        case NL_H3_FRAME_HEADERS: {
            // Handle headers frame
            stream->request.version = NL_HTTP_VERSION_3;
            stream->request.method = NL_HTTP_GET; // Default method
            
            // Simplified header parsing - look for known headers
            size_t header_offset = 0;
            while (header_offset < frame_length) {
                uint64_t index = 0;
                size_t index_len = quic_read_varint(data + offset + header_offset, frame_length - header_offset, &index);
                if (index_len == 0) break;
                
                if (index < 61) {
                    const char* name = hpack_static_table[index][0];
                    const char* value = hpack_static_table[index][1];
                    
                    if (strcmp(name, ":method") == 0 && strlen(value) > 0) {
                        stream->request.method = parse_method(value);
                    } else if (strcmp(name, ":path") == 0 && strlen(value) > 0) {
                        strncpy(stream->request.path, value, MAX_PATH - 1);
                    } else if (name[0] != ':' && stream->request.header_count < MAX_HEADERS) {
                        strncpy(stream->request.headers[stream->request.header_count].name, name, MAX_HEADER_NAME - 1);
                        strncpy(stream->request.headers[stream->request.header_count].value, value, MAX_HEADER_VALUE - 1);
                        stream->request.header_count++;
                    }
                }
                
                header_offset += index_len;
            }
            
            // Set default path if not set
            if (strlen(stream->request.path) == 0) {
                strncpy(stream->request.path, "/", MAX_PATH - 1);
            }
            
            // Call the handler
            if (handler) {
                nl_http_response_t resp = {0};
                resp.status = 200;
                handler(&stream->request, &resp, user_data);
                
                // Send response
                uint8_t response_buffer[H3_BUFFER_SIZE];
                size_t resp_offset = 0;
                
                // Headers frame
                response_buffer[resp_offset++] = NL_H3_FRAME_HEADERS;
                resp_offset += quic_write_varint(response_buffer + resp_offset, H3_BUFFER_SIZE - resp_offset, 10);
                
                // Add :status header using static table entry 8 (index 7 with 1-based)
                response_buffer[resp_offset++] = 0x88; // Indexed header
                
                // If we have custom headers, add them
                for (int i = 0; i < resp.header_count && resp_offset + 32 < H3_BUFFER_SIZE; i++) {
                    // Literal header without indexing
                    response_buffer[resp_offset++] = 0x00;
                    resp_offset += quic_write_varint(response_buffer + resp_offset, H3_BUFFER_SIZE - resp_offset, 0);
                    size_t name_len = strlen(resp.headers[i].name);
                    resp_offset += quic_write_varint(response_buffer + resp_offset, H3_BUFFER_SIZE - resp_offset, name_len);
                    memcpy(response_buffer + resp_offset, resp.headers[i].name, name_len);
                    resp_offset += name_len;
                    size_t value_len = strlen(resp.headers[i].value);
                    resp_offset += quic_write_varint(response_buffer + resp_offset, H3_BUFFER_SIZE - resp_offset, value_len);
                    memcpy(response_buffer + resp_offset, resp.headers[i].value, value_len);
                    resp_offset += value_len;
                }
                
                sendto(conn->fd, response_buffer, resp_offset, 0,
                      (struct sockaddr*)&conn->client_addr, conn->client_addr_len);
                
                // Send data frame if we have a body
                if (resp.body && resp.body_size > 0) {
                    uint8_t data_buffer[H3_BUFFER_SIZE];
                    size_t data_offset = 0;
                    
                    data_buffer[data_offset++] = NL_H3_FRAME_DATA;
                    data_offset += quic_write_varint(data_buffer + data_offset, H3_BUFFER_SIZE - data_offset, resp.body_size);
                    memcpy(data_buffer + data_offset, resp.body, resp.body_size);
                    data_offset += resp.body_size;
                    
                    sendto(conn->fd, data_buffer, data_offset, 0,
                          (struct sockaddr*)&conn->client_addr, conn->client_addr_len);
                }
                
                if (resp.body) free(resp.body);
            }
            break;
        }
        
        case NL_H3_FRAME_SETTINGS:
            // Handle settings frame
            break;
            
        case NL_H3_FRAME_GOAWAY:
            // Handle goaway frame
            conn->state = NL_QUIC_STATE_CLOSING;
            break;
    }
    
    return 0;
}

static int h3_process_quic_packet(struct nl_http3_server* server, struct nl_http3_connection* conn,
                                  const uint8_t* data, size_t len, nl_http_handler handler, void* user_data) {
    if (len < 1) return -1;
    
    uint8_t first_byte = data[0];
    nl_quic_packet_type_t packet_type;
    
    // Determine packet type
    if ((first_byte & 0x80) == 0) {
        packet_type = NL_QUIC_PACKET_SHORT;
    } else if ((first_byte & 0x40) == 0) {
        packet_type = NL_QUIC_PACKET_VERSION_NEGOTIATION;
    } else {
        uint8_t type_bits = (first_byte >> 4) & 0x03;
        switch (type_bits) {
            case 0: packet_type = NL_QUIC_PACKET_INITIAL; break;
            case 1: packet_type = NL_QUIC_PACKET_0RTT; break;
            case 2: packet_type = NL_QUIC_PACKET_HANDSHAKE; break;
            case 3: packet_type = NL_QUIC_PACKET_RETRY; break;
            default: packet_type = NL_QUIC_PACKET_INITIAL; break;
        }
    }
    
    size_t offset = 1;
    
    if (packet_type == NL_QUIC_PACKET_VERSION_NEGOTIATION) {
        // Handle version negotiation
        return 0;
    }
    
    if (packet_type != NL_QUIC_PACKET_SHORT) {
        // Read version
        if (len < offset + 4) return -1;
        uint32_t version = ((uint32_t)data[offset] << 24) | 
                           ((uint32_t)data[offset + 1] << 16) | 
                           ((uint32_t)data[offset + 2] << 8) | 
                           data[offset + 3];
        (void)version; // Version read but not used yet
        offset += 4;
        
        // Read DCID length and DCID
        if (len < offset + 1) return -1;
        uint8_t dcid_len = data[offset];
        (void)dcid_len; // DCID length read but not used yet
        offset += 1;
        
        if (dcid_len > 0 && len >= offset + dcid_len) {
            memcpy(conn->dest_conn_id.data, data + offset, dcid_len);
            conn->dest_conn_id.len = dcid_len;
            offset += dcid_len;
        }
        
        // Read SCID length and SCID
        if (len < offset + 1) return -1;
        uint8_t scid_len = data[offset];
        offset += 1;
        
        if (scid_len > 0 && len >= offset + scid_len) {
            memcpy(conn->src_conn_id.data, data + offset, scid_len);
            conn->src_conn_id.len = scid_len;
            offset += scid_len;
        }
        
        if (packet_type == NL_QUIC_PACKET_INITIAL) {
            // Handle initial packet - start handshake
            conn->state = NL_QUIC_STATE_HANDSHAKE;
            
            // Send initial response (simplified)
            uint8_t response[H3_BUFFER_SIZE];
            size_t resp_offset = 0;
            
            // Long header packet type: Initial
            response[resp_offset++] = 0xC0; // Fixed bit, long header, initial type
            
            // Version
            response[resp_offset++] = 0x00;
            response[resp_offset++] = 0x00;
            response[resp_offset++] = 0x00;
            response[resp_offset++] = 0x01;
            
            // DCID (original SCID from client)
            response[resp_offset++] = conn->src_conn_id.len;
            memcpy(response + resp_offset, conn->src_conn_id.data, conn->src_conn_id.len);
            resp_offset += conn->src_conn_id.len;
            
            // SCID (our connection ID)
            response[resp_offset++] = server->server_conn_id.len;
            memcpy(response + resp_offset, server->server_conn_id.data, server->server_conn_id.len);
            resp_offset += server->server_conn_id.len;
            
            // Simplified crypto frame for handshake
            response[resp_offset++] = NL_QUIC_FRAME_CRYPTO;
            resp_offset += quic_write_varint(response + resp_offset, H3_BUFFER_SIZE - resp_offset, 0); // Offset
            resp_offset += quic_write_varint(response + resp_offset, H3_BUFFER_SIZE - resp_offset, 2); // Length
            response[resp_offset++] = 0x01; // Simplified crypto data
            response[resp_offset++] = 0x00;
            
            sendto(conn->fd, response, resp_offset, 0,
                  (struct sockaddr*)&conn->client_addr, conn->client_addr_len);
            
            // Transition to established state for demonstration
            conn->state = NL_QUIC_STATE_ESTABLISHED;
        }
    } else {
        // Short header packet - process frames on established connection
        if (conn->state == NL_QUIC_STATE_ESTABLISHED) {
            // Read DCID
            if (len < offset + 1) return -1;
            uint8_t dcid_len = data[offset] & 0x0F; // Simplified
            (void)dcid_len; // DCID length not used in simplified processing
            offset += 1;
            
            // Read packet number (simplified)
            offset += 2; // Skip packet number for now
            
            // Process frames (simplified - assume stream 0)
            uint64_t stream_id = 0;
            struct nl_http3_stream* stream = h3_find_or_create_stream(conn, stream_id);
            if (stream) {
                h3_process_stream_frame(conn, stream, data + offset, len - offset, handler, user_data);
            }
        }
    }
    
    return 0;
}

static void h3_connection_init(struct nl_http3_connection* conn, int fd, 
                              struct sockaddr_in* client_addr, socklen_t client_addr_len,
                              struct nl_http3_server* server) {
    (void)server;
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;
    conn->client_addr = *client_addr;
    conn->client_addr_len = client_addr_len;
    conn->state = NL_QUIC_STATE_INIT;
    conn->packet_number = 0;
    conn->max_data = QUIC_INITIAL_MAX_DATA;
    conn->max_streams_bidi = QUIC_DEFAULT_MAX_STREAMS_BIDI;
    conn->max_streams_uni = QUIC_DEFAULT_MAX_STREAMS_UNI;
    
    // Initialize transport parameters
    conn->local_params.initial_max_data = QUIC_INITIAL_MAX_DATA;
    conn->local_params.initial_max_stream_data_bidi_local = QUIC_INITIAL_MAX_STREAM_DATA;
    conn->local_params.initial_max_stream_data_bidi_remote = QUIC_INITIAL_MAX_STREAM_DATA;
    conn->local_params.initial_max_stream_data_uni = QUIC_INITIAL_MAX_STREAM_DATA;
    conn->local_params.initial_max_streams_bidi = QUIC_DEFAULT_MAX_STREAMS_BIDI;
    conn->local_params.initial_max_streams_uni = QUIC_DEFAULT_MAX_STREAMS_UNI;
    conn->local_params.max_idle_timeout = QUIC_DEFAULT_MAX_IDLE_TIMEOUT;
    conn->local_params.ack_delay_exponent = 3;
    conn->local_params.max_ack_delay = 25;
    conn->local_params.active_connection_id_limit = 8;
    
    hpack_init(&conn->hpack);
    pthread_mutex_init(&conn->mutex, NULL);
}

static void h3_connection_free(struct nl_http3_connection* conn) {
    if (conn) {
        // Free all streams
        struct nl_http3_stream* stream = conn->streams;
        while (stream) {
            struct nl_http3_stream* next = stream->next;
            h3_stream_free(stream);
            stream = next;
        }
        
        if (conn->send_buffer) free(conn->send_buffer);
        pthread_mutex_destroy(&conn->mutex);
        free(conn);
    }
}

static void* http3_server_thread(void* arg) {
    nl_http3_server_t* server = (nl_http3_server_t*)arg;
    
    uint8_t buffer[H3_BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    while (server->running) {
        ssize_t bytes_read = recvfrom(server->fd, buffer, sizeof(buffer), 0, 
                                     (struct sockaddr*)&client_addr, &client_len);
        
        if (bytes_read > 0) {
            // Find or create connection
            struct nl_http3_connection* conn = NULL;
            
            // Check existing connections
            if (server->connections) {
                pthread_mutex_lock(&server->connections->mutex);
            }
            
            struct nl_http3_connection* curr = server->connections;
            while (curr) {
                if (curr->client_addr.sin_addr.s_addr == client_addr.sin_addr.s_addr &&
                    curr->client_addr.sin_port == client_addr.sin_port) {
                    conn = curr;
                    break;
                }
                curr = curr->next;
            }
            
            if (!conn) {
                // Create new connection
                conn = calloc(1, sizeof(struct nl_http3_connection));
                if (conn) {
                    h3_connection_init(conn, server->fd, &client_addr, client_len, server);
                    
                    // Add to connection list
                    conn->next = server->connections;
                    server->connections = conn;
                }
            }
            
            if (server->connections) {
                pthread_mutex_unlock(&server->connections->mutex);
            }
            
            if (conn) {
                pthread_mutex_lock(&conn->mutex);
                h3_process_quic_packet(server, conn, buffer, bytes_read, server->handler, server->user_data);
                pthread_mutex_unlock(&conn->mutex);
            }
        }
    }
    
    // Clean up connections
    struct nl_http3_connection* conn = server->connections;
    while (conn) {
        struct nl_http3_connection* next = conn->next;
        h3_connection_free(conn);
        conn = next;
    }
    
    return NULL;
}

nl_http3_server_t* nl_http3_server_create(int port) {
    nl_http3_server_t* server = calloc(1, sizeof(nl_http3_server_t));
    if (!server) return NULL;
    
    server->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->fd < 0) {
        free(server);
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }
    
    server->port = port;
    
    // Initialize default parameters
    server->default_params.initial_max_data = QUIC_INITIAL_MAX_DATA;
    server->default_params.initial_max_stream_data_bidi_local = QUIC_INITIAL_MAX_STREAM_DATA;
    server->default_params.initial_max_stream_data_bidi_remote = QUIC_INITIAL_MAX_STREAM_DATA;
    server->default_params.initial_max_stream_data_uni = QUIC_INITIAL_MAX_STREAM_DATA;
    server->default_params.initial_max_streams_bidi = QUIC_DEFAULT_MAX_STREAMS_BIDI;
    server->default_params.initial_max_streams_uni = QUIC_DEFAULT_MAX_STREAMS_UNI;
    server->default_params.max_idle_timeout = QUIC_DEFAULT_MAX_IDLE_TIMEOUT;
    server->default_params.ack_delay_exponent = 3;
    server->default_params.max_ack_delay = 25;
    server->default_params.active_connection_id_limit = 8;
    
    // Generate server connection ID
    quic_generate_conn_id(&server->server_conn_id, 8);
    
    return server;
}

void nl_http3_server_destroy(nl_http3_server_t* server) {
    if (!server) return;
    
    if (server->fd >= 0) close(server->fd);
    free(server);
}

int nl_http3_server_start(nl_http3_server_t* server) {
    if (!server || server->running) return -1;
    
    server->running = 1;
    return pthread_create(&server->thread, NULL, http3_server_thread, server);
}

void nl_http3_server_stop(nl_http3_server_t* server) {
    if (!server || !server->running) return;
    
    server->running = 0;
    close(server->fd);
    pthread_join(server->thread, NULL);
}

void nl_http3_server_set_handler(nl_http3_server_t* server, nl_http_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}

nl_http_method_t nl_http_request_get_method(const nl_http_request_t* req) {
    if (!req) return NL_HTTP_UNKNOWN;
    return req->method;
}

nl_http_version_t nl_http_request_get_version(const nl_http_request_t* req) {
    if (!req) return NL_HTTP_VERSION_1_1;
    return req->version;
}

const char* nl_http_request_get_path(const nl_http_request_t* req) {
    if (!req) return NULL;
    return req->path;
}

const char* nl_http_request_get_header(const nl_http_request_t* req, const char* name) {
    if (!req || !name) return NULL;
    for (int i = 0; i < req->header_count; i++) {
        if (strcmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

const char* nl_http_request_get_body(const nl_http_request_t* req) {
    if (!req) return NULL;
    return req->body;
}

size_t nl_http_request_get_body_size(const nl_http_request_t* req) {
    if (!req) return 0;
    return req->body_size;
}

void nl_http_response_set_status(nl_http_response_t* resp, int status) {
    if (resp) resp->status = status;
}

void nl_http_response_set_header(nl_http_response_t* resp, const char* name, const char* value) {
    if (!resp || !name || !value || resp->header_count >= MAX_HEADERS) return;
    
    strncpy(resp->headers[resp->header_count].name, name, MAX_HEADER_NAME - 1);
    strncpy(resp->headers[resp->header_count].value, value, MAX_HEADER_VALUE - 1);
    resp->header_count++;
}

void nl_http_response_set_body(nl_http_response_t* resp, const char* body, size_t len) {
    if (!resp) return;
    
    if (resp->body) free(resp->body);
    resp->body = malloc(len + 1);
    if (resp->body) {
        memcpy(resp->body, body, len);
        resp->body[len] = '\0';
        resp->body_size = len;
    }
}

// Helper functions for variable-length integers

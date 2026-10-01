/*
 * NetLeaf HTTP/2/3 公共逻辑（平台无关）。
 *
 * 本文件实现 nl_http_parse_request / hpack_* / h2_* / quic_* / h3_* /
 * 请求响应访问器等纯逻辑函数，原先在 linux/macos/windows 三份
 * netleaf_http_*.c 中重复出现，现统一抽到这里，仅保留一份。
 *
 * 平台相关的 I/O（TCP 写 h2 帧、UDP 发报文）通过 nl_http_io_t 函数表
 * 注入，使本文件不直接依赖 <sys/socket.h> / <pthread.h> / <winsock2.h>。
 */
#include "netleaf_http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 公共静态数据
 * ============================================================ */

/* H2 连接预流（"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"，共 24 字节）。
 * 供各平台线程比较预流，故为非 static 并由 internal 头 extern 声明。 */
const char* h2_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

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

/* ============================================================
 * HTTP/1 解析
 * ============================================================ */

long nl_http_find_terminator(const char* buf, size_t total) {
    if (total < 4) return -1;
    for (size_t i = 0; i + 4 <= total; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return (long)i;
    }
    return -1;
}

nlh_http_method_t nl_http_parse_method(const char* method) {
    if (strcmp(method, "GET") == 0) return NL_HTTP_GET;
    if (strcmp(method, "POST") == 0) return NL_HTTP_POST;
    if (strcmp(method, "PUT") == 0) return NL_HTTP_PUT;
    if (strcmp(method, "DELETE") == 0) return NL_HTTP_DELETE;
    if (strcmp(method, "HEAD") == 0) return NL_HTTP_HEAD;
    if (strcmp(method, "OPTIONS") == 0) return NL_HTTP_OPTIONS;
    if (strcmp(method, "PATCH") == 0) return NL_HTTP_PATCH;
    return NL_HTTP_UNKNOWN;
}

int nl_http_parse_request(nl_http_request_t* req, const char* data, size_t len) {
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

    char method[16], path[NL_HTTP_MAX_PATH], version[16];
    if (sscanf(line, "%15s %4095s %15s", method, path, version) != 3) {
        free(buffer);
        return -1;
    }

    req->method = nl_http_parse_method(method);
    memcpy(req->path, path, strlen(path) < NL_HTTP_MAX_PATH ? strlen(path) + 1 : NL_HTTP_MAX_PATH - 1);
    req->path[NL_HTTP_MAX_PATH - 1] = '\0';

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
            snprintf(req->headers[req->header_count].name, MAX_HEADER_NAME, "%s", line);
            char* value = colon + 1;
            while (*value == ' ') value++;
            snprintf(req->headers[req->header_count].value, MAX_HEADER_VALUE, "%s", value);
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

void nl_http_generate_response_http1(nl_http_response_t* resp, char** out, size_t* out_len) {
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
        /* 累加后校验返回值与剩余空间，防止 size_t 下溢导致越界写 */
        if (len < 0 || (size_t)len >= initial_size) break;
    }

    if (!content_len_added && resp->body && len >= 0 && (size_t)len < initial_size) {
        len += snprintf(buffer + len, initial_size - len, "Content-Length: %zu\r\n",
                        resp->body_size);
    }

    if (len >= 0 && (size_t)len < initial_size) {
        len += snprintf(buffer + len, initial_size - len, "\r\n");
    }

    if (resp->body && resp->body_size > 0) {
        if (len >= 0 && (size_t)len + resp->body_size < initial_size) {
            memcpy(buffer + len, resp->body, resp->body_size);
            len += resp->body_size;
        }
    }

    /* snprintf 返回的是"理论长度"，头部过多时可能大于实际容量；
     * 需夹取到实际容量内并保证 out_len <= cap，避免发送侧按 out_len 越界读取 */
    if (len < 0) {
        len = 0;
    } else if ((size_t)len > initial_size) {
        len = (int)initial_size;
    }
    *out = buffer;
    *out_len = (size_t)len;
}

/* ============================================================
 * HPACK
 * ============================================================ */

int hpack_read_varint(const uint8_t* data, size_t len, uint8_t prefix_bits, uint64_t* value, size_t* consumed) {
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

int hpack_write_varint(uint8_t* data, size_t len, uint8_t prefix_bits, uint64_t value, uint8_t prefix) {
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

void hpack_init(struct hpack_context* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->max_dynamic_table_size = 4096;
}

void hpack_free(struct hpack_context* ctx) {
    if (!ctx) return;
    free(ctx->dynamic_table);
    ctx->dynamic_table = NULL;
    ctx->dynamic_table_size = 0;
}

int hpack_find_static(const char* name, const char* value) {
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

int hpack_decode_header(struct hpack_context* ctx, const uint8_t* data, size_t len, char* name, char* value, size_t* consumed) {
    (void)ctx;
    if (len == 0) return -1;

    *consumed = 0;

    if (data[0] & 0x80) {
        uint64_t index;
        size_t c;
        if (hpack_read_varint(data, len, 7, &index, &c) != 0) return -1;
        *consumed = c;

        if (index > 0 && index <= 61) {
            snprintf(name, MAX_HEADER_NAME, "%s", hpack_static_table[index - 1][0]);
            snprintf(value, MAX_HEADER_VALUE, "%s", hpack_static_table[index - 1][1]);
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
            snprintf(name, MAX_HEADER_NAME, "%s", hpack_static_table[name_index - 1][0]);
        } else {
            memcpy(name, (const char*)(data + *consumed), value_len < MAX_HEADER_NAME ? value_len : MAX_HEADER_NAME - 1);
        }
        name[MAX_HEADER_NAME - 1] = '\0';

        memcpy(value, (const char*)(data + *consumed), value_len < MAX_HEADER_VALUE ? value_len : MAX_HEADER_VALUE - 1);
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
            snprintf(name, MAX_HEADER_NAME, "%s", hpack_static_table[name_index - 1][0]);
        } else {
            memcpy(name, (const char*)(data + *consumed), value_len < MAX_HEADER_NAME ? value_len : MAX_HEADER_NAME - 1);
        }
        name[MAX_HEADER_NAME - 1] = '\0';

        memcpy(value, (const char*)(data + *consumed), value_len < MAX_HEADER_VALUE ? value_len : MAX_HEADER_VALUE - 1);
        value[MAX_HEADER_VALUE - 1] = '\0';
        *consumed += value_len;

        return 0;
    }

    return -1;
}

int hpack_encode_header(struct hpack_context* ctx, uint8_t* data, size_t len, const char* name, const char* value) {
    (void)ctx;
    int idx = hpack_find_static(name, value);
    if (idx > 0) {
        return hpack_write_varint(data, len, 7, idx, 0x80);
    }

    size_t offset = 0;
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);

    if (idx < 0) {
        /* 索引名 literal（RFC 7541 §6.2.1）：name index 用 4 位前缀 0x10 编码。
         * 旧实现误用 6 位前缀 0x00，会把索引写成 "索引型 header"，客户端 HPACK
         * 解析错乱 → 连接能建但无响应。此处按 4 位前缀写，索引 >15 时退化为
         * 全字面形式（0x00 + name_len + 名字）。 */
        idx = -idx;
        if (idx <= 15) {
            offset += hpack_write_varint(data + offset, len - offset, 4, idx, 0x10);
        } else {
            data[offset++] = 0x00;
            offset += hpack_write_varint(data + offset, len - offset, 7, name_len, 0x00);
            if (offset + name_len > len) return 0;
            memcpy(data + offset, name, name_len);
            offset += name_len;
        }
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

/* ============================================================
 * HTTP/2 帧头与发送（通过 io 表写帧）
 * ============================================================ */

void h2_frame_header_write(uint8_t* data, const struct nl_h2_frame_header* header) {
    data[0] = (uint8_t)((header->length >> 16) & 0xFF);
    data[1] = (uint8_t)((header->length >> 8) & 0xFF);
    data[2] = (uint8_t)(header->length & 0xFF);
    data[3] = header->type;
    data[4] = header->flags;
    data[5] = (uint8_t)((header->stream_id >> 24) & 0x7F);
    data[6] = (uint8_t)((header->stream_id >> 16) & 0xFF);
    data[7] = (uint8_t)((header->stream_id >> 8) & 0xFF);
    data[8] = (uint8_t)(header->stream_id & 0xFF);
}

void h2_frame_header_read(const uint8_t* data, struct nl_h2_frame_header* header) {
    header->length = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | (uint32_t)data[2];
    header->type = data[3];
    header->flags = data[4];
    header->stream_id = ((uint32_t)(data[5] & 0x7F) << 24) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 8) | (uint32_t)data[8];
}

int h2_send_settings_ack(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn) {
    (void)conn;
    uint8_t frame[9];
    struct nl_h2_frame_header header = {0};
    header.type = NL_H2_FRAME_SETTINGS;
    header.flags = 0x01;
    header.stream_id = 0;
    h2_frame_header_write(frame, &header);
    return io->write_h2_frame(io, fd, frame, 9);
}

int h2_send_settings(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn) {
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

    return io->write_h2_frame(io, fd, frame, 9 + sizeof(payload));
}

int h2_send_window_update(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, uint32_t stream_id, uint32_t increment) {
    (void)conn;
    uint8_t frame[13];
    struct nl_h2_frame_header header = {0};
    header.length = 4;
    header.type = NL_H2_FRAME_WINDOW_UPDATE;
    header.flags = 0;
    header.stream_id = stream_id;
    h2_frame_header_write(frame, &header);
    frame[9] = (uint8_t)((increment >> 24) & 0x7F);
    frame[10] = (uint8_t)((increment >> 16) & 0xFF);
    frame[11] = (uint8_t)((increment >> 8) & 0xFF);
    frame[12] = (uint8_t)(increment & 0xFF);
    return io->write_h2_frame(io, fd, frame, 13);
}

int h2_send_headers(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, struct nl_http2_stream* stream, nl_http_response_t* resp) {
    /* 单一堆缓冲复用：前段写 HEADERS 头块（offset），后段 [9+chunk..9+chunk+chunk]
     * 作为 DATA 分块帧体，避免原 16KB(header_block) + 16KB(frame) ≈32KB 双栈占用。
     * 动态分配（按需、可回收），连接/流生命周期内复用本帧仅一次。 */
    uint8_t* frame = (uint8_t*)malloc(9 + H2_MAX_FRAME_SIZE + 9 + H2_MAX_FRAME_SIZE);
    if (!frame) return -1;
    uint8_t* header_block = frame;             /* 头块区（前 H2_MAX_FRAME_SIZE）*/
    uint8_t* data_frame   = frame + 9 + H2_MAX_FRAME_SIZE; /* DATA 分块帧区 */
    size_t offset = 0;

    char status[8];
    snprintf(status, sizeof(status), "%d", resp->status);
    offset += hpack_encode_header(&conn->hpack, header_block + offset, H2_MAX_FRAME_SIZE - offset, ":status", status);

    for (int i = 0; i < resp->header_count; i++) {
        offset += hpack_encode_header(&conn->hpack, header_block + offset, H2_MAX_FRAME_SIZE - offset,
                                     resp->headers[i].name, resp->headers[i].value);
    }

    /* 头块过长（超出 H2 单帧上限）截断以保护帧结构；正常响应头远小于此值。 */
    if (offset > H2_MAX_FRAME_SIZE) offset = H2_MAX_FRAME_SIZE;

    struct nl_h2_frame_header frame_header = {0};
    frame_header.length = offset;
    frame_header.type = NL_H2_FRAME_HEADERS;
    frame_header.flags = 0x04;
    frame_header.stream_id = stream->id;

    h2_frame_header_write(data_frame, &frame_header);
    memcpy(data_frame + 9, header_block, offset);

    int result = io->write_h2_frame(io, fd, data_frame, 9 + offset);
    if (result < 0) { free(frame); return -1; }

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

            h2_frame_header_write(data_frame, &frame_header);
            memcpy(data_frame + 9, body_ptr, chunk_size);

            result = io->write_h2_frame(io, fd, data_frame, 9 + chunk_size);
            if (result < 0) { free(frame); return -1; }

            body_ptr += chunk_size;
            remaining -= chunk_size;
        }
    }

    free(frame);
    return 0;
}

struct nl_http2_stream* h2_find_or_create_stream(struct nl_http2_connection* conn, uint32_t stream_id) {
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

int h2_apply_header_block(struct nl_http2_connection* conn, struct nl_http2_stream* stream, const uint8_t* block, size_t len) {
    if (!stream) return -1;

    size_t offset = 0;
    while (offset < len) {
        char name[MAX_HEADER_NAME];
        char value[MAX_HEADER_VALUE];
        size_t consumed = 0;

        if (hpack_decode_header(&conn->hpack, block + offset, len - offset, name, value, &consumed) == 0) {
            if (consumed == 0) break;
            if (strcmp(name, ":method") == 0) {
                stream->request.method = nl_http_parse_method(value);
            } else if (strcmp(name, ":path") == 0) {
                snprintf(stream->request.path, NL_HTTP_MAX_PATH, "%s", value);
            } else if (name[0] != ':') {
                if (stream->request.header_count < MAX_HEADERS) {
                    snprintf(stream->request.headers[stream->request.header_count].name, MAX_HEADER_NAME, "%s", name);
                    snprintf(stream->request.headers[stream->request.header_count].value, MAX_HEADER_VALUE, "%s", value);
                    stream->request.header_count++;
                }
            }
        } else {
            break;
        }
        offset += consumed;
    }
    return 0;
}

int h2_process_frame(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, const uint8_t* data, size_t len, nl_http_handler handler, void* user_data) {
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
                for (size_t i = 0; i + 6 <= header.length; i += 6) {
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
                h2_send_settings_ack(io, fd, conn);
            }
            break;

        case NL_H2_FRAME_HEADERS: {
            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
            if (!stream) return -1;

            stream->request.version = NL_HTTP_VERSION_2;

            /* END_HEADERS(0x04) 置位：头块完整于本帧；未置位：后续由 CONTINUATION 续帧。 */
            int end_headers = (header.flags & 0x04) != 0;

            if (!end_headers) {
                /* 累积已到达的头部块，等待 CONTINUATION 补全后一次性解析 */
                size_t need = (conn->pending_headers_len ? conn->pending_headers_len : 0) + header.length;
                uint8_t* buf = realloc(conn->pending_headers_block, need + 1);
                if (!buf) return -1;
                memcpy(buf + conn->pending_headers_len, payload, header.length);
                conn->pending_headers_block = buf;
                conn->pending_headers_len = need;
                conn->pending_headers_stream = header.stream_id;
                break;
            }

            if (h2_apply_header_block(conn, stream, payload, header.length) < 0) {
                return -1;
            }

            if (handler) {
                nl_http_response_t resp = {0};
                resp.status = 200;
                handler(&stream->request, &resp, user_data);
                h2_send_headers(io, fd, conn, stream, &resp);

                if (resp.body) free(resp.body);
            }
            break;
        }

        case NL_H2_FRAME_CONTINUATION: {
            /* 无流/无累积中的头块：协议违规，忽略 */
            if (header.stream_id == 0 || !conn->pending_headers_block) break;
            if (conn->pending_headers_stream != header.stream_id) break;

            int end_headers = (header.flags & 0x04) != 0;

            if (!end_headers) {
                /* 仍未结束，继续累积（本帧是中间 CONTINUATION） */
                size_t need = conn->pending_headers_len + header.length;
                uint8_t* buf = realloc(conn->pending_headers_block, need + 1);
                if (!buf) return -1;
                memcpy(buf + conn->pending_headers_len, payload, header.length);
                conn->pending_headers_block = buf;
                conn->pending_headers_len = need;
                break;
            }

            /* END_HEADERS：解析已累积的完整头块 */
            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
            if (!stream) return -1;
            stream->request.version = NL_HTTP_VERSION_2;

            if (h2_apply_header_block(conn, stream, conn->pending_headers_block, conn->pending_headers_len) < 0) {
                free(conn->pending_headers_block);
                conn->pending_headers_block = NULL;
                conn->pending_headers_len = 0;
                conn->pending_headers_stream = 0;
                return -1;
            }

            free(conn->pending_headers_block);
            conn->pending_headers_block = NULL;
            conn->pending_headers_len = 0;
            conn->pending_headers_stream = 0;

            if (handler) {
                nl_http_response_t resp = {0};
                resp.status = 200;
                handler(&stream->request, &resp, user_data);
                h2_send_headers(io, fd, conn, stream, &resp);
                if (resp.body) free(resp.body);
            }
            break;
        }

        case NL_H2_FRAME_PUSH_PROMISE: {
            /* 服务端→客户端预留流 + promise 头块（:path/:method）。
             * 这里按 H2 语义创建预留流并解析 promise 头块；若远端
             * 禁止 PUSH（本地 enable_push=0）则忽略。 */
            if (!conn->local_settings.enable_push) break;

            if (header.length < 4) break;
            /* PRIORITY 标志时 payload 首 4 字节为依赖流/优先级 */
            size_t offset = (header.flags & 0x02) ? 4 : 0;
            uint32_t promised_stream = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                                       ((uint32_t)payload[2] << 8) | payload[3];
            offset += 4;

            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, promised_stream);
            if (!stream) return -1;
            stream->request.version = NL_HTTP_VERSION_2;

            /* promise 头块（仅 :method/:path）解析进 request；不触发 handler */
            if (offset < header.length) {
                h2_apply_header_block(conn, stream, payload + offset, header.length - offset);
            }
            break;
        }

        case NL_H2_FRAME_DATA: {
            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
            if (!stream) return -1;

            if (header.length > 0) {
                char* new_body = realloc(stream->request.body, stream->request.body_size + header.length);
                if (new_body) {
                    memcpy(new_body + stream->request.body_size, payload, header.length);
                    stream->request.body = new_body;
                    stream->request.body_size += header.length;
                }
            }

            h2_send_window_update(io, fd, conn, header.stream_id, header.length);
            h2_send_window_update(io, fd, conn, 0, header.length);
            break;
        }

        case NL_H2_FRAME_WINDOW_UPDATE:
            if (header.length >= 4) {
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
            }
            break;

        case NL_H2_FRAME_PING:
            if (!(header.flags & 0x01)) {
                struct nl_h2_frame_header ack_header = header;
                ack_header.flags = 0x01;
                uint8_t frame[17];
                h2_frame_header_write(frame, &ack_header);
                if (payload + 8 <= data + len)
                    memcpy(frame + 9, payload, 8);
                io->write_h2_frame(io, fd, frame, 17);  /* PING ACK 写失败可忽略 */
            }
            break;

        case NL_H2_FRAME_RST_STREAM:
            break;

        case NL_H2_FRAME_GOAWAY:
            break;
    }

    return 0;
}

/* ============================================================
 * QUIC varint + HTTP/3
 * ============================================================ */

size_t quic_read_varint(const uint8_t* data, size_t len, uint64_t* value) {
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

size_t quic_write_varint(uint8_t* data, size_t len, uint64_t value) {
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
    } else if (value < (1ULL << 62) && len >= 8) {
        /* 8 字节档：QUIC varint 实际可表示 0..2^62-1（prefix=3 占用 2 位） */
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

struct nl_http3_stream* h3_find_or_create_stream(struct nl_http3_connection* conn, uint64_t stream_id) {
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

void h3_stream_free(struct nl_http3_stream* stream) {
    if (stream) {
        if (stream->recv_buffer) free(stream->recv_buffer);
        if (stream->request.body) free(stream->request.body);
        if (stream->response.body) free(stream->response.body);
        free(stream);
    }
}

int h3_process_stream_frame(struct nl_http_io* io, struct nl_http3_connection* conn, struct nl_http3_stream* stream,
                           const uint8_t* data, size_t len, nl_http_handler handler, void* user_data) {
    (void)user_data;
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
            /* 数据帧：追加到 recv_buffer（动态扩容） */
            if (frame_length > 0) {
                if (!stream->recv_buffer) {
                    stream->recv_buffer_size = 8192;
                    stream->recv_buffer = calloc(1, stream->recv_buffer_size);
                }

                if (stream->recv_buffer_len + frame_length > stream->recv_buffer_size) {
                    size_t new_size = stream->recv_buffer_size ? stream->recv_buffer_size : 8192;
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
            /* 头部帧：简化 hpack 静态表解析 + 调用 handler + 回响应 */
            stream->request.version = NL_HTTP_VERSION_3;
            stream->request.method = NL_HTTP_GET; /* 默认方法 */

            size_t header_offset = 0;
            while (header_offset < frame_length) {
                uint64_t index = 0;
                size_t index_len = quic_read_varint(data + offset + header_offset, frame_length - header_offset, &index);
                if (index_len == 0) break;

                if (index < 61) {
                    const char* name = hpack_static_table[index][0];
                    const char* value = hpack_static_table[index][1];

                    if (strcmp(name, ":method") == 0 && strlen(value) > 0) {
                        stream->request.method = nl_http_parse_method(value);
                    } else if (strcmp(name, ":path") == 0 && strlen(value) > 0) {
                        snprintf(stream->request.path, NL_HTTP_MAX_PATH, "%s", value);
                    } else if (name[0] != ':' && stream->request.header_count < MAX_HEADERS) {
                        snprintf(stream->request.headers[stream->request.header_count].name, MAX_HEADER_NAME, "%s", name);
                        snprintf(stream->request.headers[stream->request.header_count].value, MAX_HEADER_VALUE, "%s", value);
                        stream->request.header_count++;
                    }
                }

                header_offset += index_len;
            }

            /* 默认路径 */
            if (stream->request.path[0] == '\0') {
                snprintf(stream->request.path, NL_HTTP_MAX_PATH, "/");
            }

            /* 调用 handler 并回包 */
            if (handler) {
                nl_http_response_t resp = {0};
                resp.status = 200;
                handler(&stream->request, &resp, user_data);

                /* 用连接的堆 send_buffer 组装回包（原先两次 64KB 栈缓冲
                 * 合并复用，削减每请求 128KB 栈占用）。 */
                if (conn->send_buffer) {
                    size_t cap = conn->send_buffer_size;

                    /* 头部帧 */
                    size_t resp_offset = 0;
                    conn->send_buffer[resp_offset++] = NL_H3_FRAME_HEADERS;
                    resp_offset += quic_write_varint(conn->send_buffer + resp_offset, cap - resp_offset, 10);
                    conn->send_buffer[resp_offset++] = 0x88; /* 静态表 :status=200 (idx 8, 1-based) */

                    for (int i = 0; i < resp.header_count && resp_offset + 32 < cap; i++) {
                        conn->send_buffer[resp_offset++] = 0x00;
                        resp_offset += quic_write_varint(conn->send_buffer + resp_offset, cap - resp_offset, 0);
                        size_t name_len = strlen(resp.headers[i].name);
                        resp_offset += quic_write_varint(conn->send_buffer + resp_offset, cap - resp_offset, name_len);
                        memcpy(conn->send_buffer + resp_offset, resp.headers[i].name, name_len);
                        resp_offset += name_len;
                        size_t value_len = strlen(resp.headers[i].value);
                        resp_offset += quic_write_varint(conn->send_buffer + resp_offset, cap - resp_offset, value_len);
                        memcpy(conn->send_buffer + resp_offset, resp.headers[i].value, value_len);
                        resp_offset += value_len;
                    }

                    io->udp_send(io, conn->fd, conn->send_buffer, resp_offset,
                                 &conn->client_addr, conn->client_addr_len);

                    /* 数据帧（若有 body） */
                    if (resp.body && resp.body_size > 0 &&
                        resp.body_size < cap) {
                        size_t data_offset = 0;
                        conn->send_buffer[data_offset++] = NL_H3_FRAME_DATA;
                        data_offset += quic_write_varint(conn->send_buffer + data_offset, cap - data_offset, resp.body_size);
                        memcpy(conn->send_buffer + data_offset, resp.body, resp.body_size);
                        data_offset += resp.body_size;

                        io->udp_send(io, conn->fd, conn->send_buffer, data_offset,
                                     &conn->client_addr, conn->client_addr_len);
                    }

                    if (resp.body) free(resp.body);
                }
            }
            break;
        }

        case NL_H3_FRAME_SETTINGS:
            /* 远端 H3 SETTINGS 透传（本演示骨架未逐字段解析） */
            break;

        case NL_H3_FRAME_GOAWAY:
            conn->state = NL_QUIC_STATE_CLOSING;
            break;
    }

    /* 以下为 QUIC 传输层帧（RFC 9000）：在 H3 控制/请求流上到达，
     * 与 H3 应用帧共用 STREAM 载荷，故在同一 switch 处按帧号处理。 */
    switch (frame_type) {
        case NL_QUIC_FRAME_MAX_DATA: {
            if (frame_length < 8) return 0;
            uint64_t max_data = 0;
            quic_read_varint(data + offset, frame_length, &max_data);
            conn->max_data = max_data;
            break;
        }
        case NL_QUIC_FRAME_MAX_STREAM_DATA: {
            if (frame_length < 16 || !stream) return 0;
            uint64_t s_id = 0, ms = 0;
            size_t o = quic_read_varint(data + offset, frame_length, &s_id);
            if (o == 0) return 0;
            quic_read_varint(data + offset + o, frame_length - o, &ms);
            if (stream->id == s_id) stream->recv_window = ms;
            break;
        }
        case NL_QUIC_FRAME_MAX_STREAMS_BIDI:
        case NL_QUIC_FRAME_MAX_STREAMS_UNI: {
            if (frame_length < 8) return 0;
            uint64_t max_streams = 0;
            quic_read_varint(data + offset, frame_length, &max_streams);
            conn->max_streams_bidi = max_streams;
            break;
        }
        case NL_QUIC_FRAME_ACK: {
            /* ACK 帧：解析 largest_acknowledged + ACK range，仅记录，不影响逻辑 */
            uint64_t largest_ack = 0;
            size_t o = quic_read_varint(data + offset, frame_length, &largest_ack);
            if (o == 0) return 0;
            uint64_t ack_delay = 0, count = 0;
            o = quic_read_varint(data + offset + o, frame_length - o, &ack_delay);
            if (o == 0) return 0;
            quic_read_varint(data + offset + o * 2, frame_length - o * 2, &count);
            /* 演示骨架：无需回 ACK-ACK，仅保留包号同步 */
            if (largest_ack > conn->packet_number) conn->packet_number = largest_ack;
            break;
        }
        case NL_QUIC_FRAME_PING:
            /* PING 无载荷，无需应答（对端可自行判定存活） */
            break;
        case NL_QUIC_FRAME_CONNECTION_CLOSE: {
            uint64_t reason_code = 0;
            quic_read_varint(data + offset, frame_length, &reason_code);
            conn->state = NL_QUIC_STATE_CLOSING;
            break;
        }
        case NL_QUIC_FRAME_HANDSHAKE_DONE:
            /* 1-RTT 可开始；保留 ESTABLISHED */
            if (conn->state == NL_QUIC_STATE_HANDSHAKE) conn->state = NL_QUIC_STATE_ESTABLISHED;
            break;
        default:
            break;
    }

    return 0;
}

int h3_process_quic_packet(struct nl_http_io* io, struct nl_http3_server* server, struct nl_http3_connection* conn,
                           const uint8_t* data, size_t len, nl_http_handler handler, void* user_data) {
    if (len < 1) return -1;

    uint8_t first_byte = data[0];
    nl_quic_packet_type_t packet_type;

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
        return 0;
    }

    if (packet_type != NL_QUIC_PACKET_SHORT) {
        /* 长包头：version + DCID + SCID */
        if (len < offset + 4) return -1;
        uint32_t version = ((uint32_t)data[offset] << 24) |
                           ((uint32_t)data[offset + 1] << 16) |
                           ((uint32_t)data[offset + 2] << 8) |
                           data[offset + 3];
        (void)version;
        offset += 4;

        if (len < offset + 1) return -1;
        uint8_t dcid_len = data[offset];
        offset += 1;
        if (dcid_len > 0 && len >= offset + dcid_len) {
            memcpy(conn->dest_conn_id.data, data + offset, dcid_len);
            conn->dest_conn_id.len = dcid_len;
            offset += dcid_len;
        }

        if (len < offset + 1) return -1;
        uint8_t scid_len = data[offset];
        offset += 1;
        if (scid_len > 0 && len >= offset + scid_len) {
            memcpy(conn->src_conn_id.data, data + offset, scid_len);
            conn->src_conn_id.len = scid_len;
            offset += scid_len;
        }

        if (packet_type == NL_QUIC_PACKET_INITIAL) {
            conn->state = NL_QUIC_STATE_HANDSHAKE;

            /* 复用连接级堆 send_buffer（按需分配、随连接回收），
             * 消除原每报文 64KB(H3_BUFFER_SIZE) 栈占用。 */
            if (!conn->send_buffer && conn->send_buffer_size < H3_BUFFER_SIZE) {
                conn->send_buffer = (uint8_t*)malloc(H3_BUFFER_SIZE);
                conn->send_buffer_size = conn->send_buffer ? H3_BUFFER_SIZE : 0;
                if (!conn->send_buffer) return -1;
            }
            uint8_t* response = conn->send_buffer;
            size_t resp_offset = 0;

            response[resp_offset++] = 0xC0; /* Fixed bit, long header, initial type */
            response[resp_offset++] = 0x00;
            response[resp_offset++] = 0x00;
            response[resp_offset++] = 0x00;
            response[resp_offset++] = 0x01;

            response[resp_offset++] = conn->src_conn_id.len;
            if (conn->src_conn_id.len > 0)
                memcpy(response + resp_offset, conn->src_conn_id.data, conn->src_conn_id.len);
            resp_offset += conn->src_conn_id.len;

            response[resp_offset++] = server->server_conn_id.len;
            if (server->server_conn_id.len > 0)
                memcpy(response + resp_offset, server->server_conn_id.data, server->server_conn_id.len);
            resp_offset += server->server_conn_id.len;

            response[resp_offset++] = NL_QUIC_FRAME_CRYPTO;
            resp_offset += quic_write_varint(response + resp_offset, conn->send_buffer_size - resp_offset, 0); /* offset */
            resp_offset += quic_write_varint(response + resp_offset, conn->send_buffer_size - resp_offset, 2);  /* length */
            response[resp_offset++] = 0x01;
            response[resp_offset++] = 0x00;

            /* 经可插拔 crypto 路径做认证加密（默认零密钥演示态；
             * tls3 扩展注入真实 AEAD 后此处自动启用真实密钥 + 标签）。 */
            if (nl_quic_crypto_active && nl_quic_crypto_active->seal) {
                uint8_t aad = 0;
                uint8_t crypt[8 + NL_QUIC_AEAD_TAG_LEN];
                int sealed = nl_quic_crypto_active->seal(nl_quic_crypto_active,
                    &nl_quic_crypto_active->aead,
                    response + resp_offset - 2, 2, conn->packet_number,
                    &aad, 1, crypt);
                if (sealed > 0) {
                    memcpy(response + resp_offset - 2, crypt, sealed);
                    resp_offset = resp_offset - 2 + sealed;
                }
            }

            io->udp_send(io, conn->fd, response, resp_offset,
                         &conn->client_addr, conn->client_addr_len);

            conn->state = NL_QUIC_STATE_ESTABLISHED; /* 演示态 */
        }
    } else {
        /* 短包头：已建立连接上的帧。
         * 先解 QUIC STREAM 帧（stream_id + offset + fin + 载荷），
         * 载荷内可能是 QUIC 传输帧（控制流）或 H3 应用帧（请求流）。 */
        if (conn->state == NL_QUIC_STATE_ESTABLISHED) {
            if (len < offset + 1) return -1;

            /* 短包头：DCID 长度字节 + packet number（演示 2 字节） */
            uint8_t dcid_len = data[offset];
            offset += 1 + dcid_len + 2;

            if (offset >= len) return 0;

            /* 解析 QUIC STREAM 帧：frame_type + varint len + varint stream_id + varint offset + FIN + payload */
            uint8_t qframe = data[offset];
            size_t qf_offset = offset + 1;
            uint64_t qframe_len = 0;
            size_t ql = quic_read_varint(data + qf_offset, len - qf_offset, &qframe_len);
            if (ql == 0) return 0;
            qf_offset += ql;

            uint64_t q_stream_id = 0;
            ql = quic_read_varint(data + qf_offset, qframe_len ? (qf_offset + qframe_len <= len ? qframe_len : 0) : 0, &q_stream_id);
            if (ql == 0) return 0;
            qf_offset += ql;

            /* 在 STREAM 帧内进一步分发：QUIC 传输帧 vs H3 应用帧 */
            if (qframe == NL_QUIC_FRAME_STREAM || qframe == NL_QUIC_FRAME_STREAM_FIN) {
                /* H3 请求/控制流：跳过 varint stream_id/offset/FIN 后为 H3 应用帧载荷 */
                uint64_t q_stream_offset = 0;
                ql = quic_read_varint(data + qf_offset, qf_offset + qframe_len <= len ? qframe_len : 0, &q_stream_offset);
                if (ql == 0) return 0;
                qf_offset += ql;
                /* FIN 标志在 payload 之后，忽略即可 */

                uint64_t stream_id_for_lookup = q_stream_id;
                struct nl_http3_stream* stream = h3_find_or_create_stream(conn, stream_id_for_lookup);
                if (stream) {
                    stream->recv_offset += q_stream_offset;
                    h3_process_stream_frame(io, conn, stream, data + qf_offset,
                                            (qf_offset + qframe_len <= len ? qframe_len : 0),
                                            handler, user_data);
                }
            } else {
                /* 非 STREAM：直接对传输帧处理（控制流的传输层帧） */
                struct nl_http3_stream* ctrl = h3_find_or_create_stream(conn, 0);
                (void)ctrl;
                h3_process_stream_frame(io, conn, ctrl, data + offset,
                                        (offset + qframe_len <= len ? qframe_len : 0),
                                        handler, user_data);
            }
        }
    }

    return 0;
}

/* ============================================================
 * 连接初始化（common 部分：默认值 + hpack + client_addr 拷贝）
 *
 * 锁的初始化（pthread_mutex_init / InitializeCriticalSection）属于平台
 * 行为，由各平台 .c 在调用本函数后通过 NL_HTTP_MUTEX_LOCK 宏补齐；
 * 此处保持平台无关，避免直接依赖 <pthread.h>/<winsock2.h>。
 * ============================================================ */

void h2_connection_init(struct nl_http2_connection* conn, nl_http_handle_t fd) {
    if (!conn) return;
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;

    conn->local_settings.header_table_size = 4096;
    conn->local_settings.enable_push = 0;
    conn->local_settings.max_concurrent_streams = 100;
    conn->local_settings.initial_window_size = H2_DEFAULT_WINDOW_SIZE;
    conn->local_settings.max_frame_size = H2_MAX_FRAME_SIZE;
    conn->local_settings.max_header_list_size = 16384;

    conn->connection_window_size = H2_DEFAULT_WINDOW_SIZE;
    conn->preface_sent = 0;
    conn->settings_ack_received = 0;
    conn->streams = NULL;

    hpack_init(&conn->hpack);
}

void h3_connection_init(struct nl_http3_connection* conn, nl_http_handle_t fd,
                        void* client_addr, size_t client_addr_len,
                        struct nl_http3_server* server) {
    if (!conn) return;
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;

    /* 平台 sockaddr（如 sockaddr_in）按 client_addr_len 拷贝 */
    if (client_addr && client_addr_len > 0) {
        conn->client_addr = malloc(client_addr_len);
        if (conn->client_addr) {
            memcpy(conn->client_addr, client_addr, client_addr_len);
            conn->client_addr_len = client_addr_len;
        }
    }

    if (server) {
        conn->dest_conn_id = server->server_conn_id;
        quic_generate_conn_id(&conn->src_conn_id, QUIC_MAX_CONN_ID_LEN);
        conn->original_conn_id = conn->src_conn_id;
        conn->max_data = QUIC_INITIAL_MAX_DATA;
        conn->max_streams_bidi = QUIC_DEFAULT_MAX_STREAMS_BIDI;
        conn->max_streams_uni = QUIC_DEFAULT_MAX_STREAMS_UNI;
        /* send_buffer / local_params / 锁由平台 lock_init 初始化，避免双重分配 */
        conn->next = server->connections;
        server->connections = conn;
    } else {
        quic_generate_conn_id(&conn->src_conn_id, QUIC_MAX_CONN_ID_LEN);
        conn->original_conn_id = conn->src_conn_id;
    }

    conn->packet_number = 0;
    conn->state = NL_QUIC_STATE_INIT;
    conn->streams = NULL;

    hpack_init(&conn->hpack);
}

void h3_connection_free(struct nl_http3_connection* conn) {
    if (!conn) return;
    struct nl_http3_stream* stream = conn->streams;
    while (stream) {
        struct nl_http3_stream* next = stream->next;
        h3_stream_free(stream);
        stream = next;
    }
    hpack_free(&conn->hpack);
    if (conn->send_buffer) free(conn->send_buffer);
    if (conn->client_addr) free(conn->client_addr);
    free(conn);
}

/* ============================================================
 * 请求 / 响应 访问器
 * ============================================================ */

nlh_http_method_t nl_http_request_get_method(const nl_http_request_t* req) {
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

    snprintf(resp->headers[resp->header_count].name, MAX_HEADER_NAME, "%s", name);
    snprintf(resp->headers[resp->header_count].value, MAX_HEADER_VALUE, "%s", value);
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

/* ============================================================
 * QUIC 可插拔加密（内置零密钥演示态）
 *
 * 核心库不链接 mbedTLS，默认 crypto 仅做"拷贝 + 追加 16 字节占位标签"
 * （无真实 AEAD，aead.real_key==0）。tls3 扩展在真实密钥协商完成后，
 * 会用 mbedTLS AEAD 实现（ChaCha20-Poly1305 / AES-GCM）替换 seal/open，
 * 并把 real_key 置 1 后注入。
 * ============================================================ */
static int nl_quic_default_seal(struct nl_quic_crypto* crypto, nl_quic_aead_ctx_t* ctx,
                               const uint8_t* in, size_t in_len,
                               uint64_t pkt_num,
                               const uint8_t* aad, size_t aad_len,
                               uint8_t* out) {
    (void)crypto; (void)aad; (void)aad_len;
    if (!ctx || !ctx->real_key) {
        /* 演示态：原样拷贝，追加 16 字节零标签，返回总长 */
        if (out) {
            memcpy(out, in, in_len);
            memset(out + in_len, 0, NL_QUIC_AEAD_TAG_LEN);
            (void)pkt_num;
        }
        return (int)(in_len + NL_QUIC_AEAD_TAG_LEN);
    }
    /* 真实 AEAD 由 tls3 注入的 seal 覆盖，这里不应被调用 */
    return -1;
}

static int nl_quic_default_open(struct nl_quic_crypto* crypto, nl_quic_aead_ctx_t* ctx,
                                const uint8_t* in, size_t in_len,
                                uint64_t pkt_num,
                                const uint8_t* aad, size_t aad_len,
                                uint8_t* out) {
    (void)crypto; (void)aad; (void)aad_len;
    if (!ctx || !ctx->real_key) {
        /* 演示态：剥掉尾部 16 字节标签，返回明文体 */
        if (in_len <= NL_QUIC_AEAD_TAG_LEN) return -1;
        size_t plain = in_len - NL_QUIC_AEAD_TAG_LEN;
        if (out) memcpy(out, in, plain);
        (void)pkt_num;
        return (int)plain;
    }
    return -1;
}

static void nl_quic_default_free(struct nl_quic_crypto* crypto) {
    (void)crypto;
}

static struct nl_quic_crypto nl_quic_default_crypto = {
    .opaque   = NULL,
    .aead     = { .real_key = 0 },
    .seal     = nl_quic_default_seal,
    .open     = nl_quic_default_open,
    .free_ctx = nl_quic_default_free,
};

struct nl_quic_crypto* nl_quic_crypto_active = &nl_quic_default_crypto;

void nl_http_quic_crypto_install(struct nl_quic_crypto* crypto) {
    nl_quic_crypto_active = crypto ? crypto : &nl_quic_default_crypto;
}

nl_quic_crypto_t* nl_http_quic_crypto_default(void) {
    return &nl_quic_default_crypto;
}

/* ============================================================
 * 帧编解码回环（端对端联调）
 *
 * 纯内存，供单元测试 / 联调校验编解码正确性。
 * ============================================================ */

int h2_frame_roundtrip(uint8_t* data, size_t data_cap,
                       const struct nl_h2_frame_header* header,
                       const uint8_t* payload, size_t payload_len) {
    if (!data || !header || payload_len > H2_MAX_FRAME_SIZE) return 0;
    if (data_cap < 9 + payload_len) return 0;

    h2_frame_header_write(data, header);
    if (payload_len > 0) memcpy(data + 9, payload, payload_len);

    struct nl_h2_frame_header back;
    h2_frame_header_read(data, &back);
    if (back.length != header->length || back.type != header->type ||
        back.flags != header->flags || back.stream_id != header->stream_id) {
        return 0;
    }
    if (header->length != payload_len) return 0;
    if (payload_len > 0 && back.length > 0 &&
        memcmp(data + 9, payload, back.length) != 0) return 0;
    return 1;
}

int quic_varint_roundtrip(const uint64_t* values, size_t count,
                          uint8_t* out, size_t out_cap, size_t* out_used) {
    if (!values || !out || !out_used) return 0;
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        size_t w = quic_write_varint(out + off, out_cap - off, values[i]);
        if (w == 0) return 0;
        off += w;
    }
    *out_used = off;

    size_t p = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t v = 0;
        size_t r = quic_read_varint(out + p, off - p, &v);
        if (r == 0 || v != values[i]) return 0;
        p += r;
    }
    return 1;
}

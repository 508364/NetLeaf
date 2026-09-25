#define _GNU_SOURCE
/* MQTT-TLS functions are defined within this same library; avoid dllimport (LNK4217). */
#define NL_MQTT_TLS_STATIC
#include "netleaf_mqtt.h"
#include "netleaf_module.h"
#include "netleaf_mqtt_tls.h"
#include "netleaf_mqtt_lang.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <errno.h>
    #define snprintf _snprintf
    #define strdup _strdup
    #define closesocket_close closesocket
    typedef int fd_t;
    #ifndef EAGAIN
        #define EAGAIN 11
    #endif
    #ifndef EWOULDBLOCK
        #define EWOULDBLOCK EAGAIN
    #endif
    static inline char* nl_strndup(const char* s, size_t n) {
        size_t len = strlen(s);
        if (len > n) len = n;
        char* result = (char*)malloc(len + 1);
        if (!result) return NULL;
        memcpy(result, s, len);
        result[len] = '\0';
        return result;
    }
    #define strndup nl_strndup
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #define closesocket_close close
    typedef int fd_t;
#endif

// ============================================================
// Internal types
// ============================================================

typedef struct nl_mqtt_message {
    char*            topic;
    void*            payload;
    size_t           payload_len;
    int              qos;
    int              retain;
    int              dup;
    uint16_t         packet_id;
    nl_mqtt_message_callback_t callback;
    void*            user_data;
    struct nl_mqtt_message* next;
} nl_mqtt_message_t;

typedef struct nl_mqtt_subscription {
    char*            topic;
    int              qos;
    int              no_local;
    int              retain_as_published;
    int              retain_handling;
    nl_mqtt_message_callback_t callback;
    void*            user_data;
    struct nl_mqtt_subscription* next;
} nl_mqtt_subscription_t;

typedef struct nl_mqtt_pending {
    uint16_t         packet_id;
    nl_mqtt_msg_type_t type;
    time_t           sent_at;
    int              retries;
    struct nl_mqtt_pending* next;
} nl_mqtt_pending_t;

typedef struct nl_mqtt_client {
    int                        sock;
    int                        tls_enabled;
    int                        tls_insecure;
    nl_mqtt_tls_ctx_t*         tls_ctx;
    char*                      host;
    uint16_t                   port;
    nl_mqtt_status_t           status;
    int                        protocol_version;

    /* session state */
    char*                      client_id;
    uint16_t                   keep_alive;
    time_t                     last_ping;
    int                        clean_session;

    /* auth */
    char*                      username;
    char*                      password;

    /* will */
    int                        will_enabled;
    char*                      will_topic;
    char*                      will_msg;
    size_t                     will_msg_len;
    int                        will_qos;
    int                        will_retain;

    /* subscriptions */
    nl_mqtt_subscription_t*    subscriptions;

    /* pending PUBACK/PUBREC */
    nl_mqtt_pending_t*         pending;

    /* counters */
    uint16_t                   next_packet_id;

    /* callbacks */
    nl_mqtt_connect_callback_t on_connect;
    nl_mqtt_disconnect_callback_t on_disconnect;
    void*                      user_data;

    /* output buffer */
    char*                      out_buf;
    size_t                     out_buf_len;
    size_t                     out_buf_cap;

    /* input accumulation buffer (TCP 粘包/半包重组) */
    char*                      recv_buf;
    size_t                     recv_len;
    size_t                     recv_cap;

    /* reconnect */
    int                        auto_reconnect;
    int                        last_rc;

    /* MQTT 5.0 negotiated properties */
    uint32_t                   max_packet_size;
    uint16_t                   topic_alias_max;
    uint32_t                   receive_maximum;
    uint16_t                   maximum_qos;
    int                        retain_available;
    int                        wildcard_sub_available;
    int                        sub_identifiers_available;
    int                        shared_sub_available;
} nl_mqtt_client_t;

// ============================================================
// Module state
// ============================================================

static int g_mqtt_initialized = 0;
static int g_mqtt_available   = 0;

static nl_module_info_t g_mqtt_module_info = {
    .type            = NL_MODULE_CUSTOM,
    .name            = "mqtt",
    .version         = NL_MQTT_VERSION,
    .capabilities    = NL_CAP_CLIENT | NL_CAP_ASYNC | NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL,
    .status          = NL_MODULE_STATUS_UNINITIALIZED,
    .platform_windows = 1,
    .platform_linux   = 1,
    .platform_macos   = 1,
    .init            = nl_mqtt_init,
    .shutdown        = NULL,
    .is_available    = nl_mqtt_is_available,
    .get_version     = nl_mqtt_version,
    .description     = "MQTT v3.1.1/v5.0 client with TLS/SSL support",
    .author          = "NetLeaf",
    .next            = NULL
};

// ============================================================
// Internal helpers
// ============================================================

static void nl_mqtt_init_sock(void) {
#ifdef _WIN32
    static int ws_started = 0;
    if (!ws_started) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        ws_started = 1;
    }
#endif
}

/* 确保缓冲区容量不小于 needed。
 * 约定：本函数只负责扩容，绝不修改 *len（有效数据长度由调用方维护，
 * 调用方在成功后才会把新数据写入并自行递增 *len）。 */
static int nl_mqtt_expand_buffer(char** buf, size_t* len, size_t* cap, size_t needed) {
    (void)len; /* 保持形参以兼容既有调用签名，但不再修改其指向的值 */
    if (*cap >= needed) return 1;
    size_t new_cap = *cap;
    if (new_cap == 0) new_cap = 256;
    while (new_cap < needed) new_cap *= 2;
    char* nb = (char*)realloc(*buf, new_cap);
    if (!nb) return 0;
    *buf = nb;
    *cap = new_cap;
    return 1;
}

/* Encode length per MQTT spec (variable byte encoding) */
size_t nl_mqtt_encoded_len(size_t length) {
    size_t encoded = 0;
    do {
        encoded++;
        length /= 128;
    } while (length > 0);
    return encoded;
}

static int nl_mqtt_encode_length(char* buf, size_t len, size_t value) {
    size_t pos = 0;
    do {
        if (pos >= len) return -1;
        unsigned char digit = value % 128;
        value /= 128;
        if (value > 0) digit |= 0x80;
        buf[pos++] = (char)digit;
    } while (value > 0);
    return (int)pos;
}

/* 回收“长度前缀”预留区中未被使用的字节。
 * 构造帧时会在 reserved_off 处预留 4 字节用于可变字节编码的长度前缀，
 * 正文从 body_off(= reserved_off + 4) 开始。本函数在 reserved_off 写入
 * body_len 的最小可变字节编码，并把正文整体左移以消除多余预留字节。
 * 返回回收后报文的总长度。 */
static size_t nl_mqtt_reclaim_reserved(char* pkt, size_t reserved_off,
                                       size_t body_off, size_t body_len) {
    int enc = nl_mqtt_encode_length(pkt + reserved_off, 4, body_len);
    if (enc <= 0) {
        /* 预留 4 字节足以容纳任意合法剩余长度，此处仅为防御性处理 */
        return body_off + body_len;
    }
    if ((size_t)enc < (body_off - reserved_off)) {
        memmove(pkt + reserved_off + (size_t)enc, pkt + body_off, body_len);
    }
    return reserved_off + (size_t)enc + body_len;
}

static int nl_mqtt_read_bytes_u16(const char* buf, size_t len, size_t* offset,
                                   uint16_t* val) {
    if (*offset + 2 > len) return -1;
    *val  = ((uint8_t)buf[*offset]) << 8;
    *val |= (uint8_t)buf[*offset + 1];
    *offset += 2;
    return 0;
}

static int nl_mqtt_read_bytes_size_t(const char* buf, size_t len, size_t* offset,
                                      size_t* val) {
    if (*offset + 2 > len) return -1;
    *val  = ((uint8_t)buf[*offset]) << 8;
    *val |= (uint8_t)buf[*offset + 1];
    *offset += 2;
    return 0;
}

static int nl_mqtt_read_bytes(const char* buf, size_t len, size_t* offset,
                               uint16_t* val) {
    if (*offset + 2 > len) return -1;
    *val  = ((uint8_t)buf[*offset]) << 8;
    *val |= (uint8_t)buf[*offset + 1];
    *offset += 2;
    return 0;
}

static int nl_mqtt_read_varint(const char* buf, size_t len, size_t* offset,
                                size_t* val) {
    size_t multiplier = 1;
    *val = 0;
    int digit;
    for (int i = 0; i < 4 && *offset < len; i++, (*offset)++) {
        digit  = (uint8_t)buf[*offset];
        *val  |= (size_t)(digit & 0x7F) * multiplier;
        multiplier *= 128;
        if ((digit & 0x80) == 0) return 0;
    }
    return -1;
}

static int nl_mqtt_read_varint_u32(const char* buf, size_t len, size_t* offset,
                                    uint32_t* val) {
    size_t multiplier = 1;
    *val = 0;
    int digit;
    for (int i = 0; i < 4 && *offset < len; i++, (*offset)++) {
        digit  = (uint8_t)buf[*offset];
        *val  |= (uint32_t)((digit & 0x7F) * multiplier);
        if (i == 3 && (digit & 0x80)) return -1;
        multiplier *= 128;
        if ((digit & 0x80) == 0) return 0;
    }
    return -1;
}

static int nl_mqtt_write_utf8(char* buf, size_t len, size_t* offset,
                               const char* str) {
    size_t slen = strlen(str);
    if (*offset + 2 + slen > len) return -1;
    buf[(*offset)++] = (char)((slen >> 8) & 0xFF);
    buf[(*offset)++] = (char)(slen & 0xFF);
    memcpy(&buf[*offset], str, slen);
    *offset += slen;
    return 0;
}

static int nl_mqtt_write_bytes(char* buf, size_t len, size_t* offset,
                                const void* data, size_t dlen) {
    if (*offset + dlen > len) return -1;
    memcpy(&buf[*offset], data, dlen);
    *offset += dlen;
    return 0;
}

static int nl_mqtt_append_buf(nl_mqtt_client_t* c, const char* data, size_t len) {
    if (!nl_mqtt_expand_buffer(&c->out_buf, &c->out_buf_len, &c->out_buf_cap,
                                c->out_buf_len + len))
        return -1;
    memcpy(c->out_buf + c->out_buf_len, data, len);
    c->out_buf_len += len;
    return 0;
}

static void nl_mqtt_free_pending(nl_mqtt_client_t* c) {
    nl_mqtt_pending_t* p = c->pending;
    while (p) {
        nl_mqtt_pending_t* n = p->next;
        free(p);
        p = n;
    }
    c->pending = NULL;
}

static void nl_mqtt_free_subscriptions(nl_mqtt_client_t* c) {
    nl_mqtt_subscription_t* s = c->subscriptions;
    while (s) {
        nl_mqtt_subscription_t* n = s->next;
        free(s->topic);
        free(s);
        s = n;
    }
    c->subscriptions = NULL;
}

static void nl_mqtt_remove_pending(nl_mqtt_client_t* c, uint16_t packet_id,
                                    nl_mqtt_msg_type_t expected_type) {
    nl_mqtt_pending_t* prev = NULL;
    nl_mqtt_pending_t* cur  = c->pending;
    while (cur) {
        if (cur->packet_id == packet_id && cur->type == expected_type) {
            if (prev) prev->next = cur->next;
            else       c->pending = cur->next;
            free(cur);
            return;
        }
        prev = cur;
        cur  = cur->next;
    }
}

static nl_mqtt_pending_t* nl_mqtt_find_pending(nl_mqtt_client_t* c,
                                                uint16_t packet_id,
                                                nl_mqtt_msg_type_t type) {
    nl_mqtt_pending_t* p = c->pending;
    while (p) {
        if (p->packet_id == packet_id && p->type == type) return p;
        p = p->next;
    }
    return NULL;
}

static void nl_mqtt_queue_pending(nl_mqtt_client_t* c, uint16_t packet_id,
                                   nl_mqtt_msg_type_t type) {
    nl_mqtt_pending_t* p = (nl_mqtt_pending_t*)calloc(1, sizeof(*p));
    if (!p) return;
    p->packet_id  = packet_id;
    p->type       = type;
    p->sent_at    = time(NULL);
    p->retries    = 0;
    p->next       = c->pending;
    c->pending    = p;
}

static nl_mqtt_subscription_t* nl_mqtt_find_sub(nl_mqtt_client_t* c,
                                                 const char* topic) {
    nl_mqtt_subscription_t* s = c->subscriptions;
    while (s) {
        if (strcmp(s->topic, topic) == 0) return s;
        s = s->next;
    }
    return NULL;
}

// ============================================================
// Topic matching (wildcard: + and #)
// ============================================================

int nl_mqtt_validate_topic(const char* topic) {
    if (!topic || *topic == '\0') return 0;
    size_t len = strlen(topic);
    if (len > NL_MQTT_MAX_TOPIC_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        if (topic[i] == '\0') return 0;
    }
    return 1;
}

int nl_mqtt_topic_matches(const char* pattern, const char* topic) {
    if (!pattern || !topic) return 0;
    size_t plen = strlen(pattern);
    size_t tlen = strlen(topic);

    size_t pi = 0, ti = 0;
    size_t fi = 0, sti = 0;

    while (ti < tlen) {
        if (pi < plen && pattern[pi] == '#') {
            return (pi + 1 == plen);
        }
        if (pi < plen && pattern[pi] == '+') {
            pi++; ti++;
            continue;
        }
        if (pi < plen && pattern[pi] == topic[ti]) {
            pi++; ti++;
            continue;
        }
        if (fi > 0) {
            pi  = fi;
            ti  = ++sti;
            continue;
        }
        return 0;
    }
    while (pi < plen && pattern[pi] == '+') pi++;
    if (pi < plen && pattern[pi] == '#') pi++;
    return (pi == plen);
}

// ============================================================
// Client ID validation
// ============================================================

int nl_mqtt_client_id_valid(const char* client_id) {
    if (!client_id) return 0;
    size_t len = strlen(client_id);
    if (len == 0 || len > NL_MQTT_MAX_CLIENT_ID_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)client_id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == '/'))
            return 0;
    }
    return 1;
}

// ============================================================
// Packet ID generator
// ============================================================

uint16_t nl_mqtt_get_next_packet_id(nl_mqtt_client_t* client) {
    if (!client) return 0;
    if (client->next_packet_id == 0) client->next_packet_id = 1;
    else if (client->next_packet_id == 65535) client->next_packet_id = 1;
    else client->next_packet_id++;
    return client->next_packet_id;
}

// ============================================================
// Property management (MQTT 5.0)
// ============================================================

nl_mqtt_property_t* nl_mqtt_property_create(int type, int int_val,
                                             const char* str_val) {
    nl_mqtt_property_t* prop = (nl_mqtt_property_t*)calloc(1, sizeof(*prop));
    if (!prop) return NULL;
    prop->type = type;
    prop->int_value = int_val;
    if (str_val) {
        prop->str_value = strdup(str_val);
        prop->str_len = strlen(str_val);
    }
    return prop;
}

void nl_mqtt_property_destroy(nl_mqtt_property_t* prop) {
    if (!prop) return;
    free((void*)prop->str_value);
    free(prop);
}

void nl_mqtt_property_list_destroy(nl_mqtt_property_t* props) {
    nl_mqtt_property_t* p = props;
    while (p) {
        nl_mqtt_property_t* n = p->next;
        nl_mqtt_property_destroy(p);
        p = n;
    }
}

int nl_mqtt_property_add_int(nl_mqtt_property_t** head, int type, int val) {
    nl_mqtt_property_t* prop = nl_mqtt_property_create(type, val, NULL);
    if (!prop) return -1;
    prop->next = *head;
    *head = prop;
    return 0;
}

int nl_mqtt_property_add_str(nl_mqtt_property_t** head, int type,
                              const char* val, size_t len) {
    (void)len;
    nl_mqtt_property_t* prop = nl_mqtt_property_create(type, 0, val);
    if (!prop) return -1;
    prop->next = *head;
    *head = prop;
    return 0;
}

// ============================================================
// Reason code string
// ============================================================

const char* nl_mqtt_rc_string(int return_code) {
    switch (return_code) {
        case 0:                                          return "Success";
        case 1:                                          return "Granted QoS 1";
        case 2:                                          return "Granted QoS 2";
        case 4:                                          return "Disconnect with Will Message";
        case 16:                                         return "No Matching Subscribers";
        case 17:                                         return "No Subscription Existed";
        case 24:                                         return "Continue Authentication";
        case 25:                                         return "Reauthenticate";
        case 128:                                        return "Unspecified Error";
        case 129:                                        return "Malformed Packet";
        case 130:                                        return "Protocol Error";
        case 131:                                        return "Implementation Specific Error";
        case 132:                                        return "Unsupported Protocol Version";
        case 133:                                        return "Client Identifier Not Valid";
        case 135:                                        return "Bad Username or Password";
        case 136:                                        return "Not Authorized";
        case 137:                                        return "Server Unavailable";
        case 138:                                        return "Server Busy";
        case 139:                                        return "Banned";
        case 140:                                        return "Bad Auth Method";
        case 144:                                        return "Topic Name Invalid";
        case 149:                                        return "Packet Too Large";
        case 151:                                        return "Quota Exceeded";
        case 153:                                        return "Payload Format Invalid";
        case 154:                                        return "Retain Not Supported";
        case 155:                                        return "Topic Alias Invalid";
        case 156:                                        return "Topic Alias Required";
        case 157:                                        return "Invalid State";
        case 159:                                        return "Network Connection Closed";
        case 160:                                        return "Invalid Protocol Version";
        case 161:                                        return "Bad Client ID";
        case 163:                                        return "Will Message Invalid";
        case 164:                                        return "Will Message Too Large";
        case 165:                                        return "Identifier Removed";
        case 166:                                        return "Rate Exceeded";
        case 235:                                        return "Connect Auth";
        default:                                       return "Unknown Return Code";
    }
}

const char* nl_mqtt_msg_type_string(nl_mqtt_msg_type_t type) {
    switch (type) {
        case NL_MQTT_CONNECT:     return "CONNECT";
        case NL_MQTT_CONNACK:     return "CONNACK";
        case NL_MQTT_PUBLISH:     return "PUBLISH";
        case NL_MQTT_PUBACK:      return "PUBACK";
        case NL_MQTT_PUBREC:      return "PUBREC";
        case NL_MQTT_PUBREL:      return "PUBREL";
        case NL_MQTT_PUBCOMP:     return "PUBCOMP";
        case NL_MQTT_SUBSCRIBE:   return "SUBSCRIBE";
        case NL_MQTT_SUBACK:      return "SUBACK";
        case NL_MQTT_UNSUBSCRIBE: return "UNSUBSCRIBE";
        case NL_MQTT_UNSUBACK:    return "UNSUBACK";
        case NL_MQTT_PINGREQ:     return "PINGREQ";
        case NL_MQTT_PINGRESP:    return "PINGRESP";
        case NL_MQTT_DISCONNECT:  return "DISCONNECT";
        default:                  return "UNKNOWN";
    }
}

// ============================================================
// MQTT 5.0 Property Encoding/Decoding
// ============================================================

// MQTT 5.0 属性线格式类别(按规范)
typedef enum {
    NL_PK_BYTE, NL_PK_U16, NL_PK_U32, NL_PK_VARINT,
    NL_PK_STR, NL_PK_BIN, NL_PK_STRPAIR, NL_PK_UNKNOWN
} nl_mqtt_prop_kind_t;

static nl_mqtt_prop_kind_t nl_mqtt_prop_kind(int type) {
    switch (type) {
        case NL_MQTT_PROP_PAYLOAD_FORMAT_INDICATOR:  return NL_PK_BYTE;
        case NL_MQTT_PROP_MESSAGE_EXPIRY_INTERVAL:   return NL_PK_U32;
        case NL_MQTT_PROP_CONTENT_TYPE:              return NL_PK_STR;
        case NL_MQTT_PROP_RESPONSE_TOPIC:            return NL_PK_STR;
        case NL_MQTT_PROP_CORRELATION_DATA:          return NL_PK_BIN;
        case NL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER:   return NL_PK_VARINT;
        case NL_MQTT_PROP_SESSION_EXPIRY_INTERVAL:   return NL_PK_U32;
        case NL_MQTT_PROP_ASSIGNED_CLIENT_ID:        return NL_PK_STR;
        case NL_MQTT_PROP_SERVER_KEEP_ALIVE:         return NL_PK_U16;
        case NL_MQTT_PROP_AUTH_METHOD:               return NL_PK_STR;
        case NL_MQTT_PROP_AUTH_DATA:                 return NL_PK_BIN;
        case NL_MQTT_PROP_REQUEST_PROBLEM_INFO:      return NL_PK_BYTE;
        case NL_MQTT_PROP_WILL_DELAY_INTERVAL:       return NL_PK_U32;
        case NL_MQTT_PROP_REQUEST_RESPONSE_INFO:     return NL_PK_BYTE;
        case NL_MQTT_PROP_RESPONSE_INFO:             return NL_PK_STR;
        case NL_MQTT_PROP_SERVER_REFERENCE:          return NL_PK_STR;
        case NL_MQTT_PROP_REASON_STRING:             return NL_PK_STR;
        case NL_MQTT_PROP_RECEIVE_MAXIMUM:           return NL_PK_U16;
        case NL_MQTT_PROP_TOPIC_ALIAS_MAX:           return NL_PK_U16;
        case NL_MQTT_PROP_TOPIC_ALIAS:               return NL_PK_U16;
        case NL_MQTT_PROP_MAX_QOS:                   return NL_PK_BYTE;
        case NL_MQTT_PROP_RETAIN_AVAILABLE:          return NL_PK_BYTE;
        case NL_MQTT_PROP_USER_PROPERTY:             return NL_PK_STRPAIR;
        case NL_MQTT_PROP_MAX_PACKET_SIZE:           return NL_PK_U32;
        case NL_MQTT_PROP_WILDCARD_SUB_AVAILABLE:    return NL_PK_BYTE;
        case NL_MQTT_PROP_SUB_IDENTIFIERS_AVAILABLE: return NL_PK_BYTE;
        case NL_MQTT_PROP_SHARED_SUB_AVAILABLE:      return NL_PK_BYTE;
        default:                                     return NL_PK_UNKNOWN;
    }
}

static int nl_mqtt_encode_properties(char* buf, size_t len, size_t* offset,
                                      nl_mqtt_property_t* props) {
    if (!props) return 0;
    size_t start = *offset;
    for (nl_mqtt_property_t* p = props; p; p = p->next) {
        nl_mqtt_prop_kind_t k = nl_mqtt_prop_kind(p->type);
        if (k == NL_PK_UNKNOWN) continue;
        if (*offset + 1 > len) return -1;
        buf[(*offset)++] = (char)(p->type & 0xFF);   // 标识符(可变字节整数；本实现均 <128)

        switch (k) {
            case NL_PK_BYTE:
                if (*offset + 1 > len) return -1;
                buf[(*offset)++] = (char)(p->int_value & 0xFF);
                break;
            case NL_PK_U16:
                if (*offset + 2 > len) return -1;
                buf[(*offset)++] = (char)((p->int_value >> 8) & 0xFF);
                buf[(*offset)++] = (char)(p->int_value & 0xFF);
                break;
            case NL_PK_U32:
                if (*offset + 4 > len) return -1;
                buf[(*offset)++] = (char)((p->int_value >> 24) & 0xFF);
                buf[(*offset)++] = (char)((p->int_value >> 16) & 0xFF);
                buf[(*offset)++] = (char)((p->int_value >> 8) & 0xFF);
                buf[(*offset)++] = (char)(p->int_value & 0xFF);
                break;
            case NL_PK_VARINT: {
                int n = nl_mqtt_encode_length(buf + *offset, len - *offset,
                                              (size_t)p->int_value);
                if (n < 0) return -1;
                *offset += (size_t)n;
                break;
            }
            case NL_PK_STR:
            case NL_PK_BIN:
                if (*offset + 2 + p->str_len > len) return -1;
                buf[(*offset)++] = (char)((p->str_len >> 8) & 0xFF);
                buf[(*offset)++] = (char)(p->str_len & 0xFF);
                if (p->str_len && p->str_value)
                    memcpy(buf + *offset, p->str_value, p->str_len);
                *offset += p->str_len;
                break;
            case NL_PK_STRPAIR: {
                // 约定：int_value>0 视为键长度，str_value 为 "key=value"；否则整串为键
                size_t kl = p->int_value > 0 ? (size_t)p->int_value : p->str_len;
                size_t vl = p->str_len > kl + 1 ? p->str_len - kl - 1 : 0;
                if (*offset + 2 + kl + 2 + vl > len) return -1;
                buf[(*offset)++] = (char)((kl >> 8) & 0xFF);
                buf[(*offset)++] = (char)(kl & 0xFF);
                if (kl && p->str_value) memcpy(buf + *offset, p->str_value, kl);
                *offset += kl;
                buf[(*offset)++] = (char)((vl >> 8) & 0xFF);
                buf[(*offset)++] = (char)(vl & 0xFF);
                if (vl && p->str_value) memcpy(buf + *offset, p->str_value + kl + 1, vl);
                *offset += vl;
                break;
            }
            default: break;
        }
    }
    size_t prop_len = *offset - start;
    if (start >= 4) {
        nl_mqtt_encode_length(buf + start - 4, 4, prop_len);
    }
    return 0;
}

static int nl_mqtt_decode_property(const char* buf, size_t len, size_t* offset,
                                    nl_mqtt_property_t** out_props) {
    // 属性标识符为可变字节整数(全部标准属性 < 128，即 1 字节)
    size_t idv = 0;
    if (nl_mqtt_read_varint(buf, len, offset, &idv) != 0) return -1;
    int type = (int)idv;

    nl_mqtt_property_t* prop = (nl_mqtt_property_t*)calloc(1, sizeof(*prop));
    if (!prop) return -1;
    prop->type = type;

    switch (nl_mqtt_prop_kind(type)) {
        case NL_PK_BYTE:
            if (*offset + 1 > len) { free(prop); return -1; }
            prop->int_value = (uint8_t)buf[(*offset)++];
            break;
        case NL_PK_U16:
            if (*offset + 2 > len) { free(prop); return -1; }
            prop->int_value = ((uint8_t)buf[*offset] << 8) | (uint8_t)buf[*offset + 1];
            *offset += 2;
            break;
        case NL_PK_U32:
            if (*offset + 4 > len) { free(prop); return -1; }
            prop->int_value = ((uint8_t)buf[*offset] << 24) |
                              ((uint8_t)buf[*offset + 1] << 16) |
                              ((uint8_t)buf[*offset + 2] << 8) |
                              ((uint8_t)buf[*offset + 3]);
            *offset += 4;
            break;
        case NL_PK_VARINT: {
            size_t v = 0;
            if (nl_mqtt_read_varint(buf, len, offset, &v) != 0) { free(prop); return -1; }
            prop->int_value = (int)v;
            break;
        }
        case NL_PK_STR:
        case NL_PK_BIN: {
            if (*offset + 2 > len) { free(prop); return -1; }
            size_t sl = ((uint8_t)buf[*offset] << 8) | (uint8_t)buf[*offset + 1];
            *offset += 2;
            if (*offset + sl > len) { free(prop); return -1; }
            prop->str_value = (char*)malloc(sl + 1);
            if (!prop->str_value) { free(prop); return -1; }
            if (sl) memcpy(prop->str_value, &buf[*offset], sl);
            prop->str_value[sl] = '\0';
            prop->str_len = sl;
            *offset += sl;
            break;
        }
        case NL_PK_STRPAIR: {
            if (*offset + 2 > len) { free(prop); return -1; }
            size_t kl = ((uint8_t)buf[*offset] << 8) | (uint8_t)buf[*offset + 1];
            *offset += 2;
            if (*offset + kl + 2 > len) { free(prop); return -1; }
            size_t vl = ((uint8_t)buf[*offset + kl] << 8) | (uint8_t)buf[*offset + kl + 1];
            if (*offset + kl + 2 + vl > len) { free(prop); return -1; }
            size_t total = kl + 1 + vl;
            prop->str_value = (char*)malloc(total + 1);
            if (!prop->str_value) { free(prop); return -1; }
            if (kl) memcpy(prop->str_value, &buf[*offset], kl);
            prop->str_value[kl] = '=';
            if (vl) memcpy(prop->str_value + kl + 1, &buf[*offset + kl + 2], vl);
            prop->str_value[total] = '\0';
            prop->str_len = total;
            prop->int_value = (int)kl;
            *offset += kl + 2 + vl;
            break;
        }
        default:
            // 未知属性：类型已消费，负载长度未知，保守跳过(与服务器端一致)
            free(prop);
            return 0;
    }

    prop->next = *out_props;
    *out_props = prop;
    return 0;
}

static int nl_mqtt_decode_properties(const char* buf, size_t len, size_t* offset,
                                      nl_mqtt_property_t** out_props) {
    if (*out_props) nl_mqtt_property_list_destroy(*out_props);
    *out_props = NULL;

    if (*offset >= len) return 0;
    size_t prop_len;
    if (nl_mqtt_read_varint(buf, len, offset, &prop_len) != 0) return -1;

    size_t end = *offset + prop_len;
    while (*offset < end) {
        if (nl_mqtt_decode_property(buf, len, offset, out_props) != 0) return -1;
    }
    return 0;
}

// ============================================================
// Packet construction helpers
// ============================================================

static int nl_mqtt_build_connect_packet(nl_mqtt_client_t* c,
                                         const nl_mqtt_connect_opts_t* opts) {
    size_t pos = 0;
    size_t cap = 512;
    char* pkt = (char*)malloc(cap);
    if (!pkt) return -1;

    // 固定头：CONNECT (0x10)
    pkt[pos++] = 0x10;

    // 预留 4 字节用于剩余长度前缀（可变字节编码最多 4 字节），正文随后写入
    pos += 4;
    size_t var_start = pos; // 正文（变长头 + 负载）起点

    // Protocol name：MQTT 3.1.1 与 5.0 均使用 "MQTT"
    // （"MQIsdp" 是 MQTT 3.1 专用协议名，与其协议级别 4 组合属于非法报文）
    const char* proto_name = "MQTT";
    size_t proto_len = strlen(proto_name);
    if (pos + 2 + proto_len > cap) {
        char* nb = (char*)realloc(pkt, cap * 2);
        if (!nb) { free(pkt); return -1; }
        pkt = nb; cap *= 2;
    }
    pkt[pos++] = (char)((proto_len >> 8) & 0xFF);
    pkt[pos++] = (char)(proto_len & 0xFF);
    memcpy(pkt + pos, proto_name, proto_len);
    pos += proto_len;

    // Protocol level
    uint8_t proto_level = (c->protocol_version == 5) ? 5 : 4;
    pkt[pos++] = proto_level;

    // Connect flags
    uint8_t flags = 0;
    if (opts && opts->username) flags |= 0x80;
    if (opts && opts->password) flags |= 0x40;
    if (opts && opts->will_enabled) flags |= 0x04;
    if (opts && opts->will_qos) flags |= ((opts->will_qos & 0x03) << 3);
    if (opts && opts->will_retain) flags |= 0x20;
    // Connect flags 的 bit1 = Clean Session(3.1.1) / Clean Start(5.0)，两者极性相同：
    // 请求“清理会话/全新开始”时置位(默认置位)，请求持久会话时清零。
    if (!opts || opts->clean_session) flags |= 0x02;
    pkt[pos++] = flags;

    // Keep alive
    uint16_t ka = opts ? opts->keep_alive : 60;
    pkt[pos++] = (char)((ka >> 8) & 0xFF);
    pkt[pos++] = (char)(ka & 0xFF);

    // MQTT 5.0 属性区：属性长度前缀为必选字段，先预留 4 字节再回收多余空间
    if (c->protocol_version == 5) {
        if (pos + 4 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
        size_t props_start = pos;
        pos += 4;                 // 预留属性长度前缀
        size_t props_begin = pos; // 属性正文起点
        // 先写属性链表（其长度前缀会写入预留区），再追加常用属性
        if (opts && opts->properties) {
            if (nl_mqtt_encode_properties(pkt, cap, &pos, opts->properties) < 0) { free(pkt); return -1; }
        }
        if (opts) {
            if (opts->session_expiry_interval != 0) {
                if (pos + 6 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x11; pkt[pos++] = 0x00;
                pkt[pos++] = (char)((opts->session_expiry_interval >> 24) & 0xFF);
                pkt[pos++] = (char)((opts->session_expiry_interval >> 16) & 0xFF);
                pkt[pos++] = (char)((opts->session_expiry_interval >> 8) & 0xFF);
                pkt[pos++] = (char)(opts->session_expiry_interval & 0xFF);
            }
            if (opts->max_packet_size != 0) {
                if (pos + 6 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x27; pkt[pos++] = 0x00;
                pkt[pos++] = (char)((opts->max_packet_size >> 24) & 0xFF);
                pkt[pos++] = (char)((opts->max_packet_size >> 16) & 0xFF);
                pkt[pos++] = (char)((opts->max_packet_size >> 8) & 0xFF);
                pkt[pos++] = (char)(opts->max_packet_size & 0xFF);
            }
            if (opts->topic_alias_max != 0) {
                if (pos + 4 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x22; pkt[pos++] = 0x00;
                pkt[pos++] = (char)((opts->topic_alias_max >> 8) & 0xFF);
                pkt[pos++] = (char)(opts->topic_alias_max & 0xFF);
            }
            if (opts->request_problem_info) {
                if (pos + 2 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x19; pkt[pos++] = 0x00;
            }
            if (opts->request_response_info) {
                if (pos + 2 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x1C; pkt[pos++] = 0x00;
            }
        }
        // 用实际属性总长度回填前缀，并回收未使用的预留字节
        pos = nl_mqtt_reclaim_reserved(pkt, props_start, props_begin, pos - props_begin);
    }

    // Client ID
    const char* cid = opts ? opts->client_id : NULL;
    if (!cid) cid = "netleaf_mqtt";
    size_t cid_len = strlen(cid);
    if (pos + 2 + cid_len > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
    pkt[pos++] = (char)((cid_len >> 8) & 0xFF);
    pkt[pos++] = (char)(cid_len & 0xFF);
    memcpy(pkt + pos, cid, cid_len);
    pos += cid_len;

    // Will message (if enabled)
    if (opts && opts->will_enabled && opts->will_topic) {
        size_t will_topic_len = strlen(opts->will_topic);
        if (pos + 2 + will_topic_len > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
        pkt[pos++] = (char)((will_topic_len >> 8) & 0xFF);
        pkt[pos++] = (char)(will_topic_len & 0xFF);
        memcpy(pkt + pos, opts->will_topic, will_topic_len);
        pos += will_topic_len;

        size_t will_msg_len = opts->will_msg_len ? opts->will_msg_len : strlen(opts->will_msg ? opts->will_msg : "");
        const char* wmsg = opts->will_msg ? opts->will_msg : "";
        if (pos + 2 + will_msg_len > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
        pkt[pos++] = (char)((will_msg_len >> 8) & 0xFF);
        pkt[pos++] = (char)(will_msg_len & 0xFF);
        memcpy(pkt + pos, wmsg, will_msg_len);
        pos += will_msg_len;
    }

    // Username (if provided)
    if (opts && opts->username) {
        size_t ulen = strlen(opts->username);
        if (pos + 2 + ulen > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
        pkt[pos++] = (char)((ulen >> 8) & 0xFF);
        pkt[pos++] = (char)(ulen & 0xFF);
        memcpy(pkt + pos, opts->username, ulen);
        pos += ulen;
    }

    // Password (if provided)
    if (opts && opts->password) {
        size_t plen = strlen(opts->password);
        if (pos + 2 + plen > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
        pkt[pos++] = (char)((plen >> 8) & 0xFF);
        pkt[pos++] = (char)(plen & 0xFF);
        memcpy(pkt + pos, opts->password, plen);
        pos += plen;
    }

    // 回填剩余长度（= 固定头之后的内容长度），并回收预留的多余字节
    size_t var_len = pos - var_start;
    pos = nl_mqtt_reclaim_reserved(pkt, var_start - 4, var_start, var_len);

    // Queue to output buffer
    if (nl_mqtt_expand_buffer(&c->out_buf, &c->out_buf_len, &c->out_buf_cap,
                               c->out_buf_len + pos) == 0) {
        free(pkt);
        return -1;
    }
    memcpy(c->out_buf + c->out_buf_len, pkt, pos);
    c->out_buf_len += pos;
    free(pkt);
    return 0;
}

static int nl_mqtt_build_publish_packet(nl_mqtt_client_t* c,
                                         const char* topic,
                                         const void* payload, size_t payload_len,
                                         const nl_mqtt_pub_opts_t* opts) {
    size_t pos = 0;
    size_t cap = 256;
    char* pkt = (char*)malloc(cap);
    if (!pkt) return -1;

    // Fixed header: PUBLISH (0x30) | DUP, QoS, Retain
    uint8_t flags = 0x30;
    if (opts) {
        if (opts->dup) flags |= 0x08;
        flags |= (opts->qos & 0x03) << 1;
        if (opts->retain) flags |= 0x01;
    }
    pkt[pos++] = flags;

    // 预留 4 字节用于剩余长度前缀，正文随后写入
    pos += 4;
    size_t var_start = pos;

    // Topic
    size_t topic_len = strlen(topic);
    if (pos + 2 + topic_len > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
    pkt[pos++] = (char)((topic_len >> 8) & 0xFF);
    pkt[pos++] = (char)(topic_len & 0xFF);
    memcpy(pkt + pos, topic, topic_len);
    pos += topic_len;

    // Packet ID for QoS 1 and 2
    uint16_t packet_id = 0;
    if ((opts && opts->qos > 0) || (c->protocol_version == 5)) {
        packet_id = nl_mqtt_get_next_packet_id(c);
        if (pos + 2 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
        pkt[pos++] = (char)((packet_id >> 8) & 0xFF);
        pkt[pos++] = (char)(packet_id & 0xFF);
    }

    // MQTT 5.0 属性区：属性长度前缀为必选字段，先预留 4 字节再回收多余空间
    if (c->protocol_version == 5) {
        if (pos + 4 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
        size_t props_start = pos;
        pos += 4;
        size_t props_begin = pos;
        if (opts && opts->properties) {
            if (nl_mqtt_encode_properties(pkt, cap, &pos, opts->properties) < 0) { free(pkt); return -1; }
        }
        if (opts) {
            if (opts->message_expiry_interval != 0) {
                if (pos + 6 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x02; pkt[pos++] = 0x00;
                pkt[pos++] = (char)((opts->message_expiry_interval >> 24) & 0xFF);
                pkt[pos++] = (char)((opts->message_expiry_interval >> 16) & 0xFF);
                pkt[pos++] = (char)((opts->message_expiry_interval >> 8) & 0xFF);
                pkt[pos++] = (char)(opts->message_expiry_interval & 0xFF);
            }
            if (opts->payload_format_indicator >= 0) {
                if (pos + 3 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x01; pkt[pos++] = 0x00;
                pkt[pos++] = (char)(opts->payload_format_indicator & 0xFF);
            }
            if (opts->content_type) {
                size_t cl = strlen(opts->content_type);
                if (pos + 4 + cl > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x03; pkt[pos++] = 0x00;
                pkt[pos++] = (char)((cl >> 8) & 0xFF); pkt[pos++] = (char)(cl & 0xFF);
                memcpy(pkt + pos, opts->content_type, cl); pos += cl;
            }
            if (opts->response_topic) {
                size_t rl = strlen(opts->response_topic);
                if (pos + 4 + rl > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x08; pkt[pos++] = 0x00;
                pkt[pos++] = (char)((rl >> 8) & 0xFF); pkt[pos++] = (char)(rl & 0xFF);
                memcpy(pkt + pos, opts->response_topic, rl); pos += rl;
            }
            if (opts->correlation_data) {
                size_t cdlen = opts->correlation_data_len ? opts->correlation_data_len : strlen(opts->correlation_data);
                if (pos + 4 + cdlen > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x09; pkt[pos++] = 0x00;
                pkt[pos++] = (char)((cdlen >> 8) & 0xFF); pkt[pos++] = (char)(cdlen & 0xFF);
                memcpy(pkt + pos, opts->correlation_data, cdlen); pos += cdlen;
            }
            if (opts->topic_alias != 0) {
                if (pos + 4 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
                pkt[pos++] = 0x23; pkt[pos++] = 0x00;
                pkt[pos++] = (char)((opts->topic_alias >> 8) & 0xFF);
                pkt[pos++] = (char)(opts->topic_alias & 0xFF);
            }
        }
        // 用实际属性总长度回填前缀，并回收未使用的预留字节
        pos = nl_mqtt_reclaim_reserved(pkt, props_start, props_begin, pos - props_begin);
    }

    // Payload
    if (payload && payload_len > 0) {
        if (pos + payload_len > cap) {
            size_t new_cap = pos + payload_len;
            char* nb = (char*)realloc(pkt, new_cap);
            if (!nb) { free(pkt); return -1; }
            pkt = nb; cap = new_cap;
        }
        memcpy(pkt + pos, payload, payload_len);
        pos += payload_len;
    }

    // 回填剩余长度（= 固定头之后的内容长度），并回收预留的多余字节
    size_t var_len = pos - var_start;
    pos = nl_mqtt_reclaim_reserved(pkt, var_start - 4, var_start, var_len);

    // Queue to output buffer
    if (nl_mqtt_expand_buffer(&c->out_buf, &c->out_buf_len, &c->out_buf_cap,
                               c->out_buf_len + pos) == 0) {
        free(pkt);
        return -1;
    }
    memcpy(c->out_buf + c->out_buf_len, pkt, pos);
    c->out_buf_len += pos;
    free(pkt);

    // Queue pending for QoS 1/2
    if (packet_id > 0) {
        nl_mqtt_msg_type_t expected = (opts && opts->qos == 1) ? NL_MQTT_PUBACK
                        : (opts && opts->qos == 2) ? NL_MQTT_PUBREC : NL_MQTT_PUBACK;
        nl_mqtt_queue_pending(c, packet_id, expected);
    }
    return packet_id;
}

static int nl_mqtt_build_subscribe_packet(nl_mqtt_client_t* c,
                                           const char* topic, int qos) {
    size_t pos = 0;
    size_t cap = 256;
    char* pkt = (char*)malloc(cap);
    if (!pkt) return -1;

    pkt[pos++] = (uint8_t)0x82; // SUBSCRIBE

    // 预留 4 字节用于剩余长度前缀
    pos += 4;
    size_t var_start = pos;

    // Packet ID
    uint16_t packet_id = nl_mqtt_get_next_packet_id(c);
    pkt[pos++] = (char)((packet_id >> 8) & 0xFF);
    pkt[pos++] = (char)(packet_id & 0xFF);

    // MQTT 5.0 属性区：属性长度前缀为必选字段；当前无订阅属性，写入 0 长度
    if (c->protocol_version == 5) {
        if (pos + 1 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
        pkt[pos++] = 0x00;
    }

    // Topic
    size_t topic_len = strlen(topic);
    if (pos + 2 + topic_len + 1 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
    pkt[pos++] = (char)((topic_len >> 8) & 0xFF);
    pkt[pos++] = (char)(topic_len & 0xFF);
    memcpy(pkt + pos, topic, topic_len);
    pos += topic_len;
    pkt[pos++] = (char)(qos & 0x03); // Requested QoS

    size_t var_len = pos - var_start;
    pos = nl_mqtt_reclaim_reserved(pkt, var_start - 4, var_start, var_len);

    if (nl_mqtt_expand_buffer(&c->out_buf, &c->out_buf_len, &c->out_buf_cap,
                               c->out_buf_len + pos) == 0) { free(pkt); return -1; }
    memcpy(c->out_buf + c->out_buf_len, pkt, pos);
    c->out_buf_len += pos;
    free(pkt);

    nl_mqtt_queue_pending(c, packet_id, NL_MQTT_SUBACK);
    return packet_id;
}

static int nl_mqtt_build_unsubscribe_packet(nl_mqtt_client_t* c, const char* topic) {
    size_t pos = 0;
    size_t cap = 256;
    char* pkt = (char*)malloc(cap);
    if (!pkt) return -1;

    pkt[pos++] = (uint8_t)0xA2; // UNSUBSCRIBE

    // 预留 4 字节用于剩余长度前缀
    pos += 4;
    size_t var_start = pos;

    uint16_t packet_id = nl_mqtt_get_next_packet_id(c);
    pkt[pos++] = (char)((packet_id >> 8) & 0xFF);
    pkt[pos++] = (char)(packet_id & 0xFF);

    // MQTT 5.0 属性区：属性长度前缀为必选字段；当前无属性，写入 0 长度
    if (c->protocol_version == 5) {
        if (pos + 1 > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
        pkt[pos++] = 0x00;
    }

    size_t topic_len = strlen(topic);
    if (pos + 2 + topic_len > cap) { char* nb = realloc(pkt, cap * 2); if (!nb) { free(pkt); return -1; } pkt = nb; cap *= 2; }
    pkt[pos++] = (char)((topic_len >> 8) & 0xFF);
    pkt[pos++] = (char)(topic_len & 0xFF);
    memcpy(pkt + pos, topic, topic_len);
    pos += topic_len;

    size_t var_len = pos - var_start;
    pos = nl_mqtt_reclaim_reserved(pkt, var_start - 4, var_start, var_len);

    if (nl_mqtt_expand_buffer(&c->out_buf, &c->out_buf_len, &c->out_buf_cap,
                               c->out_buf_len + pos) == 0) { free(pkt); return -1; }
    memcpy(c->out_buf + c->out_buf_len, pkt, pos);
    c->out_buf_len += pos;
    free(pkt);

    nl_mqtt_queue_pending(c, packet_id, NL_MQTT_UNSUBACK);
    return packet_id;
}

static int nl_mqtt_build_simple_packet(nl_mqtt_client_t* c, uint8_t type) {
    char pkt[2] = { type, 0 };
    if (nl_mqtt_expand_buffer(&c->out_buf, &c->out_buf_len, &c->out_buf_cap,
                               c->out_buf_len + 2) == 0) return -1;
    memcpy(c->out_buf + c->out_buf_len, pkt, 2);
    c->out_buf_len += 2;
    return 0;
}

// ============================================================
// Public API: Client creation/destruction
// ============================================================

nl_mqtt_client_t* nl_mqtt_create(void) {
    nl_mqtt_init_sock();
    nl_mqtt_client_t* c = (nl_mqtt_client_t*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->sock = -1;
    c->status = NL_MQTT_DISCONNECTED;
    c->protocol_version = NL_MQTT_PROTOCOL_V3_1_1;
    c->keep_alive = 60;
    c->tls_enabled = 0;
    c->tls_insecure = 0;
    c->clean_session = 1;
    c->next_packet_id = 1;
    c->auto_reconnect = 0;
    c->last_rc = 0;
#ifdef NL_MQTT_TLS_ENABLE
    c->tls_ctx = nl_mqtt_tls_create();
#endif
    return c;
}

void nl_mqtt_destroy(nl_mqtt_client_t* client) {
    if (!client) return;
    nl_mqtt_disconnect(client);
    nl_mqtt_free_pending(client);
    nl_mqtt_free_subscriptions(client);
    free(client->host);
    free(client->client_id);
    free(client->username);
    free(client->password);
    free(client->will_topic);
    free(client->will_msg);
    free(client->out_buf);
    free(client->recv_buf);
#ifdef NL_MQTT_TLS_ENABLE
    if (client->tls_ctx) {
        nl_mqtt_tls_destroy(client->tls_ctx);
    }
#endif
    free(client);
}

// ============================================================
// Public API: Connection
// ============================================================

int nl_mqtt_connect(nl_mqtt_client_t* client,
                     const char* host, uint16_t port,
                     const nl_mqtt_connect_opts_t* opts,
                     nl_mqtt_connect_callback_t on_connect) {
    if (!client || !host) return -1;

    nl_mqtt_disconnect(client);

    client->host = strdup(host);
    if (!client->host) return -1;
    client->port = port;
    client->on_connect = on_connect;
    client->status = NL_MQTT_CONNECTING;

    // Copy options
    if (opts) {
        if (opts->client_id) {
            client->client_id = strdup(opts->client_id);
            if (!client->client_id) { free(client->host); client->host = NULL; client->status = NL_MQTT_DISCONNECTED; return -1; }
        }
        if (opts->username) {
            client->username = strdup(opts->username);
            if (!client->username) { free(client->host); client->host = NULL; free(client->client_id); client->client_id = NULL; client->status = NL_MQTT_DISCONNECTED; return -1; }
        }
        if (opts->password) {
            client->password = strdup(opts->password);
            if (!client->password) { free(client->host); client->host = NULL; free(client->client_id); client->client_id = NULL; free(client->username); client->username = NULL; client->status = NL_MQTT_DISCONNECTED; return -1; }
        }
        if (opts->will_enabled && opts->will_topic) {
            client->will_enabled = 1;
            client->will_topic = strdup(opts->will_topic);
            if (opts->will_msg) {
                client->will_msg = strndup(opts->will_msg, opts->will_msg_len ? opts->will_msg_len : strlen(opts->will_msg));
                client->will_msg_len = client->will_msg ? strlen(client->will_msg) : 0;
            }
            client->will_qos = opts->will_qos;
            client->will_retain = opts->will_retain;
        }
        if (opts->keep_alive > 0) client->keep_alive = opts->keep_alive;
        client->clean_session = opts->clean_session;
    }

    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        client->status = NL_MQTT_DISCONNECTED;
        return -1;
    }
    client->sock = sock;

    // Connect
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket_close(sock);
        client->sock = -1;
        client->status = NL_MQTT_DISCONNECTED;
        return -1;
    }

#ifdef NL_MQTT_TLS_ENABLE
    if (client->tls_enabled && client->tls_ctx) {
        /* 按连接目标主机设置 SNI 与证书主机名校验 */
        if (nl_mqtt_tls_set_hostname(client->tls_ctx, client->host) != NL_MQTT_TLS_OK) {
            closesocket_close(sock);
            client->sock = -1;
            client->status = NL_MQTT_DISCONNECTED;
            return -1;
        }
        int ret = nl_mqtt_tls_handshake(client->tls_ctx, sock);
        if (ret != NL_MQTT_TLS_OK) {
            closesocket_close(sock);
            client->sock = -1;
            client->status = NL_MQTT_DISCONNECTED;
            return -1;
        }
    }
#endif

    // Build and queue CONNECT packet
    if (nl_mqtt_build_connect_packet(client, opts) < 0) {
        closesocket_close(sock);
        client->sock = -1;
        client->status = NL_MQTT_DISCONNECTED;
        return -1;
    }

    client->last_ping = time(NULL);
    return 0;
}

int nl_mqtt_disconnect(nl_mqtt_client_t* client) {
    if (!client) return -1;
    if (client->sock >= 0) {
#ifdef NL_MQTT_TLS_ENABLE
        if (client->tls_enabled && client->tls_ctx) {
            nl_mqtt_tls_close(client->tls_ctx);
        }
#endif
        closesocket_close(client->sock);
        client->sock = -1;
    }
    client->status = NL_MQTT_DISCONNECTED;
    client->out_buf_len = 0;
    nl_mqtt_free_pending(client);

    /* 统一释放连接期字段与订阅链表并置 NULL，
       避免 nl_mqtt_connect() 覆盖同一指针时发生重连内存泄漏，
       同时保证 nl_mqtt_destroy() 重复释放是安全的（free(NULL)）。 */
    free(client->host);       client->host = NULL;
    free(client->client_id);  client->client_id = NULL;
    free(client->username);   client->username = NULL;
    free(client->password);   client->password = NULL;
    free(client->will_topic); client->will_topic = NULL;
    free(client->will_msg);   client->will_msg = NULL;
    client->will_enabled = 0;
    client->will_msg_len = 0;
    nl_mqtt_free_subscriptions(client);
    return 0;
}

nl_mqtt_status_t nl_mqtt_get_status(const nl_mqtt_client_t* client) {
    return client ? client->status : NL_MQTT_DISCONNECTED;
}

// ============================================================
// Public API: Publish / Subscribe
// ============================================================

int nl_mqtt_publish(nl_mqtt_client_t* client,
                     const char* topic,
                     const void* payload, size_t payload_len,
                     const nl_mqtt_pub_opts_t* opts) {
    if (!client || !topic || client->status != NL_MQTT_CONNECTED) return -1;
    return nl_mqtt_build_publish_packet(client, topic, payload, payload_len, opts);
}

int nl_mqtt_subscribe(nl_mqtt_client_t* client,
                       const char* topic,
                       const nl_mqtt_sub_opts_t* opts,
                       nl_mqtt_message_callback_t on_message,
                       void* user_data) {
    if (!client || !topic || client->status != NL_MQTT_CONNECTED) return -1;

    int qos = opts ? opts->qos : 0;

    // Add subscription
    nl_mqtt_subscription_t* sub = (nl_mqtt_subscription_t*)calloc(1, sizeof(*sub));
    if (!sub) return -1;
    sub->topic = strdup(topic);
    if (!sub->topic) { free(sub); return -1; }
    sub->qos = qos;
    sub->callback = on_message;
    sub->user_data = user_data;
    if (opts) {
        sub->no_local = opts->no_local;
        sub->retain_as_published = opts->retain_as_published;
        sub->retain_handling = opts->retain_handling;
    }

    // Insert at head
    sub->next = client->subscriptions;
    client->subscriptions = sub;

    // Build SUBSCRIBE packet
    return nl_mqtt_build_subscribe_packet(client, topic, qos);
}

int nl_mqtt_unsubscribe(nl_mqtt_client_t* client, const char* topic) {
    if (!client || !topic || client->status != NL_MQTT_CONNECTED) return -1;

    // Remove subscription
    nl_mqtt_subscription_t* prev = NULL;
    nl_mqtt_subscription_t* cur = client->subscriptions;
    while (cur) {
        if (strcmp(cur->topic, topic) == 0) {
            if (prev) prev->next = cur->next;
            else client->subscriptions = cur->next;
            free(cur->topic);
            free(cur);
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    return nl_mqtt_build_unsubscribe_packet(client, topic);
}

// ============================================================
// QoS 2 Helpers
// ============================================================

int nl_mqtt_send_puback(nl_mqtt_client_t* client, uint16_t packet_id) {
    if (!client || client->status != NL_MQTT_CONNECTED) return -1;
    char pkt[4] = { 0x40, 0x02, (char)((packet_id >> 8) & 0xFF), (char)(packet_id & 0xFF) };
    return nl_mqtt_append_buf(client, pkt, 4);
}

int nl_mqtt_send_pubrec(nl_mqtt_client_t* client, uint16_t packet_id) {
    if (!client || client->status != NL_MQTT_CONNECTED) return -1;
    char pkt[4] = { 0x50, 0x02, (char)((packet_id >> 8) & 0xFF), (char)(packet_id & 0xFF) };
    if (nl_mqtt_expand_buffer(&client->out_buf, &client->out_buf_len, &client->out_buf_cap,
                               client->out_buf_len + 4) == 0) return -1;
    memcpy(client->out_buf + client->out_buf_len, pkt, 4);
    client->out_buf_len += 4;
    return 0;
}

int nl_mqtt_send_pubrel(nl_mqtt_client_t* client, uint16_t packet_id) {
    if (!client || client->status != NL_MQTT_CONNECTED) return -1;
    char pkt[4] = { 0x62, 0x02, (char)((packet_id >> 8) & 0xFF), (char)(packet_id & 0xFF) };
    if (nl_mqtt_expand_buffer(&client->out_buf, &client->out_buf_len, &client->out_buf_cap,
                               client->out_buf_len + 4) == 0) return -1;
    memcpy(client->out_buf + client->out_buf_len, pkt, 4);
    client->out_buf_len += 4;
    nl_mqtt_queue_pending(client, packet_id, NL_MQTT_PUBCOMP);
    return 0;
}

int nl_mqtt_send_pubcomp(nl_mqtt_client_t* client, uint16_t packet_id) {
    if (!client || client->status != NL_MQTT_CONNECTED) return -1;
    char pkt[4] = { 0x70, 0x02, (char)((packet_id >> 8) & 0xFF), (char)(packet_id & 0xFF) };
    if (nl_mqtt_expand_buffer(&client->out_buf, &client->out_buf_len, &client->out_buf_cap,
                               client->out_buf_len + 4) == 0) return -1;
    memcpy(client->out_buf + client->out_buf_len, pkt, 4);
    client->out_buf_len += 4;
    return 0;
}

// ============================================================
// Keep-alive
// ============================================================

int nl_mqtt_ping(nl_mqtt_client_t* client) {
    if (!client || client->status != NL_MQTT_CONNECTED) return -1;
    return nl_mqtt_build_simple_packet(client, 0xC0);
}

int nl_mqtt_handle_pingresp(nl_mqtt_client_t* client) {
    if (!client) return -1;
    client->last_ping = time(NULL);
    return 0;
}

// ============================================================
// TLS/SSL Support
// ============================================================

int nl_mqtt_set_tls(nl_mqtt_client_t* client, int enabled) {
    if (!client) return -1;
#ifdef NL_MQTT_TLS_ENABLE
    client->tls_enabled = enabled;
    if (enabled && !client->tls_ctx) {
        client->tls_ctx = nl_mqtt_tls_create();
        if (!client->tls_ctx) return -1;
    }
    return 0;
#else
    (void)enabled;
    return -1;
#endif
}

int nl_mqtt_set_tls_cert(nl_mqtt_client_t* client,
                          const char* ca_file,
                          const char* cert_file,
                          const char* key_file) {
    if (!client || !client->tls_enabled) return -1;
#ifdef NL_MQTT_TLS_ENABLE
    if (!client->tls_ctx) return -1;
    nl_mqtt_tls_config_t cfg = {0};
    cfg.ca_file = ca_file;
    cfg.cert_file = cert_file;
    cfg.key_file = key_file;
    cfg.verify_peer = !client->tls_insecure;
    return nl_mqtt_tls_configure(client->tls_ctx, &cfg);
#else
    (void)ca_file; (void)cert_file; (void)key_file;
    return -1;
#endif
}

int nl_mqtt_set_tls_insecure(nl_mqtt_client_t* client, int insecure) {
    if (!client) return -1;

    /* 已连接/连接中/重连中不允许变更 TLS 校验模式，避免半途更换造成握手状态不一致 */
    if (client->status == NL_MQTT_CONNECTED ||
        client->status == NL_MQTT_CONNECTING ||
        client->status == NL_MQTT_RECONNECTING) {
        return -1;
    }

    client->tls_insecure = insecure ? 1 : 0;

#ifdef NL_MQTT_TLS_ENABLE
    /* 真正把新的校验开关应用到 TLS 上下文，而不是丢弃 cfg */
    if (client->tls_ctx) {
        nl_mqtt_tls_config_t cfg = {0};
        cfg.verify_peer = !client->tls_insecure;
        if (nl_mqtt_tls_configure(client->tls_ctx, &cfg) != NL_MQTT_TLS_OK) {
            return -1;
        }
    }
#else
    (void)insecure;
#endif
    return 0;
}

// ============================================================
// Event Loop Integration
// ============================================================

static int nl_mqtt_send_output(nl_mqtt_client_t* client) {
    if (client->sock < 0 || client->out_buf_len == 0) return 0;
    int sent = 0;
#ifdef NL_MQTT_TLS_ENABLE
    if (client->tls_enabled && client->tls_ctx) {
        sent = nl_mqtt_tls_send(client->tls_ctx, client->out_buf, client->out_buf_len);
    } else
#endif
    {
        sent = send(client->sock, client->out_buf, client->out_buf_len, 0);
    }
    if (sent <= 0) return -1;
    // Shift remaining data
    if ((size_t)sent < client->out_buf_len) {
        memmove(client->out_buf, client->out_buf + sent, client->out_buf_len - sent);
    }
    client->out_buf_len -= sent;
    return 0;
}

int nl_mqtt_handle_incoming(nl_mqtt_client_t* client) {
    if (!client || client->sock < 0) return -1;

    char tmp[4096];
    int n = 0;
#ifdef NL_MQTT_TLS_ENABLE
    if (client->tls_enabled && client->tls_ctx) {
        n = nl_mqtt_tls_recv(client->tls_ctx, tmp, sizeof(tmp));
    } else
#endif
    {
        n = (int)recv(client->sock, tmp, sizeof(tmp), 0);
    }

    if (n > 0) {
        // 追加到接收累积缓冲区，支持 TCP 粘包/半包
        if (!nl_mqtt_expand_buffer(&client->recv_buf, &client->recv_len, &client->recv_cap,
                                   client->recv_len + (size_t)n)) {
            return -1;
        }
        memcpy(client->recv_buf + client->recv_len, tmp, (size_t)n);
        client->recv_len += (size_t)n;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        client->status = NL_MQTT_DISCONNECTED;
        return -1;
    }

    if (client->recv_len == 0) return 0;

    // 从累积缓冲区解析：一次可能包含多条报文，也可能只有半条
    const char* buf = client->recv_buf;
    size_t total = client->recv_len;
    size_t offset = 0;
    while (offset < total) {
        size_t pkt_begin = offset;

        // Read fixed header
        if (offset + 1 > total) break;
        uint8_t type_byte = (uint8_t)buf[offset++];
        uint8_t msg_type = type_byte >> 4;
        uint8_t flags = type_byte & 0x0F;

        // Read remaining length
        size_t remaining_len = 0;
        int shift = 0;
        int multiplier = 1;
        uint8_t encoded_byte;
        int hdr_incomplete = 0;
        for (;;) {
            if (offset >= total) { hdr_incomplete = 1; break; }
            encoded_byte = (uint8_t)buf[offset++];
            remaining_len += (size_t)(encoded_byte & 0x7F) * multiplier;
            multiplier *= 128;
            shift++;
            if (shift > 4) { client->recv_len = 0; return -1; } // 非法剩余长度
            if ((encoded_byte & 0x80) == 0) break;
        }
        if (hdr_incomplete) { offset = pkt_begin; break; }

        // 读取完剩余长度后，offset 即为本报文内容（变长头 + 负载）的起点。
        // 统一以“报文起点”为基准：pkt_end 为本报文在 buf 中的绝对结束位置，
        // 后续所有读取的长度上界都用 pkt_end，避免绝对 offset 与 remaining_len 混用。
        size_t pkt_start = offset;
        if (pkt_start + remaining_len > total) { offset = pkt_begin; break; } // 报文未收全，等待后续数据
        size_t pkt_end = pkt_start + remaining_len;

        // Dispatch based on message type
        switch (msg_type) {
            case NL_MQTT_CONNACK: {
                if (remaining_len < 2) break;
                (void)buf[offset++];                 // 跳过 session present 标志
                uint8_t rc = (uint8_t)buf[offset++]; // 连接返回码
                client->status = NL_MQTT_CONNECTED;
                client->last_ping = time(NULL);
                // 解析 MQTT 5.0 属性（属性长度前缀为必选字段）
                if (client->protocol_version == 5 && offset < pkt_end) {
                    size_t rel = 0;
                    nl_mqtt_property_t* props = NULL;
                    if (nl_mqtt_decode_properties(buf + offset, pkt_end - offset, &rel, &props) == 0) {
                        // Extract useful properties
                        nl_mqtt_property_t* p = props;
                        while (p) {
                            if (p->type == NL_MQTT_PROP_MAX_PACKET_SIZE) client->max_packet_size = p->int_value;
                            if (p->type == NL_MQTT_PROP_TOPIC_ALIAS_MAX) client->topic_alias_max = p->int_value;
                            p = p->next;
                        }
                    }
                    nl_mqtt_property_list_destroy(props);
                }
                if (client->on_connect) {
                    client->on_connect(rc, client->user_data);
                }
                offset = pkt_start + remaining_len; // CONNACK：直接跳到报文末尾
                break;
            }
            case NL_MQTT_PUBLISH: {
                size_t topic_len = 0;
                if (nl_mqtt_read_bytes_size_t(buf, pkt_end, &offset, &topic_len) < 0) break;
                if (offset + topic_len > pkt_end) break;
                char* topic = (char*)malloc(topic_len + 1);
                if (!topic) break;
                memcpy(topic, buf + offset, topic_len);
                topic[topic_len] = '\0';
                offset += topic_len;

                uint16_t packet_id = 0;
                int qos = (flags >> 1) & 0x03;
                if (qos > 0) {
                    if (nl_mqtt_read_bytes(buf, pkt_end, &offset, &packet_id) < 0) { free(topic); break; }
                }

                // 跳过 MQTT 5.0 属性（属性长度前缀为必选字段）
                if (client->protocol_version == 5 && offset < pkt_end) {
                    size_t rel = 0;
                    nl_mqtt_property_t* props = NULL;
                    if (nl_mqtt_decode_properties(buf + offset, pkt_end - offset, &rel, &props) == 0) {
                        // Process will delay, topic alias, etc.
                        offset += rel;
                    }
                    nl_mqtt_property_list_destroy(props);
                }
                if (offset > pkt_end) offset = pkt_end;

                size_t payload_len = pkt_end - offset;
                const void* payload = (const void*)(buf + offset);

                // Find matching subscription
                nl_mqtt_subscription_t* sub = client->subscriptions;
                while (sub) {
                    if (nl_mqtt_topic_matches(sub->topic, topic)) {
                        if (sub->callback) {
                            sub->callback(topic, payload, payload_len, qos, (flags & 0x01), sub->user_data);
                        }
                        break;
                    }
                    sub = sub->next;
                }
                free(topic);

                // Send acknowledgements
                if (qos == 1) nl_mqtt_send_puback(client, packet_id);
                else if (qos == 2) {
                    // Send PUBREC
                    char pubrec[4] = { 0x50, 0x02, (char)((packet_id >> 8) & 0xFF), (char)(packet_id & 0xFF) };
                    nl_mqtt_append_buf(client, pubrec, 4);
                    nl_mqtt_queue_pending(client, packet_id, NL_MQTT_PUBREL);
                }
                break;
            }
            case NL_MQTT_PUBACK: {
                if (remaining_len >= 2) {
                    uint16_t pid = 0;
                    if (nl_mqtt_read_bytes(buf, pkt_end, &offset, &pid) == 0)
                        nl_mqtt_remove_pending(client, pid, NL_MQTT_PUBACK);
                }
                break;
            }
            case NL_MQTT_PUBREC: {
                if (remaining_len >= 2) {
                    uint16_t pid = 0;
                    if (nl_mqtt_read_bytes(buf, pkt_end, &offset, &pid) == 0) {
                        nl_mqtt_remove_pending(client, pid, NL_MQTT_PUBREC);
                        // Send PUBREL
                        char pubrel[4] = { 0x62, 0x02, (char)((pid >> 8) & 0xFF), (char)(pid & 0xFF) };
                        nl_mqtt_append_buf(client, pubrel, 4);
                        nl_mqtt_queue_pending(client, pid, NL_MQTT_PUBCOMP);
                    }
                }
                break;
            }
            case NL_MQTT_PUBREL: {
                if (remaining_len >= 2) {
                    uint16_t pid = 0;
                    if (nl_mqtt_read_bytes(buf, pkt_end, &offset, &pid) == 0) {
                        nl_mqtt_remove_pending(client, pid, NL_MQTT_PUBREL);
                        // Send PUBCOMP
                        char pubcomp[4] = { 0x70, 0x02, (char)((pid >> 8) & 0xFF), (char)(pid & 0xFF) };
                        nl_mqtt_append_buf(client, pubcomp, 4);
                    }
                }
                break;
            }
            case NL_MQTT_PUBCOMP: {
                if (remaining_len >= 2) {
                    uint16_t pid = 0;
                    if (nl_mqtt_read_bytes(buf, pkt_end, &offset, &pid) == 0)
                        nl_mqtt_remove_pending(client, pid, NL_MQTT_PUBCOMP);
                }
                break;
            }
            case NL_MQTT_SUBACK: {
                if (remaining_len >= 3) {
                    uint16_t pid = 0;
                    if (nl_mqtt_read_bytes(buf, pkt_end, &offset, &pid) == 0)
                        nl_mqtt_remove_pending(client, pid, NL_MQTT_SUBACK);
                }
                break;
            }
            case NL_MQTT_UNSUBACK: {
                if (remaining_len >= 2) {
                    uint16_t pid = 0;
                    if (nl_mqtt_read_bytes(buf, pkt_end, &offset, &pid) == 0)
                        nl_mqtt_remove_pending(client, pid, NL_MQTT_UNSUBACK);
                }
                break;
            }
            case NL_MQTT_PINGRESP: {
                nl_mqtt_handle_pingresp(client);
                break;
            }
            default:
                break;
        }
        // 无论分支内部是否读取过数据，都统一推进到本报文末尾，支持一次收到多条报文
        offset = pkt_end;
    }

    // 保留未处理完的尾部数据(半包)，等待下一次 recv 补齐
    {
        size_t remain = total - offset;
        if (remain > 0 && offset > 0) {
            memmove(client->recv_buf, client->recv_buf + offset, remain);
        }
        client->recv_len = remain;
    }
    return 0;
}

int nl_mqtt_handle_outgoing(nl_mqtt_client_t* client) {
    if (!client || client->sock < 0) return -1;
    return nl_mqtt_send_output(client);
}

int nl_mqtt_maintain(nl_mqtt_client_t* client, long timeout_ms) {
    if (!client) return -1;

    // Check keep-alive
    if (client->status == NL_MQTT_CONNECTED && client->keep_alive > 0) {
        time_t now = time(NULL);
        if (difftime(now, client->last_ping) > client->keep_alive) {
            nl_mqtt_ping(client);
            nl_mqtt_send_output(client);
            client->last_ping = now;
        }
    }

    // Handle outgoing data
    if (client->sock >= 0 && client->out_buf_len > 0) {
        nl_mqtt_send_output(client);
    }

    // Handle incoming data
    if (client->sock >= 0) {
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET((fd_t)client->sock, &readfds);
        select(client->sock + 1, &readfds, NULL, NULL, &tv);
        if (FD_ISSET((fd_t)client->sock, &readfds)) {
            nl_mqtt_handle_incoming(client);
        }
    }

    return 0;
}

// ============================================================
// Subscription Management
// ============================================================

int nl_mqtt_foreach_subscription(nl_mqtt_client_t* client,
                                  nl_mqtt_foreach_sub_t callback,
                                  void* user_data) {
    if (!client || !callback) return -1;
    int count = 0;
    nl_mqtt_subscription_t* s = client->subscriptions;
    while (s) {
        if (callback(s->topic, s->qos, user_data) == 0) break;
        count++;
        s = s->next;
    }
    return count;
}

int nl_mqtt_get_subscription_count(const nl_mqtt_client_t* client) {
    if (!client) return 0;
    int count = 0;
    const nl_mqtt_subscription_t* s = client->subscriptions;
    while (s) { count++; s = s->next; }
    return count;
}

// ============================================================
// Protocol Version
// ============================================================

int nl_mqtt_get_protocol_version(const nl_mqtt_client_t* client) {
    return client ? client->protocol_version : 0;
}

int nl_mqtt_set_protocol_version(nl_mqtt_client_t* client, int version) {
    if (!client) return -1;
    if (version != NL_MQTT_PROTOCOL_V3_1_1 && version != NL_MQTT_PROTOCOL_V5) return -1;
    client->protocol_version = version;
    return 0;
}

// ============================================================
// Module Registration
// ============================================================

nl_module_info_t* nl_mqtt_get_module_info(void) {
    return &g_mqtt_module_info;
}

int nl_mqtt_is_available(void) {
    return g_mqtt_available;
}

const char* nl_mqtt_version(void) {
    return NL_MQTT_VERSION;
}

int nl_mqtt_init(void) {
    if (g_mqtt_initialized) return 0;
    g_mqtt_available = 1;
    g_mqtt_initialized = 1;
    nl_mqtt_register_lang();
    return nl_module_register(&g_mqtt_module_info);
}

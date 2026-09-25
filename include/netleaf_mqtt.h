#ifndef NETLEAF_MQTT_H
#define NETLEAF_MQTT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

#ifndef NL_API
#define NL_API
#endif

#ifdef _WIN32
    #ifdef NL_MQTT_EXPORTS
        #define NL_MQTT_API __declspec(dllexport)
    #elif defined(NL_MQTT_STATIC)
        #define NL_MQTT_API
    #else
        #define NL_MQTT_API __declspec(dllimport)
    #endif
#else
    #define NL_MQTT_API
#endif

#define NL_MQTT_VERSION           "2.0.0"
#define NL_MQTT_VERSION_MAJOR     2
#define NL_MQTT_VERSION_MINOR     0
#define NL_MQTT_VERSION_PATCH     0
#define NL_MQTT_PROTOCOL_V3_1_1   3
#define NL_MQTT_PROTOCOL_V5       5

#define NL_MQTT_MAX_TOPIC_LEN     65535
#define NL_MQTT_MAX_CLIENT_ID_LEN 65535
#define NL_MQTT_MAX_PAYLOAD_LEN   268435455
#define NL_MQTT_DEFAULT_PORT      1883
#define NL_MQTT_SECURE_PORT       8883
#define NL_MQTT_WILL_QOS          0

// QoS levels
#define NL_MQTT_QOS_0  0
#define NL_MQTT_QOS_1  1
#define NL_MQTT_QOS_2  2

// Retain handling for MQTT 5.0
#define NL_MQTT_RETAIN_HANDLING_SEND_IF_SUBSCRIBED 0
#define NL_MQTT_RETAIN_HANDLING_SEND_EVERY_TIME    1
#define NL_MQTT_RETAIN_HANDLING_DO_NOT_SEND        2

// Topic alias max
#define NL_MQTT_TOPIC_ALIAS_MAX_DEFAULT 0

// Reason codes for MQTT 5.0
typedef enum {
    NL_MQTT_RC_SUCCESS                          = 0,
    NL_MQTT_RC_NORMAL_DISCONNECTION             = 0,
    NL_MQTT_RC_GRANTED_QOS0                     = 0,
    NL_MQTT_RC_GRANTED_QOS1                     = 1,
    NL_MQTT_RC_GRANTED_QOS2                     = 2,
    NL_MQTT_RC_DISCONNECT_WITH_WILL_MSG         = 4,
    NL_MQTT_RC_NO_MATCHING_SUBSCRIBERS          = 16,
    NL_MQTT_RC_NO_SUBSCRIPTION_EXISTED          = 17,
    NL_MQTT_RC_CONTINUE_AUTHENTICATION          = 24,
    NL_MQTT_RC_REAUTHENTICATE                   = 25,
    NL_MQTT_RC_UNSPECIFIED_ERROR                = 128,
    NL_MQTT_RC_MALFORMED_PACKET                 = 129,
    NL_MQTT_RC_PROTOCOL_ERROR                   = 130,
    NL_MQTT_RC_IMPLEMENTATION_SPECIFIC_ERROR    = 131,
    NL_MQTT_RC_UNSUPPORTED_PROTOCOL_VERSION   = 132,
    NL_MQTT_RC_CLIENT_IDENTIFIER_NOT_VALID    = 133,
    NL_MQTT_RC_BAD_USERNAME_OR_PASSWORD       = 135,
    NL_MQTT_RC_NOT_AUTHORIZED                 = 136,
    NL_MQTT_RC_SERVER_UNAVAILABLE             = 137,
    NL_MQTT_RC_SERVER_BUSY                    = 138,
    NL_MQTT_RC_BANNED                         = 139,
    NL_MQTT_RC_BAD_AUTH_METHOD                = 140,
    NL_MQTT_RC_TOPIC_NAME_INVALID             = 144,
    NL_MQTT_RC_PACKET_TOO_LARGE               = 149,
    NL_MQTT_RC_QUOTA_EXCEEDED                 = 151,
    NL_MQTT_RC_PAYLOAD_FORMAT_INVALID         = 153,
    NL_MQTT_RC_RETAIN_NOT_SUPPORTED           = 154,
    NL_MQTT_RC_TOPIC_ALIAS_INVALID            = 155,
    NL_MQTT_RC_TOPIC_ALIAS_REQUIRED           = 156,
    NL_MQTT_RC_INVALID_STATE                  = 157,
    NL_MQTT_RC_NETWORK_CONNECTION_CLOSED      = 159,
    NL_MQTT_RC_INVALID_PROTOCOL_VERSION       = 160,
    NL_MQTT_RC_BAD_CLIENT_ID                = 161,
    NL_MQTT_RC_WILL_MESSAGE_INVALID           = 163,
    NL_MQTT_RC_WILL_MESSAGE_TOO_LARGE       = 164,
    NL_MQTT_RC_IDENTIFIER_REMOVED           = 165,
    NL_MQTT_RC_RATE_EXCEEDED                = 166,
    NL_MQTT_RC_CONNECT_AUTH               = 235
} nl_mqtt_rc_t;

// MQTT Message Types (Fixed Header)
typedef enum {
    NL_MQTT_CONNECT    = 1,
    NL_MQTT_CONNACK    = 2,
    NL_MQTT_PUBLISH    = 3,
    NL_MQTT_PUBACK     = 4,
    NL_MQTT_PUBREC     = 5,
    NL_MQTT_PUBREL     = 6,
    NL_MQTT_PUBCOMP    = 7,
    NL_MQTT_SUBSCRIBE  = 8,
    NL_MQTT_SUBACK     = 9,
    NL_MQTT_UNSUBSCRIBE= 10,
    NL_MQTT_UNSUBACK   = 11,
    NL_MQTT_PINGREQ    = 12,
    NL_MQTT_PINGRESP   = 13,
    NL_MQTT_DISCONNECT = 14,
    NL_MQTT_RESERVED   = 15
} nl_mqtt_msg_type_t;

// =========================================
// MQTT 5.0 Property Types
// =========================================

typedef enum {
    NL_MQTT_PROP_PAYLOAD_FORMAT_INDICATOR = 0x01,
    NL_MQTT_PROP_MESSAGE_EXPIRY_INTERVAL  = 0x02,
    NL_MQTT_PROP_CONTENT_TYPE             = 0x03,
    NL_MQTT_PROP_RESPONSE_TOPIC           = 0x08,
    NL_MQTT_PROP_CORRELATION_DATA         = 0x09,
    NL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER  = 0x0B,
    NL_MQTT_PROP_SESSION_EXPIRY_INTERVAL  = 0x11,
    NL_MQTT_PROP_ASSIGNED_CLIENT_ID       = 0x12,
    NL_MQTT_PROP_SERVER_KEEP_ALIVE        = 0x13,
    NL_MQTT_PROP_AUTH_METHOD              = 0x15,
    NL_MQTT_PROP_AUTH_DATA                = 0x16,
    NL_MQTT_PROP_REQUEST_PROBLEM_INFO     = 0x17,
    NL_MQTT_PROP_WILL_DELAY_INTERVAL      = 0x18,
    NL_MQTT_PROP_REQUEST_RESPONSE_INFO    = 0x19,
    NL_MQTT_PROP_RESPONSE_INFO            = 0x1A,
    NL_MQTT_PROP_SERVER_REFERENCE         = 0x1C,
    NL_MQTT_PROP_REASON_STRING            = 0x1F,
    NL_MQTT_PROP_RECEIVE_MAXIMUM          = 0x21,
    NL_MQTT_PROP_TOPIC_ALIAS_MAX          = 0x22,
    NL_MQTT_PROP_TOPIC_ALIAS              = 0x23,
    NL_MQTT_PROP_MAX_QOS                  = 0x24,
    NL_MQTT_PROP_RETAIN_AVAILABLE         = 0x25,
    NL_MQTT_PROP_USER_PROPERTY            = 0x26,
    NL_MQTT_PROP_MAX_PACKET_SIZE          = 0x27,
    NL_MQTT_PROP_WILDCARD_SUB_AVAILABLE   = 0x28,
    NL_MQTT_PROP_SUB_IDENTIFIERS_AVAILABLE= 0x29,
    NL_MQTT_PROP_SHARED_SUB_AVAILABLE     = 0x2A
} nl_mqtt_property_type_t;

typedef struct nl_mqtt_property {
    int                    type;
    int                    int_value;
    char*                  str_value;
    size_t                 str_len;
    struct nl_mqtt_property* next;
} nl_mqtt_property_t;

// =========================================
// MQTT Connection Options
// =========================================

typedef struct nl_mqtt_connect_opts {
    const char* client_id;
    uint16_t    keep_alive;
    const char* username;
    const char* password;
    int         clean_session;
    int         will_enabled;
    const char* will_topic;
    const char* will_msg;
    size_t      will_msg_len;
    int         will_qos;
    int         will_retain;
    int         is_utf8;

    /* MQTT 5.0 properties */
    uint32_t    session_expiry_interval;
    uint32_t    will_delay_interval;
    uint32_t    max_packet_size;
    uint16_t    topic_alias_max;
    int         request_problem_info;
    int         request_response_info;
    nl_mqtt_property_t* properties;
    nl_mqtt_property_t* will_properties;
    nl_mqtt_property_t* user_properties;
} nl_mqtt_connect_opts_t;

typedef struct nl_mqtt_pub_opts {
    int   qos;
    int   retain;
    int   dup;

    /* MQTT 5.0 properties */
    uint32_t    message_expiry_interval;
    int         payload_format_indicator;
    const char* content_type;
    const char* response_topic;
    const char* correlation_data;
    size_t      correlation_data_len;
    uint16_t    topic_alias;
    nl_mqtt_property_t* properties;
    nl_mqtt_property_t* user_properties;
} nl_mqtt_pub_opts_t;

typedef struct nl_mqtt_sub_opts {
    int   qos;
    int   no_local;
    int   retain_as_published;
    int   retain_handling;
    int   subscription_identifiers_available;
    int   shared_subscription;
    const char* shared_topic;

    /* MQTT 5.0 properties */
    uint8_t    subscription_identifier;
    nl_mqtt_property_t* properties;
    nl_mqtt_property_t* user_properties;
} nl_mqtt_sub_opts_t;

typedef struct nl_mqtt_connack_opts {
    int         session_present;
    nl_mqtt_property_t* properties;
} nl_mqtt_connack_opts_t;

// =========================================
// MQTT Client Opaque Type
// =========================================

typedef struct nl_mqtt_client nl_mqtt_client_t;

// =========================================
// MQTT Message Callback Types
// =========================================

typedef void (*nl_mqtt_connect_callback_t)(int return_code, void* user_data);
typedef void (*nl_mqtt_message_callback_t)(const char* topic, const void* payload,
                                           size_t payload_len, int qos, int retain,
                                           void* user_data);
typedef void (*nl_mqtt_disconnect_callback_t)(void* user_data);

// =========================================
// MQTT Connection Status
// =========================================

typedef enum {
    NL_MQTT_DISCONNECTED = 0,
    NL_MQTT_CONNECTING   = 1,
    NL_MQTT_CONNECTED    = 2,
    NL_MQTT_RECONNECTING = 3
} nl_mqtt_status_t;

// =========================================
// Core Client API
// =========================================

NL_MQTT_API nl_mqtt_client_t* nl_mqtt_create(void);
NL_MQTT_API void nl_mqtt_destroy(nl_mqtt_client_t* client);

NL_MQTT_API int nl_mqtt_connect(nl_mqtt_client_t* client,
                                 const char* host, uint16_t port,
                                 const nl_mqtt_connect_opts_t* opts,
                                 nl_mqtt_connect_callback_t on_connect);
NL_MQTT_API int nl_mqtt_disconnect(nl_mqtt_client_t* client);
NL_MQTT_API nl_mqtt_status_t nl_mqtt_get_status(const nl_mqtt_client_t* client);

// =========================================
// Publish / Subscribe API
// =========================================

NL_MQTT_API int nl_mqtt_publish(nl_mqtt_client_t* client,
                                 const char* topic,
                                 const void* payload, size_t payload_len,
                                 const nl_mqtt_pub_opts_t* opts);

NL_MQTT_API int nl_mqtt_subscribe(nl_mqtt_client_t* client,
                                   const char* topic,
                                   const nl_mqtt_sub_opts_t* opts,
                                   nl_mqtt_message_callback_t on_message,
                                   void* user_data);

NL_MQTT_API int nl_mqtt_unsubscribe(nl_mqtt_client_t* client,
                                     const char* topic);

// =========================================
// QoS 2 Helpers
// =========================================

NL_MQTT_API int nl_mqtt_send_puback(nl_mqtt_client_t* client, uint16_t packet_id);
NL_MQTT_API int nl_mqtt_send_pubrec(nl_mqtt_client_t* client, uint16_t packet_id);
NL_MQTT_API int nl_mqtt_send_pubrel(nl_mqtt_client_t* client, uint16_t packet_id);
NL_MQTT_API int nl_mqtt_send_pubcomp(nl_mqtt_client_t* client, uint16_t packet_id);

// =========================================
// Keep-alive
// =========================================

NL_MQTT_API int nl_mqtt_ping(nl_mqtt_client_t* client);
NL_MQTT_API int nl_mqtt_handle_pingresp(nl_mqtt_client_t* client);

// =========================================
// TLS/SSL Support
// =========================================

NL_MQTT_API int nl_mqtt_set_tls(nl_mqtt_client_t* client, int enabled);
NL_MQTT_API int nl_mqtt_set_tls_cert(nl_mqtt_client_t* client,
                                      const char* ca_file,
                                      const char* cert_file,
                                      const char* key_file);
NL_MQTT_API int nl_mqtt_set_tls_insecure(nl_mqtt_client_t* client, int insecure);

// =========================================
// Event Loop Integration
// =========================================

NL_MQTT_API int nl_mqtt_handle_incoming(nl_mqtt_client_t* client);
NL_MQTT_API int nl_mqtt_handle_outgoing(nl_mqtt_client_t* client);
NL_MQTT_API int nl_mqtt_maintain(nl_mqtt_client_t* client, long timeout_ms);

// =========================================
// Subscription Management
// =========================================

typedef int (*nl_mqtt_foreach_sub_t)(const char* topic, int qos, void* user_data);

NL_MQTT_API int nl_mqtt_foreach_subscription(nl_mqtt_client_t* client,
                                              nl_mqtt_foreach_sub_t callback,
                                              void* user_data);
NL_MQTT_API int nl_mqtt_get_subscription_count(const nl_mqtt_client_t* client);

// =========================================
// Packet ID Management
// =========================================

NL_MQTT_API uint16_t nl_mqtt_get_next_packet_id(nl_mqtt_client_t* client);

// =========================================
// Utility Functions
// =========================================

NL_MQTT_API const char* nl_mqtt_rc_string(int return_code);
NL_MQTT_API const char* nl_mqtt_msg_type_string(nl_mqtt_msg_type_t type);
NL_MQTT_API int nl_mqtt_validate_topic(const char* topic);
NL_MQTT_API int nl_mqtt_topic_matches(const char* pattern, const char* topic);
NL_MQTT_API int nl_mqtt_client_id_valid(const char* client_id);
NL_MQTT_API size_t nl_mqtt_encoded_len(size_t length);

// =========================================
// MQTT 5.0 Property Management
// =========================================

NL_MQTT_API nl_mqtt_property_t* nl_mqtt_property_create(int type, int int_val,
                                                         const char* str_val);
NL_MQTT_API void nl_mqtt_property_destroy(nl_mqtt_property_t* prop);
NL_MQTT_API void nl_mqtt_property_list_destroy(nl_mqtt_property_t* props);

NL_MQTT_API int nl_mqtt_property_add_int(nl_mqtt_property_t** head, int type, int val);
NL_MQTT_API int nl_mqtt_property_add_str(nl_mqtt_property_t** head, int type,
                                          const char* val, size_t len);

// =========================================
// Module Registration
// =========================================

NL_MQTT_API nl_module_info_t* nl_mqtt_get_module_info(void);
NL_MQTT_API int nl_mqtt_is_available(void);
NL_MQTT_API const char* nl_mqtt_version(void);
NL_MQTT_API int nl_mqtt_init(void);

// =========================================
// Protocol Version Info
// =========================================

NL_MQTT_API int nl_mqtt_get_protocol_version(const nl_mqtt_client_t* client);
NL_MQTT_API int nl_mqtt_set_protocol_version(nl_mqtt_client_t* client, int version);

#ifdef __cplusplus
}
#endif

#endif

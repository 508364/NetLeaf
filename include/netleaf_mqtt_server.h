#ifndef NETLEAF_MQTT_SERVER_H
#define NETLEAF_MQTT_SERVER_H

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
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef int fd_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int fd_t;
#endif

#ifdef _WIN32
    #ifdef NL_MQTT_SERVER_EXPORTS
        #define NL_MQTT_SERVER_API __declspec(dllexport)
    #elif defined(NL_MQTT_SERVER_STATIC)
        #define NL_MQTT_SERVER_API
    #else
        #define NL_MQTT_SERVER_API __declspec(dllimport)
    #endif
#else
    #define NL_MQTT_SERVER_API
#endif

#include "netleaf_mqtt.h"

#define NL_MQTT_SERVER_VERSION      "1.0.0"
#define NL_MQTT_SERVER_DEFAULT_PORT 1883
#define NL_MQTT_SERVER_MAX_CLIENTS  1024
#define NL_MQTT_SERVER_MAX_TOPICS   4096

// Server return codes
#define NL_MQTT_SERVER_OK              0
#define NL_MQTT_SERVER_ERR_INIT       -1
#define NL_MQTT_SERVER_ERR_BIND       -2
#define NL_MQTT_SERVER_ERR_LISTEN     -3
#define NL_MQTT_SERVER_ERR_ACCEPT     -4
#define NL_MQTT_SERVER_ERR_MEMORY     -5
#define NL_MQTT_SERVER_ERR_CLIENT_FULL -6
#define NL_MQTT_SERVER_ERR_TLS        -7
#define NL_MQTT_SERVER_ERR_ALREADY_RUNNING -8
#define NL_MQTT_SERVER_ERR_NOT_RUNNING  -9
#define NL_MQTT_SERVER_ERR_STORE_LOCKED -10   // 会话落盘文件已被其他实例独占

// Client event types
typedef enum {
    NL_MQTT_SERVER_EVT_CONNECT    = 0,
    NL_MQTT_SERVER_EVT_DISCONNECT = 1,
    NL_MQTT_SERVER_EVT_SUBSCRIBE  = 2,
    NL_MQTT_SERVER_EVT_UNSUBSCRIBE= 3,
    NL_MQTT_SERVER_EVT_PUBLISH    = 4
} nl_mqtt_server_event_type_t;

// Server event
typedef struct nl_mqtt_server_event {
    nl_mqtt_server_event_type_t type;
    uint32_t                    client_id;
    const char*                 topic;
    const void*                 payload;
    size_t                      payload_len;
    int                         qos;
    int                         retain;
    // MQTT 5.0：报文原因码(0 = 成功/无)与解析出的属性链表。
    // properties 仅在回调执行期间有效，回调返回后由服务端释放，调用者不得释放或长期持有。
    int                         reason_code;
    nl_mqtt_property_t*         properties;
    void*                       user_data;
} nl_mqtt_server_event_t;

// Server callback types
typedef void (*nl_mqtt_server_event_callback_t)(const nl_mqtt_server_event_t* event, void* user_data);
typedef int  (*nl_mqtt_server_auth_callback_t)(const char* client_id,
                                                const char* username,
                                                const char* password,
                                                void* user_data);

// Server configuration
typedef struct nl_mqtt_server_config {
    uint16_t                  port;
    int                       max_clients;
    int                       tls_enabled;
    int                       tls_client_auth;   // 服务端是否要求并校验客户端证书(0/1)
    int                       auth_enabled;
    const char*               ca_file;
    const char*               cert_file;
    const char*               key_file;
    nl_mqtt_server_auth_callback_t auth_callback;
    void*                     auth_user_data;
    nl_mqtt_property_t*       properties;

    // 出站 QoS 未确认消息的重传与会话保留参数(<=0 表示使用内部默认值)
    int                       retry_timeout_sec;   // 重传间隔(秒)，默认 5
    int                       max_retries;         // 最大重传次数，默认 5(超过则断开连接)
    int                       session_expiry_sec;  // 离线会话保留时长(秒)，默认 86400

    // 入站 QoS2 待 PUBREL 超时清理与持久会话磁盘落盘参数
    int                       qos2_inbound_timeout_sec;  // 入站 QoS2 等 PUBREL 超时(秒)，默认 60
    const char*               session_store_path;        // 会话落盘文件路径(NULL/空字符串表示不落盘)
    int                       session_save_interval_sec; // 周期性落盘间隔(秒)，默认 10

    // 服务端 TLS 握手参数(仅当 tls_enabled=1 时生效)
    int                       tls_handshake_timeout_sec; // TLS 握手超时(秒)；<=0 表示不设超时

    // MQTT 5.0 服务端能力与配额(0 表示不限/不启用)
    int                       max_queued_messages;   // 每会话未确认/离线队列上限(0=不限)
    int                       receive_maximum;       // 入站在途 QoS1/2 上限(0=不限，且不在 CONNACK 通告)
    uint32_t                  max_packet_size;       // 可接收的最大报文长度(0=不限，且不在 CONNACK 通告)
    const char*               server_reference;      // Server Reference(可选；随 CONNACK 下发)
} nl_mqtt_server_config_t;

// Opaque server type
typedef struct nl_mqtt_server nl_mqtt_server_t;

// =========================================
// Server Lifecycle API
// =========================================

NL_MQTT_SERVER_API nl_mqtt_server_t* nl_mqtt_server_create(void);
NL_MQTT_SERVER_API void nl_mqtt_server_destroy(nl_mqtt_server_t* server);

NL_MQTT_SERVER_API int nl_mqtt_server_start(nl_mqtt_server_t* server,
                                            const nl_mqtt_server_config_t* config);
NL_MQTT_SERVER_API int nl_mqtt_server_stop(nl_mqtt_server_t* server);
NL_MQTT_SERVER_API int nl_mqtt_server_is_running(const nl_mqtt_server_t* server);

// =========================================
// Event Callback API
// =========================================

NL_MQTT_SERVER_API int nl_mqtt_server_set_event_callback(
    nl_mqtt_server_t* server,
    nl_mqtt_server_event_callback_t callback,
    void* user_data);

// =========================================
// Client Management API
// =========================================

NL_MQTT_SERVER_API int nl_mqtt_server_get_client_count(const nl_mqtt_server_t* server);
NL_MQTT_SERVER_API int nl_mqtt_server_disconnect_client(nl_mqtt_server_t* server,
                                                        uint32_t client_id);
NL_MQTT_SERVER_API int nl_mqtt_server_publish(nl_mqtt_server_t* server,
                                              const char* topic,
                                              const void* payload,
                                              size_t payload_len,
                                              int qos,
                                              int retain);
NL_MQTT_SERVER_API int nl_mqtt_server_handle_connection(nl_mqtt_server_t* server,
                                                         fd_t sock);

// =========================================
// Event Loop API
// =========================================

// 单步事件循环：轮询监听套接字与已连接客户端，接受新连接、处理可读数据、
// 清理保活超时连接。timeout_ms < 0 表示无限阻塞(至少 1 个事件才返回)。
// 返回本次处理的事件数；负值为错误码(见 NL_MQTT_SERVER_ERR_*)。
NL_MQTT_SERVER_API int nl_mqtt_server_poll(nl_mqtt_server_t* server, int timeout_ms);

// 阻塞式事件循环：反复调用 poll 直到 nl_mqtt_server_stop() 停止服务。
// 适合在独立线程中运行；返回 0 表示被正常停止，负值为错误码。
NL_MQTT_SERVER_API int nl_mqtt_server_run(nl_mqtt_server_t* server);

// =========================================
// Session Persistence API
// =========================================

// 将持久会话(clean_session=0)落盘到 config.session_store_path 指定的文件：
// 内容含各会话的订阅、未确认出站消息、入站 QoS2 待 PUBREL 记录。
// 采用“先写 .tmp 再原子改名”的方式，避免中途失败损坏既有文件。
// 返回 0 成功；NL_MQTT_SERVER_ERR_NOT_RUNNING(-9) 表示未配置落盘路径；其余负值为 I/O 错误。
// 说明：nl_mqtt_server_start() 会自动加载、nl_mqtt_server_stop() 会自动落盘，通常无需手动调用。
NL_MQTT_SERVER_API int nl_mqtt_server_save_sessions(nl_mqtt_server_t* server);

// 从 config.session_store_path 指定的文件加载持久会话(须在任何客户端接入前调用，
// 由 nl_mqtt_server_start() 自动完成)。文件不存在时返回 NL_MQTT_SERVER_ERR_NOT_RUNNING(-9)。
NL_MQTT_SERVER_API int nl_mqtt_server_load_sessions(nl_mqtt_server_t* server);

// =========================================
// Topic/Subscription API
// =========================================

typedef int (*nl_mqtt_server_foreach_subscriber_t)(uint32_t client_id,
                                                    const char* topic,
                                                    int qos,
                                                    void* user_data);

NL_MQTT_SERVER_API int nl_mqtt_server_foreach_subscription(
    nl_mqtt_server_t* server,
    const char* topic_pattern,
    nl_mqtt_server_foreach_subscriber_t callback,
    void* user_data);

NL_MQTT_SERVER_API int nl_mqtt_server_get_topic_subscribers(
    nl_mqtt_server_t* server,
    const char* topic,
    uint32_t* client_ids,
    size_t* count,
    size_t max_count);

// =========================================
// Statistics API
// =========================================

typedef struct nl_mqtt_server_stats {
    uint32_t total_connections;
    uint32_t total_disconnections;
    uint32_t total_publishes;
    uint32_t total_subscriptions;
    uint32_t total_unsubscriptions;
    uint32_t bytes_received;
    uint32_t bytes_sent;
} nl_mqtt_server_stats_t;

NL_MQTT_SERVER_API int nl_mqtt_server_get_stats(const nl_mqtt_server_t* server,
                                                 nl_mqtt_server_stats_t* stats);

// =========================================
// Utility Functions
// =========================================

NL_MQTT_SERVER_API const char* nl_mqtt_server_version(void);
NL_MQTT_SERVER_API int nl_mqtt_server_validate_topic(const char* topic);
NL_MQTT_SERVER_API int nl_mqtt_server_topic_matches(const char* pattern, const char* topic);

// =========================================
// Module Registration
// =========================================

NL_MQTT_SERVER_API nl_module_info_t* nl_mqtt_server_get_module_info(void);
NL_MQTT_SERVER_API int nl_mqtt_server_is_available(void);
NL_MQTT_SERVER_API const char* nl_mqtt_server_version_string(void);
NL_MQTT_SERVER_API int nl_mqtt_server_init(void);

#ifdef __cplusplus
}
#endif

#endif

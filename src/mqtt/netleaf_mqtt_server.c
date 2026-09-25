#define _GNU_SOURCE
#ifndef FD_SETSIZE
#define FD_SETSIZE 4096       /* 提高 select() 并发上限(默认 POSIX 1024 / Windows 64) */
#endif
#include "netleaf_mqtt_server.h"
#include "netleaf_mqtt.h"
#include "netleaf_mqtt_lang.h"
#include "netleaf_mqtt_server_lang.h"
#include "netleaf_module.h"
#ifdef NL_MQTT_SERVER_TLS_ENABLE
#include "netleaf_tls.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
    #define snprintf _snprintf
    #define strdup _strdup
    #define closesocket_close closesocket
    #include <windows.h>
#else
    #define closesocket_close close
    #include <errno.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/file.h>
    #include <sys/select.h>
    #include <pthread.h>
#endif

// 单调毫秒时钟：用于出站消息重传计时。
// 使用秒级 time() 会因整秒截断导致“重传间隔”出现最多 1 秒抖动，故改用毫秒级单调时钟。
#ifdef _WIN32
static long long nl_now_ms(void) { return (long long)GetTickCount64(); }
#else
static long long nl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)(ts.tv_nsec / 1000000);
}
#endif

// ============================================================
// 平台互斥量：进程内并发保护。
// 事件循环可运行在独立线程(run)，而 stop()/公开 save/load 可能来自另一线程，
// 故对落盘文件的操作需串行化。
// ============================================================
#ifdef _WIN32
typedef CRITICAL_SECTION nl_mqtt_mutex_t;
#define NL_MQTT_MUTEX_INIT(m)    InitializeCriticalSection(m)
#define NL_MQTT_MUTEX_LOCK(m)    EnterCriticalSection(m)
#define NL_MQTT_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#define NL_MQTT_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
typedef pthread_mutex_t nl_mqtt_mutex_t;
#define NL_MQTT_MUTEX_INIT(m)    ((void)pthread_mutex_init((m), NULL))
#define NL_MQTT_MUTEX_LOCK(m)    pthread_mutex_lock((m))
#define NL_MQTT_MUTEX_UNLOCK(m)  pthread_mutex_unlock((m))
#define NL_MQTT_MUTEX_DESTROY(m) pthread_mutex_destroy((m))
#endif

// 初始化可重入互斥量(事件回调内可安全再入服务端 API)
static void nl_mqtt_mutex_init_recursive(nl_mqtt_mutex_t* m) {
#ifdef _WIN32
    InitializeCriticalSection(m);   // CRITICAL_SECTION 本身可重入
#else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
}

// API 级加锁(可重入)：串行化跨线程的公开 API 调用
#define NL_SRV_LOCK(s)   do { if ((s) && (s)->api_mutex_inited) NL_MQTT_MUTEX_LOCK(&(s)->api_mutex); } while (0)
#define NL_SRV_UNLOCK(s) do { if ((s) && (s)->api_mutex_inited) NL_MQTT_MUTEX_UNLOCK(&(s)->api_mutex); } while (0)

// 进程间文件锁：运行中的 broker 独占其会话落盘文件，避免多实例互相覆盖。
// 采用 OS 建议锁(flock / LockFileEx)，随句柄关闭自动释放，不会残留死锁。
typedef struct nl_store_lock {
#ifdef _WIN32
    HANDLE     handle;
    OVERLAPPED ov;
#else
    int        fd;
#endif
    int        held;
} nl_store_lock_t;

// CRC32(IEEE 802.3，反射多项式 0xEDB88320)，用于落盘文件完整性校验。
// 表在栈上按需生成，避免静态表带来的线程初始化竞争。
static uint32_t nl_crc32(const void* data, size_t len) {
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    const unsigned char* p = (const unsigned char*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ============================================================
// Internal types
// ============================================================

// 未确认的出站消息(QoS1/QoS2)，用于超时重传与会话恢复。
// stage: 0 = 已发 PUBLISH，等 PUBACK(QoS1)/PUBREC(QoS2)；
//        1 = 已发 PUBREL，等 PUBCOMP(QoS2)。
typedef struct nl_mqtt_out_msg {
    uint16_t  packet_id;
    uint8_t   qos;        // 1 或 2
    uint8_t   stage;      // 0 或 1
    char*     topic;
    void*     payload;
    size_t    payload_len;
    int       retain;
    uint32_t  sub_id;         // v5 订阅标识符(0=无)
    long long expires_ms;     // v5 消息过期时刻(单调毫秒，0=不过期)
    long long last_sent_ms;   // 上次发送时刻(单调毫秒)
    int       retries;
    int       pending;        // 因出站流控而尚未首次发送(defer)
    struct nl_mqtt_out_msg* next;
} nl_mqtt_out_msg_t;

// 入站 QoS2 待 PUBREL 记录（带接收时刻，用于超时清理）
typedef struct nl_mqtt_qos2_inbound {
    uint16_t  packet_id;
    long long received_ms;
} nl_mqtt_qos2_inbound_t;

// 保留消息(Retained)：以主题为唯一键，保存最后一次 retain=1 的发布内容；
// 新订阅匹配到该主题时立即下发。负载为空表示清除该主题的保留消息。
// expires_ms: v5 消息过期时刻(单调毫秒，0=不过期)。
typedef struct nl_mqtt_retained_msg {
    char*    topic;
    void*    payload;
    size_t   payload_len;
    int      qos;
    long long expires_ms;
    int      dirty;                          // 增量落盘：自上次落盘后是否变更
    struct nl_mqtt_retained_msg* next;
} nl_mqtt_retained_msg_t;

// 延迟遗嘱(Will Delay Interval)：异常断开后延迟到 due_ms 才发布；
// 若同 client_id 在到期前重连，则取消发布。
typedef struct nl_mqtt_will_pending {
    char*    client_id;      // 触发方 client id(用于重连取消)
    char*    topic;
    void*    payload;
    size_t   payload_len;
    int      qos;
    int      retain;
    long long due_ms;        // 到期时刻(单调毫秒)
    struct nl_mqtt_will_pending* next;
} nl_mqtt_will_pending_t;

// 会话：以 MQTT client id 为键。clean_session=0 时跨连接保留
// 订阅关系、未确认出站消息与入站 QoS2 状态；persistent 会话可落盘。
typedef struct nl_mqtt_server_session {
    char*     client_id;
    uint32_t  client_num;              // 当前在线连接的内部 id；0 表示离线
    int       connected;
    int       persistent;              // clean_session=0 建立的持久会话(参与落盘)
    nl_mqtt_out_msg_t* out_msgs;       // 未确认出站消息
    nl_mqtt_qos2_inbound_t* qos2_inbound;  // 入站 QoS2：已发 PUBREC、等待 PUBREL
    size_t    qos2_inbound_count;
    size_t    qos2_inbound_cap;
    uint32_t  session_expiry_sec;      // v5 会话过期(秒)；expiry_set=0 时用服务端默认
    int       expiry_set;              // 是否由 CONNECT 属性显式指定
    int       dirty;                   // 增量落盘：自上次落盘后是否变更
    time_t    last_seen;
    struct nl_mqtt_server_session* next;
} nl_mqtt_server_session_t;

typedef struct nl_mqtt_server_client {
    uint32_t                  id;
    fd_t                      sock;
    char*                     client_id;
    int                       protocol_version;
    int                       clean_session;
    uint16_t                  keep_alive;
    int                       connected;
    time_t                    last_activity;
    char*                     username;
    char*                     password;
    // 遗嘱(Will)：连接异常断开时由服务端代为发布
    char*                     will_topic;
    void*                     will_payload;
    size_t                    will_payload_len;
    int                       will_qos;
    int                       will_retain;
    int                       has_will;
    uint32_t                  will_delay_interval;   // v5 遗嘱延迟(秒)
    uint32_t                  will_message_expiry;   // v5 遗嘱消息过期(秒)
    // MQTT 5.0 会话/主题别名状态
    uint32_t                  session_expiry_interval;  // CONNECT 声明(秒)
    int                       session_expiry_set;
    uint32_t                  topic_alias_max;          // 对端可接受的别名上限(仅记录)
    char*                     topic_alias[16];          // 入站主题别名表(索引 1..15)
    // MQTT 5.0 流控/长度/认证声明
    uint16_t                  receive_maximum;          // 对端声明可接收的在途 QoS1/2 上限(0=未声明)
    uint32_t                  max_packet_size;          // 对端声明可接收的最大报文(0=不限)
    char*                     auth_method;              // 对端声明的认证方法(增强认证)
    int                       assigned_id;              // v5 由服务端分配的 client id
    void*                     tls_ctx;
    nl_mqtt_server_t*         server;
    char*                     recv_buf;   // TCP 粘包/半包重组缓冲区
    size_t                    recv_len;
    size_t                    recv_cap;
    uint16_t                  next_packet_id;   // 出站报文标识符分配器
    nl_mqtt_server_session_t* session;          // 关联会话(订阅/未确认消息/入站 QoS2)
    struct nl_mqtt_server_client* next;
} nl_mqtt_server_client_t;

typedef struct nl_mqtt_server_subscription {
    char*                     topic;
    char*                     owner_client_id;   // 拥有者 MQTT client id(会话键，clean_session=0 时持久)
    uint32_t                  client_id;         // 当前在线连接内部 id；0 表示离线(无活动连接)
    int                       qos;
    uint32_t                  sub_id;            // v5 订阅标识符(0=无)
    int                       no_local;          // v5 订阅选项：不回发给发布者
    int                       rap;               // v5 订阅选项：Retain As Published
    int                       retain_handling;   // v5 订阅选项：0/1/2
    char*                     share_group;       // v5 共享订阅组名($share/<group>/<filter>)；NULL=普通订阅
    struct nl_mqtt_server_subscription* next;
} nl_mqtt_server_subscription_t;

typedef struct nl_mqtt_server {
    fd_t                      listen_sock;
    uint16_t                  port;
    int                       max_clients;
    int                       running;
    nl_mqtt_server_client_t*  clients;
    nl_mqtt_server_subscription_t* subscriptions;
    nl_mqtt_server_session_t* sessions;   // 会话表(clean_session=0 时跨连接保留)
    nl_mqtt_retained_msg_t*   retained;   // 保留消息表(按主题唯一)
    nl_mqtt_server_event_callback_t event_callback;
    void*                     event_user_data;
    nl_mqtt_server_auth_callback_t auth_callback;
    void*                     auth_user_data;
    int                       tls_enabled;
    char*                     tls_ca_file;      // 服务端 CA 证书路径(校验客户端)
    char*                     tls_cert_file;    // 服务端证书路径
    char*                     tls_key_file;     // 服务端私钥路径
    int                       tls_client_auth;  // 是否要求客户端证书
    int                       tls_handshake_timeout_sec; // 服务端 TLS 握手超时(秒)；<=0 不设超时
    uint32_t                  next_client_id;
    int                       retry_timeout_sec;   // 出站 QoS 未确认重传间隔(秒)
    int                       max_retries;         // 最大重传次数
    int                       session_expiry_sec;  // 离线会话保留时长(秒)
    int                       qos2_inbound_timeout_sec;  // 入站 QoS2 待 PUBREL 超时(秒)
    char*                     session_store_path;  // 会话落盘文件路径(NULL 表示不落盘)
    int                       session_save_interval_sec; // 周期性落盘间隔(秒)
    long long                 last_save_ms;        // 上次落盘时刻(单调毫秒)
    // MQTT 5.0 服务端能力/配额
    int                       max_queued_messages; // 每会话队列上限(0=不限)
    int                       receive_maximum;     // 服务端入站在途上限(0=不限，不在 CONNACK 通告)
    uint32_t                  max_packet_size;     // 服务端可接收最大报文(0=不限)
    char*                     server_reference;    // v5 Server Reference(可选)
    uint32_t                  share_rr;            // 共享订阅轮询计数
    // 并发保护：进程内互斥 + 进程间独占文件锁(运行期持有)
    nl_mqtt_mutex_t           store_mutex;
    int                       store_mutex_inited;
    nl_store_lock_t           store_lock;
    // API 级递归锁：串行化并发调用(事件回调内可安全再入服务端 API)
    nl_mqtt_mutex_t           api_mutex;
    int                       api_mutex_inited;
    // 延迟遗嘱队列(v5 Will Delay Interval)
    nl_mqtt_will_pending_t*   pending_wills;
    // 增量落盘状态：脏标记由各变更点设置，落盘时仅追加变更块
    int      store_loading;                   // 加载中：抑制脏标记
    uint32_t store_block_count;               // 已知日志块数(达到阈值时压缩)
    char**   deleted_session_ids;             // 待落盘的“已删除会话”client_id
    size_t   deleted_session_count;
    size_t   deleted_session_cap;
    char**   deleted_retained_topics;         // 待落盘的“已清除保留消息”主题
    size_t   deleted_retained_count;
    size_t   deleted_retained_cap;
    nl_mqtt_server_stats_t    stats;
} nl_mqtt_server_t;

// ---- 前置声明(供定义顺序较早的函数调用) ----
static void nl_mqtt_server_disconnect_client_internal(nl_mqtt_server_t* srv,
                                                      nl_mqtt_server_client_t* client,
                                                      int publish_will);
static int  nl_mqtt_server_send_disconnect(nl_mqtt_server_client_t* client,
                                           int reason_code);

// ============================================================
// Module state
// ============================================================

static int g_mqtt_server_initialized = 0;
static int g_mqtt_server_available   = 0;

static nl_module_info_t g_mqtt_server_module_info = {
    .type            = NL_MODULE_CUSTOM,
    .name            = "mqtt_server",
    .version         = NL_MQTT_SERVER_VERSION,
    .capabilities    = NL_CAP_SERVER | NL_CAP_ASYNC | NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL,
    .status          = NL_MODULE_STATUS_UNINITIALIZED,
    .platform_windows = 1,
    .platform_linux   = 1,
    .platform_macos   = 1,
    .init            = nl_mqtt_server_init,
    .shutdown        = NULL,
    .is_available    = nl_mqtt_server_is_available,
    .get_version     = nl_mqtt_server_version_string,
    .description     = "MQTT v3.1.1/v5.0 server (broker) implementation",
    .author          = "NetLeaf",
    .next            = NULL
};

// ============================================================
// Internal helpers
// ============================================================

static void nl_mqtt_server_init_sock(void) {
#ifdef _WIN32
    static int ws_started = 0;
    if (!ws_started) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        ws_started = 1;
    }
#endif
}

static nl_mqtt_server_client_t* nl_mqtt_server_find_client(nl_mqtt_server_t* srv,
                                                            uint32_t client_id) {
    nl_mqtt_server_client_t* c = srv->clients;
    while (c) {
        if (c->id == client_id) return c;
        c = c->next;
    }
    return NULL;
}

static nl_mqtt_server_client_t* nl_mqtt_server_create_client(nl_mqtt_server_t* srv,
                                                              fd_t sock) {
    nl_mqtt_server_client_t* client = (nl_mqtt_server_client_t*)calloc(1, sizeof(*client));
    if (!client) return NULL;

    client->id = srv->next_client_id++;
    client->sock = sock;
    client->connected = 1;
    client->server = srv;
    client->next = srv->clients;
    srv->clients = client;
    srv->stats.total_connections++;

    return client;
}

static nl_mqtt_server_subscription_t* nl_mqtt_server_find_sub(nl_mqtt_server_t* srv,
                                                               const char* topic,
                                                               const char* owner_client_id) {
    nl_mqtt_server_subscription_t* s = srv->subscriptions;
    while (s) {
        if (s->owner_client_id &&
            strcmp(s->topic, topic) == 0 &&
            strcmp(s->owner_client_id, owner_client_id) == 0) {
            return s;
        }
        s = s->next;
    }
    return NULL;
}

// 新增/更新订阅。返回 1 表示新建，0 表示更新既有，-1 表示失败。
static int nl_mqtt_server_add_subscription(nl_mqtt_server_t* srv,
                                            const char* topic,
                                            const char* owner_client_id,
                                            uint32_t client_num,
                                            int qos,
                                            uint32_t sub_id,
                                            int no_local, int rap, int retain_handling,
                                            const char* share_group) {
    nl_mqtt_server_subscription_t* existing =
        nl_mqtt_server_find_sub(srv, topic, owner_client_id);
    if (existing) {
        existing->qos = qos;
        existing->client_id = client_num;
        existing->sub_id = sub_id;
        existing->no_local = no_local ? 1 : 0;
        existing->rap = rap ? 1 : 0;
        existing->retain_handling = retain_handling;
        free(existing->share_group);
        existing->share_group = share_group ? strdup(share_group) : NULL;
        return 0;
    }

    nl_mqtt_server_subscription_t* sub =
        (nl_mqtt_server_subscription_t*)calloc(1, sizeof(*sub));
    if (!sub) return -1;

    sub->topic = strdup(topic);
    sub->owner_client_id = strdup(owner_client_id);
    if (!sub->topic || !sub->owner_client_id) {
        free(sub->topic);
        free(sub->owner_client_id);
        free(sub);
        return -1;
    }

    sub->client_id = client_num;
    sub->qos = qos;
    sub->sub_id = sub_id;
    sub->no_local = no_local ? 1 : 0;
    sub->rap = rap ? 1 : 0;
    sub->retain_handling = retain_handling;
    sub->share_group = share_group ? strdup(share_group) : NULL;
    sub->next = srv->subscriptions;
    srv->subscriptions = sub;
    return 1;
}

static void nl_mqtt_server_remove_subscription(nl_mqtt_server_t* srv,
                                                const char* topic,
                                                const char* owner_client_id) {
    nl_mqtt_server_subscription_t* prev = NULL;
    nl_mqtt_server_subscription_t* cur  = srv->subscriptions;
    while (cur) {
        if (cur->owner_client_id &&
            strcmp(cur->topic, topic) == 0 &&
            strcmp(cur->owner_client_id, owner_client_id) == 0) {
            if (prev) prev->next = cur->next;
            else       srv->subscriptions = cur->next;
            free(cur->topic);
            free(cur->owner_client_id);
            free(cur->share_group);
            free(cur);
            return;
        }
        prev = cur;
        cur  = cur->next;
    }
}

// 移除某个会话(按 MQTT client id)拥有的全部订阅
static void nl_mqtt_server_remove_client_subscriptions(nl_mqtt_server_t* srv,
                                                        const char* owner_client_id) {
    nl_mqtt_server_subscription_t* prev = NULL;
    nl_mqtt_server_subscription_t* cur  = srv->subscriptions;
    while (cur) {
        if (cur->owner_client_id && strcmp(cur->owner_client_id, owner_client_id) == 0) {
            nl_mqtt_server_subscription_t* next = cur->next;
            if (prev) prev->next = next;
            else      srv->subscriptions = next;
            free(cur->topic);
            free(cur->owner_client_id);
            free(cur->share_group);
            free(cur);
            cur = next;
        } else {
            prev = cur;
            cur  = cur->next;
        }
    }
}

// 会话重新上线时，把其订阅重新指向新的在线连接内部 id
static void nl_mqtt_server_rebind_subscriptions(nl_mqtt_server_t* srv,
                                                 const char* owner_client_id,
                                                 uint32_t client_num) {
    for (nl_mqtt_server_subscription_t* s = srv->subscriptions; s; s = s->next) {
        if (s->owner_client_id && strcmp(s->owner_client_id, owner_client_id) == 0) {
            s->client_id = client_num;
        }
    }
}

// ============================================================
// 会话管理
// ============================================================

static void nl_mqtt_server_free_out_msg(nl_mqtt_out_msg_t* m) {
    if (!m) return;
    free(m->topic);
    free(m->payload);
    free(m);
}

static void nl_mqtt_server_free_out_msgs(nl_mqtt_out_msg_t* m) {
    while (m) {
        nl_mqtt_out_msg_t* next = m->next;
        nl_mqtt_server_free_out_msg(m);
        m = next;
    }
}

static nl_mqtt_server_session_t* nl_mqtt_server_find_session(nl_mqtt_server_t* srv,
                                                              const char* client_id) {
    if (!srv || !client_id) return NULL;
    for (nl_mqtt_server_session_t* s = srv->sessions; s; s = s->next) {
        if (strcmp(s->client_id, client_id) == 0) return s;
    }
    return NULL;
}

// 增量落盘的会话删除标记辅助(定义见后文)
static void nl_mqtt_server_note_session_deleted(nl_mqtt_server_t* srv,
                                                const char* client_id);
static void nl_mqtt_server_undo_session_deleted(nl_mqtt_server_t* srv,
                                                const char* client_id);

static void nl_mqtt_server_destroy_session(nl_mqtt_server_t* srv,
                                            nl_mqtt_server_session_t* session) {
    if (!srv || !session) return;

    // 先从会话表摘除
    nl_mqtt_server_session_t* prev = NULL;
    nl_mqtt_server_session_t* cur  = srv->sessions;
    while (cur) {
        if (cur == session) {
            if (prev) prev->next = cur->next;
            else      srv->sessions = cur->next;
            break;
        }
        prev = cur;
        cur  = cur->next;
    }

    // 持久会话被删除：记入待落盘删除列表(增量落盘)
    if (session->persistent) {
        nl_mqtt_server_note_session_deleted(srv, session->client_id);
    }
    nl_mqtt_server_free_out_msgs(session->out_msgs);
    free(session->qos2_inbound);
    free(session->client_id);
    free(session);
}

// 获取或创建会话：clean_session=1 会丢弃旧会话与旧订阅。
// out_session_present 返回是否为“已存在的会话”(用于 CONNACK Session Present)。
// session_expiry_sec/expiry_set 为该连接的 v5 会话过期声明(未声明时用服务端默认)。
static nl_mqtt_server_session_t* nl_mqtt_server_acquire_session(nl_mqtt_server_t* srv,
                                                                 const char* client_id,
                                                                 int clean_session,
                                                                 uint32_t client_num,
                                                                 uint32_t session_expiry_sec,
                                                                 int expiry_set,
                                                                 int* out_session_present) {
    if (!srv || !client_id) return NULL;

    nl_mqtt_server_session_t* sess = nl_mqtt_server_find_session(srv, client_id);

    if (clean_session && sess) {
        nl_mqtt_server_remove_client_subscriptions(srv, client_id);
        nl_mqtt_server_destroy_session(srv, sess);
        sess = NULL;
    }

    if (!sess) {
        sess = (nl_mqtt_server_session_t*)calloc(1, sizeof(*sess));
        if (!sess) return NULL;
        sess->client_id = strdup(client_id);
        if (!sess->client_id) { free(sess); return NULL; }
        sess->next = srv->sessions;
        srv->sessions = sess;
        if (out_session_present) *out_session_present = 0;
        nl_mqtt_server_undo_session_deleted(srv, client_id);   // 撤销删除标记
    } else {
        if (out_session_present) *out_session_present = 1;
    }

    uint32_t old_num = sess->client_num;
    int was_connected = sess->connected;

    sess->connected  = 1;
    sess->client_num = client_num;
    sess->last_seen  = time(NULL);

    // v5 会话过期：显式声明优先；否则 clean_session=1 -> 断开即失效，其余用服务端默认。
    if (expiry_set) {
        sess->session_expiry_sec = session_expiry_sec;
    } else {
        sess->session_expiry_sec = clean_session ? 0u : (uint32_t)srv->session_expiry_sec;
    }
    sess->expiry_set = expiry_set;
    sess->persistent = sess->session_expiry_sec > 0 ? 1 : 0;   // 仅持久会话参与落盘
    sess->dirty      = 1;

    // 会话恢复：重置未确认消息的重传计数与计时，使其在新连接上立即重新投递
    // (last_sent=0 使下一次 poll 即触发重传：stage0 -> PUBLISH(DUP=1)；stage1 -> PUBREL)
    for (nl_mqtt_out_msg_t* m = sess->out_msgs; m; m = m->next) {
        m->retries      = 0;
        m->last_sent_ms = 0;
    }

    // 同 client id 已有活动连接 -> 连接接管：旧连接以 0x8E 断开(不动会话/订阅)
    if (was_connected && old_num != 0 && old_num != client_num) {
        nl_mqtt_server_client_t* old = nl_mqtt_server_find_client(srv, old_num);
        if (old) {
            nl_mqtt_server_send_disconnect(old, 0x8E);   // Session taken over
            old->session = NULL;                          // 会话已由新连接接管
            free(old->client_id);
            old->client_id = NULL;
            nl_mqtt_server_disconnect_client_internal(srv, old, 1);
        }
    }

    return sess;
}

// 会话内入站 QoS2 待 PUBREL 集合(带接收时刻，用于超时清理)
static int nl_mqtt_server_session_qos2_contains(const nl_mqtt_server_session_t* s,
                                                 uint16_t packet_id) {
    if (!s) return 0;
    for (size_t i = 0; i < s->qos2_inbound_count; i++) {
        if (s->qos2_inbound[i].packet_id == packet_id) return 1;
    }
    return 0;
}

static int nl_mqtt_server_session_qos2_add(nl_mqtt_server_session_t* s,
                                            uint16_t packet_id) {
    if (!s) return -1;

    // 已存在：视为重复 PUBLISH，仅刷新接收时刻(不重复投递)
    for (size_t i = 0; i < s->qos2_inbound_count; i++) {
        if (s->qos2_inbound[i].packet_id == packet_id) {
            s->qos2_inbound[i].received_ms = nl_now_ms();
            return 0;
        }
    }

    if (s->qos2_inbound_count >= s->qos2_inbound_cap) {
        size_t ncap = s->qos2_inbound_cap ? s->qos2_inbound_cap * 2 : 8;
        nl_mqtt_qos2_inbound_t* nb = (nl_mqtt_qos2_inbound_t*)realloc(
            s->qos2_inbound, ncap * sizeof(nl_mqtt_qos2_inbound_t));
        if (!nb) return -1;
        s->qos2_inbound = nb;
        s->qos2_inbound_cap = ncap;
    }
    s->qos2_inbound[s->qos2_inbound_count].packet_id   = packet_id;
    s->qos2_inbound[s->qos2_inbound_count].received_ms = nl_now_ms();
    s->qos2_inbound_count++;
    s->dirty = 1;   // 增量落盘
    return 1;
}

static void nl_mqtt_server_session_qos2_remove(nl_mqtt_server_session_t* s,
                                                uint16_t packet_id) {
    if (!s) return;
    for (size_t i = 0; i < s->qos2_inbound_count; i++) {
        if (s->qos2_inbound[i].packet_id == packet_id) {
            s->qos2_inbound[i] = s->qos2_inbound[s->qos2_inbound_count - 1];
            s->qos2_inbound_count--;
            s->dirty = 1;   // 增量落盘
            return;
        }
    }
}

// 清理超时未收到 PUBREL 的入站 QoS2 记录
static void nl_mqtt_server_session_qos2_prune(nl_mqtt_server_session_t* s,
                                               long long now_ms,
                                               long long timeout_ms) {
    if (!s || timeout_ms <= 0) return;
    size_t i = 0;
    while (i < s->qos2_inbound_count) {
        if (now_ms - s->qos2_inbound[i].received_ms > timeout_ms) {
            s->qos2_inbound[i] = s->qos2_inbound[s->qos2_inbound_count - 1];
            s->qos2_inbound_count--;
        } else {
            i++;
        }
    }
}

// 会话内未确认出站消息
static nl_mqtt_out_msg_t* nl_mqtt_server_out_msg_add(nl_mqtt_server_session_t* sess,
                                                      uint16_t packet_id, uint8_t qos,
                                                      const char* topic,
                                                      const void* payload,
                                                      size_t payload_len,
                                                      int retain, uint32_t sub_id,
                                                      long long expires_ms,
                                                      uint8_t stage) {
    if (!sess) return NULL;

    nl_mqtt_out_msg_t* m = (nl_mqtt_out_msg_t*)calloc(1, sizeof(*m));
    if (!m) return NULL;

    if (topic) {
        m->topic = strdup(topic);
        if (!m->topic) { free(m); return NULL; }
    }

    if (payload_len > 0 && payload) {
        m->payload = malloc(payload_len);
        if (!m->payload) { free(m->topic); free(m); return NULL; }
        memcpy(m->payload, payload, payload_len);
    }

    m->packet_id   = packet_id;
    m->qos         = qos;
    m->stage       = stage;
    m->payload_len = payload_len;
    m->retain      = retain;
    m->sub_id      = sub_id;
    m->expires_ms  = expires_ms;
    m->last_sent_ms = nl_now_ms();
    m->retries     = 0;
    m->next        = sess->out_msgs;
    sess->out_msgs = m;
    sess->dirty    = 1;   // 增量落盘
    return m;
}

static void nl_mqtt_server_out_msg_remove(nl_mqtt_server_session_t* sess,
                                           uint16_t packet_id, uint8_t qos, uint8_t stage) {
    if (!sess) return;
    nl_mqtt_out_msg_t* prev = NULL;
    nl_mqtt_out_msg_t* cur  = sess->out_msgs;
    while (cur) {
        if (cur->packet_id == packet_id && cur->qos == qos && cur->stage == stage) {
            if (prev) prev->next = cur->next;
            else      sess->out_msgs = cur->next;
            nl_mqtt_server_free_out_msg(cur);
            sess->dirty = 1;   // 增量落盘
            return;
        }
        prev = cur;
        cur  = cur->next;
    }
}

// ============================================================
// 保留消息(Retained)
// ============================================================

// 增量落盘的删除标记辅助(定义见下文)
static void nl_mqtt_server_note_retained_deleted(nl_mqtt_server_t* srv,
                                                 const char* topic);
static void nl_mqtt_server_undo_retained_deleted(nl_mqtt_server_t* srv,
                                                 const char* topic);

static nl_mqtt_retained_msg_t* nl_mqtt_server_find_retained(nl_mqtt_server_t* srv,
                                                            const char* topic) {
    if (!srv || !topic) return NULL;
    for (nl_mqtt_retained_msg_t* r = srv->retained; r; r = r->next) {
        if (strcmp(r->topic, topic) == 0) return r;
    }
    return NULL;
}

static void nl_mqtt_server_remove_retained(nl_mqtt_server_t* srv, const char* topic) {
    if (!srv || !topic) return;
    nl_mqtt_retained_msg_t* prev = NULL;
    nl_mqtt_retained_msg_t* cur  = srv->retained;
    while (cur) {
        if (strcmp(cur->topic, topic) == 0) {
            if (prev) prev->next = cur->next;
            else      srv->retained = cur->next;
            free(cur->topic);
            free(cur->payload);
            free(cur);
            nl_mqtt_server_note_retained_deleted(srv, topic);   // 增量落盘
            return;
        }
        prev = cur;
        cur  = cur->next;
    }
}

// 保存 retain=1 的发布内容；payload_len==0 表示清除该主题的保留消息(MQTT 规范)。
// expiry_sec>0 时记录 v5 消息过期时刻(到期后于补发/清理时丢弃)。
static void nl_mqtt_server_set_retained(nl_mqtt_server_t* srv, const char* topic,
                                        const void* payload, size_t payload_len,
                                        int qos, uint32_t expiry_sec) {
    if (!srv || !topic) return;

    if (payload_len == 0 || !payload) {
        nl_mqtt_server_remove_retained(srv, topic);
        return;
    }

    long long exp = expiry_sec > 0 ? nl_now_ms() + (long long)expiry_sec * 1000 : 0;

    nl_mqtt_retained_msg_t* r = nl_mqtt_server_find_retained(srv, topic);
    if (r) {
        void* np = malloc(payload_len);
        if (!np) return;
        memcpy(np, payload, payload_len);
        free(r->payload);
        r->payload     = np;
        r->payload_len = payload_len;
        r->qos         = qos;
        r->expires_ms  = exp;
        r->dirty       = 1;
        return;
    }

    r = (nl_mqtt_retained_msg_t*)calloc(1, sizeof(*r));
    if (!r) return;
    r->topic   = strdup(topic);
    r->payload = malloc(payload_len);
    if (!r->topic || !r->payload) {
        free(r->topic);
        free(r->payload);
        free(r);
        return;
    }
    memcpy(r->payload, payload, payload_len);
    r->payload_len = payload_len;
    r->qos         = qos;
    r->expires_ms  = exp;
    r->dirty       = 1;
    r->next        = srv->retained;
    srv->retained  = r;
    nl_mqtt_server_undo_retained_deleted(srv, topic);   // 撤销可能存在的删除标记
}

static void nl_mqtt_server_free_retained(nl_mqtt_server_t* srv) {
    nl_mqtt_retained_msg_t* r = srv ? srv->retained : NULL;
    while (r) {
        nl_mqtt_retained_msg_t* next = r->next;
        free(r->topic);
        free(r->payload);
        free(r);
        r = next;
    }
    if (srv) srv->retained = NULL;
}

// ============================================================
// 增量落盘辅助：脏标记由各变更点直接置位；删除项记入“待落盘删除列表”。
// 落盘时仅序列化“脏对象 + 删除项”，避免每次整表重写。
// ============================================================

// 记录一个被删除的会话 client_id(去重)
static void nl_mqtt_server_note_session_deleted(nl_mqtt_server_t* srv,
                                                const char* client_id) {
    if (!srv || !client_id) return;
    for (size_t i = 0; i < srv->deleted_session_count; i++) {
        if (strcmp(srv->deleted_session_ids[i], client_id) == 0) return;
    }
    if (srv->deleted_session_count == srv->deleted_session_cap) {
        size_t ncap = srv->deleted_session_cap ? srv->deleted_session_cap * 2 : 8;
        char** nd = (char**)realloc(srv->deleted_session_ids, ncap * sizeof(char*));
        if (!nd) return;
        srv->deleted_session_ids = nd;
        srv->deleted_session_cap = ncap;
    }
    char* dup = strdup(client_id);
    if (!dup) return;
    srv->deleted_session_ids[srv->deleted_session_count++] = dup;
}

// 会话重新出现时，撤销其待删除标记
static void nl_mqtt_server_undo_session_deleted(nl_mqtt_server_t* srv,
                                                const char* client_id) {
    if (!srv || !client_id) return;
    for (size_t i = 0; i < srv->deleted_session_count; i++) {
        if (strcmp(srv->deleted_session_ids[i], client_id) == 0) {
            free(srv->deleted_session_ids[i]);
            srv->deleted_session_ids[i] =
                srv->deleted_session_ids[--srv->deleted_session_count];
            return;
        }
    }
}

// 记录一个被清除的保留消息主题(去重)
static void nl_mqtt_server_note_retained_deleted(nl_mqtt_server_t* srv,
                                                 const char* topic) {
    if (!srv || !topic) return;
    for (size_t i = 0; i < srv->deleted_retained_count; i++) {
        if (strcmp(srv->deleted_retained_topics[i], topic) == 0) return;
    }
    if (srv->deleted_retained_count == srv->deleted_retained_cap) {
        size_t ncap = srv->deleted_retained_cap ? srv->deleted_retained_cap * 2 : 8;
        char** nd = (char**)realloc(srv->deleted_retained_topics, ncap * sizeof(char*));
        if (!nd) return;
        srv->deleted_retained_topics = nd;
        srv->deleted_retained_cap = ncap;
    }
    char* dup = strdup(topic);
    if (!dup) return;
    srv->deleted_retained_topics[srv->deleted_retained_count++] = dup;
}

static void nl_mqtt_server_undo_retained_deleted(nl_mqtt_server_t* srv,
                                                 const char* topic) {
    if (!srv || !topic) return;
    for (size_t i = 0; i < srv->deleted_retained_count; i++) {
        if (strcmp(srv->deleted_retained_topics[i], topic) == 0) {
            free(srv->deleted_retained_topics[i]);
            srv->deleted_retained_topics[i] =
                srv->deleted_retained_topics[--srv->deleted_retained_count];
            return;
        }
    }
}

static void nl_mqtt_server_clear_deleted_lists(nl_mqtt_server_t* srv) {
    for (size_t i = 0; i < srv->deleted_session_count; i++) free(srv->deleted_session_ids[i]);
    srv->deleted_session_count = 0;
    for (size_t i = 0; i < srv->deleted_retained_count; i++) free(srv->deleted_retained_topics[i]);
    srv->deleted_retained_count = 0;
}

static void nl_mqtt_server_free_deleted_lists(nl_mqtt_server_t* srv) {
    nl_mqtt_server_clear_deleted_lists(srv);
    free(srv->deleted_session_ids);
    srv->deleted_session_ids = NULL;
    srv->deleted_session_cap = 0;
    free(srv->deleted_retained_topics);
    srv->deleted_retained_topics = NULL;
    srv->deleted_retained_cap = 0;
}

// ---- 入站主题别名表 ----
static void nl_mqtt_server_topic_alias_set(nl_mqtt_server_client_t* client,
                                           uint16_t alias, const char* topic) {
    if (!client || alias == 0 || alias >= 16) return;
    free(client->topic_alias[alias]);
    client->topic_alias[alias] = topic ? strdup(topic) : NULL;
}
static const char* nl_mqtt_server_topic_alias_get(nl_mqtt_server_client_t* client,
                                                  uint16_t alias) {
    if (!client || alias == 0 || alias >= 16) return NULL;
    return client->topic_alias[alias];
}
static void nl_mqtt_server_topic_alias_clear(nl_mqtt_server_client_t* client) {
    if (!client) return;
    for (int i = 0; i < 16; i++) {
        free(client->topic_alias[i]);
        client->topic_alias[i] = NULL;
    }
}

// 真实下发实现见下文（依赖 PUBLISH 构造与出站 QoS2 状态辅助，故置于其后）。
// 返回成功投递的订阅者数；publisher_id=0 表示服务端发起(不受 No Local 影响)。
static int nl_mqtt_server_broadcast(nl_mqtt_server_t* srv,
                                     const char* topic,
                                     const void* payload,
                                     size_t payload_len,
                                     int qos,
                                     int retain,
                                     uint32_t publisher_id,
                                     uint32_t msg_expiry_sec,
                                     nl_mqtt_property_t* fwd_props);

// 会话磁盘落盘/加载：定义见文件末尾(在生命周期 API 中提前调用，故此处前置声明)。
// 未配置 session_store_path 时返回 NL_MQTT_SERVER_ERR_NOT_RUNNING。
int nl_mqtt_server_save_sessions(nl_mqtt_server_t* server);
int nl_mqtt_server_load_sessions(nl_mqtt_server_t* server);
// 进程间独占文件锁：start 期间获取、stop/destroy 释放(定义见文件末尾)。
static int  nl_mqtt_server_store_lock(nl_mqtt_server_t* server);
static void nl_mqtt_server_store_unlock(nl_mqtt_server_t* server);
// 增量落盘压缩(写单个快照)；stop 时调用以保证文件紧凑(定义见后文)。
static int  nl_mqtt_server_store_compact(nl_mqtt_server_t* srv);

// ============================================================
// 协议级别与报文长度编解码辅助
// ============================================================

// CONNECT 报文中的协议级别(Protocol Level)：4 = MQTT 3.1.1，5 = MQTT 5.0
#define NL_MQTT_SERVER_PROTO_LEVEL_V3_1_1 4
#define NL_MQTT_SERVER_PROTO_LEVEL_V5     NL_MQTT_PROTOCOL_V5

// 单次 SUBSCRIBE 允许的最大主题过滤器个数(同时限制 SUBACK 返回码个数)
#define NL_MQTT_SERVER_MAX_SUBACK_CODES   16

// 出站未确认消息(QoS1/2)的重传间隔与最大重传次数默认值(可被 config 覆盖)
#define NL_MQTT_SERVER_RETRY_TIMEOUT_SEC  5
#define NL_MQTT_SERVER_MAX_RETRIES        5

// 离线会话保留时长默认值(秒)：超过后清理并丢弃其订阅与未确认消息(可被 config 覆盖)
#define NL_MQTT_SERVER_SESSION_EXPIRY_SEC 86400

// 入站 QoS2 等待对端 PUBREL 的超时默认值(秒)：超时后丢弃待确认记录(可被 config 覆盖)
#define NL_MQTT_SERVER_QOS2_INBOUND_TIMEOUT_SEC 60

// 会话周期落盘间隔默认值(秒)(可被 config 覆盖)
#define NL_MQTT_SERVER_SESSION_SAVE_INTERVAL_SEC 10

// 判断该连接是否使用 MQTT 5.0（含 properties）报文格式；
// 只有协议级别恰好为 5 才走含属性路径，其余(含未知级别)按无属性处理。
static int nl_mqtt_server_is_v5(const nl_mqtt_server_client_t* client) {
    return (client && client->protocol_version == NL_MQTT_SERVER_PROTO_LEVEL_V5) ? 1 : 0;
}

// 读取 MQTT「剩余长度」变长整数(最多 4 字节)，成功返回 0。
// 关键点：每字节贡献 (digit & 0x7F) * multiplier，而不是累加后再整体乘以 multiplier。
static int nl_mqtt_server_read_varint(const char* buf, size_t len,
                                      size_t* offset, size_t* out) {
    size_t value = 0;
    size_t multiplier = 1;
    for (int i = 0; i < 4; i++) {
        if (*offset >= len) return -1;
        uint8_t digit = (uint8_t)buf[(*offset)++];
        value += (size_t)(digit & 0x7F) * multiplier;
        if ((digit & 0x80) == 0) {
            if (out) *out = value;
            return 0;
        }
        multiplier *= 128;
    }
    return -1;  // 超过 4 字节属于非法编码
}

// 将「剩余长度」编码为变长整数，返回占用字节数；-1 表示容量不足
static int nl_mqtt_server_encode_varint(char* buf, size_t cap, size_t value) {
    int written = 0;
    do {
        if ((size_t)written >= cap) return -1;
        uint8_t digit = (uint8_t)(value % 128);
        value /= 128;
        if (value > 0) digit |= 0x80;
        buf[written++] = (char)digit;
    } while (value > 0);
    return written;
}

// 写入固定头(首字节 + 剩余长度)，返回固定头字节数；-1 表示失败
static int nl_mqtt_server_write_fixed_header(char* buf, size_t cap,
                                             uint8_t first_byte,
                                             size_t remaining) {
    if (cap < 2) return -1;
    buf[0] = (char)first_byte;
    int n = nl_mqtt_server_encode_varint(buf + 1, cap - 1, remaining);
    if (n < 0) return -1;
    return n + 1;
}

// 解析(或跳过) MQTT 5.0 属性段：先读属性长度，再跳过属性内容。
// limit 为报文绝对边界，offset 为绝对偏移。
static int nl_mqtt_server_skip_properties(const char* buf, size_t limit,
                                          size_t* offset) {
    size_t prop_len = 0;
    if (nl_mqtt_server_read_varint(buf, limit, offset, &prop_len) != 0) return -1;
    if (*offset + prop_len > limit) return -1;
    *offset += prop_len;
    return 0;
}

// ============================================================
// MQTT 5.0 属性编解码（按规范线格式，覆盖全部标准属性类型）
// 属性列表以公开类型 nl_mqtt_property_t 表示；本模块自行分配/释放，
// 事件回调期间有效(回调返回后即释放)，使用者不得自行释放。
// ============================================================

typedef enum {
    PT_BYTE, PT_U16, PT_U32, PT_VARINT, PT_STR, PT_BIN, PT_STRPAIR, PT_UNKNOWN
} nl_prop_kind_t;

// 按属性标识符返回其线格式类别（MQTT 5.0 规范）。
static nl_prop_kind_t nl_prop_kind(int type) {
    switch (type) {
        case NL_MQTT_PROP_PAYLOAD_FORMAT_INDICATOR:  return PT_BYTE;
        case NL_MQTT_PROP_MESSAGE_EXPIRY_INTERVAL:   return PT_U32;
        case NL_MQTT_PROP_CONTENT_TYPE:              return PT_STR;
        case NL_MQTT_PROP_RESPONSE_TOPIC:            return PT_STR;
        case NL_MQTT_PROP_CORRELATION_DATA:          return PT_BIN;
        case NL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER:   return PT_VARINT;
        case NL_MQTT_PROP_SESSION_EXPIRY_INTERVAL:   return PT_U32;
        case NL_MQTT_PROP_ASSIGNED_CLIENT_ID:        return PT_STR;
        case NL_MQTT_PROP_SERVER_KEEP_ALIVE:         return PT_U16;
        case NL_MQTT_PROP_AUTH_METHOD:               return PT_STR;
        case NL_MQTT_PROP_AUTH_DATA:                 return PT_BIN;
        case NL_MQTT_PROP_REQUEST_PROBLEM_INFO:      return PT_BYTE;
        case NL_MQTT_PROP_WILL_DELAY_INTERVAL:       return PT_U32;
        case NL_MQTT_PROP_REQUEST_RESPONSE_INFO:     return PT_BYTE;
        case NL_MQTT_PROP_RESPONSE_INFO:             return PT_STR;
        case NL_MQTT_PROP_TOPIC_ALIAS_MAX:           return PT_U16;
        case NL_MQTT_PROP_TOPIC_ALIAS:               return PT_U16;
        case NL_MQTT_PROP_MAX_QOS:                   return PT_BYTE;
        case NL_MQTT_PROP_RETAIN_AVAILABLE:          return PT_BYTE;
        case NL_MQTT_PROP_USER_PROPERTY:             return PT_STRPAIR;
        case NL_MQTT_PROP_MAX_PACKET_SIZE:           return PT_U32;
        case NL_MQTT_PROP_WILDCARD_SUB_AVAILABLE:    return PT_BYTE;
        case NL_MQTT_PROP_SUB_IDENTIFIERS_AVAILABLE: return PT_BYTE;
        case NL_MQTT_PROP_SHARED_SUB_AVAILABLE:      return PT_BYTE;
        default:                                     return PT_UNKNOWN;
    }
}

// 释放本模块解析出的属性链表
static void nl_mqtt_server_props_free(nl_mqtt_property_t* props) {
    while (props) {
        nl_mqtt_property_t* n = props->next;
        free(props->str_value);
        free(props);
        props = n;
    }
}

// 解析属性段为属性链表。成功返回 0；属性格式非法返回 -1。
// 未知属性类型无法确定负载长度，保守地就地停止解析(已解析部分保留)。
static int nl_mqtt_server_parse_properties(const char* buf, size_t limit,
                                           size_t* offset,
                                           nl_mqtt_property_t** out_props) {
    if (out_props) *out_props = NULL;

    size_t prop_len = 0;
    if (nl_mqtt_server_read_varint(buf, limit, offset, &prop_len) != 0) return -1;
    if (*offset + prop_len > limit) return -1;
    size_t end = *offset + prop_len;

    while (*offset < end) {
        // 属性标识符为可变字节整数(全部标准属性 < 128，即 1 字节)
        size_t idv = 0;
        if (nl_mqtt_server_read_varint(buf, end, offset, &idv) != 0) goto fail;
        int type = (int)idv;

        nl_mqtt_property_t* prop = (nl_mqtt_property_t*)calloc(1, sizeof(*prop));
        if (!prop) goto fail;
        prop->type = type;

        switch (nl_prop_kind(type)) {
            case PT_BYTE:
                if (*offset + 1 > end) { free(prop); goto fail; }
                prop->int_value = (uint8_t)buf[(*offset)++];
                break;
            case PT_U16:
                if (*offset + 2 > end) { free(prop); goto fail; }
                prop->int_value = ((uint8_t)buf[*offset] << 8) | (uint8_t)buf[*offset + 1];
                *offset += 2;
                break;
            case PT_U32:
                if (*offset + 4 > end) { free(prop); goto fail; }
                prop->int_value = (int)(((uint32_t)(uint8_t)buf[*offset] << 24) |
                                        ((uint32_t)(uint8_t)buf[*offset + 1] << 16) |
                                        ((uint32_t)(uint8_t)buf[*offset + 2] << 8) |
                                        ((uint32_t)(uint8_t)buf[*offset + 3]));
                *offset += 4;
                break;
            case PT_VARINT: {
                size_t v = 0;
                if (nl_mqtt_server_read_varint(buf, end, offset, &v) != 0) {
                    free(prop); goto fail;
                }
                prop->int_value = (int)v;
                break;
            }
            case PT_STR:
            case PT_BIN: {
                if (*offset + 2 > end) { free(prop); goto fail; }
                size_t sl = (size_t)(((uint8_t)buf[*offset] << 8) | (uint8_t)buf[*offset + 1]);
                *offset += 2;
                if (*offset + sl > end) { free(prop); goto fail; }
                prop->str_value = (char*)malloc(sl + 1);
                if (!prop->str_value) { free(prop); goto fail; }
                if (sl) memcpy(prop->str_value, buf + *offset, sl);
                prop->str_value[sl] = '\0';
                prop->str_len = sl;
                *offset += sl;
                break;
            }
            case PT_STRPAIR: {
                // 用户属性：键值对，合并为 "key=value" 存于 str_value，int_value 记键长度
                if (*offset + 2 > end) { free(prop); goto fail; }
                size_t kl = (size_t)(((uint8_t)buf[*offset] << 8) | (uint8_t)buf[*offset + 1]);
                *offset += 2;
                if (*offset + kl + 2 > end) { free(prop); goto fail; }
                size_t vl = (size_t)(((uint8_t)buf[*offset + kl] << 8) |
                                     (uint8_t)buf[*offset + kl + 1]);
                if (*offset + kl + 2 + vl > end) { free(prop); goto fail; }
                size_t total = kl + 1 + vl;
                prop->str_value = (char*)malloc(total + 1);
                if (!prop->str_value) { free(prop); goto fail; }
                if (kl) memcpy(prop->str_value, buf + *offset, kl);
                prop->str_value[kl] = '=';
                if (vl) memcpy(prop->str_value + kl + 1, buf + *offset + kl + 2, vl);
                prop->str_value[total] = '\0';
                prop->str_len = total;
                prop->int_value = (int)kl;
                *offset += kl + 2 + vl;
                break;
            }
            default:
                // 未知属性：类型已消费，负载长度未知，停止解析(不影响已解析属性)
                free(prop);
                return 0;
        }

        prop->next = *out_props;
        *out_props = prop;
    }
    return 0;

fail:
    if (out_props) {
        nl_mqtt_server_props_free(*out_props);
        *out_props = NULL;
    }
    return -1;
}

// 在属性链表中查找指定类型的属性（首个匹配）
static nl_mqtt_property_t* nl_mqtt_server_prop_find(nl_mqtt_property_t* props,
                                                     int type) {
    for (nl_mqtt_property_t* p = props; p; p = p->next) {
        if (p->type == type) return p;
    }
    return NULL;
}

// 编码属性正文(不含长度前缀)所需字节数
static size_t nl_mqtt_server_props_body_len(nl_mqtt_property_t* props) {
    size_t n = 0;
    for (nl_mqtt_property_t* p = props; p; p = p->next) {
        switch (nl_prop_kind(p->type)) {
            case PT_BYTE:  n += 1 + 1; break;
            case PT_U16:   n += 1 + 2; break;
            case PT_U32:   n += 1 + 4; break;
            case PT_VARINT: {
                size_t v = (size_t)p->int_value, bytes = 1;
                while (v >= 128) { v /= 128; bytes++; }
                n += 1 + bytes;
                break;
            }
            case PT_STR:
            case PT_BIN:   n += 1 + 2 + p->str_len; break;
            case PT_STRPAIR: {
                size_t kl = (size_t)(p->int_value > 0 ? p->int_value : 0);
                size_t vl = p->str_len > kl + 1 ? p->str_len - kl - 1 : 0;
                n += 1 + 2 + kl + 2 + vl;
                break;
            }
            default: break;
        }
    }
    return n;
}

// 写入属性正文(不含长度前缀，标识符为 1 字节)，返回写入字节数
static size_t nl_mqtt_server_props_write(char* buf, size_t cap,
                                          nl_mqtt_property_t* props) {
    size_t pos = 0;
    for (nl_mqtt_property_t* p = props; p; p = p->next) {
        if (pos + 1 > cap) break;
        buf[pos++] = (char)(p->type & 0xFF);   // 标识符(均 <128)

        switch (nl_prop_kind(p->type)) {
            case PT_BYTE:
                if (pos + 1 > cap) return pos;
                buf[pos++] = (char)(p->int_value & 0xFF);
                break;
            case PT_U16:
                if (pos + 2 > cap) return pos;
                buf[pos++] = (char)((p->int_value >> 8) & 0xFF);
                buf[pos++] = (char)(p->int_value & 0xFF);
                break;
            case PT_U32:
                if (pos + 4 > cap) return pos;
                buf[pos++] = (char)((p->int_value >> 24) & 0xFF);
                buf[pos++] = (char)((p->int_value >> 16) & 0xFF);
                buf[pos++] = (char)((p->int_value >> 8) & 0xFF);
                buf[pos++] = (char)(p->int_value & 0xFF);
                break;
            case PT_VARINT: {
                char tmp[4];
                int n = nl_mqtt_server_encode_varint(tmp, sizeof(tmp), (size_t)p->int_value);
                if (n < 0 || pos + (size_t)n > cap) return pos;
                memcpy(buf + pos, tmp, (size_t)n);
                pos += (size_t)n;
                break;
            }
            case PT_STR:
            case PT_BIN:
                if (pos + 2 + p->str_len > cap) return pos;
                buf[pos++] = (char)((p->str_len >> 8) & 0xFF);
                buf[pos++] = (char)(p->str_len & 0xFF);
                if (p->str_len && p->str_value) memcpy(buf + pos, p->str_value, p->str_len);
                pos += p->str_len;
                break;
            case PT_STRPAIR: {
                size_t kl = (size_t)(p->int_value > 0 ? p->int_value : 0);
                size_t vl = p->str_len > kl + 1 ? p->str_len - kl - 1 : 0;
                if (pos + 2 + kl + 2 + vl > cap) return pos;
                buf[pos++] = (char)((kl >> 8) & 0xFF);
                buf[pos++] = (char)(kl & 0xFF);
                if (kl && p->str_value) memcpy(buf + pos, p->str_value, kl);
                pos += kl;
                buf[pos++] = (char)((vl >> 8) & 0xFF);
                buf[pos++] = (char)(vl & 0xFF);
                if (vl && p->str_value) memcpy(buf + pos, p->str_value + kl + 1, vl);
                pos += vl;
                break;
            }
            default: break;
        }
    }
    return pos;
}


// 校验负载是否为合法 UTF-8(用于 Payload Format Indicator=1)
static int nl_mqtt_server_utf8_valid(const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        size_t n;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        else return 0;
        if (i + n > len) return 0;
        for (size_t k = 1; k < n; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return 0;
        }
        i += n;
    }
    return 1;
}

// 统一的事件派发：构造事件(含 v5 原因码与属性)、回调后释放属性。
// props 的所有权转移给本函数；无论是否有回调都会被释放。
static void nl_mqtt_server_emit(nl_mqtt_server_client_t* client,
                                nl_mqtt_server_event_type_t type,
                                const char* topic, const void* payload,
                                size_t payload_len, int qos, int retain,
                                int reason_code, nl_mqtt_property_t* props) {
    if (!client || !client->server || !client->server->event_callback) {
        nl_mqtt_server_props_free(props);
        return;
    }
    nl_mqtt_server_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type         = type;
    ev.client_id    = client->id;
    ev.topic        = topic;
    ev.payload      = payload;
    ev.payload_len  = payload_len;
    ev.qos          = qos;
    ev.retain       = retain;
    ev.reason_code  = reason_code;
    ev.properties   = props;
    ev.user_data    = client->server->event_user_data;
    client->server->event_callback(&ev, client->server->event_user_data);
    nl_mqtt_server_props_free(props);
}

// 为已接受的连接建立服务端 TLS(握手)。未启用 TLS 时返回 0。
// 成功时把 TLS 上下文挂到 client->tls_ctx；失败返回 -1(调用方应断开该连接)。
static int nl_mqtt_server_tls_accept(nl_mqtt_server_t* srv,
                                     nl_mqtt_server_client_t* client) {
    if (!srv || !srv->tls_enabled) return 0;
#ifdef NL_MQTT_SERVER_TLS_ENABLE
    if (!srv->tls_cert_file || !srv->tls_key_file) return -1;   // 缺少服务端证书/私钥
    nl_tls_ctx_t* tls = nl_tls_create();
    if (!tls) return -1;
    nl_tls_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.is_server   = 1;
    cfg.cert_file   = srv->tls_cert_file;
    cfg.key_file    = srv->tls_key_file;
    cfg.ca_file     = srv->tls_ca_file;
    cfg.client_auth = srv->tls_client_auth;
    cfg.verify_peer = 0;
    if (nl_tls_configure(tls, &cfg) != NL_TLS_OK) { nl_tls_destroy(tls); return -1; }
    int timeout_ms = srv->tls_handshake_timeout_sec > 0
                         ? srv->tls_handshake_timeout_sec * 1000
                         : 0;
    if (nl_tls_handshake_ex(tls, (int)client->sock, timeout_ms) != NL_TLS_OK) {
        nl_tls_destroy(tls); return -1;
    }
    client->tls_ctx = tls;
    return 0;
#else
    (void)client;
    return -1;   // 构建时未启用 TLS 支持
#endif
}

// 从客户端读取数据：启用 TLS 时走 TLS，否则走裸 socket。
// 返回值语义与 recv 一致(>0 字节数；<=0 断开/无数据)。
static int nl_mqtt_server_client_recv(nl_mqtt_server_client_t* client,
                                      char* buf, size_t len) {
#ifdef NL_MQTT_SERVER_TLS_ENABLE
    if (client->tls_ctx) {
        return nl_tls_recv((nl_tls_ctx_t*)client->tls_ctx, buf, len);
    }
#endif
    return (int)recv(client->sock, buf, len, 0);
}

// 循环发送直到写完全部字节(处理部分发送)；成功返回 0
static int nl_mqtt_server_send_all(nl_mqtt_server_client_t* client,
                                   const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef NL_MQTT_SERVER_TLS_ENABLE
        if (client->tls_ctx) {
            int n = nl_tls_send((nl_tls_ctx_t*)client->tls_ctx, buf + sent, len - sent);
            if (n <= 0) return -1;
            sent += (size_t)n;
            continue;
        }
#endif
        ssize_t n = send(client->sock, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    if (client->server) client->server->stats.bytes_sent += (uint32_t)len;
    return 0;
}

// 发送 CONNACK：
//   MQTT 3.1.1 —— 无属性，剩余长度 = 2(会话标志 + 返回码)
//   MQTT 5.0   —— 含属性段，返回服务端能力属性(Session Expiry / Keep Alive /
//                 Topic Alias Maximum / Maximum QoS / Retain Available /
//                 Wildcard|SubId|Shared Subscription Available)
// 未知协议级别(protocol_version 非 5)回落到 3.1.1 的无属性格式。
static int nl_mqtt_server_send_connack(nl_mqtt_server_client_t* client, int rc,
                                        int session_present) {
    char buf[384];
    int with_props = nl_mqtt_server_is_v5(client);

    char props[256];
    size_t plen = 0;
    if (with_props) {
        uint32_t se = (uint32_t)(client->server ? client->server->session_expiry_sec : 0);
        props[plen++] = 0x11;                       // Session Expiry Interval (u32)
        props[plen++] = (char)((se >> 24) & 0xFF);
        props[plen++] = (char)((se >> 16) & 0xFF);
        props[plen++] = (char)((se >> 8) & 0xFF);
        props[plen++] = (char)(se & 0xFF);
        props[plen++] = 0x13;                       // Server Keep Alive (u16)
        props[plen++] = 0x00;
        props[plen++] = 0x3C;                       // 60 秒
        props[plen++] = 0x22;                       // Topic Alias Maximum (u16)
        props[plen++] = 0x00;
        props[plen++] = 0x0A;                       // 支持 10 个主题别名
        props[plen++] = 0x24; props[plen++] = 0x02; // Maximum QoS = 2
        props[plen++] = 0x25; props[plen++] = 0x01; // Retain Available = 1
        props[plen++] = 0x28; props[plen++] = 0x01; // Wildcard Subscription Available = 1
        props[plen++] = 0x29; props[plen++] = 0x01; // Subscription Identifiers Available = 1
        props[plen++] = 0x2A; props[plen++] = 0x00; // Shared Subscription Available = 0

        nl_mqtt_server_t* srv = client->server;
        // Receive Maximum(服务端入站在途上限)
        if (srv && srv->receive_maximum > 0) {
            props[plen++] = 0x21;
            props[plen++] = (char)((srv->receive_maximum >> 8) & 0xFF);
            props[plen++] = (char)(srv->receive_maximum & 0xFF);
        }
        // Maximum Packet Size
        if (srv && srv->max_packet_size > 0) {
            uint32_t mps = srv->max_packet_size;
            props[plen++] = 0x27;
            props[plen++] = (char)((mps >> 24) & 0xFF);
            props[plen++] = (char)((mps >> 16) & 0xFF);
            props[plen++] = (char)((mps >> 8) & 0xFF);
            props[plen++] = (char)(mps & 0xFF);
        }
        // Assigned Client Identifier(服务端分配的 client id)
        if (client->assigned_id && client->client_id) {
            size_t l = strlen(client->client_id);
            if (l > 120) l = 120;
            props[plen++] = 0x12;
            props[plen++] = (char)((l >> 8) & 0xFF);
            props[plen++] = (char)(l & 0xFF);
            memcpy(props + plen, client->client_id, l);
            plen += l;
        }
        // Server Reference(重定向)
        if (srv && srv->server_reference && srv->server_reference[0]) {
            size_t l = strlen(srv->server_reference);
            if (l > 120) l = 120;
            props[plen++] = 0x1C;
            props[plen++] = (char)((l >> 8) & 0xFF);
            props[plen++] = (char)(l & 0xFF);
            memcpy(props + plen, srv->server_reference, l);
            plen += l;
        }
    }

    char vb[4];
    int plen_bytes = 0;
    if (with_props) {
        plen_bytes = nl_mqtt_server_encode_varint(vb, sizeof(vb), plen);
        if (plen_bytes < 0) return -1;
    }

    size_t remaining = 2 + (with_props ? (size_t)plen_bytes + plen : 0);
    int hdr = nl_mqtt_server_write_fixed_header(buf, sizeof(buf), 0x20, remaining);
    if (hdr < 0) return -1;
    size_t pos = (size_t)hdr;

    buf[pos++] = (char)(session_present ? 0x01 : 0x00);  // 会话标志(Session Present)
    buf[pos++] = (char)rc;                               // 返回码 / 原因码

    if (with_props) {
        memcpy(buf + pos, vb, (size_t)plen_bytes);
        pos += (size_t)plen_bytes;
        memcpy(buf + pos, props, plen);
        pos += plen;
    }

    return nl_mqtt_server_send_all(client, buf, pos);
}

// 发送服务端发起的 DISCONNECT：
//   3.1.1 —— 仅固定头(剩余长度 0)；5.0 —— 追加原因码 + 属性长度(0)
static int nl_mqtt_server_send_disconnect(nl_mqtt_server_client_t* client,
                                           int reason_code) {
    int with_props = nl_mqtt_server_is_v5(client);
    char buf[8];
    int hdr = nl_mqtt_server_write_fixed_header(buf, sizeof(buf), 0xE0,
                                                with_props ? 2u : 0u);
    if (hdr < 0) return -1;
    size_t pos = (size_t)hdr;
    if (with_props) {
        buf[pos++] = (char)(reason_code & 0xFF);
        buf[pos++] = 0x00;   // 属性长度 0
    }
    return nl_mqtt_server_send_all(client, buf, pos);
}

// 发送 SUBACK：
//   MQTT 3.1.1 —— 仅含报文标识符 + 各返回码
//   MQTT 5.0   —— 追加 1 字节属性长度(0x00)
static int nl_mqtt_server_send_suback(nl_mqtt_server_client_t* client,
                                       uint16_t packet_id,
                                       const int* granteeds,
                                       int count) {
    if (count < 0 || count > NL_MQTT_SERVER_MAX_SUBACK_CODES) return -1;

    int with_props = nl_mqtt_server_is_v5(client);
    size_t remaining = 2 + (size_t)count;       // 报文标识符 + 返回码
    if (with_props) remaining += 1;             // 属性长度前缀

    char buf[64];
    int hdr = nl_mqtt_server_write_fixed_header(buf, sizeof(buf), 0x90, remaining);
    if (hdr < 0) return -1;
    size_t pos = (size_t)hdr;

    buf[pos++] = (char)((packet_id >> 8) & 0xFF);
    buf[pos++] = (char)(packet_id & 0xFF);
    if (with_props) buf[pos++] = 0x00;          // 属性长度 0

    for (int i = 0; i < count; i++) {
        buf[pos++] = (char)granteeds[i];
    }

    return nl_mqtt_server_send_all(client, buf, pos);
}

// 发送 UNSUBACK：
//   MQTT 3.1.1 —— 仅含报文标识符
//   MQTT 5.0   —— 追加 1 字节属性长度(0x00) + 每个过滤器 1 字节原因码(0x00 成功)
static int nl_mqtt_server_send_unsuback(nl_mqtt_server_client_t* client,
                                         uint16_t packet_id, int count) {
    if (count < 0) count = 0;
    int with_props = nl_mqtt_server_is_v5(client);
    size_t remaining = 2 + (with_props ? (size_t)1 + (size_t)count : 0);

    size_t cap = 5 + remaining;
    char* buf = (char*)malloc(cap);
    if (!buf) return -1;
    int hdr = nl_mqtt_server_write_fixed_header(buf, cap, 0xB0, remaining);
    if (hdr < 0) { free(buf); return -1; }
    size_t pos = (size_t)hdr;

    buf[pos++] = (char)((packet_id >> 8) & 0xFF);
    buf[pos++] = (char)(packet_id & 0xFF);
    if (with_props) {
        buf[pos++] = 0x00;                                    // 属性长度 0
        for (int i = 0; i < count; i++) buf[pos++] = 0x00;    // 原因码：成功
    }

    int rc = nl_mqtt_server_send_all(client, buf, pos);
    free(buf);
    return rc;
}

// 发送 QoS 确认类报文(PUBACK 0x40 / PUBREC 0x50 / PUBREL 0x62 / PUBCOMP 0x70)：
//   MQTT 3.1.1 —— 仅报文标识符；
//   MQTT 5.0   —— 追加原因码(成功=0x00) + 属性长度(0)。
static int nl_mqtt_server_send_pcm(nl_mqtt_server_client_t* client,
                                   uint8_t first_byte, uint16_t packet_id,
                                   int reason_code) {
    int with_props = nl_mqtt_server_is_v5(client);
    size_t remaining = 2 + (with_props ? 2u : 0u);
    char buf[8];
    int hdr = nl_mqtt_server_write_fixed_header(buf, sizeof(buf), first_byte, remaining);
    if (hdr < 0) return -1;
    size_t pos = (size_t)hdr;

    buf[pos++] = (char)((packet_id >> 8) & 0xFF);
    buf[pos++] = (char)(packet_id & 0xFF);
    if (with_props) {
        buf[pos++] = (char)(reason_code & 0xFF);
        buf[pos++] = 0x00;                     // 属性长度 0
    }
    return nl_mqtt_server_send_all(client, buf, pos);
}

// 发送 PUBCOMP(0x70)：QoS2 接收方向收尾确认
static int nl_mqtt_server_send_pubcomp(nl_mqtt_server_client_t* client,
                                        uint16_t packet_id) {
    return nl_mqtt_server_send_pcm(client, 0x70, packet_id, 0x00);
}

// 发送 PUBREL(0x62)：QoS2 出站方向第二步(收到订阅者 PUBREC 后发送)。
// 注意固定头低 4 位按协议固定为 0x02，故首字节为 0x62 而非 0x60。
static int nl_mqtt_server_send_pubrel(nl_mqtt_server_client_t* client,
                                       uint16_t packet_id) {
    return nl_mqtt_server_send_pcm(client, 0x62, packet_id, 0x00);
}

// 构造并发送一条 PUBLISH(服务端 -> 订阅者)。qos > 0 时携带调用方分配的 packet_id。
// fwd_props：随报文转发的原始属性(Response Topic/Correlation Data/Content Type/
//            Payload Format Indicator/User Property 等)，可为 NULL。
static int nl_mqtt_server_send_publish(nl_mqtt_server_client_t* client,
                                        const char* topic,
                                        const void* payload,
                                        size_t payload_len,
                                        int qos,
                                        int retain,
                                        uint16_t packet_id,
                                        int dup,
                                        uint32_t sub_id,
                                        uint32_t msg_expiry_sec,
                                        nl_mqtt_property_t* fwd_props) {
    if (!client || !topic) return -1;

    size_t topic_len = strlen(topic);
    int with_props = nl_mqtt_server_is_v5(client);

    size_t fwd_len = with_props ? nl_mqtt_server_props_body_len(fwd_props) : 0;

    char pv[4];
    int pvlen = 0;
    size_t pblen = 0;
    if (with_props) {
        // 本端属性长度：Message Expiry(0x02) + Subscription Identifier(0x0B)
        size_t own = 0;
        if (msg_expiry_sec > 0) own += 1 + 4;
        if (sub_id > 0) {
            char tmp[4];
            int n = nl_mqtt_server_encode_varint(tmp, sizeof(tmp), sub_id);
            own += 1 + (n > 0 ? (size_t)n : 1);
        }
        pblen = own + fwd_len;
        pvlen = nl_mqtt_server_encode_varint(pv, sizeof(pv), pblen);
        if (pvlen < 0) return -1;
    }

    size_t remaining = 2 + topic_len + (qos > 0 ? 2 : 0)
                     + (with_props ? (size_t)pvlen + pblen : 0) + payload_len;

    size_t cap = 5 + remaining;
    char* buf = (char*)malloc(cap);
    if (!buf) return -1;

    uint8_t first = (uint8_t)(0x30 | (dup ? 0x08 : 0x00) |
                              ((qos & 0x03) << 1) | (retain ? 0x01 : 0x00));
    int hdr = nl_mqtt_server_write_fixed_header(buf, cap, first, remaining);
    if (hdr < 0) { free(buf); return -1; }
    size_t pos = (size_t)hdr;

    buf[pos++] = (char)((topic_len >> 8) & 0xFF);
    buf[pos++] = (char)(topic_len & 0xFF);
    memcpy(buf + pos, topic, topic_len);
    pos += topic_len;

    if (qos > 0) {
        buf[pos++] = (char)((packet_id >> 8) & 0xFF);
        buf[pos++] = (char)(packet_id & 0xFF);
    }

    if (with_props) {
        memcpy(buf + pos, pv, (size_t)pvlen);
        pos += (size_t)pvlen;
        if (msg_expiry_sec > 0) {
            buf[pos++] = 0x02;
            buf[pos++] = (char)((msg_expiry_sec >> 24) & 0xFF);
            buf[pos++] = (char)((msg_expiry_sec >> 16) & 0xFF);
            buf[pos++] = (char)((msg_expiry_sec >> 8) & 0xFF);
            buf[pos++] = (char)(msg_expiry_sec & 0xFF);
        }
        if (sub_id > 0) {
            buf[pos++] = 0x0B;
            int n = nl_mqtt_server_encode_varint(buf + pos, cap - pos, sub_id);
            if (n < 0) { free(buf); return -1; }
            pos += (size_t)n;
        }
        pos += nl_mqtt_server_props_write(buf + pos, cap - pos, fwd_props);
    }

    if (payload_len > 0 && payload) {
        memcpy(buf + pos, payload, payload_len);
        pos += payload_len;
    }

    // 遵守对端声明的 Maximum Packet Size：超出则不发送该报文
    if (client->max_packet_size > 0 && pos > client->max_packet_size) {
        free(buf);
        return -1;
    }

    int rc = nl_mqtt_server_send_all(client, buf, pos);
    free(buf);
    return rc;
}

// 分配下一个非零报文标识符(0 为非法值，回绕时跳过)
static uint16_t nl_mqtt_server_next_pid(nl_mqtt_server_client_t* client) {
    if (++client->next_packet_id == 0) client->next_packet_id = 1;
    return client->next_packet_id;
}

// ============================================================
// QoS2 入站状态跟踪（保证 exactly-once 投递）
// 语义：收到 QoS2 PUBLISH -> 记录 packet_id 并回 PUBREC；
//       收到 PUBREL -> 移除 packet_id 并回 PUBCOMP。
//       若在 PUBREL 之前重传同一 packet_id 的 PUBLISH(DUP)，视为重复，
//       不再向上层/订阅者重复投递，仅重发 PUBREC。
// ============================================================

// 注：入站 QoS2 待确认集合与出站未确认消息均已迁移到会话（session）中，
//     以支持 clean_session=0 的跨连接持久化与统一重传。相关辅助见上文会话管理区块。

// ============================================================
// 真实下发：向所有匹配订阅者发送 PUBLISH
// 每个订阅者按其“最高匹配订阅 QoS”与“发布 QoS”的较小值下发；
// QoS>0 会登记到该客户端会话的未确认消息表，用于超时重传与会话恢复。
// ============================================================

// 某客户端在途未确认消息数(出站 QoS1/2)
static int nl_mqtt_server_inflight_count(nl_mqtt_server_client_t* c) {
    int n = 0;
    if (c && c->session) {
        for (nl_mqtt_out_msg_t* m = c->session->out_msgs; m; m = m->next) n++;
    }
    return n;
}

// 会话队列配额：超过上限时丢弃最旧(链表尾部)消息
static void nl_mqtt_server_session_quota_enforce(nl_mqtt_server_session_t* s, int limit) {
    if (!s || limit <= 0) return;
    size_t count = 0;
    for (nl_mqtt_out_msg_t* m = s->out_msgs; m; m = m->next) count++;
    while (count >= (size_t)limit && s->out_msgs) {
        nl_mqtt_out_msg_t* prev = NULL;
        nl_mqtt_out_msg_t* cur  = s->out_msgs;
        while (cur->next) { prev = cur; cur = cur->next; }
        if (prev) prev->next = NULL;
        else      s->out_msgs = NULL;
        nl_mqtt_server_free_out_msg(cur);
        s->dirty = 1;
        count--;
    }
}

// 向单个订阅者投递一条 PUBLISH，并按需登记未确认消息。返回 1 表示已投递。
static int nl_mqtt_server_deliver(nl_mqtt_server_t* srv, nl_mqtt_server_client_t* target,
                                  const char* topic, const void* payload, size_t payload_len,
                                  int qos, int retain, int best_qos, uint32_t sub_id,
                                  int rap, long long expires, uint32_t msg_expiry_sec,
                                  nl_mqtt_property_t* fwd_props) {
    int eff_qos = qos < best_qos ? qos : best_qos;
    if (eff_qos < 0) eff_qos = 0;
    int out_retain = (retain && rap) ? 1 : 0;   // Retain As Published

    // 出站流控：在途已达对端 Receive Maximum -> 延后发送
    int defer = 0;
    if (eff_qos > 0 && target->receive_maximum > 0 &&
        nl_mqtt_server_inflight_count(target) >= (int)target->receive_maximum) {
        defer = 1;
    }

    uint16_t pid = 0;
    if (eff_qos > 0) pid = nl_mqtt_server_next_pid(target);

    if (!defer) {
        if (nl_mqtt_server_send_publish(target, topic, payload, payload_len, eff_qos,
                                        out_retain, pid, 0, sub_id, msg_expiry_sec,
                                        fwd_props) != 0) {
            return 0;
        }
    }
    if (eff_qos > 0 && target->session) {
        if (srv->max_queued_messages > 0)
            nl_mqtt_server_session_quota_enforce(target->session, srv->max_queued_messages);
        nl_mqtt_out_msg_t* m = nl_mqtt_server_out_msg_add(target->session, pid,
                                 (uint8_t)eff_qos, topic, payload, payload_len,
                                 out_retain, sub_id, expires, 0);
        if (m && defer) { m->pending = 1; m->last_sent_ms = 0; }
    }
    return 1;
}

static int nl_mqtt_server_broadcast(nl_mqtt_server_t* srv,
                                     const char* topic,
                                     const void* payload,
                                     size_t payload_len,
                                     int qos,
                                     int retain,
                                     uint32_t publisher_id,
                                     uint32_t msg_expiry_sec,
                                     nl_mqtt_property_t* fwd_props) {
    if (!srv || !topic) return 0;

    long long now = nl_now_ms();
    long long expires = msg_expiry_sec > 0 ? now + (long long)msg_expiry_sec * 1000 : 0;
    int delivered = 0;

    // 1) 普通订阅(非共享)：每个订阅者按其最高匹配订阅下发一次
    for (nl_mqtt_server_client_t* target = srv->clients; target; target = target->next) {
        if (!target->connected || !target->client_id) continue;

        int best_qos = -1;
        uint32_t best_sub_id = 0;
        int best_rap = 0;
        for (nl_mqtt_server_subscription_t* sub = srv->subscriptions; sub; sub = sub->next) {
            if (sub->share_group || !sub->owner_client_id) continue;
            if (strcmp(sub->owner_client_id, target->client_id) != 0) continue;
            if (!nl_mqtt_topic_matches(sub->topic, topic)) continue;
            if (sub->no_local && target->id == publisher_id) continue;   // No Local
            if (sub->qos > best_qos) {
                best_qos = sub->qos;
                best_sub_id = sub->sub_id;
                best_rap = sub->rap;
            }
        }
        if (best_qos < 0) continue;

        delivered += nl_mqtt_server_deliver(srv, target, topic, payload, payload_len,
                                            qos, retain, best_qos, best_sub_id, best_rap,
                                            expires, msg_expiry_sec, fwd_props);
    }

    // 2) 共享订阅：按 (组名, 过滤器) 归组，每组轮询选一个在线成员投递一次
    for (nl_mqtt_server_subscription_t* g = srv->subscriptions; g; g = g->next) {
        if (!g->share_group || !g->owner_client_id) continue;
        if (!nl_mqtt_topic_matches(g->topic, topic)) continue;

        int dup_group = 0;
        for (nl_mqtt_server_subscription_t* q = srv->subscriptions; q != g; q = q->next) {
            if (q->share_group && q->owner_client_id &&
                strcmp(q->share_group, g->share_group) == 0 &&
                strcmp(q->topic, g->topic) == 0) { dup_group = 1; break; }
        }
        if (dup_group) continue;

        nl_mqtt_server_client_t* members[64];
        int n = 0;
        for (nl_mqtt_server_subscription_t* q = srv->subscriptions; q && n < 64; q = q->next) {
            if (!q->share_group || !q->owner_client_id) continue;
            if (strcmp(q->share_group, g->share_group) != 0) continue;
            if (strcmp(q->topic, g->topic) != 0) continue;
            for (nl_mqtt_server_client_t* c = srv->clients; c; c = c->next) {
                if (!c->connected || !c->client_id) continue;
                if (strcmp(c->client_id, q->owner_client_id) != 0) continue;
                if (q->no_local && c->id == publisher_id) continue;
                members[n++] = c;
                break;
            }
        }
        if (n == 0) continue;

        int idx = (int)(srv->share_rr++ % (uint32_t)n);
        nl_mqtt_server_client_t* chosen = members[idx];
        uint32_t sub_id = 0;
        int sqos = 0;
        for (nl_mqtt_server_subscription_t* q = srv->subscriptions; q; q = q->next) {
            if (q->share_group && q->owner_client_id &&
                strcmp(q->share_group, g->share_group) == 0 &&
                strcmp(q->topic, g->topic) == 0 &&
                strcmp(q->owner_client_id, chosen->client_id) == 0) {
                sub_id = q->sub_id;
                sqos = q->qos;
                break;
            }
        }
        delivered += nl_mqtt_server_deliver(srv, chosen, topic, payload, payload_len,
                                            qos, retain, sqos, sub_id, 0,
                                            expires, msg_expiry_sec, fwd_props);
    }

    return delivered;
}

// 向某订阅者补发与其订阅过滤器匹配的全部保留消息(新订阅时调用)。
// 下发 QoS = min(订阅授予 QoS, 保留消息 QoS)，并置 RETAIN=1；
// 已过期的保留消息(v5 Message Expiry)跳过；sub_id 为该订阅的 v5 订阅标识符。
static void nl_mqtt_server_send_retained(nl_mqtt_server_client_t* client,
                                         const char* filter, int granted_qos,
                                         uint32_t sub_id) {
    if (!client || !client->server || !filter) return;
    if (granted_qos < 0 || granted_qos > 2) return;

    nl_mqtt_server_t* srv = client->server;
    long long now = nl_now_ms();
    for (nl_mqtt_retained_msg_t* r = srv->retained; r; r = r->next) {
        if (!nl_mqtt_topic_matches(filter, r->topic)) continue;
        if (r->expires_ms && now >= r->expires_ms) continue;   // 已过期

        int eff_qos = r->qos < granted_qos ? r->qos : granted_qos;
        uint32_t rem_exp = 0;
        if (r->expires_ms) {
            long long left = r->expires_ms - now;
            rem_exp = left > 0 ? (uint32_t)((left + 999) / 1000) : 0;
        }
        uint16_t pid = 0;
        if (eff_qos > 0) pid = nl_mqtt_server_next_pid(client);

        if (nl_mqtt_server_send_publish(client, r->topic, r->payload, r->payload_len,
                                        eff_qos, 1 /*retain*/, pid, 0,
                                        sub_id, rem_exp, NULL) != 0) {
            continue;
        }
        if (eff_qos > 0 && client->session) {
            nl_mqtt_server_out_msg_add(client->session, pid, (uint8_t)eff_qos,
                                       r->topic, r->payload, r->payload_len, 1,
                                       sub_id, r->expires_ms, 0);
        }
    }
}

// 按“主题/负载”发布一条遗嘱(供立即发布与延迟到期复用)
static void nl_mqtt_server_emit_will(nl_mqtt_server_t* srv, const char* topic,
                                     const void* payload, size_t payload_len,
                                     int qos, int retain, uint32_t msg_expiry,
                                     uint32_t exclude_client_id) {
    if (!srv || !topic) return;
    if (retain) {
        nl_mqtt_server_set_retained(srv, topic, payload, payload_len, qos, msg_expiry);
    }
    srv->stats.total_publishes++;
    nl_mqtt_server_broadcast(srv, topic, payload, payload_len, qos,
                             retain ? 1 : 0, exclude_client_id, msg_expiry, NULL);
}

// 立即发布某连接的遗嘱(Will Delay=0 或会话已结束的路径)
static void nl_mqtt_server_publish_will(nl_mqtt_server_t* srv,
                                        nl_mqtt_server_client_t* client) {
    if (!srv || !client || !client->has_will || !client->will_topic) return;
    nl_mqtt_server_emit_will(srv, client->will_topic, client->will_payload,
                             client->will_payload_len, client->will_qos,
                             client->will_retain, client->will_message_expiry,
                             client->id);
}

// ---- 延迟遗嘱队列(v5 Will Delay Interval) ----
static void nl_mqtt_server_pending_will_add(nl_mqtt_server_t* srv,
                                            nl_mqtt_server_client_t* client,
                                            long long delay_ms) {
    if (!srv || !client || !client->has_will || !client->will_topic) return;
    nl_mqtt_will_pending_t* w = (nl_mqtt_will_pending_t*)calloc(1, sizeof(*w));
    if (!w) return;
    w->client_id   = client->client_id ? strdup(client->client_id) : NULL;
    w->topic       = strdup(client->will_topic);
    w->payload_len = client->will_payload_len;
    if (w->payload_len && client->will_payload) {
        w->payload = malloc(w->payload_len);
        if (w->payload) memcpy(w->payload, client->will_payload, w->payload_len);
    }
    w->qos    = client->will_qos;
    w->retain = client->will_retain;
    w->due_ms = nl_now_ms() + delay_ms;
    w->next   = srv->pending_wills;
    srv->pending_wills = w;
}

// 取消某 client_id 的待发布遗嘱(重连时调用)
static void nl_mqtt_server_pending_will_cancel(nl_mqtt_server_t* srv,
                                               const char* client_id) {
    if (!srv || !client_id) return;
    nl_mqtt_will_pending_t* prev = NULL;
    nl_mqtt_will_pending_t* cur  = srv->pending_wills;
    while (cur) {
        nl_mqtt_will_pending_t* next = cur->next;
        if (cur->client_id && strcmp(cur->client_id, client_id) == 0) {
            if (prev) prev->next = next;
            else      srv->pending_wills = next;
            free(cur->client_id);
            free(cur->topic);
            free(cur->payload);
            free(cur);
        } else {
            prev = cur;
        }
        cur = next;
    }
}

// 发布所有到期的延迟遗嘱(由 poll 周期调用)，返回发布条数
static int nl_mqtt_server_pending_will_flush(nl_mqtt_server_t* srv, long long now_ms) {
    if (!srv) return 0;
    int published = 0;
    nl_mqtt_will_pending_t* prev = NULL;
    nl_mqtt_will_pending_t* cur  = srv->pending_wills;
    while (cur) {
        nl_mqtt_will_pending_t* next = cur->next;
        if (now_ms >= cur->due_ms) {
            nl_mqtt_server_emit_will(srv, cur->topic, cur->payload, cur->payload_len,
                                     cur->qos, cur->retain, 0, 0);
            published++;
            if (prev) prev->next = next;
            else      srv->pending_wills = next;
            free(cur->client_id);
            free(cur->topic);
            free(cur->payload);
            free(cur);
        } else {
            prev = cur;
        }
        cur = next;
    }
    return published;
}

static void nl_mqtt_server_free_pending_wills(nl_mqtt_server_t* srv) {
    if (!srv) return;
    nl_mqtt_will_pending_t* w = srv->pending_wills;
    while (w) {
        nl_mqtt_will_pending_t* next = w->next;
        free(w->client_id);
        free(w->topic);
        free(w->payload);
        free(w);
        w = next;
    }
    srv->pending_wills = NULL;
}

static int nl_mqtt_server_handle_connect(nl_mqtt_server_client_t* client,
                                          const char* buf, size_t len) {
    size_t offset = 0;
    if (len < 2) return -1;

    // 固定头首字节必须是 CONNECT(0x10)
    uint8_t fixed = (uint8_t)buf[offset++];
    if ((fixed >> 4) != NL_MQTT_CONNECT) return -1;

    // 解析剩余长度(变长整数)，并以此确定报文绝对边界
    size_t remaining_len = 0;
    if (nl_mqtt_server_read_varint(buf, len, &offset, &remaining_len) != 0) return -1;
    if (offset + remaining_len > len) return -1;
    size_t packet_end = offset + remaining_len;   // 绝对边界基准

    // 协议名(UTF-8 字符串)
    if (offset + 2 > packet_end) return -1;
    uint16_t protocol_name_len =
        (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
    offset += 2;
    if (offset + protocol_name_len > packet_end) return -1;

    int name_is_mqtt = (protocol_name_len == 4 &&
                        memcmp(buf + offset, "MQTT", 4) == 0);
    offset += protocol_name_len;

    // 协议级别(Protocol Level)
    if (offset + 1 > packet_end) return -1;
    uint8_t protocol_level = (uint8_t)buf[offset++];

    // 仅支持：4 = MQTT 3.1.1，5 = MQTT 5.0；其余(含 MQIsdp/3.1)一律拒绝
    if (!name_is_mqtt ||
        (protocol_level != NL_MQTT_SERVER_PROTO_LEVEL_V3_1_1 &&
         protocol_level != NL_MQTT_SERVER_PROTO_LEVEL_V5)) {
        client->protocol_version = 0;              // 未知级别 -> 无属性格式
        nl_mqtt_server_send_connack(client, 0x01, 0); // 0x01 = 不支持的协议版本
        return -3;                                 // 通知调用方断开连接
    }

    // 记录该连接的协议级别，后续编码/解析据此分发
    client->protocol_version = protocol_level;

    // 连接标志
    if (offset + 1 > packet_end) return -1;
    uint8_t connect_flags = (uint8_t)buf[offset++];
    client->clean_session = (connect_flags >> 1) & 0x01;

    // 保活时间
    if (offset + 2 > packet_end) return -1;
    client->keep_alive =
        (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
    offset += 2;

    // MQTT 5.0 CONNECT 属性：解析并透传；提取会话过期/主题别名上限
    nl_mqtt_property_t* conn_props = NULL;
    if (nl_mqtt_server_is_v5(client)) {
        if (nl_mqtt_server_parse_properties(buf, packet_end, &offset, &conn_props) != 0)
            return -1;
        nl_mqtt_property_t* p;
        if ((p = nl_mqtt_server_prop_find(conn_props, NL_MQTT_PROP_SESSION_EXPIRY_INTERVAL))) {
            client->session_expiry_interval = (uint32_t)p->int_value;
            client->session_expiry_set = 1;
        }
        if ((p = nl_mqtt_server_prop_find(conn_props, NL_MQTT_PROP_TOPIC_ALIAS_MAX)))
            client->topic_alias_max = (uint32_t)p->int_value;
        if ((p = nl_mqtt_server_prop_find(conn_props, NL_MQTT_PROP_RECEIVE_MAXIMUM)))
            client->receive_maximum = (uint16_t)p->int_value;
        if ((p = nl_mqtt_server_prop_find(conn_props, NL_MQTT_PROP_MAX_PACKET_SIZE)))
            client->max_packet_size = (uint32_t)p->int_value;
        if ((p = nl_mqtt_server_prop_find(conn_props, NL_MQTT_PROP_AUTH_METHOD)))
            client->auth_method = strdup(p->str_value ? p->str_value : "");
    }

    // 客户端标识符
    if (offset + 2 > packet_end) goto conn_fail;
    uint16_t client_id_len =
        (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
    offset += 2;
    if (offset + client_id_len > packet_end) goto conn_fail;

    client->client_id = (char*)malloc((size_t)client_id_len + 1);
    if (!client->client_id) goto conn_fail;
    memcpy(client->client_id, buf + offset, client_id_len);
    client->client_id[client_id_len] = '\0';
    offset += client_id_len;

    // 零长度 client id：clean_session=0 拒绝(0x02)；否则由服务端分配(Assigned Client Identifier)
    if (client_id_len == 0) {
        if (!client->clean_session) {
            nl_mqtt_server_send_connack(client, 0x02, 0);   // Identifier rejected
            nl_mqtt_server_props_free(conn_props);
            return -4;
        }
        char auto_id[40];
        snprintf(auto_id, sizeof(auto_id), "auto-%u", client->id);
        free(client->client_id);
        client->client_id = strdup(auto_id);
        if (!client->client_id) goto conn_fail;
        client->assigned_id = 1;
    }

    // 遗嘱(Will)：解析 will 属性(v5)与 will topic/payload、QoS、Retain 标志
    if (connect_flags & 0x04) {
        int will_qos    = (connect_flags >> 3) & 0x03;
        int will_retain = (connect_flags >> 5) & 0x01;
        if (will_qos > 2) goto conn_fail;   // 非法遗嘱 QoS

        if (nl_mqtt_server_is_v5(client)) {
            nl_mqtt_property_t* will_props = NULL;
            if (nl_mqtt_server_parse_properties(buf, packet_end, &offset, &will_props) != 0)
                goto conn_fail;
            nl_mqtt_property_t* wp;
            if ((wp = nl_mqtt_server_prop_find(will_props, NL_MQTT_PROP_WILL_DELAY_INTERVAL)))
                client->will_delay_interval = (uint32_t)wp->int_value;
            if ((wp = nl_mqtt_server_prop_find(will_props, NL_MQTT_PROP_MESSAGE_EXPIRY_INTERVAL)))
                client->will_message_expiry = (uint32_t)wp->int_value;
            nl_mqtt_server_props_free(will_props);
        }
        if (offset + 2 > packet_end) goto conn_fail;
        uint16_t will_topic_len =
            (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
        offset += 2;
        if (will_topic_len == 0 || offset + will_topic_len > packet_end) goto conn_fail;
        client->will_topic = (char*)malloc((size_t)will_topic_len + 1);
        if (!client->will_topic) goto conn_fail;
        memcpy(client->will_topic, buf + offset, will_topic_len);
        client->will_topic[will_topic_len] = '\0';
        offset += will_topic_len;

        if (offset + 2 > packet_end) goto conn_fail;
        uint16_t will_msg_len =
            (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
        offset += 2;
        if (offset + will_msg_len > packet_end) goto conn_fail;
        if (will_msg_len > 0) {
            client->will_payload = malloc((size_t)will_msg_len);
            if (!client->will_payload) goto conn_fail;
            memcpy(client->will_payload, buf + offset, will_msg_len);
        }
        client->will_payload_len = will_msg_len;
        client->will_qos         = will_qos;
        client->will_retain      = will_retain;
        client->has_will         = 1;
        offset += will_msg_len;
    }

    // 用户名
    if (connect_flags & 0x80) {
        if (offset + 2 > packet_end) goto conn_fail;
        uint16_t username_len =
            (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
        offset += 2;
        if (offset + username_len > packet_end) goto conn_fail;
        client->username = (char*)malloc((size_t)username_len + 1);
        if (!client->username) goto conn_fail;
        memcpy(client->username, buf + offset, username_len);
        client->username[username_len] = '\0';
        offset += username_len;
    }

    // 密码
    if (connect_flags & 0x40) {
        if (offset + 2 > packet_end) goto conn_fail;
        uint16_t password_len =
            (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
        offset += 2;
        if (offset + password_len > packet_end) goto conn_fail;
        client->password = (char*)malloc((size_t)password_len + 1);
        if (!client->password) goto conn_fail;
        memcpy(client->password, buf + offset, password_len);
        client->password[password_len] = '\0';
        offset += password_len;
    }

    // 认证：失败时回 CONNACK 并断开
    if ((connect_flags & 0x80) && client->server && client->server->auth_callback) {
        if (client->server->auth_callback(client->client_id, client->username,
                                          client->password,
                                          client->server->auth_user_data) != 0) {
            nl_mqtt_server_send_connack(client, 4, 0);  // 4 = 用户名或密码错误
            nl_mqtt_server_props_free(conn_props);
            return -4;
        }
    }

    // 增强认证：声明了认证方法但服务端未配置认证回调 -> 不支持的认证方法(0x8C)
    if (client->auth_method && client->auth_method[0] &&
        !(client->server && client->server->auth_callback)) {
        nl_mqtt_server_send_connack(client, 0x8C, 0);
        nl_mqtt_server_props_free(conn_props);
        return -4;
    }

    // 建立/恢复会话（v5 会话过期优先；3.1.1 由 clean_session 决定）
    int session_present = 0;
    client->session = nl_mqtt_server_acquire_session(client->server,
                                                     client->client_id,
                                                     client->clean_session,
                                                     client->id,
                                                     client->session_expiry_interval,
                                                     client->session_expiry_set,
                                                     &session_present);
    if (!client->session) goto conn_fail;

    // 会话恢复时，把既有订阅重新指向本条新连接
    if (session_present) {
        nl_mqtt_server_rebind_subscriptions(client->server, client->client_id, client->id);
    }

    // 重连：取消其待发布的延迟遗嘱(v5 Will Delay —— 到期前重连则不发布)
    nl_mqtt_server_pending_will_cancel(client->server, client->client_id);

    // 回复 CONNACK(0x00 = 连接已接受)
    if (nl_mqtt_server_send_connack(client, 0, session_present) != 0) goto conn_fail;

    // 通知上层有新客户端接入(透传 v5 CONNECT 属性；所有权转交 emit 释放)
    nl_mqtt_server_emit(client, NL_MQTT_SERVER_EVT_CONNECT, NULL, NULL, 0, 0, 0, 0,
                        conn_props);
    return 0;

conn_fail:
    nl_mqtt_server_props_free(conn_props);
    return -1;
}

static int nl_mqtt_server_handle_publish(nl_mqtt_server_client_t* client,
                                          const char* buf, size_t len) {
    size_t offset = 0;
    if (len < 2) return -1;

    // 固定头首字节：报文类型(0x3) + DUP/QoS/RETAIN 标志
    uint8_t first_byte = (uint8_t)buf[offset++];
    int qos = (first_byte >> 1) & 0x03;
    int retain = first_byte & 0x01;

    // QoS 3 非法(保留值) -> 协议错误
    if (qos == 3) {
        nl_mqtt_server_send_disconnect(client, 0x81);   // Malformed Packet
        return -1;
    }

    // 解析剩余长度(变长整数)，并确定报文绝对边界
    size_t remaining_len = 0;
    if (nl_mqtt_server_read_varint(buf, len, &offset, &remaining_len) != 0) return -1;
    if (offset + remaining_len > len) return -1;
    size_t packet_end = offset + remaining_len;   // 绝对边界基准

    // 主题名(可为空：v5 允许用主题别名表示)
    if (offset + 2 > packet_end) return -1;
    uint16_t topic_len =
        (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
    offset += 2;
    if (offset + topic_len > packet_end) return -1;

    char* topic = NULL;
    if (topic_len > 0) {
        topic = (char*)malloc((size_t)topic_len + 1);
        if (!topic) return -1;
        memcpy(topic, buf + offset, topic_len);
        topic[topic_len] = '\0';
    }
    offset += topic_len;

    // 报文标识符(仅 QoS > 0 时存在)
    uint16_t packet_id = 0;
    if (qos > 0) {
        if (offset + 2 > packet_end) { free(topic); return -1; }
        packet_id = (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
        offset += 2;
    }

    // MQTT 5.0 PUBLISH 属性：解析并透传；提取消息过期与主题别名
    nl_mqtt_property_t* props = NULL;
    if (nl_mqtt_server_is_v5(client)) {
        if (nl_mqtt_server_parse_properties(buf, packet_end, &offset, &props) != 0) {
            free(topic);
            return -1;
        }
    }

    uint32_t msg_expiry = 0;
    nl_mqtt_property_t* pe = nl_mqtt_server_prop_find(props, NL_MQTT_PROP_MESSAGE_EXPIRY_INTERVAL);
    if (pe) msg_expiry = (uint32_t)pe->int_value;

    nl_mqtt_property_t* pa = nl_mqtt_server_prop_find(props, NL_MQTT_PROP_TOPIC_ALIAS);
    if (pa) {
        uint16_t alias = (uint16_t)pa->int_value;
        if (alias == 0 || alias > 10) {                     // 主题别名超出服务端上限
            nl_mqtt_server_send_disconnect(client, 0x94);   // Topic Alias invalid
            nl_mqtt_server_props_free(props);
            free(topic);
            return -1;
        }
        if (topic) {
            nl_mqtt_server_topic_alias_set(client, alias, topic);   // 建立别名映射
        } else {
            const char* mapped = nl_mqtt_server_topic_alias_get(client, alias);
            if (!mapped) { nl_mqtt_server_props_free(props); return -1; }
            topic = strdup(mapped);                                 // 由别名解析主题
            if (!topic) { nl_mqtt_server_props_free(props); return -1; }
        }
    }
    if (!topic) { nl_mqtt_server_props_free(props); return -1; }    // 空主题且无别名

    // 负载长度 = 报文绝对边界 - 已消费的可变头绝对偏移
    size_t payload_len = packet_end - offset;
    const void* payload = buf + offset;

    // Payload Format Indicator：声明为 UTF-8 时必须校验负载
    nl_mqtt_property_t* ppfi =
        nl_mqtt_server_prop_find(props, NL_MQTT_PROP_PAYLOAD_FORMAT_INDICATOR);
    if (ppfi && ppfi->int_value == 1 && !nl_mqtt_server_utf8_valid(payload, payload_len)) {
        nl_mqtt_server_send_disconnect(client, 0x99);   // Payload format invalid
        nl_mqtt_server_props_free(props);
        free(topic);
        return -1;
    }

    // 入站流控：QoS2 待 PUBREL 数量不得超过服务端 Receive Maximum
    if (qos == 2 && client->server && client->server->receive_maximum > 0 &&
        client->session &&
        client->session->qos2_inbound_count >= (size_t)client->server->receive_maximum) {
        nl_mqtt_server_send_disconnect(client, 0x93);   // Receive Maximum exceeded
        nl_mqtt_server_props_free(props);
        free(topic);
        return -1;
    }

    // QoS2 去重：在收到 PUBREL 之前，同一 packet_id 的重传(DUP)不再重复投递
    int dup_qos2 = (qos == 2 &&
                    nl_mqtt_server_session_qos2_contains(client->session, packet_id));

    int matched = 0;
    if (!dup_qos2) {
        if (client->server) {
            client->server->stats.total_publishes++;
        }

        // 广播必须在释放 topic 之前进行(否则会触发 use-after-free)
        // 返回值 = 匹配订阅者数，用于 QoS1/2 确认的原因码(0x10 = 无匹配订阅)
        matched = nl_mqtt_server_broadcast(client->server, topic, payload, payload_len,
                                            qos, retain, client->id, msg_expiry, props);
        matched = matched > 0 ? 1 : 0;

        // 保留消息：retain=1 时保存(负载为空则清除)，供后续新订阅补发
        if (retain) {
            nl_mqtt_server_set_retained(client->server, topic, payload, payload_len,
                                        qos, msg_expiry);
        }

        // 触发事件回调(透传 v5 属性；所有权转交 emit 释放)
        nl_mqtt_server_emit(client, NL_MQTT_SERVER_EVT_PUBLISH, topic, payload,
                            payload_len, qos, retain, 0, props);
        props = NULL;
    }
    nl_mqtt_server_props_free(props);   // 重复 QoS2 路径：直接释放

    // QoS2：登记待确认标识符(须在回 PUBREC 之前)。
    // 重复 PUBLISH 亦调用：对已存在记录仅刷新接收时刻，避免客户端持续重传期间
    // 该记录被入站 QoS2 超时误清理，从而导致同一消息被重复投递。
    if (qos == 2 && client->session) {
        nl_mqtt_server_session_qos2_add(client->session, packet_id);
    }

    // 按 QoS 回复确认：仅当“首次且无匹配订阅”时原因码为 0x10(No matching subscribers)
    int reason = (qos > 0 && !dup_qos2 && matched == 0) ? 0x10 : 0x00;
    if (qos == 1) {
        nl_mqtt_server_send_pcm(client, 0x40, packet_id, reason);
    } else if (qos == 2) {
        nl_mqtt_server_send_pcm(client, 0x50, packet_id, reason);
    }

    free(topic);
    return 0;
}

// 向订阅了 $SYS 主题的客户端补发若干服务端统计($SYS/broker/...)，retained=1。
static void nl_mqtt_server_publish_sys(nl_mqtt_server_client_t* client,
                                       const char* filter) {
    if (!client || !client->server || !filter) return;
    nl_mqtt_server_t* srv = client->server;
    char v0[24], v1[24], v2[24], v3[24];
    snprintf(v0, sizeof(v0), "%u", srv->stats.total_connections);
    snprintf(v1, sizeof(v1), "%u", srv->stats.total_publishes);
    snprintf(v2, sizeof(v2), "%u", srv->stats.total_subscriptions);
    snprintf(v3, sizeof(v3), "%u", srv->stats.total_disconnections);
    const char* t[4] = { "$SYS/broker/clients/total",
                         "$SYS/broker/messages/received",
                         "$SYS/broker/subscriptions/count",
                         "$SYS/broker/clients/disconnected" };
    const char* v[4] = { v0, v1, v2, v3 };
    for (int i = 0; i < 4; i++) {
        if (!nl_mqtt_topic_matches(filter, t[i])) continue;
        nl_mqtt_server_send_publish(client, t[i], v[i], strlen(v[i]), 0, 1, 0, 0, 0, 0,
                                    NULL);
    }
}

static int nl_mqtt_server_handle_subscribe(nl_mqtt_server_client_t* client,
                                            const char* buf, size_t len) {
    size_t offset = 0;
    if (len < 2) return -1;

    offset++;  // 固定头首字节(SUBSCRIBE 固定为 0x82)

    // 剩余长度 -> 报文绝对边界
    size_t remaining_len = 0;
    if (nl_mqtt_server_read_varint(buf, len, &offset, &remaining_len) != 0) return -1;
    if (offset + remaining_len > len) return -1;
    size_t packet_end = offset + remaining_len;

    // 报文标识符
    if (offset + 2 > packet_end) return -1;
    uint16_t packet_id =
        (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
    offset += 2;

    // MQTT 5.0 SUBSCRIBE 属性：解析并透传；提取订阅标识符
    nl_mqtt_property_t* props = NULL;
    if (nl_mqtt_server_is_v5(client)) {
        if (nl_mqtt_server_parse_properties(buf, packet_end, &offset, &props) != 0)
            return -1;
    }
    uint32_t sub_id = 0;
    nl_mqtt_property_t* ps =
        nl_mqtt_server_prop_find(props, NL_MQTT_PROP_SUBSCRIPTION_IDENTIFIER);
    if (ps) sub_id = (uint32_t)ps->int_value;

    // 逐个解析「主题过滤器 + 订阅选项」
    int grant_count = 0;
    int granteeds[NL_MQTT_SERVER_MAX_SUBACK_CODES];
    char* filters[NL_MQTT_SERVER_MAX_SUBACK_CODES];
    int is_new_arr[NL_MQTT_SERVER_MAX_SUBACK_CODES];
    int rh_arr[NL_MQTT_SERVER_MAX_SUBACK_CODES];

    while (offset < packet_end && grant_count < NL_MQTT_SERVER_MAX_SUBACK_CODES) {
        if (offset + 2 > packet_end) break;
        uint16_t topic_len =
            (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
        offset += 2;
        if (topic_len == 0 || offset + topic_len > packet_end) break;

        char* topic = (char*)malloc((size_t)topic_len + 1);
        if (!topic) break;
        memcpy(topic, buf + offset, topic_len);
        topic[topic_len] = '\0';
        offset += topic_len;

        if (offset >= packet_end) { free(topic); break; }
        uint8_t opt = (uint8_t)buf[offset++];
        uint8_t requested_qos = opt & 0x03;
        int no_local = (opt >> 2) & 0x01;
        int rap      = (opt >> 3) & 0x01;
        int rh       = (opt >> 4) & 0x03;

        // 共享订阅前缀 $share/<group>/<filter>
        char* share_group = NULL;
        const char* eff = topic;
        int prefix_ok = 1;
        if (strncmp(topic, "$share/", 7) == 0) {
            const char* rest  = topic + 7;
            const char* slash = strchr(rest, '/');
            if (!slash || slash == rest || slash[1] == '\0') {
                prefix_ok = 0;
            } else {
                size_t gl = (size_t)(slash - rest);
                share_group = (char*)malloc(gl + 1);
                if (share_group) {
                    memcpy(share_group, rest, gl);
                    share_group[gl] = '\0';
                    eff = slash + 1;
                } else {
                    prefix_ok = 0;
                }
            }
        }

        // 请求 QoS 非法或主题/过滤器非法 -> 返回码 0x80
        int granted = (prefix_ok && requested_qos <= 2 && nl_mqtt_validate_topic(eff))
                          ? (int)requested_qos : 0x80;
        if (granted != 0x80) {
            int is_new = nl_mqtt_server_add_subscription(client->server, eff,
                                            client->client_id, client->id, granted,
                                            sub_id, no_local, rap, rh, share_group);
            filters[grant_count] = strdup(eff);   // 供 SUBACK 后补发保留消息
            is_new_arr[grant_count] = (is_new == 1);
            if (client->session) client->session->dirty = 1;   // 增量落盘
        } else {
            filters[grant_count] = NULL;
            is_new_arr[grant_count] = 0;
        }
        rh_arr[grant_count] = rh;
        granteeds[grant_count++] = granted;
        free(share_group);
        free(topic);
    }

    // 回复 SUBACK(3.1.1 仅含返回码；5.0 含属性长度前缀)
    if (grant_count > 0) {
        nl_mqtt_server_send_suback(client, packet_id, granteeds, grant_count);
    }

    // 补发匹配的保留消息(按 MQTT 惯例置于 SUBACK 之后)，并遵循 Retain Handling
    for (int i = 0; i < grant_count; i++) {
        if (granteeds[i] != 0x80 && filters[i]) {
            int send_ret = 1;
            if (rh_arr[i] == 1)      send_ret = is_new_arr[i] ? 1 : 0;   // 仅新建订阅时
            else if (rh_arr[i] == 2) send_ret = 0;                        // 从不
            if (send_ret) {
                nl_mqtt_server_send_retained(client, filters[i], granteeds[i], sub_id);
            }
            if (strncmp(filters[i], "$SYS/", 5) == 0) {
                nl_mqtt_server_publish_sys(client, filters[i]);
            }
        }
        free(filters[i]);
    }

    if (client->server) client->server->stats.total_subscriptions++;

    // 事件回调(透传 v5 属性；所有权转交 emit 释放)
    nl_mqtt_server_emit(client, NL_MQTT_SERVER_EVT_SUBSCRIBE, NULL, NULL, 0, 0, 0, 0,
                        props);
    return 0;
}

static int nl_mqtt_server_handle_unsubscribe(nl_mqtt_server_client_t* client,
                                              const char* buf, size_t len) {
    size_t offset = 0;
    if (len < 2) return -1;

    offset++;  // 固定头首字节(UNSUBSCRIBE 固定为 0xA2)

    // 剩余长度 -> 报文绝对边界
    size_t remaining_len = 0;
    if (nl_mqtt_server_read_varint(buf, len, &offset, &remaining_len) != 0) return -1;
    if (offset + remaining_len > len) return -1;
    size_t packet_end = offset + remaining_len;

    // 报文标识符
    if (offset + 2 > packet_end) return -1;
    uint16_t packet_id =
        (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
    offset += 2;

    // MQTT 5.0 UNSUBSCRIBE 属性：解析并透传
    nl_mqtt_property_t* props = NULL;
    if (nl_mqtt_server_is_v5(client)) {
        if (nl_mqtt_server_parse_properties(buf, packet_end, &offset, &props) != 0)
            return -1;
    }

    // 逐个解析主题过滤器并移除订阅
    int unsub_count = 0;
    while (offset < packet_end) {
        if (offset + 2 > packet_end) break;
        uint16_t topic_len =
            (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
        offset += 2;
        if (topic_len == 0 || offset + topic_len > packet_end) break;

        char* topic = (char*)malloc((size_t)topic_len + 1);
        if (!topic) break;
        memcpy(topic, buf + offset, topic_len);
        topic[topic_len] = '\0';
        offset += topic_len;

        nl_mqtt_server_remove_subscription(client->server, topic, client->client_id);
        unsub_count++;
        if (client->session) client->session->dirty = 1;   // 增量落盘
        free(topic);
    }

    // 回复 UNSUBACK(3.1.1 无属性；5.0 含属性长度前缀 + 各过滤器原因码)
    nl_mqtt_server_send_unsuback(client, packet_id, unsub_count);

    if (client->server) client->server->stats.total_unsubscriptions++;

    // 事件回调(透传 v5 属性；所有权转交 emit 释放)
    nl_mqtt_server_emit(client, NL_MQTT_SERVER_EVT_UNSUBSCRIBE, NULL, NULL, 0, 0, 0, 0,
                        props);
    return 0;
}

static int nl_mqtt_server_handle_pingreq(nl_mqtt_server_client_t* client) {
    char buf[2] = {(char)0xD0, 0x00};  // PINGRESP 固定头 0xD0 + 剩余长度 0
    return nl_mqtt_server_send_all(client, buf, 2);
}

// 从确认类报文(PUBACK/PUBREC/PUBREL/PUBCOMP)中解析报文标识符。
//   MQTT 3.1.1 —— 仅报文标识符；
//   MQTT 5.0   —— 报文标识符 + 可选原因码 + 可选属性。
// 成功返回 0；out_reason 可为 NULL。
static int nl_mqtt_server_read_ack_pid(nl_mqtt_server_client_t* client,
                                        const char* buf, size_t len,
                                        uint8_t expect_type, uint16_t* out_pid,
                                        int* out_reason) {
    size_t offset = 0;
    if (len < 2) return -1;

    uint8_t first_byte = (uint8_t)buf[offset++];
    if ((first_byte >> 4) != expect_type) return -1;

    size_t remaining_len = 0;
    if (nl_mqtt_server_read_varint(buf, len, &offset, &remaining_len) != 0) return -1;
    if (offset + remaining_len > len) return -1;
    size_t packet_end = offset + remaining_len;

    if (offset + 2 > packet_end) return -1;
    *out_pid = (uint16_t)(((uint8_t)buf[offset] << 8) | (uint8_t)buf[offset + 1]);
    offset += 2;

    if (out_reason) *out_reason = 0;

    if (nl_mqtt_server_is_v5(client)) {
        if (offset < packet_end) {
            if (out_reason) *out_reason = (uint8_t)buf[offset];
            offset++;   // 原因码(缺省 0x00)
        }
        if (offset < packet_end) {
            if (nl_mqtt_server_skip_properties(buf, packet_end, &offset) != 0) return -1;
        }
    }
    return 0;
}

// 处理 PUBREL(0x62)：入站 QoS2 第二步。收到后释放对应 packet_id 并回 PUBCOMP。
// 若该 packet_id 不在待确认集合中(例如重复 PUBREL)，仍按规范回 PUBCOMP。
static int nl_mqtt_server_handle_pubrel(nl_mqtt_server_client_t* client,
                                         const char* buf, size_t len) {
    uint16_t packet_id = 0;
    if (nl_mqtt_server_read_ack_pid(client, buf, len, NL_MQTT_PUBREL, &packet_id, NULL) != 0)
        return -1;

    nl_mqtt_server_session_qos2_remove(client->session, packet_id);
    return nl_mqtt_server_send_pubcomp(client, packet_id);
}

// 处理 PUBACK(0x40)：出站 QoS1 确认。移除未确认出站消息(终止该消息的重传)。
static int nl_mqtt_server_handle_puback(nl_mqtt_server_client_t* client,
                                         const char* buf, size_t len) {
    uint16_t packet_id = 0;
    if (nl_mqtt_server_read_ack_pid(client, buf, len, NL_MQTT_PUBACK, &packet_id, NULL) != 0)
        return -1;

    nl_mqtt_server_out_msg_remove(client->session, packet_id, 1, 0);
    return 0;
}

// 处理 PUBREC(0x50)：出站 QoS2 第一步。移除 stage0 记录，发送 PUBREL，
// 并把该消息推进到 stage1(等待 PUBCOMP)，同时刷新重传计时。
static int nl_mqtt_server_handle_pubrec(nl_mqtt_server_client_t* client,
                                         const char* buf, size_t len) {
    uint16_t packet_id = 0;
    if (nl_mqtt_server_read_ack_pid(client, buf, len, NL_MQTT_PUBREC, &packet_id, NULL) != 0)
        return -1;

    nl_mqtt_server_out_msg_remove(client->session, packet_id, 2, 0);

    if (nl_mqtt_server_send_pubrel(client, packet_id) != 0) return -1;

    // 转为 stage1（PUBREL 无负载，仅需 packet_id 供重传）
    nl_mqtt_server_out_msg_add(client->session, packet_id, 2, NULL, NULL, 0, 0, 0, 0, 1);
    return 0;
}

// 处理 PUBCOMP(0x70)：出站 QoS2 收尾，移除 stage1 记录。
static int nl_mqtt_server_handle_pubcomp(nl_mqtt_server_client_t* client,
                                          const char* buf, size_t len) {
    uint16_t packet_id = 0;
    if (nl_mqtt_server_read_ack_pid(client, buf, len, NL_MQTT_PUBCOMP, &packet_id, NULL) != 0)
        return -1;

    nl_mqtt_server_out_msg_remove(client->session, packet_id, 2, 1);
    return 0;
}

// 处理 DISCONNECT(0xE0)：客户端主动断开。v5 可携带原因码与属性(含会话过期覆盖)。
// 返回 1 表示连接已被释放。
static int nl_mqtt_server_handle_disconnect(nl_mqtt_server_client_t* client,
                                            const char* buf, size_t len) {
    int reason = 0;
    nl_mqtt_property_t* props = NULL;

    if (len >= 2 && nl_mqtt_server_is_v5(client)) {
        size_t offset = 1;   // 跳过固定头首字节
        size_t rem = 0;
        if (nl_mqtt_server_read_varint(buf, len, &offset, &rem) == 0) {
            size_t packet_end = offset + rem;
            if (offset < packet_end) {
                reason = (uint8_t)buf[offset++];
            }
            if (offset < packet_end) {
                if (nl_mqtt_server_parse_properties(buf, packet_end, &offset, &props) != 0) {
                    nl_mqtt_server_props_free(props);
                    props = NULL;
                }
            }
            // v5：DISCONNECT 属性可覆盖会话过期时间
            nl_mqtt_property_t* pe = props
                ? nl_mqtt_server_prop_find(props, NL_MQTT_PROP_SESSION_EXPIRY_INTERVAL)
                : NULL;
            if (pe && client->session) {
                client->session->session_expiry_sec = (uint32_t)pe->int_value;
                client->session->expiry_set = 1;
                client->session->persistent =
                    client->session->session_expiry_sec > 0 ? 1 : 0;
                client->session->dirty = 1;
            }
        }
    }

    nl_mqtt_server_emit(client, NL_MQTT_SERVER_EVT_DISCONNECT, NULL, NULL, 0, 0, 0,
                        reason, props);

    nl_mqtt_server_disconnect_client_internal(client->server, client, 0);
    return 1;
}

// 处理 AUTH(0xF0 = 类型 15)：增强认证。当前实现不支持重新认证，
// 统一以 0x8C(Bad authentication method) 断开。
static int nl_mqtt_server_handle_auth(nl_mqtt_server_client_t* client,
                                      const char* buf, size_t len) {
    (void)buf; (void)len;
    nl_mqtt_server_send_disconnect(client, 0x8C);
    return -1;
}

// 断开并释放一个连接。
// publish_will=1 表示“非正常断开”(网络错误/保活超时/协议错误/管理端强制)，
// 需发布该连接的遗嘱；收到客户端 DISCONNECT 或服务端停止时传 0(抑制遗嘱)。
static void nl_mqtt_server_disconnect_client_internal(nl_mqtt_server_t* srv,
                                                      nl_mqtt_server_client_t* client,
                                                      int publish_will) {
    if (!client || !srv) return;

    // 遗嘱必须在释放资源、摘除订阅之前处理(以便下发给其他订阅者)
    if (publish_will) {
        // v5 Will Delay Interval：与会话过期时间取较小者；会话立即结束则立即发布
        uint32_t sess_exp = client->session
            ? client->session->session_expiry_sec
            : (uint32_t)(client->clean_session ? 0 : srv->session_expiry_sec);
        uint32_t eff = client->will_delay_interval;
        if (sess_exp == 0) eff = 0;
        else if (eff > sess_exp) eff = sess_exp;

        if (eff > 0) {
            nl_mqtt_server_pending_will_add(srv, client, (long long)eff * 1000);
        } else {
            nl_mqtt_server_publish_will(srv, client);
        }
    }

    // 会话处理：
    //   非持久会话(clean_session=1 或 v5 会话过期=0) -> 丢弃会话及其订阅
    //   持久会话 -> 保留(订阅/未确认出站消息/入站 QoS2)，仅标记离线，待同 id 重连恢复
    if (client->session && client->client_id) {
        client->session->connected  = 0;
        client->session->client_num = 0;
        client->session->last_seen  = time(NULL);
        client->session->dirty      = 1;   // 增量落盘

        if (client->clean_session || !client->session->persistent) {
            nl_mqtt_server_remove_client_subscriptions(srv, client->client_id);
            nl_mqtt_server_destroy_session(srv, client->session);
        }
        client->session = NULL;
    } else if (client->client_id) {
        // 未建立会话(如 CONNECT 阶段即出错)时，兜底清理其订阅
        nl_mqtt_server_remove_client_subscriptions(srv, client->client_id);
    }

    // Free client resources
    nl_mqtt_server_topic_alias_clear(client);
    free(client->client_id);
    free(client->username);
    free(client->password);
    free(client->auth_method);
    free(client->will_topic);
    free(client->will_payload);

    // 关闭 TLS(发送 close_notify)并释放上下文
#ifdef NL_MQTT_SERVER_TLS_ENABLE
    if (client->tls_ctx) {
        nl_tls_close((nl_tls_ctx_t*)client->tls_ctx);
        nl_tls_destroy((nl_tls_ctx_t*)client->tls_ctx);
        client->tls_ctx = NULL;
    }
#endif

    // Close socket
#ifdef _WIN32
    closesocket_close(client->sock);
#else
    closesocket_close(client->sock);
#endif

    // Remove from linked list
    nl_mqtt_server_client_t* prev = NULL;
    nl_mqtt_server_client_t* cur = srv->clients;
    while (cur) {
        if (cur == client) {
            if (prev) prev->next = cur->next;
            else       srv->clients = cur->next;
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    free(client->recv_buf);
    free(client);
}

// 扩展客户端接收缓冲区
static int nl_mqtt_server_expand_recv(nl_mqtt_server_client_t* client, size_t needed) {
    if (client->recv_cap >= needed) return 1;
    // 上限：配置的 Maximum Packet Size + 头部余量；未配置时默认 16MB，防止缓冲区无限增长
    size_t limit = (client->server && client->server->max_packet_size > 0)
                       ? (size_t)client->server->max_packet_size + 16
                       : (16u * 1024u * 1024u);
    if (needed > limit) return 0;
    size_t cap = client->recv_cap ? client->recv_cap : 256;
    while (cap < needed) cap *= 2;
    char* nb = (char*)realloc(client->recv_buf, cap);
    if (!nb) return 0;
    client->recv_buf = nb;
    client->recv_cap = cap;
    return 1;
}

// 从接收缓冲区解析所有完整报文，尾部半包保留待下次补齐。
// 返回 1 表示客户端已在处理过程中被释放；0 表示客户端仍然有效。
static int nl_mqtt_server_process_buffer(nl_mqtt_server_t* srv,
                                          nl_mqtt_server_client_t* client) {
    size_t offset = 0;
    size_t total = client->recv_len;

    while (offset < total) {
        const char* buf = client->recv_buf;
        size_t pkt_begin = offset;

        if (offset + 1 > total) break;
        uint8_t type_byte = (uint8_t)buf[offset++];
        uint8_t msg_type = type_byte >> 4;

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
            if (shift > 4) { client->recv_len = 0; return 0; } // 非法剩余长度：丢弃缓冲
            if ((encoded_byte & 0x80) == 0) break;
        }
        if (hdr_incomplete) { offset = pkt_begin; break; }

        // Maximum Packet Size 限制：声明的报文长度超过服务端上限即拒绝
        if (srv->max_packet_size > 0 &&
            remaining_len + (offset - pkt_begin) > (size_t)srv->max_packet_size) {
            nl_mqtt_server_send_disconnect(client, 0x95);   // Packet too large
            nl_mqtt_server_disconnect_client_internal(srv, client, 1);
            return 1;
        }

        size_t pkt_start = offset;
        if (pkt_start + remaining_len > total) { offset = pkt_begin; break; } // 半包
        size_t pkt_end = pkt_start + remaining_len;
        const char* pkt = buf + pkt_begin;
        size_t pkt_len = pkt_end - pkt_begin;

        int rc = 0;
        switch (msg_type) {
            case NL_MQTT_CONNECT:
                rc = nl_mqtt_server_handle_connect(client, pkt, pkt_len);
                break;
            case NL_MQTT_PUBLISH:
                rc = nl_mqtt_server_handle_publish(client, pkt, pkt_len);
                break;
            case NL_MQTT_SUBSCRIBE:
                rc = nl_mqtt_server_handle_subscribe(client, pkt, pkt_len);
                break;
            case NL_MQTT_UNSUBSCRIBE:
                rc = nl_mqtt_server_handle_unsubscribe(client, pkt, pkt_len);
                break;
            case NL_MQTT_PINGREQ:
                rc = nl_mqtt_server_handle_pingreq(client);
                break;
            case NL_MQTT_PUBREL:
                rc = nl_mqtt_server_handle_pubrel(client, pkt, pkt_len);
                break;
            case NL_MQTT_PUBACK:
                rc = nl_mqtt_server_handle_puback(client, pkt, pkt_len);
                break;
            case NL_MQTT_PUBREC:
                rc = nl_mqtt_server_handle_pubrec(client, pkt, pkt_len);
                break;
            case NL_MQTT_PUBCOMP:
                rc = nl_mqtt_server_handle_pubcomp(client, pkt, pkt_len);
                break;
            case NL_MQTT_RESERVED:   // 15 = AUTH (MQTT 5.0)
                rc = nl_mqtt_server_handle_auth(client, pkt, pkt_len);
                break;
            case NL_MQTT_DISCONNECT:
                // 客户端主动断开：解析 v5 原因码/属性后结束并释放连接(抑制遗嘱)
                return nl_mqtt_server_handle_disconnect(client, pkt, pkt_len);
            default:
                break;
        }

        // 处理失败(协议错误/不支持的版本/发送失败)时断开连接(发布遗嘱)
        if (rc != 0) {
            nl_mqtt_server_disconnect_client_internal(srv, client, 1);
            return 1;
        }
        offset = pkt_end;
    }

    // 保留未处理完的尾部数据(半包)，等待下一次 recv 补齐
    {
        size_t remain = client->recv_len - offset;
        if (remain > 0 && offset > 0) {
            memmove(client->recv_buf, client->recv_buf + offset, remain);
        }
        client->recv_len = remain;
    }
    return 0;
}

static void nl_mqtt_server_handle_client(nl_mqtt_server_t* srv,
                                          nl_mqtt_server_client_t* client) {
    char tmp[4096];
    int received = nl_mqtt_server_client_recv(client, tmp, sizeof(tmp));

    if (received <= 0) {
        // Client disconnected
        if (client->connected) {
            client->connected = 0;
            srv->stats.total_disconnections++;

            // Fire disconnect event
            if (srv->event_callback) {
                nl_mqtt_server_event_t event = {
                    .type = NL_MQTT_SERVER_EVT_DISCONNECT,
                    .client_id = client->id,
                    .user_data = srv->event_user_data
                };
                srv->event_callback(&event, srv->event_user_data);
            }
        }
        nl_mqtt_server_disconnect_client_internal(srv, client, 1);
        return;
    }

    client->last_activity = time(NULL);
    srv->stats.bytes_received += (uint32_t)received;

    // 追加到接收缓冲区后统一解析，支持 TCP 粘包/半包
    if (!nl_mqtt_server_expand_recv(client, client->recv_len + (size_t)received)) {
        nl_mqtt_server_disconnect_client_internal(srv, client, 1);
        return;
    }
    memcpy(client->recv_buf + client->recv_len, tmp, (size_t)received);
    client->recv_len += (size_t)received;

    nl_mqtt_server_process_buffer(srv, client);
}

// ============================================================
// Public API Implementation
// ============================================================

nl_mqtt_server_t* nl_mqtt_server_create(void) {
    nl_mqtt_server_t* srv = (nl_mqtt_server_t*)calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->listen_sock = -1;
    srv->port = NL_MQTT_SERVER_DEFAULT_PORT;
    srv->max_clients = NL_MQTT_SERVER_MAX_CLIENTS;
    srv->running = 0;
    srv->next_client_id = 1;
    srv->tls_enabled = 0;
    srv->tls_client_auth = 0;
    srv->tls_handshake_timeout_sec = 0;
    srv->retry_timeout_sec  = NL_MQTT_SERVER_RETRY_TIMEOUT_SEC;
    srv->max_retries        = NL_MQTT_SERVER_MAX_RETRIES;
    srv->session_expiry_sec = NL_MQTT_SERVER_SESSION_EXPIRY_SEC;
    srv->qos2_inbound_timeout_sec  = NL_MQTT_SERVER_QOS2_INBOUND_TIMEOUT_SEC;
    srv->session_save_interval_sec = NL_MQTT_SERVER_SESSION_SAVE_INTERVAL_SEC;
    srv->session_store_path = NULL;
    srv->last_save_ms       = 0;
    srv->max_queued_messages = 0;
    srv->receive_maximum     = 0;
    srv->max_packet_size     = 0;
    srv->server_reference    = NULL;
    srv->share_rr            = 0;

    // 并发保护初始化
    srv->retained = NULL;
    NL_MQTT_MUTEX_INIT(&srv->store_mutex);
    srv->store_mutex_inited = 1;
    nl_mqtt_mutex_init_recursive(&srv->api_mutex);
    srv->api_mutex_inited = 1;
#ifdef _WIN32
    srv->store_lock.handle = NULL;
#else
    srv->store_lock.fd = -1;
#endif
    srv->store_lock.held = 0;

    return srv;
}

void nl_mqtt_server_destroy(nl_mqtt_server_t* server) {
    if (!server) return;

    // Stop server if running
    if (server->running) {
        nl_mqtt_server_stop(server);
    }

    // Free all clients
    nl_mqtt_server_client_t* client = server->clients;
    while (client) {
        nl_mqtt_server_client_t* next = client->next;
        nl_mqtt_server_topic_alias_clear(client);
        free(client->client_id);
        free(client->username);
        free(client->password);
        free(client->auth_method);
        free(client->will_topic);
        free(client->will_payload);
#ifdef NL_MQTT_SERVER_TLS_ENABLE
        if (client->tls_ctx) {
            nl_tls_close((nl_tls_ctx_t*)client->tls_ctx);
            nl_tls_destroy((nl_tls_ctx_t*)client->tls_ctx);
            client->tls_ctx = NULL;
        }
#endif
#ifdef _WIN32
        closesocket_close(client->sock);
#else
        closesocket_close(client->sock);
#endif
        free(client->recv_buf);
        free(client);
        client = next;
    }

    // Free all subscriptions
    nl_mqtt_server_subscription_t* sub = server->subscriptions;
    while (sub) {
        nl_mqtt_server_subscription_t* next = sub->next;
        free(sub->topic);
        free(sub->owner_client_id);
        free(sub->share_group);
        free(sub);
        sub = next;
    }

    // 释放所有会话(含未确认出站消息与入站 QoS2 状态)
    nl_mqtt_server_session_t* sess = server->sessions;
    while (sess) {
        nl_mqtt_server_session_t* next = sess->next;
        nl_mqtt_server_free_out_msgs(sess->out_msgs);
        free(sess->qos2_inbound);
        free(sess->client_id);
        free(sess);
        sess = next;
    }
    server->sessions = NULL;

    if (server->listen_sock >= 0) {
#ifdef _WIN32
        closesocket_close(server->listen_sock);
#else
        closesocket_close(server->listen_sock);
#endif
    }

    // 释放保留消息表、延迟遗嘱队列、落盘删除列表、独占锁与互斥量
    nl_mqtt_server_free_retained(server);
    nl_mqtt_server_free_pending_wills(server);
    nl_mqtt_server_free_deleted_lists(server);
    nl_mqtt_server_store_unlock(server);
    if (server->api_mutex_inited) {
        NL_MQTT_MUTEX_DESTROY(&server->api_mutex);
        server->api_mutex_inited = 0;
    }
    if (server->store_mutex_inited) {
        NL_MQTT_MUTEX_DESTROY(&server->store_mutex);
        server->store_mutex_inited = 0;
    }

    free(server->tls_ca_file);
    free(server->tls_cert_file);
    free(server->tls_key_file);
    free(server->server_reference);
    free(server->session_store_path);
    free(server);
}

int nl_mqtt_server_start(nl_mqtt_server_t* server,
                          const nl_mqtt_server_config_t* config) {
    if (!server) return -1;
    NL_SRV_LOCK(server);
    if (server->running) { NL_SRV_UNLOCK(server); return -8; }

    // 会话落盘：先记录路径并获取进程间独占锁。
    // 提前获取可在“端口已被占用/文件被其他实例锁定”时立即失败，避免占用端口后才发现冲突。
    if (config && config->session_store_path && *config->session_store_path) {
        free(server->session_store_path);
        server->session_store_path = strdup(config->session_store_path);
        if (!server->session_store_path) return -5;
    }
    if (server->session_store_path && !server->store_lock.held) {
        if (nl_mqtt_server_store_lock(server) != 0) {
            free(server->session_store_path);
            server->session_store_path = NULL;
            return NL_MQTT_SERVER_ERR_STORE_LOCKED;   // 落盘文件已被其他实例独占
        }
    }

    nl_mqtt_server_init_sock();

    // Create socket
    server->listen_sock = (fd_t)socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_sock < 0) {
        nl_mqtt_server_store_unlock(server);
        return -2;
    }

    // Set socket options
    int opt = 1;
    setsockopt(server->listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));

#ifdef SO_NOSIGPIPE
    setsockopt(server->listen_sock, SOL_SOCKET, SO_NOSIGPIPE,
               (const char*)&opt, sizeof(opt));
#endif

    // Configure address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config ? config->port : server->port);

    // Bind
    if (bind(server->listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket_close(server->listen_sock);
        server->listen_sock = -1;
        nl_mqtt_server_store_unlock(server);
        return -3;
    }

    // Listen
    if (listen(server->listen_sock, 128) < 0) {
        closesocket_close(server->listen_sock);
        server->listen_sock = -1;
        nl_mqtt_server_store_unlock(server);
        return -4;
    }

    // Update config
    if (config) {
        server->port = config->port;
        server->max_clients = config->max_clients;
        server->tls_enabled = config->tls_enabled;
        server->tls_client_auth = config->tls_client_auth;
        if (config->tls_handshake_timeout_sec > 0)
            server->tls_handshake_timeout_sec = config->tls_handshake_timeout_sec;
        free(server->tls_ca_file);
        free(server->tls_cert_file);
        free(server->tls_key_file);
        server->tls_ca_file   = config->ca_file   ? strdup(config->ca_file)   : NULL;
        server->tls_cert_file = config->cert_file ? strdup(config->cert_file) : NULL;
        server->tls_key_file  = config->key_file  ? strdup(config->key_file)  : NULL;
        server->auth_callback = config->auth_callback;
        server->auth_user_data = config->auth_user_data;
        // 重传/会话过期参数：>0 时覆盖默认值
        if (config->retry_timeout_sec  > 0) server->retry_timeout_sec  = config->retry_timeout_sec;
        if (config->max_retries        > 0) server->max_retries        = config->max_retries;
        if (config->session_expiry_sec > 0) server->session_expiry_sec = config->session_expiry_sec;
        if (config->qos2_inbound_timeout_sec  > 0)
            server->qos2_inbound_timeout_sec  = config->qos2_inbound_timeout_sec;
        if (config->session_save_interval_sec > 0)
            server->session_save_interval_sec = config->session_save_interval_sec;
        // MQTT 5.0 能力与配额(>0 时覆盖)
        if (config->max_queued_messages > 0) server->max_queued_messages = config->max_queued_messages;
        if (config->receive_maximum     > 0) server->receive_maximum     = config->receive_maximum;
        if (config->max_packet_size     > 0) server->max_packet_size     = config->max_packet_size;
        free(server->server_reference);
        server->server_reference = config->server_reference ? strdup(config->server_reference) : NULL;
    }

    // 会话落盘：尝试加载既有会话/保留消息(须在任何客户端接入之前完成)
    if (server->session_store_path) {
        nl_mqtt_server_load_sessions(server);   // 无落盘文件时返回 -9，可忽略
    }

    server->last_save_ms = nl_now_ms();
    server->running = 1;

    // Fire connect event for server start
    if (server->event_callback) {
        nl_mqtt_server_event_t event = {
            .type = NL_MQTT_SERVER_EVT_CONNECT,
            .user_data = server->event_user_data
        };
        server->event_callback(&event, server->event_user_data);
    }

    NL_SRV_UNLOCK(server);
    return 0;
}

int nl_mqtt_server_stop(nl_mqtt_server_t* server) {
    if (!server || !server->running) return -9;
    NL_SRV_LOCK(server);

    // Disconnect all clients
    nl_mqtt_server_client_t* client = server->clients;
    while (client) {
        nl_mqtt_server_client_t* next = client->next;
        nl_mqtt_server_disconnect_client_internal(server, client, 0);   // 服务端停止：抑制遗嘱
        client = next;
    }

    // Close listen socket
    if (server->listen_sock >= 0) {
#ifdef _WIN32
        closesocket_close(server->listen_sock);
#else
        closesocket_close(server->listen_sock);
#endif
        server->listen_sock = -1;
    }

    // 会话落盘：优雅停止时压缩为单个快照(兼顾增量写入与文件紧凑)
    if (server->session_store_path) {
        nl_mqtt_server_store_compact(server);
    }

    // 释放进程间独占锁，允许其他实例接管该落盘文件
    nl_mqtt_server_store_unlock(server);

    server->running = 0;
    NL_SRV_UNLOCK(server);
    return 0;
}

int nl_mqtt_server_is_running(const nl_mqtt_server_t* server) {
    if (!server) return 0;
    NL_SRV_LOCK(server);
    int v = server->running ? 1 : 0;
    NL_SRV_UNLOCK(server);
    return v;
}

int nl_mqtt_server_set_event_callback(nl_mqtt_server_t* server,
                                       nl_mqtt_server_event_callback_t callback,
                                       void* user_data) {
    if (!server) return -1;
    NL_SRV_LOCK(server);
    server->event_callback = callback;
    server->event_user_data = user_data;
    NL_SRV_UNLOCK(server);
    return 0;
}

int nl_mqtt_server_get_client_count(const nl_mqtt_server_t* server) {
    if (!server) return 0;
    NL_SRV_LOCK(server);
    int count = 0;
    nl_mqtt_server_client_t* client = server->clients;
    while (client) {
        count++;
        client = client->next;
    }
    NL_SRV_UNLOCK(server);
    return count;
}

int nl_mqtt_server_disconnect_client(nl_mqtt_server_t* server, uint32_t client_id) {
    if (!server) return -1;
    NL_SRV_LOCK(server);
    nl_mqtt_server_client_t* client = nl_mqtt_server_find_client(server, client_id);
    if (!client) { NL_SRV_UNLOCK(server); return -11; }

    // Send DISCONNECT(3.1.1 仅固定头；5.0 含原因码)
    nl_mqtt_server_send_disconnect(client, 0x00);

    nl_mqtt_server_disconnect_client_internal(server, client, 1);   // 管理端强制断开：发布遗嘱
    NL_SRV_UNLOCK(server);
    return 0;
}

int nl_mqtt_server_handle_connection(nl_mqtt_server_t* server, fd_t sock) {
    if (!server || sock < 0) return -1;
    NL_SRV_LOCK(server);

    int rc = 0;
    if (!server->running) { rc = -9; goto done; }
    if (server->stats.total_connections >= (uint32_t)server->max_clients) { rc = -6; goto done; }

    nl_mqtt_server_client_t* client = nl_mqtt_server_create_client(server, sock);
    if (!client) { rc = -5; goto done; }

    // 启用 TLS 时先完成服务端握手
    if (server->tls_enabled && nl_mqtt_server_tls_accept(server, client) != 0) {
        nl_mqtt_server_disconnect_client_internal(server, client, 0);
        rc = -7;   // NL_MQTT_SERVER_ERR_TLS
        goto done;
    }

    // Process incoming data
    char tmp[4096];
    int received = nl_mqtt_server_client_recv(client, tmp, sizeof(tmp));

    if (received <= 0) {
        nl_mqtt_server_disconnect_client_internal(server, client, 1);
        rc = -1;
        goto done;
    }

    client->last_activity = time(NULL);
    server->stats.bytes_received += (uint32_t)received;

    // 追加到接收缓冲区后统一解析，支持 TCP 粘包/半包
    if (!nl_mqtt_server_expand_recv(client, client->recv_len + (size_t)received)) {
        nl_mqtt_server_disconnect_client_internal(server, client, 1);
        rc = -1;
        goto done;
    }
    memcpy(client->recv_buf + client->recv_len, tmp, (size_t)received);
    client->recv_len += (size_t)received;

    (void)nl_mqtt_server_process_buffer(server, client);

done:
    NL_SRV_UNLOCK(server);
    return rc;
}

// ============================================================
// 出站未确认消息重传 & 离线会话过期清理
// ============================================================

// 重传超时未确认的出站消息：stage0 重发 PUBLISH(DUP=1)，stage1 重发 PUBREL。
// 超过最大重传次数则断开该连接(消息仍保留在会话中，待重连后继续)。
static int nl_mqtt_server_retransmit(nl_mqtt_server_t* srv, long long now_ms) {
    int sent = 0;
    long long timeout_ms = (long long)srv->retry_timeout_sec * 1000;
    nl_mqtt_server_client_t* c = srv->clients;
    while (c) {
        nl_mqtt_server_client_t* next = c->next;
        int give_up = 0;

        if (c->connected && c->sock >= 0 && c->session) {
            nl_mqtt_out_msg_t* prev = NULL;
            nl_mqtt_out_msg_t* m = c->session->out_msgs;
            while (m) {
                nl_mqtt_out_msg_t* mnext = m->next;

                // v5 消息过期：到期未确认的消息直接丢弃，不再重传
                if (m->expires_ms && now_ms >= m->expires_ms) {
                    if (prev) prev->next = mnext;
                    else      c->session->out_msgs = mnext;
                    nl_mqtt_server_free_out_msg(m);
                    c->session->dirty = 1;
                    m = mnext;
                    continue;
                }

                // 因出站流控延后的消息：窗口空闲后首次发送(不计入重传次数)
                if (m->pending) {
                    if (m->stage == 0) {
                        uint32_t rem = 0;
                        if (m->expires_ms) {
                            long long left = m->expires_ms - now_ms;
                            rem = left > 0 ? (uint32_t)((left + 999) / 1000) : 0;
                        }
                        nl_mqtt_server_send_publish(c, m->topic, m->payload,
                                                    m->payload_len, m->qos, m->retain,
                                                    m->packet_id, 0 /*dup*/,
                                                    m->sub_id, rem, NULL);
                    } else {
                        nl_mqtt_server_send_pubrel(c, m->packet_id);
                    }
                    m->pending = 0;
                    m->last_sent_ms = now_ms;
                    sent++;
                    prev = m;
                    m = mnext;
                    continue;
                }

                if (now_ms - m->last_sent_ms >= timeout_ms) {
                    if (m->retries >= srv->max_retries) { give_up = 1; break; }

                    if (m->stage == 0) {
                        uint32_t rem = 0;
                        if (m->expires_ms) {
                            long long left = m->expires_ms - now_ms;
                            rem = left > 0 ? (uint32_t)((left + 999) / 1000) : 0;
                        }
                        nl_mqtt_server_send_publish(c, m->topic, m->payload,
                                                    m->payload_len, m->qos, m->retain,
                                                    m->packet_id, 1 /*dup*/,
                                                    m->sub_id, rem, NULL);
                    } else {
                        nl_mqtt_server_send_pubrel(c, m->packet_id);
                    }
                    m->last_sent_ms = now_ms;
                    m->retries++;
                    sent++;
                }

                prev = m;
                m = mnext;
            }
        }

        if (give_up) {
            nl_mqtt_server_disconnect_client_internal(srv, c, 1);   // 重传超限：断开并发布遗嘱
            sent++;
        }
        c = next;
    }
    return sent;
}

// 清理：① 超时未收到 PUBREL 的入站 QoS2 记录；② 超期未重连的离线会话及其订阅；
//       ③ 已过期的保留消息(v5 Message Expiry)。
static void nl_mqtt_server_prune_sessions(nl_mqtt_server_t* srv, time_t now,
                                          long long now_ms) {
    long long qos2_timeout_ms = (long long)srv->qos2_inbound_timeout_sec * 1000;

    nl_mqtt_server_session_t* s = srv->sessions;
    while (s) {
        nl_mqtt_server_session_t* next = s->next;

        // ① 入站 QoS2 超时清理
        nl_mqtt_server_session_qos2_prune(s, now_ms, qos2_timeout_ms);

        // ② 离线会话过期清理(按该会话自身的 v5/默认过期时间)
        long long exp_sec = (long long)s->session_expiry_sec;
        if (exp_sec > 0 && !s->connected &&
            (long long)(now - s->last_seen) > exp_sec) {
            nl_mqtt_server_remove_client_subscriptions(srv, s->client_id);
            nl_mqtt_server_destroy_session(srv, s);
        }
        s = next;
    }

    // ③ 过期保留消息清理
    nl_mqtt_retained_msg_t* prev = NULL;
    nl_mqtt_retained_msg_t* r = srv->retained;
    while (r) {
        nl_mqtt_retained_msg_t* rnext = r->next;
        if (r->expires_ms && now_ms >= r->expires_ms) {
            if (prev) prev->next = rnext;
            else      srv->retained = rnext;
            nl_mqtt_server_note_retained_deleted(srv, r->topic);   // 增量落盘
            free(r->topic);
            free(r->payload);
            free(r);
        } else {
            prev = r;
        }
        r = rnext;
    }
}

// ============================================================
// 会话/保留消息磁盘落盘（增量日志格式，显式小端）
// 持久化：clean_session=0 的会话(订阅 + 未确认出站消息 + 入站 QoS2 待 PUBREL)
//         以及 broker 级保留消息(Retained)。
// 时间戳/单调过期时刻等运行时状态不落盘，加载后重置。
//
// 文件布局(v3 = 追加型日志)：
//   u32 magic('NLOG') / u32 version(=3)
//   随后为一串"块"：
//     u32 block_type / u32 payload_len / u32 payload_crc32 / payload[payload_len]
//   块类型：
//     SNAPSHOT = 全量快照：u32 session_count / u32 retained_count / <sessions> / <retained>
//     DELTA    = 增量变更：u32 op_count / <op...>
//   加载时顺序读块并应用；遇块头不完整/长度越界/CRC 失败即停止(视为尾部半写，安全忽略)。
//   运行中把变更以 DELTA 追加；块数达到阈值或 start/stop 时压缩为单个 SNAPSHOT。
// ============================================================

#define NL_LOG_MAGIC     0x4E4C4F47u   /* "NLOG" */
#define NL_LOG_VERSION   3u
#define NL_BLOCK_SNAPSHOT 1u
#define NL_BLOCK_DELTA    2u
#define OP_SESSION_SET   1u
#define OP_SESSION_DEL   2u
#define OP_RETAINED_SET  3u
#define OP_RETAINED_DEL  4u
#define NL_STORE_COMPACT_THRESHOLD 64u   /* DELTA 块数超过即压缩为快照 */
#define NL_SESSION_MAX_FIELD     65535u        /* 单字段(主题/客户端 id)长度上限 */
#define NL_STORE_MAX_PAYLOAD     (64u * 1024u * 1024u)  /* 单文件 payload 上限(防异常文件) */

// ---- 内存写缓冲(显式小端) ----
typedef struct {
    unsigned char* data;
    size_t         len;
    size_t         cap;
    int            ok;
} nl_store_buf_t;

static int nl_sb_reserve(nl_store_buf_t* b, size_t extra) {
    if (!b->ok) return -1;
    if (b->len + extra <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra) ncap *= 2;
    unsigned char* nd = (unsigned char*)realloc(b->data, ncap);
    if (!nd) { b->ok = 0; return -1; }
    b->data = nd;
    b->cap  = ncap;
    return 0;
}
static int nl_fw_bytes(nl_store_buf_t* b, const void* p, uint32_t len) {
    if (len == 0) return b->ok ? 0 : -1;
    if (nl_sb_reserve(b, len) != 0) return -1;
    memcpy(b->data + b->len, p, len);
    b->len += len;
    return 0;
}
static int nl_fw_u8(nl_store_buf_t* b, uint8_t v)  { return nl_fw_bytes(b, &v, 1); }
static int nl_fw_u16(nl_store_buf_t* b, uint16_t v) {
    unsigned char t[2] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF) };
    return nl_fw_bytes(b, t, 2);
}
static int nl_fw_u32(nl_store_buf_t* b, uint32_t v) {
    unsigned char t[4] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF),
                           (unsigned char)((v >> 16) & 0xFF), (unsigned char)((v >> 24) & 0xFF) };
    return nl_fw_bytes(b, t, 4);
}
static int nl_fw_u64(nl_store_buf_t* b, uint64_t v) {
    unsigned char t[8];
    for (int i = 0; i < 8; i++) t[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
    return nl_fw_bytes(b, t, 8);
}

// ---- 内存读游标(显式小端，越界即将 ok 置 0) ----
typedef struct {
    const unsigned char* data;
    size_t               len;
    size_t               pos;
    int                  ok;
} nl_store_rd_t;

static int nl_fr_bytes(nl_store_rd_t* r, void* out, uint32_t len) {
    if (!r->ok || r->pos + len > r->len) { r->ok = 0; return -1; }
    if (len) memcpy(out, r->data + r->pos, len);
    r->pos += len;
    return r->ok ? 0 : -1;
}
static int nl_fr_u8(nl_store_rd_t* r, uint8_t* v)  { return nl_fr_bytes(r, v, 1); }
static int nl_fr_u16(nl_store_rd_t* r, uint16_t* v) {
    unsigned char t[2];
    if (nl_fr_bytes(r, t, 2) != 0) return -1;
    *v = (uint16_t)(t[0] | (t[1] << 8));
    return 0;
}
static int nl_fr_u32(nl_store_rd_t* r, uint32_t* v) {
    unsigned char t[4];
    if (nl_fr_bytes(r, t, 4) != 0) return -1;
    *v = (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
         ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
    return 0;
}
static int nl_fr_u64(nl_store_rd_t* r, uint64_t* v) {
    unsigned char t[8];
    if (nl_fr_bytes(r, t, 8) != 0) return -1;
    uint64_t x = 0;
    for (int i = 0; i < 8; i++) x |= ((uint64_t)t[i]) << (8 * i);
    *v = x;
    return 0;
}

// ============================================================
// 进程间独占文件锁（锁文件 = 落盘路径 + ".lock"）
// 运行中的 broker 独占该锁，避免多实例读写同一落盘文件而互相覆盖。
// 采用 OS 建议锁，随句柄关闭自动释放，不会因崩溃残留死锁。
// ============================================================

static int nl_mqtt_server_store_lock_path(nl_mqtt_server_t* server,
                                          char* out, size_t cap) {
    if (!server || !server->session_store_path) return -1;
    int n = snprintf(out, cap, "%s.lock", server->session_store_path);
    return (n < 0 || n >= (int)cap) ? -1 : 0;
}

static int nl_mqtt_server_store_lock(nl_mqtt_server_t* server) {
    if (!server || !server->session_store_path) return -1;
    if (server->store_lock.held) return 0;   /* 已持有 */

    char lock_path[1088];
    if (nl_mqtt_server_store_lock_path(server, lock_path, sizeof(lock_path)) != 0)
        return -1;

#ifdef _WIN32
    HANDLE h = CreateFileA(lock_path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(&server->store_lock.ov, 0, sizeof(server->store_lock.ov));
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &server->store_lock.ov)) {
        CloseHandle(h);
        return -1;   /* 已被其他实例占用 */
    }
    server->store_lock.handle = h;
#else
    int fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;   /* 已被其他实例占用 */
    }
    server->store_lock.fd = fd;
#endif
    server->store_lock.held = 1;
    return 0;
}

static void nl_mqtt_server_store_unlock(nl_mqtt_server_t* server) {
    if (!server || !server->store_lock.held) return;
#ifdef _WIN32
    if (server->store_lock.handle) {
        UnlockFileEx(server->store_lock.handle, 0, 1, 0, &server->store_lock.ov);
        CloseHandle(server->store_lock.handle);
        server->store_lock.handle = NULL;
    }
#else
    if (server->store_lock.fd >= 0) {
        flock(server->store_lock.fd, LOCK_UN);
        close(server->store_lock.fd);
        server->store_lock.fd = -1;
    }
#endif
    server->store_lock.held = 0;
}

// 剩余的 v5 消息过期秒数(0 = 不过期/已过期)
static uint32_t nl_store_rem_sec(long long expires_ms) {
    if (!expires_ms) return 0;
    long long left = expires_ms - nl_now_ms();
    return left > 0 ? (uint32_t)((left + 999) / 1000) : 0;
}

// 序列化单个持久会话
static void nl_store_put_session(nl_store_buf_t* b, nl_mqtt_server_t* srv,
                                 nl_mqtt_server_session_t* s) {
    uint32_t cid_len = (uint32_t)strlen(s->client_id);
    nl_fw_u32(b, cid_len);
    nl_fw_bytes(b, s->client_id, cid_len);
    nl_fw_u64(b, (uint64_t)s->last_seen);
    nl_fw_u32(b, s->session_expiry_sec);
    nl_fw_u8(b, (uint8_t)(s->expiry_set ? 1 : 0));

    /* 订阅 */
    uint32_t sub_count = 0;
    for (nl_mqtt_server_subscription_t* sub = srv->subscriptions; sub; sub = sub->next) {
        if (sub->owner_client_id && strcmp(sub->owner_client_id, s->client_id) == 0)
            sub_count++;
    }
    nl_fw_u32(b, sub_count);
    for (nl_mqtt_server_subscription_t* sub = srv->subscriptions; sub; sub = sub->next) {
        if (!(sub->owner_client_id && strcmp(sub->owner_client_id, s->client_id) == 0))
            continue;
        uint32_t tl = (uint32_t)strlen(sub->topic);
        nl_fw_u32(b, tl);
        nl_fw_bytes(b, sub->topic, tl);
        nl_fw_u8(b, (uint8_t)sub->qos);
        nl_fw_u32(b, sub->sub_id);
    }

    /* 未确认出站消息 */
    uint32_t out_count = 0;
    for (nl_mqtt_out_msg_t* m = s->out_msgs; m; m = m->next) out_count++;
    nl_fw_u32(b, out_count);
    for (nl_mqtt_out_msg_t* m = s->out_msgs; m; m = m->next) {
        uint32_t tl = m->topic ? (uint32_t)strlen(m->topic) : 0;
        nl_fw_u16(b, m->packet_id);
        nl_fw_u8(b, m->qos);
        nl_fw_u8(b, m->stage);
        nl_fw_u8(b, (uint8_t)(m->retain ? 1 : 0));
        nl_fw_u32(b, m->sub_id);
        nl_fw_u32(b, nl_store_rem_sec(m->expires_ms));
        nl_fw_u32(b, tl);
        if (tl) nl_fw_bytes(b, m->topic, tl);
        nl_fw_u32(b, (uint32_t)m->payload_len);
        if (m->payload_len && m->payload)
            nl_fw_bytes(b, m->payload, (uint32_t)m->payload_len);
    }

    /* 入站 QoS2 待 PUBREL */
    nl_fw_u32(b, (uint32_t)s->qos2_inbound_count);
    for (size_t i = 0; i < s->qos2_inbound_count; i++)
        nl_fw_u16(b, s->qos2_inbound[i].packet_id);
}

// 序列化单条保留消息
static void nl_store_put_retained(nl_store_buf_t* b, nl_mqtt_retained_msg_t* r) {
    uint32_t tl = (uint32_t)strlen(r->topic);
    nl_fw_u32(b, tl);
    nl_fw_bytes(b, r->topic, tl);
    nl_fw_u8(b, (uint8_t)r->qos);
    nl_fw_u32(b, nl_store_rem_sec(r->expires_ms));
    nl_fw_u32(b, (uint32_t)r->payload_len);
    if (r->payload_len && r->payload)
        nl_fw_bytes(b, r->payload, (uint32_t)r->payload_len);
}

// 清空内存中的会话/订阅/保留消息(用于应用快照前重置)
static void nl_store_reset_state(nl_mqtt_server_t* srv) {
    nl_mqtt_server_session_t* s = srv->sessions;
    while (s) {
        nl_mqtt_server_session_t* n = s->next;
        nl_mqtt_server_free_out_msgs(s->out_msgs);
        free(s->qos2_inbound);
        free(s->client_id);
        free(s);
        s = n;
    }
    srv->sessions = NULL;

    nl_mqtt_server_subscription_t* sub = srv->subscriptions;
    while (sub) {
        nl_mqtt_server_subscription_t* n = sub->next;
        free(sub->topic);
        free(sub->owner_client_id);
        free(sub->share_group);
        free(sub);
        sub = n;
    }
    srv->subscriptions = NULL;

    nl_mqtt_retained_msg_t* r = srv->retained;
    while (r) {
        nl_mqtt_retained_msg_t* n = r->next;
        free(r->topic);
        free(r->payload);
        free(r);
        r = n;
    }
    srv->retained = NULL;
}

// 反序列化一个会话(覆盖同名旧会话)
static void nl_store_get_session(nl_mqtt_server_t* srv, nl_store_rd_t* rd) {
    uint32_t cid_len = 0;
    if (nl_fr_u32(rd, &cid_len) || cid_len == 0 || cid_len > NL_SESSION_MAX_FIELD) {
        rd->ok = 0;
        return;
    }
    char* cid = (char*)malloc((size_t)cid_len + 1);
    if (!cid) { rd->ok = 0; return; }
    if (nl_fr_bytes(rd, cid, cid_len) != 0) { free(cid); return; }
    cid[cid_len] = '\0';

    uint64_t last_seen = 0;
    uint32_t se = 0;
    uint8_t eset = 0;
    if (nl_fr_u64(rd, &last_seen) || nl_fr_u32(rd, &se) || nl_fr_u8(rd, &eset)) {
        free(cid);
        return;
    }

    nl_mqtt_server_session_t* old = nl_mqtt_server_find_session(srv, cid);
    if (old) {
        nl_mqtt_server_remove_client_subscriptions(srv, cid);
        nl_mqtt_server_destroy_session(srv, old);
    }
    nl_mqtt_server_session_t* sess = (nl_mqtt_server_session_t*)calloc(1, sizeof(*sess));
    if (!sess) { free(cid); rd->ok = 0; return; }
    sess->client_id          = cid;
    sess->persistent         = 1;
    sess->connected          = 0;
    sess->client_num         = 0;
    sess->last_seen          = (time_t)last_seen;
    sess->session_expiry_sec = se;
    sess->expiry_set         = eset ? 1 : 0;
    sess->next               = srv->sessions;
    srv->sessions            = sess;

    /* 订阅 */
    uint32_t sub_count = 0;
    if (nl_fr_u32(rd, &sub_count)) { rd->ok = 0; return; }
    for (uint32_t k = 0; k < sub_count && rd->ok; k++) {
        uint32_t tl = 0;
        if (nl_fr_u32(rd, &tl) || tl > NL_SESSION_MAX_FIELD) { rd->ok = 0; break; }
        char* topic = (char*)malloc((size_t)tl + 1);
        if (!topic) { rd->ok = 0; break; }
        if (nl_fr_bytes(rd, topic, tl) != 0) { free(topic); break; }
        topic[tl] = '\0';
        uint8_t qos = 0;
        uint32_t sid = 0;
        if (nl_fr_u8(rd, &qos) || nl_fr_u32(rd, &sid)) { free(topic); break; }
        nl_mqtt_server_add_subscription(srv, topic, sess->client_id, 0, (int)qos, sid,
                                        0, 0, 0, NULL);
        free(topic);
    }
    if (!rd->ok) return;

    /* 未确认出站消息 */
    uint32_t out_count = 0;
    if (nl_fr_u32(rd, &out_count)) { rd->ok = 0; return; }
    for (uint32_t k = 0; k < out_count && rd->ok; k++) {
        uint16_t pid = 0;
        uint8_t qos = 0, stage = 0, retain = 0;
        uint32_t sid = 0, rem = 0, tl = 0, pl = 0;
        char* topic = NULL;
        void* payload = NULL;

        if (nl_fr_u16(rd, &pid) || nl_fr_u8(rd, &qos) || nl_fr_u8(rd, &stage) ||
            nl_fr_u8(rd, &retain) || nl_fr_u32(rd, &sid) || nl_fr_u32(rd, &rem) ||
            nl_fr_u32(rd, &tl) || tl > NL_SESSION_MAX_FIELD) { rd->ok = 0; break; }
        if (tl) {
            topic = (char*)malloc((size_t)tl + 1);
            if (!topic) { rd->ok = 0; break; }
            if (nl_fr_bytes(rd, topic, tl) != 0) { free(topic); break; }
            topic[tl] = '\0';
        }
        if (nl_fr_u32(rd, &pl) || pl > NL_STORE_MAX_PAYLOAD) { free(topic); rd->ok = 0; break; }
        if (pl) {
            payload = malloc(pl);
            if (!payload) { free(topic); rd->ok = 0; break; }
            if (nl_fr_bytes(rd, payload, pl) != 0) { free(topic); free(payload); break; }
        }

        long long exp = rem > 0 ? nl_now_ms() + (long long)rem * 1000 : 0;
        nl_mqtt_out_msg_t* m = nl_mqtt_server_out_msg_add(
            sess, pid, qos, topic, payload, pl, retain ? 1 : 0, sid, exp, stage);
        if (m) { m->last_sent_ms = 0; m->retries = 0; }   /* 重连后立即重投 */

        free(topic);
        free(payload);
    }
    if (!rd->ok) return;

    /* 入站 QoS2 待 PUBREL */
    uint32_t q2 = 0;
    if (nl_fr_u32(rd, &q2)) { rd->ok = 0; return; }
    for (uint32_t k = 0; k < q2 && rd->ok; k++) {
        uint16_t pid = 0;
        if (nl_fr_u16(rd, &pid)) break;
        nl_mqtt_server_session_qos2_add(sess, pid);
    }
}

// 反序列化一条保留消息
static void nl_store_get_retained(nl_mqtt_server_t* srv, nl_store_rd_t* rd) {
    uint32_t tl = 0, rem = 0, pl = 0;
    uint8_t qos = 0;
    if (nl_fr_u32(rd, &tl) || tl == 0 || tl > NL_SESSION_MAX_FIELD) { rd->ok = 0; return; }
    char* topic = (char*)malloc((size_t)tl + 1);
    if (!topic) { rd->ok = 0; return; }
    if (nl_fr_bytes(rd, topic, tl) != 0) { free(topic); return; }
    topic[tl] = '\0';
    if (nl_fr_u8(rd, &qos) || nl_fr_u32(rd, &rem) || nl_fr_u32(rd, &pl) ||
        pl > NL_STORE_MAX_PAYLOAD) { free(topic); rd->ok = 0; return; }
    void* payload = NULL;
    if (pl) {
        payload = malloc(pl);
        if (!payload) { free(topic); rd->ok = 0; return; }
        if (nl_fr_bytes(rd, payload, pl) != 0) { free(topic); free(payload); return; }
    }
    nl_mqtt_server_set_retained(srv, topic, payload, pl, (int)qos, rem);
    free(topic);
    free(payload);
}

// 构造并写入一个日志块(块头 12 字节：type/len/crc32)到文件末尾
static int nl_mqtt_server_store_append_block(nl_mqtt_server_t* srv, uint32_t type,
                                             const unsigned char* payload, size_t len) {
    FILE* f = fopen(srv->session_store_path, "ab");
    if (!f) return -1;
    uint32_t crc = nl_crc32(payload, len);
    unsigned char hdr[12];
    hdr[0] = (unsigned char)(type & 0xFF);
    hdr[1] = (unsigned char)((type >> 8) & 0xFF);
    hdr[2] = (unsigned char)((type >> 16) & 0xFF);
    hdr[3] = (unsigned char)((type >> 24) & 0xFF);
    hdr[4] = (unsigned char)(len & 0xFF);
    hdr[5] = (unsigned char)((len >> 8) & 0xFF);
    hdr[6] = (unsigned char)((len >> 16) & 0xFF);
    hdr[7] = (unsigned char)((len >> 24) & 0xFF);
    hdr[8] = (unsigned char)(crc & 0xFF);
    hdr[9] = (unsigned char)((crc >> 8) & 0xFF);
    hdr[10] = (unsigned char)((crc >> 16) & 0xFF);
    hdr[11] = (unsigned char)((crc >> 24) & 0xFF);

    int ok = (fwrite(hdr, 1, 12, f) == 12);
    if (ok && len > 0) ok = (fwrite(payload, 1, len, f) == len);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

// 将全部状态写入单个 SNAPSHOT 块(原子替换文件)，并清除脏标记/删除列表
static int nl_mqtt_server_store_compact(nl_mqtt_server_t* srv) {
    nl_store_buf_t body;
    memset(&body, 0, sizeof(body));
    body.ok = 1;

    uint32_t session_count = 0;
    for (nl_mqtt_server_session_t* s = srv->sessions; s; s = s->next) {
        if (s->persistent) session_count++;
    }
    uint32_t retained_count = 0;
    for (nl_mqtt_retained_msg_t* r = srv->retained; r; r = r->next) retained_count++;

    nl_fw_u32(&body, session_count);
    nl_fw_u32(&body, retained_count);
    for (nl_mqtt_server_session_t* s = srv->sessions; s; s = s->next) {
        if (s->persistent) nl_store_put_session(&body, srv, s);
    }
    for (nl_mqtt_retained_msg_t* r = srv->retained; r; r = r->next) {
        nl_store_put_retained(&body, r);
    }
    if (!body.ok) { free(body.data); return -1; }

    char tmp_path[1088];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", srv->session_store_path);
    if (n < 0 || n >= (int)sizeof(tmp_path)) { free(body.data); return -1; }

    FILE* f = fopen(tmp_path, "wb");
    if (!f) { free(body.data); return -1; }

    unsigned char fh[8];
    fh[0] = (unsigned char)(NL_LOG_MAGIC & 0xFF);
    fh[1] = (unsigned char)((NL_LOG_MAGIC >> 8) & 0xFF);
    fh[2] = (unsigned char)((NL_LOG_MAGIC >> 16) & 0xFF);
    fh[3] = (unsigned char)((NL_LOG_MAGIC >> 24) & 0xFF);
    fh[4] = (unsigned char)(NL_LOG_VERSION & 0xFF);
    fh[5] = (unsigned char)((NL_LOG_VERSION >> 8) & 0xFF);
    fh[6] = (unsigned char)((NL_LOG_VERSION >> 16) & 0xFF);
    fh[7] = (unsigned char)((NL_LOG_VERSION >> 24) & 0xFF);

    uint32_t crc = nl_crc32(body.data, body.len);
    unsigned char bh[12];
    bh[0] = (unsigned char)(NL_BLOCK_SNAPSHOT & 0xFF);
    bh[1] = 0; bh[2] = 0; bh[3] = 0;
    bh[4] = (unsigned char)(body.len & 0xFF);
    bh[5] = (unsigned char)((body.len >> 8) & 0xFF);
    bh[6] = (unsigned char)((body.len >> 16) & 0xFF);
    bh[7] = (unsigned char)((body.len >> 24) & 0xFF);
    bh[8] = (unsigned char)(crc & 0xFF);
    bh[9] = (unsigned char)((crc >> 8) & 0xFF);
    bh[10] = (unsigned char)((crc >> 16) & 0xFF);
    bh[11] = (unsigned char)((crc >> 24) & 0xFF);

    int ok = (fwrite(fh, 1, 8, f) == 8) && (fwrite(bh, 1, 12, f) == 12);
    if (ok && body.len > 0) ok = (fwrite(body.data, 1, body.len, f) == body.len);
    if (fclose(f) != 0) ok = 0;
    free(body.data);
    if (!ok) { remove(tmp_path); return -1; }

    remove(srv->session_store_path);
    if (rename(tmp_path, srv->session_store_path) != 0) { remove(tmp_path); return -1; }

    srv->store_block_count = 1;
    nl_mqtt_server_clear_deleted_lists(srv);
    for (nl_mqtt_server_session_t* s = srv->sessions; s; s = s->next) s->dirty = 0;
    for (nl_mqtt_retained_msg_t* r = srv->retained; r; r = r->next) r->dirty = 0;
    srv->last_save_ms = nl_now_ms();
    return 0;
}

// 把自上次落盘以来的变更以单个 DELTA 块追加到日志尾部
static int nl_mqtt_server_store_append_delta(nl_mqtt_server_t* srv) {
    nl_store_buf_t body;
    memset(&body, 0, sizeof(body));
    body.ok = 1;

    uint32_t op_count = 0;
    for (nl_mqtt_server_session_t* s = srv->sessions; s; s = s->next) {
        if (s->persistent && s->dirty) op_count++;
    }
    for (nl_mqtt_retained_msg_t* r = srv->retained; r; r = r->next) {
        if (r->dirty) op_count++;
    }
    op_count += (uint32_t)srv->deleted_session_count;
    op_count += (uint32_t)srv->deleted_retained_count;

    nl_fw_u32(&body, op_count);

    for (nl_mqtt_server_session_t* s = srv->sessions; s; s = s->next) {
        if (!(s->persistent && s->dirty)) continue;
        nl_fw_u8(&body, OP_SESSION_SET);
        nl_store_put_session(&body, srv, s);
    }
    for (nl_mqtt_retained_msg_t* r = srv->retained; r; r = r->next) {
        if (!r->dirty) continue;
        nl_fw_u8(&body, OP_RETAINED_SET);
        nl_store_put_retained(&body, r);
    }
    for (size_t i = 0; i < srv->deleted_session_count; i++) {
        uint32_t tl = (uint32_t)strlen(srv->deleted_session_ids[i]);
        nl_fw_u8(&body, OP_SESSION_DEL);
        nl_fw_u32(&body, tl);
        nl_fw_bytes(&body, srv->deleted_session_ids[i], tl);
    }
    for (size_t i = 0; i < srv->deleted_retained_count; i++) {
        uint32_t tl = (uint32_t)strlen(srv->deleted_retained_topics[i]);
        nl_fw_u8(&body, OP_RETAINED_DEL);
        nl_fw_u32(&body, tl);
        nl_fw_bytes(&body, srv->deleted_retained_topics[i], tl);
    }

    if (!body.ok) { free(body.data); return -1; }

    int rc = nl_mqtt_server_store_append_block(srv, NL_BLOCK_DELTA, body.data, body.len);
    free(body.data);
    if (rc != 0) return rc;

    srv->store_block_count++;
    nl_mqtt_server_clear_deleted_lists(srv);
    for (nl_mqtt_server_session_t* s = srv->sessions; s; s = s->next) s->dirty = 0;
    for (nl_mqtt_retained_msg_t* r = srv->retained; r; r = r->next) r->dirty = 0;
    srv->last_save_ms = nl_now_ms();
    return 0;
}

// 增量落盘：仅在有待落盘变更时写入；首次/超阈值时压缩为快照。
int nl_mqtt_server_save_sessions(nl_mqtt_server_t* server) {
    if (!server) return -1;
    if (!server->session_store_path || !*server->session_store_path)
        return NL_MQTT_SERVER_ERR_NOT_RUNNING;   /* 未启用落盘 */

    // 进程内互斥：周期性落盘(事件循环线程)与外部 stop/save 可能并发
    if (server->store_mutex_inited) NL_MQTT_MUTEX_LOCK(&server->store_mutex);

    int result = 0;

    // 是否存在待落盘变更
    int has_change = (server->deleted_session_count > 0 ||
                      server->deleted_retained_count > 0);
    for (nl_mqtt_server_session_t* s = server->sessions; !has_change && s; s = s->next) {
        if (s->persistent && s->dirty) has_change = 1;
    }
    for (nl_mqtt_retained_msg_t* r = server->retained; !has_change && r; r = r->next) {
        if (r->dirty) has_change = 1;
    }

    if (!has_change) {
        result = 0;                                  // 无变更：零写入(增量优势)
    } else if (server->store_block_count == 0 ||
               server->store_block_count >= NL_STORE_COMPACT_THRESHOLD) {
        result = nl_mqtt_server_store_compact(server);
    } else {
        result = nl_mqtt_server_store_append_delta(server);
        if (result == 0 && server->store_block_count >= NL_STORE_COMPACT_THRESHOLD) {
            nl_mqtt_server_store_compact(server);
        }
    }

    if (server->store_mutex_inited) NL_MQTT_MUTEX_UNLOCK(&server->store_mutex);
    return result;
}

int nl_mqtt_server_load_sessions(nl_mqtt_server_t* server) {
    if (!server) return -1;
    if (!server->session_store_path || !*server->session_store_path)
        return NL_MQTT_SERVER_ERR_NOT_RUNNING;   /* 未启用落盘 */

    // 进程内互斥：与并发落盘串行化
    if (server->store_mutex_inited) NL_MQTT_MUTEX_LOCK(&server->store_mutex);

    int result = 0;
    FILE* f = NULL;
    unsigned char* raw = NULL;
    nl_store_rd_t rd;
    memset(&rd, 0, sizeof(rd));

    f = fopen(server->session_store_path, "rb");
    if (!f) { result = NL_MQTT_SERVER_ERR_NOT_RUNNING; goto done; }   /* 无文件(首次运行) */

    if (fseek(f, 0, SEEK_END) != 0) { result = -1; goto done; }
    long fsz = ftell(f);
    if (fsz < 8 || (unsigned long)fsz > 8u + NL_STORE_MAX_PAYLOAD) { result = -1; goto done; }
    if (fseek(f, 0, SEEK_SET) != 0) { result = -1; goto done; }

    raw = (unsigned char*)malloc((size_t)fsz);
    if (!raw) { result = -1; goto done; }
    if (fread(raw, 1, (size_t)fsz, f) != (size_t)fsz) { result = -1; goto done; }

    rd.data = raw; rd.len = (size_t)fsz; rd.pos = 0; rd.ok = 1;

    uint32_t magic = 0, ver = 0;
    if (nl_fr_u32(&rd, &magic) || nl_fr_u32(&rd, &ver)) { result = -1; goto done; }
    if (magic != NL_LOG_MAGIC || ver != NL_LOG_VERSION) { result = -1; goto done; }

    // 索引优化：仅做“块头”扫描定位最后一个 SNAPSHOT，从该处开始回放，
    // 跳过其之前已被快照取代的块(不必解析它们的内容)。
    {
        size_t p = rd.pos;
        int have_snap = 0;
        size_t last_snap = p;
        while (p + 12 <= rd.len) {
            uint32_t t = (uint32_t)rd.data[p] | ((uint32_t)rd.data[p + 1] << 8) |
                         ((uint32_t)rd.data[p + 2] << 16) | ((uint32_t)rd.data[p + 3] << 24);
            uint32_t l = (uint32_t)rd.data[p + 4] | ((uint32_t)rd.data[p + 5] << 8) |
                         ((uint32_t)rd.data[p + 6] << 16) | ((uint32_t)rd.data[p + 7] << 24);
            if (t == NL_BLOCK_SNAPSHOT) { last_snap = p; have_snap = 1; }
            if (l > NL_STORE_MAX_PAYLOAD || p + 12 + l > rd.len) break;
            p += 12 + l;
        }
        if (have_snap) rd.pos = last_snap;
    }

    int blocks = 0;

    while (1) {
        if (rd.pos + 12 > rd.len) break;                 // 无更多块
        size_t block_start = rd.pos;

        uint32_t type = 0, len = 0, crc = 0;
        if (nl_fr_u32(&rd, &type) || nl_fr_u32(&rd, &len) || nl_fr_u32(&rd, &crc)) {
            rd.pos = block_start;
            break;
        }
        if (len > NL_STORE_MAX_PAYLOAD || rd.pos + len > rd.len) { rd.pos = block_start; break; }
        if (nl_crc32(rd.data + rd.pos, len) != crc) { rd.pos = block_start; break; }  // 校验失败(半写/损坏)

        if (type == NL_BLOCK_SNAPSHOT) {
            nl_store_reset_state(server);
            uint32_t sc = 0, rc2 = 0;
            if (nl_fr_u32(&rd, &sc) || nl_fr_u32(&rd, &rc2)) { result = -1; break; }
            for (uint32_t i = 0; i < sc && rd.ok; i++) nl_store_get_session(server, &rd);
            for (uint32_t i = 0; i < rc2 && rd.ok; i++) nl_store_get_retained(server, &rd);
            if (!rd.ok) { result = -1; break; }
        } else if (type == NL_BLOCK_DELTA) {
            uint32_t opc = 0;
            if (nl_fr_u32(&rd, &opc)) { result = -1; break; }
            for (uint32_t i = 0; i < opc && rd.ok; i++) {
                uint8_t op = 0;
                if (nl_fr_u8(&rd, &op)) { rd.ok = 0; break; }

                if (op == OP_SESSION_SET) {
                    nl_store_get_session(server, &rd);
                } else if (op == OP_SESSION_DEL) {
                    uint32_t l = 0;
                    if (nl_fr_u32(&rd, &l) || l == 0 || l > NL_SESSION_MAX_FIELD) { rd.ok = 0; break; }
                    char* cid = (char*)malloc((size_t)l + 1);
                    if (!cid) { rd.ok = 0; break; }
                    if (nl_fr_bytes(&rd, cid, l) != 0) { free(cid); break; }
                    cid[l] = '\0';
                    nl_mqtt_server_session_t* s = nl_mqtt_server_find_session(server, cid);
                    if (s) {
                        nl_mqtt_server_remove_client_subscriptions(server, cid);
                        nl_mqtt_server_destroy_session(server, s);
                    }
                    free(cid);
                } else if (op == OP_RETAINED_SET) {
                    nl_store_get_retained(server, &rd);
                } else if (op == OP_RETAINED_DEL) {
                    uint32_t l = 0;
                    if (nl_fr_u32(&rd, &l) || l == 0 || l > NL_SESSION_MAX_FIELD) { rd.ok = 0; break; }
                    char* t = (char*)malloc((size_t)l + 1);
                    if (!t) { rd.ok = 0; break; }
                    if (nl_fr_bytes(&rd, t, l) != 0) { free(t); break; }
                    t[l] = '\0';
                    nl_mqtt_server_remove_retained(server, t);
                    free(t);
                } else {
                    rd.ok = 0;
                    break;
                }
            }
            if (!rd.ok) { result = -1; break; }
        } else {
            break;   // 未知块类型 -> 停止
        }
        blocks++;
    }

    server->store_block_count = (uint32_t)blocks;

    // 加载后清除运行时脏标记与删除列表(它们不属于已落盘状态)
    nl_mqtt_server_clear_deleted_lists(server);
    for (nl_mqtt_server_session_t* s = server->sessions; s; s = s->next) s->dirty = 0;
    for (nl_mqtt_retained_msg_t* r = server->retained; r; r = r->next) r->dirty = 0;

    if (blocks == 0) result = -1;   // 无有效块(空或损坏文件)

done:
    if (f) fclose(f);
    free(raw);
    if (server->store_mutex_inited) NL_MQTT_MUTEX_UNLOCK(&server->store_mutex);
    return result;
}

// ============================================================
// Event Loop API
// ============================================================

#ifndef _WIN32
// 在 poll() 结果数组中查询某 fd 是否可读/异常
static int nl_poll_ready(struct pollfd* pfds, size_t nf, int fd) {
    for (size_t i = 0; i < nf; i++) {
        if (pfds[i].fd == fd) {
            return (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0;
        }
    }
    return 0;
}
#endif

// 单步事件循环：接受新连接 + 处理可读客户端 + 保活超时清理。
// timeout_ms < 0 表示无限阻塞；返回本次处理的事件数，负值为错误码。
// POSIX 使用 poll()(无 FD_SETSIZE 上限)；Windows 使用 select()。
int nl_mqtt_server_poll(nl_mqtt_server_t* server, int timeout_ms) {
    if (!server) return -1;
    NL_SRV_LOCK(server);

    int result;
    if (!server->running) { result = NL_MQTT_SERVER_ERR_NOT_RUNNING; goto done; }

    int events = 0;

#if defined(_WIN32)
    fd_set readfds;
    FD_ZERO(&readfds);
    if (server->listen_sock >= 0) {
        FD_SET(server->listen_sock, &readfds);
    }
    for (nl_mqtt_server_client_t* c = server->clients; c; c = c->next) {
        if (c->sock >= 0) {
            FD_SET(c->sock, &readfds);
        }
    }
    struct timeval tv;
    struct timeval* ptv = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int nready = select(0, &readfds, NULL, NULL, ptv);
    if (nready < 0) { result = -2; goto done; }
#define NL_READY(fd) FD_ISSET((fd), &readfds)
#else
    size_t cap = 1;
    for (nl_mqtt_server_client_t* c = server->clients; c; c = c->next) cap++;
    struct pollfd* pfds = (struct pollfd*)malloc(cap * sizeof(struct pollfd));
    if (!pfds) { result = -1; goto done; }
    size_t nf = 0;
    if (server->listen_sock >= 0) {
        pfds[nf].fd = server->listen_sock; pfds[nf].events = POLLIN; pfds[nf].revents = 0; nf++;
    }
    for (nl_mqtt_server_client_t* c = server->clients; c; c = c->next) {
        if (c->sock >= 0) { pfds[nf].fd = c->sock; pfds[nf].events = POLLIN; pfds[nf].revents = 0; nf++; }
    }
    int nready = poll(pfds, (nfds_t)nf, timeout_ms);
    if (nready < 0) { free(pfds); result = -2; goto done; }
#define NL_READY(fd) nl_poll_ready(pfds, nf, (fd))
#endif

    // 1) 接受新连接
    if (server->listen_sock >= 0 && NL_READY(server->listen_sock)) {
        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
#ifdef _WIN32
        int plen = (int)sizeof(peer);
#else
        socklen_t plen = sizeof(peer);
#endif
        fd_t sock = (fd_t)accept(server->listen_sock, (struct sockaddr*)&peer, &plen);
        if (sock >= 0) {
            if (server->stats.total_connections >= (uint32_t)server->max_clients) {
                closesocket_close(sock);   // 超出并发上限，直接拒绝
            } else {
                nl_mqtt_server_client_t* nc = nl_mqtt_server_create_client(server, sock);
                if (!nc) {
                    closesocket_close(sock);
                } else if (server->tls_enabled &&
                           nl_mqtt_server_tls_accept(server, nc) != 0) {
                    nl_mqtt_server_disconnect_client_internal(server, nc, 0);   // TLS 握手失败
                }
            }
            events++;
        }
    }

    // 2) 处理已连接客户端：handle_client 可能释放当前节点，故先保存 next
    nl_mqtt_server_client_t* c = server->clients;
    while (c) {
        nl_mqtt_server_client_t* next = c->next;

        if (c->sock >= 0 && NL_READY(c->sock)) {
            nl_mqtt_server_handle_client(server, c);   // 注意：可能 free(c)
            events++;
        } else if (c->connected && c->keep_alive > 0) {
            time_t now = time(NULL);
            time_t limit = (time_t)((c->keep_alive * 3) / 2);   // 1.5 倍保活窗口
            if (limit > 0 && now - c->last_activity > limit) {
                nl_mqtt_server_disconnect_client_internal(server, c, 1);   // 保活超时：发布遗嘱
                events++;
            }
        }
        c = next;
    }

    // 3) 出站未确认消息重传
    long long now_ms = nl_now_ms();
    events += nl_mqtt_server_retransmit(server, now_ms);

    // 4) 发布到期的延迟遗嘱(v5 Will Delay Interval)
    events += nl_mqtt_server_pending_will_flush(server, now_ms);

    // 5) 清理超时未收到 PUBREL 的入站 QoS2 记录 + 超期离线会话 + 过期保留消息
    nl_mqtt_server_prune_sessions(server, time(NULL), now_ms);

    // 6) 周期性增量落盘(仅在配置了落盘路径时)
    if (server->session_store_path &&
        server->session_save_interval_sec > 0 &&
        now_ms - server->last_save_ms >=
            (long long)server->session_save_interval_sec * 1000) {
        nl_mqtt_server_save_sessions(server);
    }

#if !defined(_WIN32)
    free(pfds);
#endif
    result = events;

done:
    NL_SRV_UNLOCK(server);
    return result;
}
#undef NL_READY

// 阻塞式事件循环：重复 poll 直到 nl_mqtt_server_stop() 将 running 置 0。
int nl_mqtt_server_run(nl_mqtt_server_t* server) {
    if (!server) return -1;
    if (!server->running) return NL_MQTT_SERVER_ERR_NOT_RUNNING;

    while (server->running) {
        int rc = nl_mqtt_server_poll(server, 100);
        if (rc < 0) return rc;
    }
    return 0;
}

// ============================================================
// 服务端发布：向所有匹配订阅者真实下发
// ============================================================

int nl_mqtt_server_publish(nl_mqtt_server_t* server,
                            const char* topic,
                            const void* payload,
                            size_t payload_len,
                            int qos,
                            int retain) {
    if (!server || !topic) return -1;
    if (!nl_mqtt_validate_topic(topic)) return -10;
    if (qos < 0 || qos > 2) return -11;
    if (payload_len > 0 && !payload) return -1;

    NL_SRV_LOCK(server);

    // retain=1 时更新保留消息(负载为空则清除)，供后续新订阅补发
    if (retain) {
        nl_mqtt_server_set_retained(server, topic, payload, payload_len, qos, 0);
    }

    // exclude_client_id = 0 表示不排除任何客户端(id 从 1 开始分配)
    nl_mqtt_server_broadcast(server, topic, payload, payload_len,
                              qos, retain ? 1 : 0, 0, 0, NULL);

    server->stats.total_publishes++;

    NL_SRV_UNLOCK(server);
    return 0;
}

int nl_mqtt_server_foreach_subscription(nl_mqtt_server_t* server,
                                         const char* topic_pattern,
                                         nl_mqtt_server_foreach_subscriber_t callback,
                                         void* user_data) {
    if (!server || !callback) return -1;

    nl_mqtt_server_subscription_t* sub = server->subscriptions;
    while (sub) {
        if (topic_pattern == NULL ||
            nl_mqtt_topic_matches(topic_pattern, sub->topic)) {
            if (callback(sub->client_id, sub->topic, sub->qos, user_data) != 0) {
                return 0;
            }
        }
        sub = sub->next;
    }
    return 0;
}

int nl_mqtt_server_get_topic_subscribers(nl_mqtt_server_t* server,
                                          const char* topic,
                                          uint32_t* client_ids,
                                          size_t* count,
                                          size_t max_count) {
    if (!server || !count) return -1;
    NL_SRV_LOCK(server);
    *count = 0;
    nl_mqtt_server_subscription_t* sub = server->subscriptions;
    while (sub && *count < max_count) {
        if (nl_mqtt_topic_matches(sub->topic, topic)) {
            client_ids[*count] = sub->client_id;
            (*count)++;
        }
        sub = sub->next;
    }
    NL_SRV_UNLOCK(server);
    return 0;
}

int nl_mqtt_server_get_stats(const nl_mqtt_server_t* server,
                              nl_mqtt_server_stats_t* stats) {
    if (!server || !stats) return -1;
    NL_SRV_LOCK(server);
    *stats = server->stats;
    NL_SRV_UNLOCK(server);
    return 0;
}

const char* nl_mqtt_server_version(void) {
    return NL_MQTT_SERVER_VERSION;
}

int nl_mqtt_server_validate_topic(const char* topic) {
    if (!topic || *topic == '\0') return 0;
    size_t len = strlen(topic);
    if (len > NL_MQTT_MAX_TOPIC_LEN) return 0;

    // Check for invalid characters
    for (size_t i = 0; i < len; i++) {
        char c = topic[i];
        if (c == '\0' || c == '\x01' || c == '\x02' || c == '\x03' ||
            c == '\x04' || c == '\x05' || c == '\x06' || c == '\x07' ||
            c == '\x08' || c == '\x0B' || c == '\x0C' || c == '\x0E' ||
            c == '\x0F' || c == '\x10') {
            return 0;
        }
    }
    return 1;
}

int nl_mqtt_server_topic_matches(const char* pattern, const char* topic) {
    if (!pattern || !topic) return 0;

    size_t plen = strlen(pattern);
    size_t tlen = strlen(topic);

    // Use the same matching logic as client
    return nl_mqtt_topic_matches(pattern, topic);
}

nl_module_info_t* nl_mqtt_server_get_module_info(void) {
    return &g_mqtt_server_module_info;
}

int nl_mqtt_server_is_available(void) {
    return g_mqtt_server_available;
}

const char* nl_mqtt_server_version_string(void) {
    return NL_MQTT_SERVER_VERSION;
}

int nl_mqtt_server_init(void) {
    if (g_mqtt_server_initialized) return 0;

    g_mqtt_server_available = 1;
    g_mqtt_server_initialized = 1;

    // Register lang support
    nl_mqtt_server_register_lang();

    return nl_module_register(&g_mqtt_server_module_info);
}

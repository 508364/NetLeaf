/*
 * test_mqtt_server_loopback.c - MQTT 服务端事件循环 / 真实下发 集成测试
 *
 * 覆盖点：
 *   - nl_mqtt_server_poll()/run() 事件循环：accept 新连接、处理可读数据；
 *   - 服务端 -> 订阅者 真实下发（QoS0/1/2），订阅 QoS 与发布 QoS 取较小值；
 *   - 出站 QoS2 流程：PUBLISH -> 收 PUBREC -> 发 PUBREL -> 收 PUBCOMP
 *     （以“服务端多发送了字节”间接断言 PUBREL 已发出）；
 *   - 入站 QoS1/QoS2：客户端上行触发服务端 PUBLISH 事件；
 *   - 无匹配订阅不下发；非法主题被拒绝；
 *   - 停止后 poll/run 返回未运行错误码。
 *
 * 通过回环 socket(127.0.0.1) 做真实收发，不使用任何 mock。
 * 构建：随顶层 CMakeLists.txt 测试段(BUILD_TESTS + BUILD_MQTT + BUILD_MQTT_SERVER)注册。
 */

#include "netleaf_mqtt_server.h"
#include "netleaf_mqtt.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
    static void nl_sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
    #include <sys/select.h>
    static void nl_sleep_ms(int ms) { usleep((unsigned int)(ms) * 1000u); }
#endif

#define TEST_PORT 18831
#define TEST_PORT2 18832   /* 会话持久化(内存) */
#define TEST_PORT3 18833   /* 出站 QoS 超时重传 */
#define TEST_PORT4 18834   /* 会话磁盘落盘跨重启 */
#define TEST_PORT5 18835   /* 入站 QoS2 超时清理 */
#define TEST_PORT6 18836   /* 保留消息(Retained) */
#define TEST_PORT7 18837   /* 遗嘱消息(Will) */
#define TEST_PORT8 18838   /* 校验和 / 并发独占锁 */
#define TEST_PORT9 18839   /* 并发独占锁：第二实例(冲突) */
#define TEST_PORT10 18840  /* MQTT 5.0 属性透传 / 订阅标识符 */
#define TEST_PORT11 18841  /* MQTT 5.0 主题别名 */
#define TEST_PORT12 18842  /* 增量落盘(快照 + 变更追加) */
#define TEST_PORT13 18843  /* MQTT 5.0 增补(能力/共享/NoLocal/原因码/QoS3/AUTH) */

#ifdef _WIN32
    #define RAW_CLOSE closesocket
#else
    #define RAW_CLOSE close
#endif

static int g_total = 0;
static int g_fail  = 0;

static void expect_true(const char* what, int ok) {
    g_total++;
    if (ok) printf("  [PASS] %s\n", what);
    else { g_fail++; printf("  [FAIL] %s\n", what); }
}

static void expect_int(const char* what, long got, long want) {
    g_total++;
    if (got == want) printf("  [PASS] %-38s = %ld\n", what, got);
    else { g_fail++; printf("  [FAIL] %-38s = %ld (期望 %ld)\n", what, got, want); }
}

/* ---------- 客户端收到消息的观测点 ---------- */
static int  g_msg_count = 0;
static char g_msg_topic[128];
static char g_msg_payload[128];
static int  g_msg_qos = -1;

/* 裸客户端扫描时累计收到的“带 RETAIN 位”的 PUBLISH 数量 */
static int  g_scan_retain = 0;

static void on_message(const char* topic, const void* payload, size_t len,
                       int qos, int retain, void* user_data) {
    (void)retain; (void)user_data;
    g_msg_count++;
    if (topic) snprintf(g_msg_topic, sizeof(g_msg_topic), "%s", topic);
    size_t n = len < (sizeof(g_msg_payload) - 1) ? len : (sizeof(g_msg_payload) - 1);
    if (payload) memcpy(g_msg_payload, payload, n);
    g_msg_payload[n] = '\0';
    g_msg_qos = qos;
}

/* ---------- 服务端事件观测点 ---------- */
static int  g_conn_events = 0;
static int  g_publish_events = 0;
static char g_evt_topic[128];
static int  g_evt_qos = -1;
/* 最近一次事件的 v5 属性类型位图(按 type 位)与原因码 */
static unsigned long long g_evt_props = 0;
static int  g_evt_reason = -1;

static void on_server_event(const nl_mqtt_server_event_t* ev, void* user_data) {
    (void)user_data;
    if (!ev) return;

    g_evt_props = 0;
    for (nl_mqtt_property_t* p = ev->properties; p; p = p->next) {
        if (p->type >= 0 && p->type < 64) g_evt_props |= (1ull << p->type);
    }
    g_evt_reason = ev->reason_code;

    if (ev->type == NL_MQTT_SERVER_EVT_CONNECT) {
        g_conn_events++;
    } else if (ev->type == NL_MQTT_SERVER_EVT_PUBLISH) {
        g_publish_events++;
        if (ev->topic) snprintf(g_evt_topic, sizeof(g_evt_topic), "%s", ev->topic);
        g_evt_qos = ev->qos;
    }
}

static int g_connect_rc = -999;
static void on_connect_cb(int rc, void* user_data) { (void)user_data; g_connect_rc = rc; }

/* ---------- 事件循环驱动 ---------- */
static void pump(nl_mqtt_server_t* srv, nl_mqtt_client_t* c, int iters) {
    for (int i = 0; i < iters; i++) {
        if (srv) nl_mqtt_server_poll(srv, 0);
        if (c)   nl_mqtt_maintain(c, 0);
        nl_sleep_ms(3);
    }
}

static void pump_until_connected(nl_mqtt_server_t* srv, nl_mqtt_client_t* c,
                                  int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        nl_mqtt_server_poll(srv, 0);
        nl_mqtt_maintain(c, 0);
        if (nl_mqtt_get_status(c) == NL_MQTT_CONNECTED) return;
        nl_sleep_ms(5);
    }
}

/* ---------- 最小裸 MQTT 客户端（用于验证服务端 -> 客户端的重传） ----------
 * 该客户端只做 TCP 收发与最小报文构造/解析：连接、订阅后故意不应答
 * PUBLISH，以便观察服务端是否按 retry_timeout 重传并置 DUP 位。 */
typedef int rsock_t;

static rsock_t raw_connect(const char* host, unsigned short port) {
    rsock_t s = (rsock_t)socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = inet_addr(host);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) {
        RAW_CLOSE(s);
        return -1;
    }
    return s;
}

/* 构造最小 CONNECT（协议名 MQTT / 级别 4 / keepalive 60） */
static int raw_build_connect(unsigned char* out, const char* cid, int clean) {
    size_t idlen = strlen(cid);
    size_t rem = 10 + 2 + idlen;
    size_t p = 0;
    out[p++] = 0x10;
    out[p++] = (unsigned char)rem;          /* 假定 < 128 */
    out[p++] = 0x00; out[p++] = 0x04;
    memcpy(out + p, "MQTT", 4); p += 4;
    out[p++] = 0x04;                         /* 协议级别 4 */
    out[p++] = (unsigned char)(clean ? 0x02 : 0x00);
    out[p++] = 0x00; out[p++] = 0x3C;        /* keepalive 60 */
    out[p++] = (unsigned char)(idlen >> 8);
    out[p++] = (unsigned char)(idlen & 0xFF);
    memcpy(out + p, cid, idlen); p += idlen;
    return (int)p;
}

/* 构造最小 SUBSCRIBE（单主题） */
static int raw_build_subscribe(unsigned char* out, const char* topic, int qos,
                               unsigned short pid) {
    size_t tl = strlen(topic);
    size_t rem = 2 + 2 + tl + 1;
    size_t p = 0;
    out[p++] = 0x82;
    out[p++] = (unsigned char)rem;
    out[p++] = (unsigned char)(pid >> 8);
    out[p++] = (unsigned char)(pid & 0xFF);
    out[p++] = (unsigned char)(tl >> 8);
    out[p++] = (unsigned char)(tl & 0xFF);
    memcpy(out + p, topic, tl); p += tl;
    out[p++] = (unsigned char)qos;
    return (int)p;
}

/* 构造最小 PUBLISH(QoS2)：dup 控制 DUP 位(重传置 1)。假定剩余长度 < 128 */
static int raw_build_publish_qos2(unsigned char* out, const char* topic,
                                  const char* payload, unsigned short pid, int dup) {
    size_t tl = strlen(topic);
    size_t pl = strlen(payload);
    size_t rem = 2 + tl + 2 + pl;
    size_t p = 0;
    out[p++] = (unsigned char)(0x30 | 0x04 | (dup ? 0x08 : 0x00));  /* QoS2 */
    out[p++] = (unsigned char)rem;
    out[p++] = (unsigned char)(tl >> 8);
    out[p++] = (unsigned char)(tl & 0xFF);
    memcpy(out + p, topic, tl); p += tl;
    out[p++] = (unsigned char)(pid >> 8);
    out[p++] = (unsigned char)(pid & 0xFF);
    memcpy(out + p, payload, pl); p += pl;
    return (int)p;
}

/* 构造带遗嘱的 CONNECT(协议名 MQTT / 级别 4)。wqos/wretain 写入遗嘱标志位 */
static int raw_build_connect_will(unsigned char* out, const char* cid, int clean,
                                  const char* wtopic, const char* wpay,
                                  int wqos, int wretain) {
    size_t idlen = strlen(cid);
    size_t wtl = strlen(wtopic);
    size_t wpl = strlen(wpay);
    size_t rem = 10 + 2 + idlen + 2 + wtl + 2 + wpl;
    size_t p = 0;
    out[p++] = 0x10;
    out[p++] = (unsigned char)rem;                    /* 假定 < 128 */
    out[p++] = 0x00; out[p++] = 0x04;
    memcpy(out + p, "MQTT", 4); p += 4;
    out[p++] = 0x04;                                  /* 协议级别 4 */
    unsigned char flags = (unsigned char)((clean ? 0x02 : 0x00) | 0x04 |
                                          ((wqos & 0x03) << 3) |
                                          ((wretain & 0x01) << 5));
    out[p++] = flags;
    out[p++] = 0x00; out[p++] = 0x3C;                 /* keepalive 60 */
    out[p++] = (unsigned char)(idlen >> 8);
    out[p++] = (unsigned char)(idlen & 0xFF);
    memcpy(out + p, cid, idlen); p += idlen;
    out[p++] = (unsigned char)(wtl >> 8);
    out[p++] = (unsigned char)(wtl & 0xFF);
    memcpy(out + p, wtopic, wtl); p += wtl;
    out[p++] = (unsigned char)(wpl >> 8);
    out[p++] = (unsigned char)(wpl & 0xFF);
    memcpy(out + p, wpay, wpl); p += wpl;
    return (int)p;
}

/* 构造 DISCONNECT(0xE0, 剩余长度 0) */
static int raw_build_disconnect(unsigned char* out) {
    out[0] = 0xE0;
    out[1] = 0x00;
    return 2;
}

/* 构造 MQTT 5.0 CONNECT(协议级别 5，含属性段；假定属性长度 < 128) */
static int raw_build_connect_v5(unsigned char* out, const char* cid, int clean,
                                const unsigned char* props, size_t props_len) {
    size_t idlen = strlen(cid);
    size_t rem = 10 + 1 + props_len + 2 + idlen;
    size_t p = 0;
    out[p++] = 0x10;
    out[p++] = (unsigned char)rem;
    out[p++] = 0x00; out[p++] = 0x04;
    memcpy(out + p, "MQTT", 4); p += 4;
    out[p++] = 0x05;                                  /* 协议级别 5 */
    out[p++] = (unsigned char)(clean ? 0x02 : 0x00);
    out[p++] = 0x00; out[p++] = 0x3C;                 /* keepalive 60 */
    out[p++] = (unsigned char)props_len;
    if (props_len) { memcpy(out + p, props, props_len); p += props_len; }
    out[p++] = (unsigned char)(idlen >> 8);
    out[p++] = (unsigned char)(idlen & 0xFF);
    memcpy(out + p, cid, idlen); p += idlen;
    return (int)p;
}

/* 构造 MQTT 5.0 SUBSCRIBE(含属性段) */
static int raw_build_subscribe_v5(unsigned char* out, const char* topic, int qos,
                                  unsigned short pid, const unsigned char* props,
                                  size_t props_len) {
    size_t tl = strlen(topic);
    size_t rem = 2 + 1 + props_len + 2 + tl + 1;
    size_t p = 0;
    out[p++] = 0x82;
    out[p++] = (unsigned char)rem;
    out[p++] = (unsigned char)(pid >> 8);
    out[p++] = (unsigned char)(pid & 0xFF);
    out[p++] = (unsigned char)props_len;
    if (props_len) { memcpy(out + p, props, props_len); p += props_len; }
    out[p++] = (unsigned char)(tl >> 8);
    out[p++] = (unsigned char)(tl & 0xFF);
    memcpy(out + p, topic, tl); p += tl;
    out[p++] = (unsigned char)qos;
    return (int)p;
}

/* 构造 MQTT 5.0 PUBLISH(含属性段)。topic 可为 NULL/空(用主题别名表示) */
static int raw_build_publish_v5(unsigned char* out, const char* topic,
                                const char* payload, unsigned short pid, int qos,
                                int retain, int dup, const unsigned char* props,
                                size_t props_len) {
    size_t tl = topic ? strlen(topic) : 0;
    size_t pl = payload ? strlen(payload) : 0;
    size_t rem = 2 + tl + (qos > 0 ? 2 : 0) + 1 + props_len + pl;
    size_t p = 0;
    out[p++] = (unsigned char)(0x30 | (dup ? 0x08 : 0) | ((qos & 3) << 1) |
                               (retain ? 1 : 0));
    out[p++] = (unsigned char)rem;
    out[p++] = (unsigned char)(tl >> 8);
    out[p++] = (unsigned char)(tl & 0xFF);
    if (tl) { memcpy(out + p, topic, tl); p += tl; }
    if (qos > 0) {
        out[p++] = (unsigned char)(pid >> 8);
        out[p++] = (unsigned char)(pid & 0xFF);
    }
    out[p++] = (unsigned char)props_len;
    if (props_len) { memcpy(out + p, props, props_len); p += props_len; }
    if (pl) { memcpy(out + p, payload, pl); p += pl; }
    return (int)p;
}

/* 在字节流中查找 2 字节序列(用于验证出站报文含指定属性) */
static int raw_has_bytes(const unsigned char* buf, size_t len,
                         unsigned char a, unsigned char b) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == a && buf[i + 1] == b) return 1;
    }
    return 0;
}

/* 构造 MQTT 5.0 SUBSCRIBE(单主题，可指定订阅选项字节 opt) */
static int raw_build_subscribe_v5_opt(unsigned char* out, const char* topic,
                                      unsigned char opt, unsigned short pid,
                                      const unsigned char* props, size_t props_len) {
    size_t tl = strlen(topic);
    size_t rem = 2 + 1 + props_len + 2 + tl + 1;
    size_t p = 0;
    out[p++] = 0x82;
    out[p++] = (unsigned char)rem;
    out[p++] = (unsigned char)(pid >> 8);
    out[p++] = (unsigned char)(pid & 0xFF);
    out[p++] = (unsigned char)props_len;
    if (props_len) { memcpy(out + p, props, props_len); p += props_len; }
    out[p++] = (unsigned char)(tl >> 8);
    out[p++] = (unsigned char)(tl & 0xFF);
    memcpy(out + p, topic, tl); p += tl;
    out[p++] = opt;
    return (int)p;
}

/* 在字节流中查找指定类型的确认报文(PUBACK/PUBREC)并返回原因码(找不到 -1) */
static int raw_find_ack_reason(const unsigned char* buf, size_t len, unsigned char type) {
    for (size_t i = 0; i + 5 < len; i++) {
        if ((buf[i] >> 4) == type && buf[i + 1] == 0x04) return buf[i + 4];
    }
    return -1;
}

/* 扫描累积缓冲中的完整报文，统计 PUBLISH 数量与其中 DUP 数量；
 * 未收全的尾部前移保留。 */
static void raw_scan(unsigned char* acc, size_t* len, int* pub_total, int* dup_total) {
    size_t off = 0;
    while (off + 2 <= *len) {
        unsigned char b0 = acc[off];
        size_t rem = 0;
        int mult = 1, shift = 0, incomplete = 0;
        size_t q = off + 1;
        for (;;) {
            if (q >= *len) { incomplete = 1; break; }
            unsigned char e = acc[q++];
            rem += (size_t)(e & 0x7F) * mult;
            mult *= 128;
            shift++;
            if (!(e & 0x80)) break;
            if (shift > 4) { incomplete = 1; break; }
        }
        if (incomplete || q + rem > *len) break;   /* 长度或报文未收全 */
        if ((b0 >> 4) == 0x03) {                   /* PUBLISH */
            (*pub_total)++;
            if (b0 & 0x08) (*dup_total)++;         /* DUP 位 */
            if (b0 & 0x01) g_scan_retain++;        /* RETAIN 位 */
        }
        off = q + rem;
    }
    if (off > 0) {
        memmove(acc, acc + off, *len - off);
        *len -= off;
    }
}

/* 在累积缓冲中查找 CONNACK，找到返回 1 并输出 Session Present 位 */
static int raw_connack_present(const unsigned char* acc, size_t len, int* present) {
    size_t off = 0;
    while (off + 2 <= len) {
        unsigned char b0 = acc[off];
        size_t rem = 0;
        int mult = 1, shift = 0, incomplete = 0;
        size_t q = off + 1;
        for (;;) {
            if (q >= len) { incomplete = 1; break; }
            unsigned char e = acc[q++];
            rem += (size_t)(e & 0x7F) * mult;
            mult *= 128;
            shift++;
            if (!(e & 0x80)) break;
            if (shift > 4) { incomplete = 1; break; }
        }
        if (incomplete || q + rem > len) break;
        if ((b0 >> 4) == 0x02 && rem >= 2) {   /* CONNACK */
            *present = acc[q] & 0x01;
            return 1;
        }
        off = q + rem;
    }
    return 0;
}

/* 等待 CONNACK 到达(不消费缓冲)，找到返回 1 并输出 Session Present 位 */
static int raw_wait_connack(nl_mqtt_server_t* srv, rsock_t rs,
                            unsigned char* acc, size_t* acc_len, int ms_total,
                            int* present) {
    int steps = ms_total / 10;
    for (int i = 0; i < steps; i++) {
        nl_mqtt_server_poll(srv, 0);

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(rs, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select((int)rs + 1, &rf, NULL, NULL, &tv) > 0 && *acc_len < 1024) {
            int n = recv(rs, (char*)acc + *acc_len, (int)(1024 - *acc_len), 0);
            if (n > 0) *acc_len += (size_t)n;
        }
        if (raw_connack_present(acc, *acc_len, present)) return 1;
        nl_sleep_ms(10);
    }
    return 0;
}

/* 交替驱动服务端与裸客户端，持续 ms_total 毫秒(步长 10ms) */
static void raw_pump(nl_mqtt_server_t* srv, rsock_t rs,
                     unsigned char* acc, size_t* acc_len, int ms_total,
                     int* pub_total, int* dup_total) {
    int steps = ms_total / 10;
    for (int i = 0; i < steps; i++) {
        nl_mqtt_server_poll(srv, 0);

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(rs, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select((int)rs + 1, &rf, NULL, NULL, &tv) > 0 && *acc_len < 1024) {
            int n = recv(rs, (char*)acc + *acc_len, (int)(1024 - *acc_len), 0);
            if (n > 0) *acc_len += (size_t)n;
        }
        raw_scan(acc, acc_len, pub_total, dup_total);
        nl_sleep_ms(10);
    }
}

/* 同 raw_pump，但只累积原始字节、不解析/不消费(用于检查报文内部属性字节) */
static void raw_capture(nl_mqtt_server_t* srv, rsock_t rs,
                        unsigned char* acc, size_t* acc_len, int ms_total) {
    int steps = ms_total / 10;
    for (int i = 0; i < steps; i++) {
        nl_mqtt_server_poll(srv, 0);

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(rs, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select((int)rs + 1, &rf, NULL, NULL, &tv) > 0 && *acc_len < 1024) {
            int n = recv(rs, (char*)acc + *acc_len, (int)(1024 - *acc_len), 0);
            if (n > 0) *acc_len += (size_t)n;
        }
        nl_sleep_ms(10);
    }
}

/* 删除落盘文件及其衍生的 .lock / .tmp 文件(测试收尾清理) */
static void remove_store_files(const char* store) {
    char path[1088];
    remove(store);
    snprintf(path, sizeof(path), "%s.lock", store);
    remove(path);
    snprintf(path, sizeof(path), "%s.tmp", store);
    remove(path);
}

/* 二进制复制文件(用于复制“快照+变更”日志以验证增量回放) */
static int copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return -1;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    unsigned char buf[4096];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    return ok ? 0 : -1;
}

int main(void) {
    printf("=== MQTT 服务端事件循环 / 真实下发 集成测试 ===\n");

    /* 1) 服务端启动 */
    nl_mqtt_server_t* srv = nl_mqtt_server_create();
    expect_true("服务端创建成功", srv != NULL);
    if (!srv) return 1;

    nl_mqtt_server_set_event_callback(srv, on_server_event, NULL);

    nl_mqtt_server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = TEST_PORT;
    cfg.max_clients = 8;

    expect_int("服务端启动返回 0", nl_mqtt_server_start(srv, &cfg), 0);
    expect_int("服务端处于运行状态", nl_mqtt_server_is_running(srv), 1);

    int conn_before = g_conn_events;   /* start() 自身会触发一次 CONNECT 事件 */

    /* 2) 客户端连接 */
    nl_mqtt_client_t* c = nl_mqtt_create();
    expect_true("客户端创建成功", c != NULL);
    if (!c) { nl_mqtt_server_destroy(srv); return 1; }
    nl_mqtt_set_protocol_version(c, NL_MQTT_PROTOCOL_V3_1_1);

    nl_mqtt_connect_opts_t copts;
    memset(&copts, 0, sizeof(copts));
    copts.client_id     = "sub-1";
    copts.keep_alive    = 60;
    copts.clean_session = 1;

    expect_int("客户端发起连接返回 0",
               nl_mqtt_connect(c, "127.0.0.1", TEST_PORT, &copts, on_connect_cb), 0);

    pump_until_connected(srv, c, 200);
    expect_int("客户端状态 = CONNECTED",
               (long)nl_mqtt_get_status(c), (long)NL_MQTT_CONNECTED);
    expect_int("连接回调返回码 0", g_connect_rc, 0);
    expect_true("服务端收到新连接事件", g_conn_events > conn_before);

    /* 3) 订阅 t/hello (QoS2，以便后续真实验证出站 QoS2 流程) */
    nl_mqtt_sub_opts_t sopts;
    memset(&sopts, 0, sizeof(sopts));
    sopts.qos = 2;
    /* nl_mqtt_subscribe 成功时返回报文标识符(>0)，失败返回 -1 */
    expect_true("订阅返回非负",
                nl_mqtt_subscribe(c, "t/hello", &sopts, on_message, NULL) >= 0);
    pump(srv, c, 20);

    /* 4) 服务端 -> 客户端 QoS1 下发 */
    g_msg_count = 0;
    expect_int("服务端 QoS1 发布返回 0",
               nl_mqtt_server_publish(srv, "t/hello", "hi-q1", 5, 1, 0), 0);
    pump(srv, c, 25);
    expect_int("QoS1 消息送达次数", g_msg_count, 1);
    expect_true("QoS1 主题正确", strcmp(g_msg_topic, "t/hello") == 0);
    expect_true("QoS1 负载正确", strcmp(g_msg_payload, "hi-q1") == 0);
    expect_int("QoS1 送达 QoS", g_msg_qos, 1);

    nl_mqtt_server_stats_t st1;
    memset(&st1, 0, sizeof(st1));
    nl_mqtt_server_get_stats(srv, &st1);

    /* 5) 服务端 -> 客户端 QoS2 下发（触发 PUBREC/PUBREL/PUBCOMP 出站流程） */
    g_msg_count = 0;
    expect_int("服务端 QoS2 发布返回 0",
               nl_mqtt_server_publish(srv, "t/hello", "hi-q2", 5, 2, 0), 0);
    pump(srv, c, 40);
    expect_int("QoS2 消息送达次数", g_msg_count, 1);
    expect_true("QoS2 负载正确", strcmp(g_msg_payload, "hi-q2") == 0);
    expect_int("QoS2 送达 QoS", g_msg_qos, 2);

    nl_mqtt_server_stats_t st2;
    memset(&st2, 0, sizeof(st2));
    nl_mqtt_server_get_stats(srv, &st2);
    /* QoS2 相比 QoS1：PUBLISH 多 2 字节报文标识符，且额外发出 PUBREL(4 字节) */
    expect_true("QoS2 出站多发送字节(证明 PUBREL 已发出)",
                st2.bytes_sent > st1.bytes_sent);
    expect_int("QoS2 出站后客户端仍连接",
               (long)nl_mqtt_get_status(c), (long)NL_MQTT_CONNECTED);

    /* 6) 无匹配订阅的主题不应送达 */
    g_msg_count = 0;
    nl_mqtt_server_publish(srv, "t/other", "x", 1, 1, 0);
    pump(srv, c, 15);
    expect_int("无匹配订阅不送达", g_msg_count, 0);

    /* 7) 客户端 -> 服务端 QoS1 上行 */
    int before = g_publish_events;
    nl_mqtt_pub_opts_t popts;
    memset(&popts, 0, sizeof(popts));
    popts.qos = 1;
    nl_mqtt_publish(c, "up/q1", "u1", 2, &popts);
    pump(srv, c, 25);
    expect_int("上行 QoS1 触发 PUBLISH 事件次数", g_publish_events - before, 1);
    expect_true("上行 QoS1 主题正确", strcmp(g_evt_topic, "up/q1") == 0);
    expect_int("上行 QoS1 事件 QoS", g_evt_qos, 1);

    /* 8) 客户端 -> 服务端 QoS2 上行（完成 PUBREC/PUBREL/PUBCOMP 握手） */
    before = g_publish_events;
    popts.qos = 2;
    nl_mqtt_publish(c, "up/q2", "u2", 2, &popts);
    pump(srv, c, 40);
    expect_int("上行 QoS2 触发 PUBLISH 事件次数", g_publish_events - before, 1);
    expect_int("上行 QoS2 后客户端仍连接",
               (long)nl_mqtt_get_status(c), (long)NL_MQTT_CONNECTED);

    /* 9) 非法主题被拒绝 */
    expect_true("发布非法(空)主题被拒绝",
                nl_mqtt_server_publish(srv, "", "x", 1, 0, 0) != 0);

    /* 10) 停止服务端：poll/run 返回未运行错误码 */
    nl_mqtt_server_stop(srv);
    expect_int("停止后 is_running = 0", nl_mqtt_server_is_running(srv), 0);
    expect_int("停止后 poll 返回未运行",
               nl_mqtt_server_poll(srv, 0), NL_MQTT_SERVER_ERR_NOT_RUNNING);
    expect_int("停止后 run 返回未运行",
               nl_mqtt_server_run(srv), NL_MQTT_SERVER_ERR_NOT_RUNNING);

    nl_mqtt_destroy(c);
    nl_mqtt_server_destroy(srv);

    /* 11) 会话持久化：clean_session=0 时服务端跨重连保留订阅(裸客户端直接验证) */
    {
        nl_mqtt_server_t* srv2 = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfg2;
        memset(&cfg2, 0, sizeof(cfg2));
        cfg2.port = TEST_PORT2;
        cfg2.max_clients = 8;
        expect_int("持久会话服务端启动", nl_mqtt_server_start(srv2, &cfg2), 0);

        unsigned char cbuf[256];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int pubs = 0, dups = 0, present = -1;

        /* 首次连接：clean_session=0(请求持久会话) */
        rsock_t rs1 = raw_connect("127.0.0.1", TEST_PORT2);
        expect_true("持久会话首次 TCP 连接", rs1 >= 0);
        int cl = raw_build_connect(cbuf, "S2", 0);
        send(rs1, (char*)cbuf, cl, 0);
        expect_true("首次 CONNACK 可解析",
                    raw_wait_connack(srv2, rs1, acc, &acc_len, 300, &present));
        expect_int("首次 CONNACK Session Present = 0", present, 0);
        acc_len = 0;   /* 丢弃已解析的 CONNACK */

        /* 订阅 s2/t(qos1)，随后断开连接 */
        int sl = raw_build_subscribe(cbuf, "s2/t", 1, 1);
        send(rs1, (char*)cbuf, sl, 0);
        raw_pump(srv2, rs1, acc, &acc_len, 300, &pubs, &dups);
        RAW_CLOSE(rs1);
        pump(srv2, NULL, 40);   /* 让服务端感知断开：clean_session=0 应保留会话与订阅 */

        /* 同一 client id 重连(clean=0)：应恢复会话 */
        unsigned char acc2[1024];
        size_t acc2_len = 0;
        int pubs2 = 0, dups2 = 0, present2 = -1;
        rsock_t rs2 = raw_connect("127.0.0.1", TEST_PORT2);
        expect_true("持久会话重连 TCP 连接", rs2 >= 0);
        int cl2 = raw_build_connect(cbuf, "S2", 0);
        send(rs2, (char*)cbuf, cl2, 0);
        expect_true("重连 CONNACK 可解析",
                    raw_wait_connack(srv2, rs2, acc2, &acc2_len, 300, &present2));
        expect_int("重连 CONNACK Session Present = 1", present2, 1);
        acc2_len = 0;

        /* 不重新订阅，直接由服务端发布：仍应送达(证明服务端保留了订阅) */
        pubs2 = 0; dups2 = 0; acc2_len = 0;
        nl_mqtt_server_publish(srv2, "s2/t", "resumed", 7, 1, 0);
        raw_pump(srv2, rs2, acc2, &acc2_len, 500, &pubs2, &dups2);
        expect_true("会话恢复后未重订阅仍收到 PUBLISH", pubs2 >= 1);

        RAW_CLOSE(rs2);
        nl_mqtt_server_destroy(srv2);
    }

    /* 12) 出站 QoS 超时重传：裸客户端不应答 PUBLISH，观察服务端重传(DUP=1) */
    {
        nl_mqtt_server_t* srv3 = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfg3;
        memset(&cfg3, 0, sizeof(cfg3));
        cfg3.port = TEST_PORT3;
        cfg3.max_clients = 8;
        cfg3.retry_timeout_sec = 1;   /* 缩短重传间隔以便测试 */
        cfg3.max_retries       = 5;
        expect_int("重传测试服务端启动", nl_mqtt_server_start(srv3, &cfg3), 0);

        rsock_t rs = raw_connect("127.0.0.1", TEST_PORT3);
        expect_true("裸客户端 TCP 连接成功", rs >= 0);

        unsigned char cbuf[256];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int pubs = 0, dups = 0;

        int cl = raw_build_connect(cbuf, "R1", 1);
        send(rs, (char*)cbuf, cl, 0);
        raw_pump(srv3, rs, acc, &acc_len, 300, &pubs, &dups);   /* accept + CONNACK */

        int sl = raw_build_subscribe(cbuf, "r/t", 1, 1);
        send(rs, (char*)cbuf, sl, 0);
        raw_pump(srv3, rs, acc, &acc_len, 300, &pubs, &dups);   /* SUBACK */

        /* 仅统计发布之后的报文 */
        pubs = 0; dups = 0; acc_len = 0;
        nl_mqtt_server_publish(srv3, "r/t", "hi", 2, 1, 0);

        /* 首个窗口(400ms < 重传间隔 1s)：应仅 1 条、非 DUP */
        raw_pump(srv3, rs, acc, &acc_len, 400, &pubs, &dups);
        expect_int("首窗口收到 1 条 PUBLISH", pubs, 1);
        expect_int("首条为非 DUP", dups, 0);

        /* 继续等待超过重传间隔：应收到带 DUP 的重传 */
        raw_pump(srv3, rs, acc, &acc_len, 1600, &pubs, &dups);
        expect_true("超时后收到重传(总数 > 1)", pubs >= 2);
        expect_true("重传报文带 DUP 标记", dups >= 1);

        RAW_CLOSE(rs);
        nl_mqtt_server_destroy(srv3);
    }

    /* 13) 会话磁盘落盘：destroy + 重建服务端(进程级重启)后，
     *     持久会话的订阅仍能从磁盘文件恢复(CONNACK Session Present=1) */
    {
        const char* store = "nl_session_store_test.bin";
        remove_store_files(store);   /* 清理历史残留，保证首次为空白 */

        nl_mqtt_server_t* srvA = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfgA;
        memset(&cfgA, 0, sizeof(cfgA));
        cfgA.port = TEST_PORT4;
        cfgA.max_clients = 8;
        cfgA.session_store_path = store;
        cfgA.session_save_interval_sec = 1;
        expect_int("落盘服务端A启动", nl_mqtt_server_start(srvA, &cfgA), 0);

        unsigned char cbuf[256];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int pubs = 0, dups = 0, present = -1;

        rsock_t rs1 = raw_connect("127.0.0.1", TEST_PORT4);
        expect_true("落盘测试客户端TCP连接", rs1 >= 0);
        int cl = raw_build_connect(cbuf, "P1", 0);   /* clean=0 -> 持久会话 */
        send(rs1, (char*)cbuf, cl, 0);
        expect_true("落盘客户端CONNACK可解析",
                    raw_wait_connack(srvA, rs1, acc, &acc_len, 300, &present));
        expect_int("落盘客户端首次Session Present=0", present, 0);
        acc_len = 0;

        int sl = raw_build_subscribe(cbuf, "p/t", 1, 1);
        send(rs1, (char*)cbuf, sl, 0);
        raw_pump(srvA, rs1, acc, &acc_len, 300, &pubs, &dups);   /* SUBACK */
        RAW_CLOSE(rs1);
        pump(srvA, NULL, 30);   /* 服务端感知断开，保留持久会话 */

        expect_int("落盘服务端A停止(自动落盘)", nl_mqtt_server_stop(srvA), 0);
        nl_mqtt_server_destroy(srvA);

        /* 重建服务端 + 相同落盘文件：start() 应自动加载 */
        nl_mqtt_server_t* srvB = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfgB;
        memset(&cfgB, 0, sizeof(cfgB));
        cfgB.port = TEST_PORT4;
        cfgB.max_clients = 8;
        cfgB.session_store_path = store;
        expect_int("落盘服务端B启动(自动加载)", nl_mqtt_server_start(srvB, &cfgB), 0);

        unsigned char acc2[1024];
        size_t acc2_len = 0;
        int pubs2 = 0, dups2 = 0, present2 = -1;
        rsock_t rs2 = raw_connect("127.0.0.1", TEST_PORT4);
        expect_true("重启后客户端TCP连接", rs2 >= 0);
        int cl2 = raw_build_connect(cbuf, "P1", 0);
        send(rs2, (char*)cbuf, cl2, 0);
        expect_true("重启后CONNACK可解析",
                    raw_wait_connack(srvB, rs2, acc2, &acc2_len, 300, &present2));
        expect_int("重启后Session Present=1(磁盘恢复)", present2, 1);
        acc2_len = 0;

        /* 未重新订阅，服务端发布仍应送达 -> 证明订阅已从磁盘恢复 */
        pubs2 = 0; dups2 = 0;
        nl_mqtt_server_publish(srvB, "p/t", "from-disk", 9, 1, 0);
        raw_pump(srvB, rs2, acc2, &acc2_len, 500, &pubs2, &dups2);
        expect_true("磁盘恢复的订阅仍能收到PUBLISH", pubs2 >= 1);

        RAW_CLOSE(rs2);
        nl_mqtt_server_destroy(srvB);
        remove_store_files(store);
    }

    /* 14) 入站 QoS2 超时清理：客户端发 PUBLISH(QoS2) 但不回 PUBREL，
     *     超时后同一 packet_id 的重传应被当作新消息再次投递 */
    {
        nl_mqtt_server_t* srv4 = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfg4;
        memset(&cfg4, 0, sizeof(cfg4));
        cfg4.port = TEST_PORT5;
        cfg4.max_clients = 8;
        cfg4.qos2_inbound_timeout_sec = 1;   /* 1 秒超时，便于测试 */
        nl_mqtt_server_set_event_callback(srv4, on_server_event, NULL);
        expect_int("QoS2超时测试服务端启动", nl_mqtt_server_start(srv4, &cfg4), 0);

        rsock_t rs = raw_connect("127.0.0.1", TEST_PORT5);
        expect_true("QoS2超时裸客户端连接", rs >= 0);
        unsigned char cbuf[256];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int pubs = 0, dups = 0;
        int cl = raw_build_connect(cbuf, "Q1", 1);
        send(rs, (char*)cbuf, cl, 0);
        raw_pump(srv4, rs, acc, &acc_len, 200, &pubs, &dups);   /* CONNACK */

        int before = g_publish_events;
        int pl = raw_build_publish_qos2(cbuf, "q/in", "v1", 7, 0);
        send(rs, (char*)cbuf, pl, 0);
        raw_pump(srv4, rs, acc, &acc_len, 200, &pubs, &dups);
        expect_int("首次入站QoS2触发PUBLISH事件", g_publish_events - before, 1);

        /* 超时前重传同一 pid(未发 PUBREL)：应识别为重复，不再投递 */
        int pl2 = raw_build_publish_qos2(cbuf, "q/in", "v2", 7, 1);
        send(rs, (char*)cbuf, pl2, 0);
        raw_pump(srv4, rs, acc, &acc_len, 300, &pubs, &dups);
        expect_int("超时前重复PUBLISH被去重", g_publish_events - before, 1);

        /* 等待超过超时窗口(1s)：待 PUBREL 记录应被清理 */
        raw_pump(srv4, rs, acc, &acc_len, 1600, &pubs, &dups);

        /* 再次重传同一 pid：记录已清理，应作为新消息再次投递 */
        int pl3 = raw_build_publish_qos2(cbuf, "q/in", "v3", 7, 1);
        send(rs, (char*)cbuf, pl3, 0);
        raw_pump(srv4, rs, acc, &acc_len, 300, &pubs, &dups);
        expect_int("超时清理后同一pid再次投递", g_publish_events - before, 2);

        RAW_CLOSE(rs);
        nl_mqtt_server_destroy(srv4);
    }

    /* 15) 保留消息(Retained)：retain=1 的发布被保存，新订阅者立即收到(带 RETAIN 位)；
     *     空负载的 retain=1 发布用于清除保留消息 */
    {
        nl_mqtt_server_t* srv5 = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfg5;
        memset(&cfg5, 0, sizeof(cfg5));
        cfg5.port = TEST_PORT6;
        cfg5.max_clients = 8;
        expect_int("保留消息服务端启动", nl_mqtt_server_start(srv5, &cfg5), 0);

        unsigned char cbuf[256];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int pubs = 0, dups = 0, present = -1;

        /* 先有订阅者在场(接收 retain=1 的实时下发) */
        rsock_t sub = raw_connect("127.0.0.1", TEST_PORT6);
        int cl = raw_build_connect(cbuf, "RS", 1);
        send(sub, (char*)cbuf, cl, 0);
        raw_pump(srv5, sub, acc, &acc_len, 200, &pubs, &dups);
        int sl = raw_build_subscribe(cbuf, "r/keep", 0, 1);
        send(sub, (char*)cbuf, sl, 0);
        raw_pump(srv5, sub, acc, &acc_len, 200, &pubs, &dups);

        /* 服务端发布一条保留消息 */
        expect_int("服务端发布保留消息返回 0",
                   nl_mqtt_server_publish(srv5, "r/keep", "rv1", 3, 0, 1), 0);
        raw_pump(srv5, sub, acc, &acc_len, 200, &pubs, &dups);

        /* 新订阅者订阅同一主题：应立即收到保留消息且带 RETAIN 位 */
        unsigned char acc2[1024];
        size_t acc2_len = 0;
        int pubs2 = 0, dups2 = 0;
        rsock_t late = raw_connect("127.0.0.1", TEST_PORT6);
        int cl2 = raw_build_connect(cbuf, "RL", 1);
        send(late, (char*)cbuf, cl2, 0);
        raw_wait_connack(srv5, late, acc2, &acc2_len, 200, &present);
        acc2_len = 0;
        g_scan_retain = 0;
        int sl2 = raw_build_subscribe(cbuf, "r/keep", 0, 1);
        send(late, (char*)cbuf, sl2, 0);
        raw_pump(srv5, late, acc2, &acc2_len, 300, &pubs2, &dups2);
        expect_true("新订阅者收到保留消息", pubs2 >= 1);
        expect_true("保留消息带 RETAIN 位", g_scan_retain >= 1);

        /* 空负载 retain=1：清除保留消息 */
        nl_mqtt_server_publish(srv5, "r/keep", "", 0, 0, 1);
        raw_pump(srv5, late, acc2, &acc2_len, 200, &pubs2, &dups2);

        unsigned char acc3[1024];
        size_t acc3_len = 0;
        int pubs3 = 0, dups3 = 0;
        rsock_t late2 = raw_connect("127.0.0.1", TEST_PORT6);
        int cl3 = raw_build_connect(cbuf, "RL2", 1);
        send(late2, (char*)cbuf, cl3, 0);
        raw_wait_connack(srv5, late2, acc3, &acc3_len, 200, &present);
        acc3_len = 0;
        int sl3 = raw_build_subscribe(cbuf, "r/keep", 0, 1);
        send(late2, (char*)cbuf, sl3, 0);
        raw_pump(srv5, late2, acc3, &acc3_len, 300, &pubs3, &dups3);
        expect_int("清除后新订阅者不再收到保留消息", pubs3, 0);

        RAW_CLOSE(sub);
        RAW_CLOSE(late);
        RAW_CLOSE(late2);
        nl_mqtt_server_destroy(srv5);
    }

    /* 16) 遗嘱消息(Will)：异常断开(直接断 TCP，无 DISCONNECT)时由服务端发布；
     *     收到 DISCONNECT 的优雅断开不发布 */
    {
        nl_mqtt_server_t* srv6 = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfg6;
        memset(&cfg6, 0, sizeof(cfg6));
        cfg6.port = TEST_PORT7;
        cfg6.max_clients = 8;
        expect_int("遗嘱服务端启动", nl_mqtt_server_start(srv6, &cfg6), 0);

        unsigned char cbuf[256];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int pubs = 0, dups = 0, present = -1;

        /* 订阅者 */
        rsock_t sub = raw_connect("127.0.0.1", TEST_PORT7);
        int cl = raw_build_connect(cbuf, "WS", 1);
        send(sub, (char*)cbuf, cl, 0);
        raw_pump(srv6, sub, acc, &acc_len, 200, &pubs, &dups);
        int sl = raw_build_subscribe(cbuf, "w/gone", 0, 1);
        send(sub, (char*)cbuf, sl, 0);
        raw_pump(srv6, sub, acc, &acc_len, 200, &pubs, &dups);

        /* 遗嘱客户端：直接断 TCP -> 服务端应发布遗嘱 */
        rsock_t wc = raw_connect("127.0.0.1", TEST_PORT7);
        int wcl = raw_build_connect_will(cbuf, "WW", 1, "w/gone", "bye", 0, 0);
        send(wc, (char*)cbuf, wcl, 0);
        expect_true("遗嘱客户端 CONNACK 可解析",
                    raw_wait_connack(srv6, wc, acc, &acc_len, 200, &present));
        acc_len = 0;
        raw_pump(srv6, sub, acc, &acc_len, 100, &pubs, &dups);

        pubs = 0; dups = 0; acc_len = 0;
        RAW_CLOSE(wc);                                    /* 异常断开 */
        raw_pump(srv6, sub, acc, &acc_len, 500, &pubs, &dups);
        expect_true("异常断开后订阅者收到遗嘱", pubs >= 1);

        /* 优雅断开：发 DISCONNECT -> 不发布遗嘱 */
        rsock_t wc2 = raw_connect("127.0.0.1", TEST_PORT7);
        int wcl2 = raw_build_connect_will(cbuf, "WW2", 1, "w/gone", "bye2", 0, 0);
        send(wc2, (char*)cbuf, wcl2, 0);
        raw_wait_connack(srv6, wc2, acc, &acc_len, 200, &present);
        acc_len = 0;
        raw_pump(srv6, sub, acc, &acc_len, 100, &pubs, &dups);

        pubs = 0; dups = 0; acc_len = 0;
        unsigned char disc[2];
        int dl = raw_build_disconnect(disc);
        send(wc2, (char*)disc, dl, 0);
        raw_pump(srv6, sub, acc, &acc_len, 400, &pubs, &dups);
        RAW_CLOSE(wc2);
        raw_pump(srv6, sub, acc, &acc_len, 200, &pubs, &dups);
        expect_int("优雅断开发布遗嘱次数", pubs, 0);

        RAW_CLOSE(sub);
        nl_mqtt_server_destroy(srv6);
    }

    /* 17) 落盘并发独占锁 + 校验和：
     *   - 第二实例使用同一落盘文件启动被拒(NL_MQTT_SERVER_ERR_STORE_LOCKED)；
     *   - 落盘文件被篡改时加载被拒(会话不恢复，Session Present=0) */
    {
        const char* store = "nl_ck_test.bin";
        remove_store_files(store);

        nl_mqtt_server_t* srvA = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfgA;
        memset(&cfgA, 0, sizeof(cfgA));
        cfgA.port = TEST_PORT8;
        cfgA.max_clients = 8;
        cfgA.session_store_path = store;
        expect_int("校验和测试服务端A启动(持锁)", nl_mqtt_server_start(srvA, &cfgA), 0);

        /* 第二实例使用同一落盘文件：应因独占锁失败 */
        nl_mqtt_server_t* srvB = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfgB;
        memset(&cfgB, 0, sizeof(cfgB));
        cfgB.port = TEST_PORT9;
        cfgB.max_clients = 8;
        cfgB.session_store_path = store;
        expect_int("第二实例同落盘文件启动被拒(-10)",
                   nl_mqtt_server_start(srvB, &cfgB), NL_MQTT_SERVER_ERR_STORE_LOCKED);
        nl_mqtt_server_destroy(srvB);

        unsigned char cbuf[256];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int pubs = 0, dups = 0, present = -1;

        /* 建立持久会话并订阅，随后停止 A(自动落盘并释放锁) */
        rsock_t rs = raw_connect("127.0.0.1", TEST_PORT8);
        int cl = raw_build_connect(cbuf, "CK", 0);
        send(rs, (char*)cbuf, cl, 0);
        raw_wait_connack(srvA, rs, acc, &acc_len, 200, &present);
        acc_len = 0;
        int sl = raw_build_subscribe(cbuf, "ck/t", 1, 1);
        send(rs, (char*)cbuf, sl, 0);
        raw_pump(srvA, rs, acc, &acc_len, 200, &pubs, &dups);
        RAW_CLOSE(rs);
        pump(srvA, NULL, 30);
        expect_int("服务端A停止(落盘并释放锁)", nl_mqtt_server_stop(srvA), 0);
        nl_mqtt_server_destroy(srvA);

        /* 篡改落盘文件末尾 1 字节 -> 校验和失效 */
        int corrupt_ok = 0;
        FILE* fp = fopen(store, "r+b");
        if (fp) {
            if (fseek(fp, -1, SEEK_END) == 0) {
                int c = fgetc(fp);
                if (c != EOF) {
                    fseek(fp, -1, SEEK_END);
                    fputc((c ^ 0xFF) & 0xFF, fp);
                    corrupt_ok = 1;
                }
            }
            fclose(fp);
        }
        expect_true("落盘文件已被篡改", corrupt_ok);

        /* 篡改文件下仍可启动(加载失败被容忍)，但持久会话未恢复 */
        nl_mqtt_server_t* srvC = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfgC;
        memset(&cfgC, 0, sizeof(cfgC));
        cfgC.port = TEST_PORT8;
        cfgC.max_clients = 8;
        cfgC.session_store_path = store;
        expect_int("篡改文件下服务端C仍可启动", nl_mqtt_server_start(srvC, &cfgC), 0);

        unsigned char acc2[1024];
        size_t acc2_len = 0;
        int present2 = -1;
        rsock_t rs2 = raw_connect("127.0.0.1", TEST_PORT8);
        int cl2 = raw_build_connect(cbuf, "CK", 0);
        send(rs2, (char*)cbuf, cl2, 0);
        expect_true("服务端C CONNACK 可解析",
                    raw_wait_connack(srvC, rs2, acc2, &acc2_len, 200, &present2));
        expect_int("篡改文件被拒: Session Present=0", present2, 0);

        RAW_CLOSE(rs2);
        nl_mqtt_server_destroy(srvC);
        remove_store_files(store);
    }

    /* 18) MQTT 5.0 属性：CONNECT 属性透传 + CONNACK 能力属性 + 订阅标识符回传 */
    {
        nl_mqtt_server_t* srv7 = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfg7;
        memset(&cfg7, 0, sizeof(cfg7));
        cfg7.port = TEST_PORT10;
        cfg7.max_clients = 8;
        nl_mqtt_server_set_event_callback(srv7, on_server_event, NULL);
        expect_int("v5 属性服务端启动", nl_mqtt_server_start(srv7, &cfg7), 0);

        unsigned char cbuf[512];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int present = -1, pubs = 0, dups = 0;

        /* CONNECT v5：Session Expiry Interval=120、User Property(k=v) */
        unsigned char cprops[] = { 0x11, 0x00, 0x00, 0x00, 0x78,
                                   0x26, 0x00, 0x01, 'k', 0x00, 0x01, 'v' };
        rsock_t rc = raw_connect("127.0.0.1", TEST_PORT10);
        expect_true("v5 客户端 TCP 连接", rc >= 0);
        int cl = raw_build_connect_v5(cbuf, "V5C", 1, cprops, sizeof(cprops));
        send(rc, (char*)cbuf, cl, 0);
        expect_true("v5 CONNACK 可解析",
                    raw_wait_connack(srv7, rc, acc, &acc_len, 300, &present));
        expect_int("v5 CONNACK Session Present=0", present, 0);
        expect_true("v5 CONNACK 含能力属性(Retain Available=1)",
                    raw_has_bytes(acc, acc_len, 0x25, 0x01));
        expect_true("CONNECT 事件透传 Session Expiry(0x11)", (g_evt_props >> 0x11) & 1ull);
        expect_true("CONNECT 事件透传 User Property(0x26)", (g_evt_props >> 0x26) & 1ull);
        acc_len = 0;

        /* SUBSCRIBE v5：订阅标识符(0x0B)=7 */
        unsigned char sprops[] = { 0x0B, 0x07 };
        int sl = raw_build_subscribe_v5(cbuf, "v5/t", 0, 1, sprops, sizeof(sprops));
        send(rc, (char*)cbuf, sl, 0);
        raw_pump(srv7, rc, acc, &acc_len, 200, &pubs, &dups);

        /* 服务端下发 -> 出站 v5 PUBLISH 应含订阅标识符(0x0B=7) */
        acc_len = 0;
        nl_mqtt_server_publish(srv7, "v5/t", "m5", 2, 0, 0);
        raw_capture(srv7, rc, acc, &acc_len, 300);
        expect_true("出站 v5 PUBLISH 含订阅标识符(0x0B=7)",
                    raw_has_bytes(acc, acc_len, 0x0B, 0x07));

        /* 上行 v5 PUBLISH：Message Expiry(0x02)+User Property -> 事件透传 */
        unsigned char pprops[] = { 0x02, 0x00, 0x00, 0x00, 0x3C,
                                   0x26, 0x00, 0x01, 'a', 0x00, 0x01, 'b' };
        int before = g_publish_events;
        int pl = raw_build_publish_v5(cbuf, "v5/up", "u", 0, 0, 0, 0, pprops,
                                      sizeof(pprops));
        send(rc, (char*)cbuf, pl, 0);
        raw_pump(srv7, rc, acc, &acc_len, 200, &pubs, &dups);
        expect_int("v5 上行 PUBLISH 触发事件", g_publish_events - before, 1);
        expect_true("PUBLISH 事件透传 Message Expiry(0x02)", (g_evt_props >> 0x02) & 1ull);
        expect_true("PUBLISH 事件透传 User Property(0x26)", (g_evt_props >> 0x26) & 1ull);

        RAW_CLOSE(rc);
        nl_mqtt_server_destroy(srv7);
    }

    /* 19) MQTT 5.0 主题别名：空主题 + 别名解析为已登记主题 */
    {
        nl_mqtt_server_t* srv8 = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfg8;
        memset(&cfg8, 0, sizeof(cfg8));
        cfg8.port = TEST_PORT11;
        cfg8.max_clients = 8;
        nl_mqtt_server_set_event_callback(srv8, on_server_event, NULL);
        expect_int("主题别名服务端启动", nl_mqtt_server_start(srv8, &cfg8), 0);

        unsigned char cbuf[512];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int present = -1, pubs = 0, dups = 0;

        rsock_t rc = raw_connect("127.0.0.1", TEST_PORT11);
        int cl = raw_build_connect_v5(cbuf, "V5A", 1, NULL, 0);
        send(rc, (char*)cbuf, cl, 0);
        raw_wait_connack(srv8, rc, acc, &acc_len, 300, &present);
        acc_len = 0;

        /* 首条：带主题 + 别名=3(登记映射) */
        unsigned char aprops[] = { 0x23, 0x00, 0x03 };
        int before = g_publish_events;
        int p1 = raw_build_publish_v5(cbuf, "alias/t", "one", 0, 0, 0, 0,
                                      aprops, sizeof(aprops));
        send(rc, (char*)cbuf, p1, 0);
        raw_pump(srv8, rc, acc, &acc_len, 200, &pubs, &dups);
        expect_int("别名首条触发事件", g_publish_events - before, 1);
        expect_true("别名首条主题正确", strcmp(g_evt_topic, "alias/t") == 0);

        /* 次条：空主题 + 别名=3 -> 解析为 alias/t */
        before = g_publish_events;
        int p2 = raw_build_publish_v5(cbuf, NULL, "two", 0, 0, 0, 0,
                                      aprops, sizeof(aprops));
        send(rc, (char*)cbuf, p2, 0);
        raw_pump(srv8, rc, acc, &acc_len, 200, &pubs, &dups);
        expect_int("别名声空主题触发事件", g_publish_events - before, 1);
        expect_true("别名声空主题解析正确", strcmp(g_evt_topic, "alias/t") == 0);

        RAW_CLOSE(rc);
        nl_mqtt_server_destroy(srv8);
    }

    /* 20) 增量落盘：快照 + 变更追加；加载时回放(快照+变更)恢复会话与订阅 */
    {
        const char* store  = "nl_inc_test.bin";
        const char* store2 = "nl_inc_test2.bin";
        remove_store_files(store);
        remove_store_files(store2);

        nl_mqtt_server_t* srv9 = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfg9;
        memset(&cfg9, 0, sizeof(cfg9));
        cfg9.port = TEST_PORT12;
        cfg9.max_clients = 8;
        cfg9.session_store_path = store;
        expect_int("增量落盘服务端启动", nl_mqtt_server_start(srv9, &cfg9), 0);

        unsigned char cbuf[256];
        unsigned char acc[1024];
        size_t acc_len = 0;
        int pubs = 0, dups = 0, present = -1;

        /* C1: 持久会话 + 订阅 inc/t */
        rsock_t c1 = raw_connect("127.0.0.1", TEST_PORT12);
        int l1 = raw_build_connect(cbuf, "INC1", 0);
        send(c1, (char*)cbuf, l1, 0);
        raw_wait_connack(srv9, c1, acc, &acc_len, 200, &present);
        acc_len = 0;
        int s1 = raw_build_subscribe(cbuf, "inc/t", 1, 1);
        send(c1, (char*)cbuf, s1, 0);
        raw_pump(srv9, c1, acc, &acc_len, 200, &pubs, &dups);

        expect_int("首次落盘(写快照)", nl_mqtt_server_save_sessions(srv9), 0);

        /* C2: 第二个持久会话 + 订阅 inc/z */
        rsock_t c2 = raw_connect("127.0.0.1", TEST_PORT12);
        int l2 = raw_build_connect(cbuf, "INC2", 0);
        send(c2, (char*)cbuf, l2, 0);
        raw_wait_connack(srv9, c2, acc, &acc_len, 200, &present);
        acc_len = 0;
        int s2 = raw_build_subscribe(cbuf, "inc/z", 1, 1);
        send(c2, (char*)cbuf, s2, 0);
        raw_pump(srv9, c2, acc, &acc_len, 200, &pubs, &dups);

        expect_int("增量落盘(追加变更)", nl_mqtt_server_save_sessions(srv9), 0);

        /* 复制“快照+变更”日志，供后续验证增量回放 */
        expect_int("复制增量日志", copy_file(store, store2), 0);

        RAW_CLOSE(c1);
        RAW_CLOSE(c2);
        pump(srv9, NULL, 30);
        nl_mqtt_server_stop(srv9);
        nl_mqtt_server_destroy(srv9);

        /* 从“快照+变更”日志重启加载 */
        nl_mqtt_server_t* srv10 = nl_mqtt_server_create();
        nl_mqtt_server_config_t cfg10;
        memset(&cfg10, 0, sizeof(cfg10));
        cfg10.port = TEST_PORT12;
        cfg10.max_clients = 8;
        cfg10.session_store_path = store2;
        expect_int("加载增量日志服务端启动", nl_mqtt_server_start(srv10, &cfg10), 0);

        /* INC1 会话与订阅恢复 */
        unsigned char accA[1024];
        size_t accA_len = 0;
        int presentA = -1;
        rsock_t r1 = raw_connect("127.0.0.1", TEST_PORT12);
        int lr1 = raw_build_connect(cbuf, "INC1", 0);
        send(r1, (char*)cbuf, lr1, 0);
        raw_wait_connack(srv10, r1, accA, &accA_len, 200, &presentA);
        expect_int("增量回放后 INC1 Session Present=1", presentA, 1);
        accA_len = 0;
        int pubsA = 0, dupsA = 0;
        nl_mqtt_server_publish(srv10, "inc/t", "x", 1, 1, 0);
        raw_pump(srv10, r1, accA, &accA_len, 400, &pubsA, &dupsA);
        expect_true("增量回放后 INC1 订阅有效", pubsA >= 1);

        /* INC2(由 DELTA 追加)会话与订阅恢复 */
        unsigned char accB[1024];
        size_t accB_len = 0;
        int presentB = -1;
        rsock_t r2 = raw_connect("127.0.0.1", TEST_PORT12);
        int lr2 = raw_build_connect(cbuf, "INC2", 0);
        send(r2, (char*)cbuf, lr2, 0);
        raw_wait_connack(srv10, r2, accB, &accB_len, 200, &presentB);
        expect_int("增量回放后 INC2 Session Present=1", presentB, 1);
        accB_len = 0;
        int pubsB = 0, dupsB = 0;
        nl_mqtt_server_publish(srv10, "inc/z", "y", 1, 1, 0);
        raw_pump(srv10, r2, accB, &accB_len, 400, &pubsB, &dupsB);
        expect_true("增量回放后 INC2 订阅有效", pubsB >= 1);

        RAW_CLOSE(r1);
        RAW_CLOSE(r2);
        nl_mqtt_server_destroy(srv10);
        remove_store_files(store);
        remove_store_files(store2);
    }

    /* 21) MQTT 5.0 增补：能力通告 / $SYS / 共享订阅 / No Local / 原因码 / QoS3 / AUTH */
    {
        nl_mqtt_server_t* srv11 = nl_mqtt_server_create();
        nl_mqtt_server_config_t c11;
        memset(&c11, 0, sizeof(c11));
        c11.port = TEST_PORT13;
        c11.max_clients = 16;
        c11.receive_maximum  = 20;
        c11.max_packet_size  = 4096;
        c11.server_reference = "other.example:1883";
        nl_mqtt_server_set_event_callback(srv11, on_server_event, NULL);
        expect_int("v5 增补服务端启动", nl_mqtt_server_start(srv11, &c11), 0);

        unsigned char cbuf[512];
        unsigned char acc[2048];
        size_t acc_len = 0;
        int present = -1;

        /* 空 client id(v5) -> 服务端分配 + CONNACK 能力属性 */
        rsock_t rc = raw_connect("127.0.0.1", TEST_PORT13);
        int cl = raw_build_connect_v5(cbuf, "", 1, NULL, 0);
        send(rc, (char*)cbuf, cl, 0);
        expect_true("空 client id CONNACK 可解析",
                    raw_wait_connack(srv11, rc, acc, &acc_len, 300, &present));
        expect_true("CONNACK 含 Assigned Client Identifier(0x12)",
                    raw_has_bytes(acc, acc_len, 0x12, 0x00));
        expect_true("CONNACK 含 Receive Maximum(0x21)",
                    raw_has_bytes(acc, acc_len, 0x21, 0x00));
        expect_true("CONNACK 含 Maximum Packet Size(0x27)",
                    raw_has_bytes(acc, acc_len, 0x27, 0x00));
        expect_true("CONNACK 含 Server Reference(0x1C)",
                    raw_has_bytes(acc, acc_len, 0x1C, 0x00));
        acc_len = 0;

        /* QoS3(非法) -> 服务端断开连接 */
        {
            rsock_t bad = raw_connect("127.0.0.1", TEST_PORT13);
            send(bad, (char*)cbuf, raw_build_connect_v5(cbuf, "Q3", 1, NULL, 0), 0);
            unsigned char a2[1024];
            size_t a2l = 0;
            int pr = -1;
            raw_wait_connack(srv11, bad, a2, &a2l, 300, &pr);
            /* QoS3 PUBLISH：0x36 + 剩余5 + topic(1) + packet id(2) */
            unsigned char p3[7] = { 0x36, 0x05, 0x00, 0x01, 't', 0x00, 0x01 };
            send(bad, (char*)p3, (int)sizeof(p3), 0);
            int rn = -1;
            char tb[64];
            for (int i = 0; i < 30; i++) {
                nl_mqtt_server_poll(srv11, 0);
                rn = recv(bad, tb, sizeof(tb), 0);
                if (rn == 0) break;
                nl_sleep_ms(10);
            }
            expect_true("QoS3 被拒并断开", rn == 0);
            RAW_CLOSE(bad);
        }

        /* AUTH 报文 -> 不支持增强认证(0x8C)并断开 */
        {
            rsock_t au = raw_connect("127.0.0.1", TEST_PORT13);
            send(au, (char*)cbuf, raw_build_connect_v5(cbuf, "AU1", 1, NULL, 0), 0);
            unsigned char a4[1024];
            size_t a4l = 0;
            int pr = -1;
            raw_wait_connack(srv11, au, a4, &a4l, 300, &pr);
            unsigned char authf[2] = { 0xF0, 0x00 };
            send(au, (char*)authf, 2, 0);
            int rn = -1;
            char tb[64];
            for (int i = 0; i < 30; i++) {
                nl_mqtt_server_poll(srv11, 0);
                rn = recv(au, tb, sizeof(tb), 0);
                if (rn == 0) break;
                nl_sleep_ms(10);
            }
            expect_true("AUTH 被拒并断开", rn == 0);
            RAW_CLOSE(au);
        }

        /* $SYS：订阅 $SYS/# 应收到统计 PUBLISH */
        {
            rsock_t sys = raw_connect("127.0.0.1", TEST_PORT13);
            send(sys, (char*)cbuf, raw_build_connect_v5(cbuf, "SYS", 1, NULL, 0), 0);
            unsigned char a3[1024];
            size_t a3l = 0;
            int pr = -1, sp = 0, sd = 0;
            raw_wait_connack(srv11, sys, a3, &a3l, 300, &pr);
            a3l = 0;
            send(sys, (char*)cbuf, raw_build_subscribe_v5(cbuf, "$SYS/#", 0, 1, NULL, 0), 0);
            raw_pump(srv11, sys, a3, &a3l, 300, &sp, &sd);
            expect_true("订阅 $SYS/# 收到统计 PUBLISH", sp >= 1);
            RAW_CLOSE(sys);
        }

        /* 共享订阅：组内两成员轮流各收一次 */
        {
            rsock_t s1 = raw_connect("127.0.0.1", TEST_PORT13);
            rsock_t s2 = raw_connect("127.0.0.1", TEST_PORT13);
            unsigned char z1[1024], z2[1024];
            size_t z1l = 0, z2l = 0;
            int pp = -1, p1 = 0, d1 = 0, p2 = 0, d2 = 0;
            send(s1, (char*)cbuf, raw_build_connect_v5(cbuf, "SH1", 1, NULL, 0), 0);
            raw_wait_connack(srv11, s1, z1, &z1l, 300, &pp);
            z1l = 0;
            send(s2, (char*)cbuf, raw_build_connect_v5(cbuf, "SH2", 1, NULL, 0), 0);
            raw_wait_connack(srv11, s2, z2, &z2l, 300, &pp);
            z2l = 0;
            send(s1, (char*)cbuf, raw_build_subscribe_v5(cbuf, "$share/g1/sh/t", 0, 1, NULL, 0), 0);
            raw_pump(srv11, s1, z1, &z1l, 200, &p1, &d1);
            send(s2, (char*)cbuf, raw_build_subscribe_v5(cbuf, "$share/g1/sh/t", 0, 1, NULL, 0), 0);
            raw_pump(srv11, s2, z2, &z2l, 200, &p2, &d2);
            p1 = 0; p2 = 0;
            nl_mqtt_server_publish(srv11, "sh/t", "a", 1, 0, 0);
            nl_mqtt_server_publish(srv11, "sh/t", "b", 1, 0, 0);
            raw_pump(srv11, s1, z1, &z1l, 300, &p1, &d1);
            raw_pump(srv11, s2, z2, &z2l, 300, &p2, &d2);
            expect_int("共享订阅合计仅投递 2 次", p1 + p2, 2);
            expect_true("共享订阅组内各收一次", p1 == 1 && p2 == 1);
            RAW_CLOSE(s1);
            RAW_CLOSE(s2);
        }

        /* No Local：订阅带 NL 后，自己发布的消息不应回发给自己 */
        {
            rsock_t n1 = raw_connect("127.0.0.1", TEST_PORT13);
            send(n1, (char*)cbuf, raw_build_connect_v5(cbuf, "NL1", 1, NULL, 0), 0);
            unsigned char nacc[1024];
            size_t nl2 = 0;
            int pp = -1, p = 0, d = 0;
            raw_wait_connack(srv11, n1, nacc, &nl2, 300, &pp);
            nl2 = 0;
            send(n1, (char*)cbuf,
                 raw_build_subscribe_v5_opt(cbuf, "nl/t", 0x04 /*No Local*/, 1, NULL, 0), 0);
            raw_pump(srv11, n1, nacc, &nl2, 200, &p, &d);
            p = 0;
            send(n1, (char*)cbuf,
                 raw_build_publish_v5(cbuf, "nl/t", "self", 0, 0, 0, 0, NULL, 0), 0);
            raw_pump(srv11, n1, nacc, &nl2, 300, &p, &d);
            expect_int("No Local 不回发给发布者", p, 0);
            RAW_CLOSE(n1);
        }

        /* 原因码：QoS1 发布到无订阅主题 -> PUBACK 0x10 */
        {
            rsock_t q1 = raw_connect("127.0.0.1", TEST_PORT13);
            send(q1, (char*)cbuf, raw_build_connect_v5(cbuf, "RC1", 1, NULL, 0), 0);
            unsigned char racc[1024];
            size_t rl = 0;
            int pp = -1;
            raw_wait_connack(srv11, q1, racc, &rl, 300, &pp);
            rl = 0;
            send(q1, (char*)cbuf,
                 raw_build_publish_v5(cbuf, "no/sub", "x", 5, 1, 0, 0, NULL, 0), 0);
            raw_capture(srv11, q1, racc, &rl, 300);
            expect_int("无匹配订阅 PUBACK 原因码=0x10",
                       raw_find_ack_reason(racc, rl, 0x4), 0x10);
            RAW_CLOSE(q1);
        }

        RAW_CLOSE(rc);
        nl_mqtt_server_destroy(srv11);
    }

    printf("\n=== 结果：%d/%d 通过，%d 失败 ===\n",
           g_total - g_fail, g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}

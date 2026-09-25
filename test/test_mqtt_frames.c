/*
 * test_mqtt_frames.c - MQTT 客户端帧构造与缓冲区扩容的回归测试
 *
 * 本测试不依赖任何真实网络：直接复用 netleaf_mqtt.c 内部的静态构造函数
 * （nl_mqtt_build_connect/publish/subscribe/unsubscribe_packet）与
 * nl_mqtt_expand_buffer()，对编解码长度做断言。
 *
 * 覆盖点：
 *   - H1：nl_mqtt_expand_buffer 扩容时不得改写调用方维护的有效长度 *len；
 *   - H2：固定头“剩余长度”前缀预留区的回收（memmove 左移）是否正确，
 *         保证 帧长 == 1(类型) + 长度字段字节数 + 剩余长度；
 *   - 长度字段在 1/2/3 字节三种可变字节编码下的结果。
 *
 * 构建：随顶层 CMakeLists.txt 的测试段（BUILD_TESTS=ON）自动注册。
 */

#ifndef NL_MQTT_STATIC
#define NL_MQTT_STATIC /* 本测试编译并链接实现本体，禁用 dllimport */
#endif

#include "netleaf_mqtt.c" /* 直接包含实现以访问内部静态函数 */

#include <stdio.h>
#include <string.h>

static int g_total = 0;
static int g_fail  = 0;

static void expect_long(const char* what, long got, long want) {
    g_total++;
    if (got == want) {
        printf("  [PASS] %-28s = %ld\n", what, got);
    } else {
        g_fail++;
        printf("  [FAIL] %-28s = %ld (期望 %ld)\n", what, got, want);
    }
}

static void expect_nonnull(const char* what, const void* p) {
    g_total++;
    if (p) {
        printf("  [PASS] %-28s != NULL\n", what);
    } else {
        g_fail++;
        printf("  [FAIL] %-28s == NULL\n", what);
    }
}

/* 解析 MQTT 可变字节编码，返回其数值；out_bytes 返回编码占用字节数 */
static size_t decode_varint(const unsigned char* p, int* out_bytes) {
    size_t value = 0;
    size_t multiplier = 1;
    int i = 0;
    for (; i < 4; i++) {
        unsigned char b = p[i];
        value += (size_t)(b & 0x7F) * multiplier;
        multiplier *= 128;
        if ((b & 0x80) == 0) { i++; break; }
    }
    if (out_bytes) *out_bytes = i;
    return value;
}

/* 在缓冲区中查找子串（避免依赖平台相关实现） */
static int contains_bytes(const char* hay, size_t hay_len,
                          const char* needle, size_t needle_len) {
    if (needle_len == 0 || needle_len > hay_len) return 0;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

static nl_mqtt_client_t* make_client(int protocol) {
    nl_mqtt_client_t* c = nl_mqtt_create();
    if (c && protocol != 0) nl_mqtt_set_protocol_version(c, protocol);
    return c;
}

/* 校验 c->out_buf 中 [start, out_buf_len) 这段报文的固定头与长度自洽性 */
static void verify_frame(const char* name, nl_mqtt_client_t* c, size_t start,
                         uint8_t want_type, size_t want_remaining, size_t want_total) {
    size_t total = c->out_buf_len - start;
    const unsigned char* p = (const unsigned char*)c->out_buf + start;
    int vbytes = 0;
    size_t remaining = decode_varint(p + 1, &vbytes);

    printf("  -- %s --\n", name);
    expect_long("帧总长度", (long)total, (long)want_total);
    expect_long("固定头类型", (long)p[0], (long)want_type);
    expect_long("剩余长度", (long)remaining, (long)want_remaining);
    /* 自洽性：报文体必须恰好填满固定头之后的空间 */
    expect_long("总长=类型+长度字段+剩余", (long)total, (long)(1 + vbytes) + (long)remaining);
}

static void test_encoded_len(void) {
    printf("\n[用例] MQTT 长度字段编码位数\n");
    expect_long("encoded_len(0)",       (long)nl_mqtt_encoded_len(0), 1);
    expect_long("encoded_len(127)",     (long)nl_mqtt_encoded_len(127), 1);
    expect_long("encoded_len(128)",     (long)nl_mqtt_encoded_len(128), 2);
    expect_long("encoded_len(16383)",   (long)nl_mqtt_encoded_len(16383), 2);
    expect_long("encoded_len(16384)",   (long)nl_mqtt_encoded_len(16384), 3);
    expect_long("encoded_len(2097151)", (long)nl_mqtt_encoded_len(2097151), 3);
    expect_long("encoded_len(2097152)", (long)nl_mqtt_encoded_len(2097152), 4);
}

static void test_expand_buffer(void) {
    printf("\n[用例] nl_mqtt_expand_buffer 语义（H1）\n");

    char*  buf = NULL;
    size_t len = 7;   /* 模拟调用方已有的有效数据长度 */
    size_t cap = 0;

    int ok = nl_mqtt_expand_buffer(&buf, &len, &cap, 1000);
    expect_long("扩容返回", ok, 1);
    expect_nonnull("缓冲区指针", buf);
    expect_long("有效长度未被改写", (long)len, 7);
    g_total++;
    if (cap >= 1000) { printf("  [PASS] 容量满足需求 >= 1000\n"); }
    else { g_fail++; printf("  [FAIL] 容量 = %zu (< 1000)\n", cap); }

    /* 写入标记数据，验证扩容不破坏既有内容 */
    for (int i = 0; i < 7; i++) buf[i] = (char)(0x40 + i);

    ok = nl_mqtt_expand_buffer(&buf, &len, &cap, 5000);
    expect_long("二次扩容返回", ok, 1);
    expect_long("二次扩容后有效长度", (long)len, 7);
    int intact = 1;
    for (int i = 0; i < 7; i++) {
        if (buf[i] != (char)(0x40 + i)) { intact = 0; break; }
    }
    expect_long("扩容后既有数据完整", intact, 1);

    /* 容量已足够时：不扩容、不改写长度 */
    size_t cap_before = cap;
    ok = nl_mqtt_expand_buffer(&buf, &len, &cap, 16);
    expect_long("容量足够时返回", ok, 1);
    expect_long("容量足够时长度不变", (long)len, 7);
    expect_long("容量足够时不重分配", (long)cap, (long)cap_before);

    free(buf);
}

static void test_connect_frame(void) {
    printf("\n[用例] CONNECT 帧编码\n");

    /* v3.1.1，默认选项：MQTT + 默认 client id "netleaf_mqtt"
       剩余长度 = 2+4(协议名) + 1(级别) + 1(标志) + 2(keepalive) + 2+12(client id) = 24 */
    nl_mqtt_client_t* c = make_client(NL_MQTT_PROTOCOL_V3_1_1);
    expect_nonnull("client(v3)", c);
    if (c) {
        size_t start = c->out_buf_len;
        expect_long("build_connect 返回", nl_mqtt_build_connect_packet(c, NULL), 0);
        verify_frame("CONNECT v3.1.1", c, start, 0x10, 24, 26);
        nl_mqtt_destroy(c);
    }

    /* v5：MQTT + 强制 1 字节属性长度前缀(0x00)
       剩余长度 = 2+4 + 1 + 1 + 2 + 1 + 2+12 = 25 */
    c = make_client(NL_MQTT_PROTOCOL_V5);
    expect_nonnull("client(v5)", c);
    if (c) {
        size_t start = c->out_buf_len;
        expect_long("build_connect(v5) 返回", nl_mqtt_build_connect_packet(c, NULL), 0);
        verify_frame("CONNECT v5.0", c, start, 0x10, 25, 27);
        nl_mqtt_destroy(c);
    }

    /* v5 + 一个属性（MAX_PACKET_SIZE=1000，占 5 字节：1 字节标识符 + 4 字节值）：
       属性长度前缀由 4 字节预留区回收为 1 字节
       剩余长度 = 2+4 + 1 + 1 + 2 + (1+5) + 2+12 = 30 */
    c = make_client(NL_MQTT_PROTOCOL_V5);
    expect_nonnull("client(v5+prop)", c);
    if (c) {
        nl_mqtt_connect_opts_t opts;
        nl_mqtt_property_t* props = NULL;
        memset(&opts, 0, sizeof(opts));
        opts.keep_alive = 60;
        expect_long("property_add_int 返回",
                    nl_mqtt_property_add_int(&props, NL_MQTT_PROP_MAX_PACKET_SIZE, 1000), 0);
        opts.properties = props;

        size_t start = c->out_buf_len;
        expect_long("build_connect(v5+prop) 返回",
                    nl_mqtt_build_connect_packet(c, &opts), 0);
        verify_frame("CONNECT v5.0 + 属性", c, start, 0x10, 30, 32);

        nl_mqtt_property_list_destroy(props);
        nl_mqtt_destroy(c);
    }
}

static void test_publish_frame(void) {
    printf("\n[用例] PUBLISH 帧编码\n");
    const char* topic = "a/b";
    const char* payload = "hi";

    /* v3.1.1 QoS0：2+3(topic) + 2(payload) = 7 */
    nl_mqtt_client_t* c = make_client(NL_MQTT_PROTOCOL_V3_1_1);
    if (c) {
        size_t start = c->out_buf_len;
        nl_mqtt_build_publish_packet(c, topic, payload, 2, NULL);
        verify_frame("PUBLISH v3.1.1 QoS0", c, start, 0x30, 7, 9);
        expect_long("topic 出现在报文中", contains_bytes(c->out_buf + start, c->out_buf_len - start, topic, 3), 1);
        nl_mqtt_destroy(c);
    }

    /* v5 QoS0：额外 2 字节报文标识符 + 1 字节空属性
       剩余长度 = 2+3 + 2 + 1 + 2 = 10 */
    c = make_client(NL_MQTT_PROTOCOL_V5);
    if (c) {
        size_t start = c->out_buf_len;
        nl_mqtt_build_publish_packet(c, topic, payload, 2, NULL);
        verify_frame("PUBLISH v5.0 QoS0", c, start, 0x30, 10, 12);
        nl_mqtt_destroy(c);
    }

    /* 大负载 → 2 字节长度字段：剩余长度 = 2+3 + 200 = 205 */
    c = make_client(NL_MQTT_PROTOCOL_V3_1_1);
    if (c) {
        char big[200];
        memset(big, 0x5A, sizeof(big));
        size_t start = c->out_buf_len;
        nl_mqtt_build_publish_packet(c, topic, big, sizeof(big), NULL);
        verify_frame("PUBLISH v3.1.1 负载200", c, start, 0x30, 205, 208);
        nl_mqtt_destroy(c);
    }

    /* 大负载 → 3 字节长度字段：剩余长度 = 2+3 + 20000 = 20005 */
    c = make_client(NL_MQTT_PROTOCOL_V3_1_1);
    if (c) {
        char* big = (char*)malloc(20000);
        expect_nonnull("20000 字节负载缓冲", big);
        if (big) {
            memset(big, 0x33, 20000);
            size_t start = c->out_buf_len;
            nl_mqtt_build_publish_packet(c, topic, big, 20000, NULL);
            verify_frame("PUBLISH v3.1.1 负载20000", c, start, 0x30, 20005, 20009);
            free(big);
        }
        nl_mqtt_destroy(c);
    }
}

static void test_subscribe_unsubscribe_frames(void) {
    printf("\n[用例] SUBSCRIBE / UNSUBSCRIBE 帧编码\n");
    const char* topic = "a/b";

    /* SUBSCRIBE v3.1.1：2(pid) + 2+3(topic) + 1(qos) = 8 */
    nl_mqtt_client_t* c = make_client(NL_MQTT_PROTOCOL_V3_1_1);
    if (c) {
        size_t start = c->out_buf_len;
        nl_mqtt_build_subscribe_packet(c, topic, 1);
        verify_frame("SUBSCRIBE v3.1.1", c, start, 0x82, 8, 10);
        nl_mqtt_destroy(c);
    }

    /* SUBSCRIBE v5：额外 1 字节空属性 → 9 */
    c = make_client(NL_MQTT_PROTOCOL_V5);
    if (c) {
        size_t start = c->out_buf_len;
        nl_mqtt_build_subscribe_packet(c, topic, 1);
        verify_frame("SUBSCRIBE v5.0", c, start, 0x82, 9, 11);
        nl_mqtt_destroy(c);
    }

    /* UNSUBSCRIBE v3.1.1：2(pid) + 2+3(topic) = 7 */
    c = make_client(NL_MQTT_PROTOCOL_V3_1_1);
    if (c) {
        size_t start = c->out_buf_len;
        nl_mqtt_build_unsubscribe_packet(c, topic);
        verify_frame("UNSUBSCRIBE v3.1.1", c, start, 0xA2, 7, 9);
        nl_mqtt_destroy(c);
    }

    /* UNSUBSCRIBE v5：额外 1 字节空属性 → 8 */
    c = make_client(NL_MQTT_PROTOCOL_V5);
    if (c) {
        size_t start = c->out_buf_len;
        nl_mqtt_build_unsubscribe_packet(c, topic);
        verify_frame("UNSUBSCRIBE v5.0", c, start, 0xA2, 8, 10);
        nl_mqtt_destroy(c);
    }
}

static void test_qos_ack_frames(void) {
    printf("\n[用例] QoS 确认帧（PUBACK/PUBREC/PUBREL/PUBCOMP）\n");

    struct { uint8_t type; int (*fn)(nl_mqtt_client_t*, uint16_t); const char* name; } cases[] = {
        { 0x40, nl_mqtt_send_puback,  "PUBACK"  },
        { 0x50, nl_mqtt_send_pubrec,  "PUBREC"  },
        { 0x62, nl_mqtt_send_pubrel,  "PUBREL"  },
        { 0x70, nl_mqtt_send_pubcomp, "PUBCOMP" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        nl_mqtt_client_t* c = make_client(NL_MQTT_PROTOCOL_V3_1_1);
        if (!c) {
            g_total++; g_fail++;
            printf("  [FAIL] %s 客户端创建失败\n", cases[i].name);
            continue;
        }
        c->status = NL_MQTT_CONNECTED;

        size_t start = c->out_buf_len;
        int rc = cases[i].fn(c, 0x1234);
        expect_long("发送返回", rc, 0);
        verify_frame(cases[i].name, c, start, cases[i].type, 2, 4);

        /* 报文标识符 0x12 0x34 必须原样保留 */
        g_total++;
        {
            const unsigned char* p = (const unsigned char*)c->out_buf + start;
            if (p[2] == 0x12 && p[3] == 0x34) {
                printf("  [PASS] %-28s\n", "报文标识符原样保留");
            } else {
                g_fail++;
                printf("  [FAIL] %s 报文标识符 = %02x%02x\n", cases[i].name, p[2], p[3]);
            }
        }
        nl_mqtt_destroy(c);
    }
}

int main(void) {
    printf("=== MQTT 帧构造与缓冲区扩容回归测试 ===\n");

    nl_mqtt_init_sock();

    test_encoded_len();
    test_expand_buffer();
    test_connect_frame();
    test_publish_frame();
    test_subscribe_unsubscribe_frames();
    test_qos_ack_frames();

    printf("\n=== 结果：%d/%d 通过，%d 失败 ===\n",
           g_total - g_fail, g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}

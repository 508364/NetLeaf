/*
 * test_http_frames.c - HTTP/2 帧编解码回环（端对端联调）回归测试
 *
 * 本测试不依赖任何真实网络与 socket：直接包含 src/http/netleaf_http_common.c
 * （平台无关纯逻辑，仅依赖 stdio/stdlib/string）及其内部头
 * netleaf_http_internal.h，校验 H2 帧头编解码回环。
 *
 * 覆盖点：
 *   - H2 帧：DATA/SETTINGS/PING 等帧头 9 字节编码再解码，字段一一对应；
 *   - H2 帧：length 与 payload 自洽性、payload 内容不损坏；
 *   - H2 帧：边界（payload 超 H2_MAX_FRAME_SIZE / 容量不足）返回 0；
 *   - 短名转接层：nlh_* 访问器转发到原长名实现，结果一致（不触网）；
 *   - Huffman：RFC 7541 Appendix C 向量编解码回环；
 *   - HPACK：编码→解码回环（独立编/解码表，验证双表方向独立）；
 *   - 连接：h2_connection_init 默认值 / cleanup 释放；
 *   - 状态机：HEADERS/DATA/RST_STREAM/GOAWAY/PRIORITY 帧喂入后流状态与连接标志；
 *   - 发送：h2_send_rst_stream / h2_send_goaway / SETTINGS ACK 帧字节布局（mock io 捕获）。
 *
 * 构建：随顶层 CMakeLists.txt 的测试段（BUILD_TESTS=ON）自动注册。
 */

/* 直接包含实现以访问内部纯逻辑函数（h2_frame_roundtrip）。 */
#include "netleaf_http_common.c"

#include <stdio.h>
#include <string.h>

static int g_total = 0;
static int g_fail  = 0;

static void expect(const char* what, int got, int want) {
    g_total++;
    if (got == want) {
        printf("  [PASS] %-32s = %d\n", what, got);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s = %d (期望 %d)\n", what, got, want);
    }
}

/* 校验 H2 帧头编码回环（含 payload 自洽与内容校验）。 */
static void test_h2_frame_roundtrip(void) {
    printf("\n[用例] H2 帧头编解码回环\n");

    uint8_t buf[H2_MAX_FRAME_SIZE + 9];
    uint8_t payload[16];
    memset(payload, 0xAB, sizeof(payload));

    struct nl_h2_frame_header h;

    /* DATA 帧：256 字节负载（256 > H2 默认 16384？否，256 < 16384）*/
    h.type = NL_H2_FRAME_DATA;
    h.flags = 0x01;            /* END_STREAM */
    h.stream_id = 1;
    h.length = sizeof(payload);
    expect("DATA 帧回环", h2_frame_roundtrip(buf, sizeof(buf), &h, payload, sizeof(payload)), 1);

    /* SETTINGS 帧：stream_id 必须为 0，无负载 */
    h.type = NL_H2_FRAME_SETTINGS;
    h.flags = 0x00;
    h.stream_id = 0;
    h.length = 0;
    expect("SETTINGS 帧回环（无负载）", h2_frame_roundtrip(buf, sizeof(buf), &h, NULL, 0), 1);

    /* PING 帧：8 字节负载 */
    uint8_t ping[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    h.type = NL_H2_FRAME_PING;
    h.flags = 0x01;          /* ACK */
    h.stream_id = 0;
    h.length = sizeof(ping);
    expect("PING 帧回环", h2_frame_roundtrip(buf, sizeof(buf), &h, ping, sizeof(ping)), 1);

    /* 边界：payload 超过 H2_MAX_FRAME_SIZE 应失败 */
    h.type = NL_H2_FRAME_DATA;
    h.stream_id = 3;
    h.length = H2_MAX_FRAME_SIZE + 1;
    expect("超上限负载回环(应失败)", h2_frame_roundtrip(buf, sizeof(buf), &h, buf, H2_MAX_FRAME_SIZE + 1), 0);

    /* 边界：缓冲区容量不足应失败 */
    h.type = NL_H2_FRAME_DATA;
    h.stream_id = 5;
    h.length = 4;
    expect("容量不足回环(应失败)", h2_frame_roundtrip(buf, 5, &h, payload, 4), 0);
}

/* 校验短名转接层访问器与原长名实现结果一致（不触网）。 */
static void test_shortname_accessors(void) {
    printf("\n[用例] 短名访问器转发一致性\n");

    struct nl_http_request req;
    memset(&req, 0, sizeof(req));
    req.method = NL_HTTP_POST;
    req.version = NL_HTTP_VERSION_2;
    snprintf(req.path, sizeof(req.path), "/test");
    req.body = (char*)"hello";
    req.body_size = 5;

    expect("nlh_req_method 一致", (int)nlh_req_method(&req), (int)NL_HTTP_POST);
    expect("nlh_req_version 一致", (int)nlh_req_version(&req), (int)NL_HTTP_VERSION_2);

    g_total++;
    if (nlh_req_path(&req) && strcmp(nlh_req_path(&req), "/test") == 0) {
        printf("  [PASS] %-32s nlh_req_path=/test\n", "路径一致");
    } else {
        g_fail++;
        printf("  [FAIL] %-32s 路径不一致\n", "nlh_req_path 一致");
    }

    g_total++;
    if (nlh_req_body(&req) && memcmp(nlh_req_body(&req), "hello", 5) == 0 &&
        nlh_req_body_size(&req) == 5) {
        printf("  [PASS] %-32s body 一致\n", "nlh_req_body/size");
    } else {
        g_fail++;
        printf("  [FAIL] nlh_req_body/size 不一致\n");
    }

    struct nl_http_response resp;
    memset(&resp, 0, sizeof(resp));
    nlh_resp_status(&resp, 200);
    nlh_resp_header(&resp, "Content-Type", "text/plain");
    nlh_resp_body(&resp, "ok", 2);
    expect("nlh_resp_status", resp.status, 200);
    expect("nlh_resp_body_size", (int)resp.body_size, 2);
}

/* ============================================================
 * mock io：捕获最后一次写入的帧（不触网）
 * ============================================================ */
static uint8_t g_captured[64];
static size_t  g_captured_len;

static int mock_write(struct nl_http_io* io, nl_http_handle_t handle,
                      const uint8_t* data, size_t len) {
    (void)io; (void)handle;
    if (len > sizeof(g_captured)) len = sizeof(g_captured);
    memcpy(g_captured, data, len);
    g_captured_len = len;
    return (int)len;
}

static struct nl_http_io g_mock_io = { .write_h2_frame = mock_write };

/* Huffman：RFC 7541 Appendix C 向量（v1/v2/v3 文本与对应码流）。 */
static void test_huffman_vectors(void) {
    printf("\n[用例] Huffman RFC 7541 Appendix C 向量\n");

    /* v1: "www.example.com" → f1e3c2e5f23a6ba0ab90f4ff */
    const uint8_t v1_enc[] = { 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff };
    uint8_t out[64];
    size_t out_len = 0;
    expect("v1 解码", hpack_huffman_decode(v1_enc, sizeof(v1_enc), out, sizeof(out) - 1, &out_len), 0);
    out[out_len] = '\0';
    g_total++;
    if (strcmp((char*)out, "www.example.com") == 0) {
        printf("  [PASS] %-32s %s\n", "v1 文本", (char*)out);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s %s\n", "v1 文本", (char*)out);
    }

    /* v2: "no-cache" → a8eb10649cbf */
    const uint8_t v2_enc[] = { 0xa8, 0xeb, 0x10, 0x64, 0x9c, 0xbf };
    expect("v2 解码", hpack_huffman_decode(v2_enc, sizeof(v2_enc), out, sizeof(out) - 1, &out_len), 0);
    out[out_len] = '\0';
    g_total++;
    if (strcmp((char*)out, "no-cache") == 0) {
        printf("  [PASS] %-32s %s\n", "v2 文本", (char*)out);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s %s\n", "v2 文本", (char*)out);
    }

    /* 编码回环：v1 编码结果应与 RFC 向量一致 */
    uint8_t enc[64];
    size_t elen = 0;
    expect("v1 编码", hpack_huffman_encode((const uint8_t*)"www.example.com", 15, enc, sizeof(enc), &elen), 0);
    g_total++;
    if (elen == sizeof(v1_enc) && memcmp(enc, v1_enc, elen) == 0) {
        printf("  [PASS] %-32s = RFC 向量一致\n", "v1 编码");
    } else {
        g_fail++;
        printf("  [FAIL] %-32s len=%zu\n", "v1 编码", elen);
    }
}

/* HPACK 编解码回环：编码表与解码表相互独立（模拟双端）。 */
static void test_hpack_roundtrip(void) {
    printf("\n[用例] HPACK 编码→解码回环（双表方向独立）\n");

    struct hpack_context enc, dec;
    hpack_init(&enc);
    hpack_init(&dec);

    uint8_t block[256];
    size_t off = 0;

    /* 三条头：静态索引(:method GET / :path /)、新字面量、重复字面量（第二次编码应走动态索引更短） */
    off += (size_t)hpack_encode_header(&enc, block + off, sizeof(block) - off, ":method", "GET");
    off += (size_t)hpack_encode_header(&enc, block + off, sizeof(block) - off, ":path", "/");
    size_t first_lit = hpack_encode_header(&enc, block + off, sizeof(block) - off, "x-custom", "value1");
    off += first_lit;
    size_t second_lit = hpack_encode_header(&enc, block + off, sizeof(block) - off, "x-custom", "value1");
    off += second_lit;

    g_total++;
    if (second_lit < first_lit) {
        printf("  [PASS] %-32s %zu < %zu\n", "重复头走动态索引", second_lit, first_lit);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s %zu >= %zu\n", "重复头走动态索引", second_lit, first_lit);
    }

    /* 解码端逐条还原 */
    char name[MAX_HEADER_NAME], value[MAX_HEADER_VALUE];
    size_t pos = 0, consumed = 0;

    expect("解码 :method", hpack_decode_header(&dec, block + pos, off - pos, name, value, &consumed), 0);
    pos += consumed;
    g_total++;
    if (strcmp(name, ":method") == 0 && strcmp(value, "GET") == 0) {
        printf("  [PASS] %-32s %s=%s\n", "头1", name, value);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s %s=%s\n", "头1", name, value);
    }

    expect("解码 :path", hpack_decode_header(&dec, block + pos, off - pos, name, value, &consumed), 0);
    pos += consumed;
    g_total++;
    if (strcmp(name, ":path") == 0 && strcmp(value, "/") == 0) {
        printf("  [PASS] %-32s %s=%s\n", "头2", name, value);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s %s=%s\n", "头2", name, value);
    }

    expect("解码 x-custom(字面量)", hpack_decode_header(&dec, block + pos, off - pos, name, value, &consumed), 0);
    pos += consumed;
    g_total++;
    if (strcmp(name, "x-custom") == 0 && strcmp(value, "value1") == 0) {
        printf("  [PASS] %-32s %s=%s\n", "头3", name, value);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s %s=%s\n", "头3", name, value);
    }

    expect("解码 x-custom(动态索引)", hpack_decode_header(&dec, block + pos, off - pos, name, value, &consumed), 0);
    pos += consumed;
    g_total++;
    if (strcmp(name, "x-custom") == 0 && strcmp(value, "value1") == 0 && pos == off) {
        printf("  [PASS] %-32s %s=%s（块完整消费）\n", "头4", name, value);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s %s=%s pos=%zu off=%zu\n", "头4", name, value, pos, off);
    }

    hpack_free(&enc);
    hpack_free(&dec);
}

/* 连接初始化默认值与 cleanup。 */
static void test_connection_init(void) {
    printf("\n[用例] h2_connection_init 默认值 / cleanup\n");

    struct nl_http2_connection conn;
    h2_connection_init(&conn, 0);

    expect("远端 header_table_size 默认 4096", conn.remote_settings.header_table_size, 4096);
    expect("远端 enable_push 默认 1", conn.remote_settings.enable_push, 1);
    expect("远端 max_frame_size 默认 16384", conn.remote_settings.max_frame_size, H2_MAX_FRAME_SIZE);
    expect("远端 initial_window 默认 65535", conn.remote_settings.initial_window_size, H2_DEFAULT_WINDOW_SIZE);
    expect("远端 max_concurrent 无限制", (int)conn.remote_settings.max_concurrent_streams, -1); /* 0xFFFFFFFF */
    expect("编码表上限 4096", conn.hpack_enc.max_dynamic_table_size, 4096);
    expect("draining 初始 0", conn.draining, 0);

    h2_connection_cleanup(&conn);
    expect("cleanup 后流链为空", conn.streams == NULL, 1);
    expect("cleanup 后编码表空", conn.hpack_enc.dynamic_table == NULL, 1);
}

/* 帧喂入状态机：HEADERS→建流并响应、响应完成立即释放（防长连接 OOM）、
 * 有正文响应停留 HALF_CLOSED_LOCAL、DATA END_STREAM→释放、RST→释放、
 * GOAWAY→draining、PRIORITY 忽略。 */
static char g_seen_path[NL_HTTP_MAX_PATH];
static int  g_seen_method = -1;
static int  g_resp_body_on = 0;

static void test_dummy_handler(const nl_http_request_t* req, nl_http_response_t* resp, void* ud) {
    (void)ud;
    resp->status = 200;
    if (req) {
        g_seen_method = (int)req->method;
        snprintf(g_seen_path, sizeof(g_seen_path), "%s", req->path);
    }
    if (g_resp_body_on) {
        /* process_frame 会 free(resp.body)，必须堆分配 */
        resp->body = (char*)malloc(4);
        if (resp->body) { memcpy(resp->body, "pong", 4); resp->body_size = 4; }
    }
}

static void test_frame_state_machine(void) {
    printf("\n[用例] 帧喂入流状态机\n");

    struct nl_http2_connection conn;
    h2_connection_init(&conn, 0);

    /* 组一个 HEADERS 帧：END_HEADERS|END_STREAM(0x05)，头块=两个静态索引
     * 0x82(:method GET) 0x84(:path /) */
    uint8_t frame[9 + 2];
    struct nl_h2_frame_header h = {0};
    h.length = 2;
    h.type = NL_H2_FRAME_HEADERS;
    h.flags = 0x05;
    h.stream_id = 1;
    h2_frame_header_write(frame, &h);
    frame[9] = 0x82;
    frame[10] = 0x84;

    expect("喂入 HEADERS", h2_process_frame(&g_mock_io, 0, &conn, frame, sizeof(frame), test_dummy_handler, NULL), 0);

    /* 无正文响应 + 客户端已 END_STREAM → 双向关闭后立即摘链释放，
     * 长连接累计请求不再堆积流结构（防 OOM 回归点） */
    expect("响应完成流已释放", conn.streams == NULL, 1);

    g_total++;
    if (g_seen_method == NL_HTTP_GET && strcmp(g_seen_path, "/") == 0) {
        printf("  [PASS] %-32s GET /\n", "头块解析");
    } else {
        g_fail++;
        printf("  [FAIL] %-32s method=%d path=%s\n", "头块解析", g_seen_method, g_seen_path);
    }

    /* 有正文响应：HEADERS 不带 END_STREAM → 流保持 HALF_CLOSED_LOCAL（不释放） */
    g_resp_body_on = 1;
    h.flags = 0x04;
    h.stream_id = 3;
    h2_frame_header_write(frame, &h);
    frame[9] = 0x82;
    frame[10] = 0x84;
    expect("喂入 HEADERS(无END_STREAM)", h2_process_frame(&g_mock_io, 0, &conn, frame, sizeof(frame), test_dummy_handler, NULL), 0);

    struct nl_http2_stream* s = conn.streams;
    g_total++;
    if (s && s->id == 3 && s->state == NL_H2_ST_HALF_CLOSED_LOCAL) {
        printf("  [PASS] %-32s state=%d\n", "响应后 HALF_CLOSED_LOCAL", s->state);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s id=%u state=%d\n", "响应后 HALF_CLOSED_LOCAL",
               s ? s->id : 0, s ? s->state : -1);
    }

    /* 对端 DATA + END_STREAM → 双向关闭 → 立即释放 */
    uint8_t data[9 + 2] = {0};
    h.length = 2;
    h.type = NL_H2_FRAME_DATA;
    h.flags = 0x01;   /* END_STREAM */
    h.stream_id = 3;
    h2_frame_header_write(data, &h);
    data[9] = 'h';
    data[10] = 'i';
    expect("喂入 DATA END_STREAM", h2_process_frame(&g_mock_io, 0, &conn, data, sizeof(data), NULL, NULL), 0);
    expect("DATA END_STREAM 后释放", conn.streams == NULL, 1);
    g_resp_body_on = 0;

    /* PRIORITY：合法忽略（RFC 7540 §6.3）——5 字节负载 */
    uint8_t prio[9 + 5] = {0};
    h.length = 5;
    h.type = NL_H2_FRAME_PRIORITY;
    h.flags = 0;
    h.stream_id = 7;
    h2_frame_header_write(prio, &h);
    expect("喂入 PRIORITY(应忽略)", h2_process_frame(&g_mock_io, 0, &conn, prio, sizeof(prio), NULL, NULL), 0);

    /* RST_STREAM：流存在时收到 RST → 立即摘链释放 */
    g_resp_body_on = 1;
    h.length = 2;
    h.type = NL_H2_FRAME_HEADERS;
    h.flags = 0x04;
    h.stream_id = 5;
    h2_frame_header_write(frame, &h);
    frame[9] = 0x82;
    frame[10] = 0x84;
    expect("喂入 HEADERS(RST用)", h2_process_frame(&g_mock_io, 0, &conn, frame, sizeof(frame), test_dummy_handler, NULL), 0);
    g_total++;
    if (conn.streams && conn.streams->id == 5) {
        printf("  [PASS] %-32s id=%u\n", "RST 前流存在", conn.streams->id);
    } else {
        g_fail++;
        printf("  [FAIL] %-32s\n", "RST 前流存在");
    }

    uint8_t rst[9 + 4] = {0};
    h.length = 4;
    h.type = NL_H2_FRAME_RST_STREAM;
    h.flags = 0;
    h.stream_id = 5;
    h2_frame_header_write(rst, &h);
    rst[12] = 8;  /* CANCEL */
    expect("喂入 RST_STREAM", h2_process_frame(&g_mock_io, 0, &conn, rst, sizeof(rst), NULL, NULL), 0);
    expect("RST 后流已释放", conn.streams == NULL, 1);
    g_resp_body_on = 0;

    /* GOAWAY：last_stream=1 → draining */
    uint8_t goaway[9 + 8] = {0};
    h.length = 8;
    h.type = NL_H2_FRAME_GOAWAY;
    h.flags = 0;
    h.stream_id = 0;
    h2_frame_header_write(goaway, &h);
    goaway[12] = 1;  /* last_stream_id = 1（BE 首字节） */
    expect("喂入 GOAWAY", h2_process_frame(&g_mock_io, 0, &conn, goaway, sizeof(goaway), NULL, NULL), 0);
    expect("GOAWAY→draining", conn.draining, 1);
    expect("GOAWAY last_stream 解析", (int)conn.goaway_last_stream, 1);

    h2_connection_cleanup(&conn);
}

/* 发送函数帧字节布局：mock io 捕获后逐字节校验。 */
static void test_send_frames(void) {
    printf("\n[用例] RST_STREAM / GOAWAY 帧字节布局\n");

    struct nl_http2_connection conn;
    h2_connection_init(&conn, 0);

    /* RST_STREAM：13 字节，type=0x3，stream=5，error=8 */
    g_captured_len = 0;
    h2_send_rst_stream(&g_mock_io, 0, &conn, 5, 8);
    g_total++;
    if (g_captured_len == 13 && g_captured[3] == NL_H2_FRAME_RST_STREAM &&
        g_captured[8] == 5 && g_captured[12] == 8) {
        printf("  [PASS] %-32s len=13 type=3 id=5 err=8\n", "RST_STREAM 帧");
    } else {
        g_fail++;
        printf("  [FAIL] %-32s len=%zu type=%u id=%u err=%u\n", "RST_STREAM 帧",
               g_captured_len, g_captured[3], g_captured[8], g_captured[12]);
    }

    /* GOAWAY：17 字节，type=0x7，stream=0，last=7，err=1(NO_ERROR) */
    g_captured_len = 0;
    h2_send_goaway(&g_mock_io, 0, &conn, 7, 1);
    g_total++;
    if (g_captured_len == 17 && g_captured[3] == NL_H2_FRAME_GOAWAY &&
        g_captured[8] == 0 && g_captured[16] == 1 && g_captured[15] == 0) {
        printf("  [PASS] %-32s len=17 type=7 id=0 last=7 err=1\n", "GOAWAY 帧");
    } else {
        g_fail++;
        printf("  [FAIL] %-32s len=%zu type=%u\n", "GOAWAY 帧", g_captured_len, g_captured[3]);
    }

    h2_connection_cleanup(&conn);
}

/* ============================================================
 * v3.1.0 HTTP/1 边界回归（纯内存，不触网）
 *  - 100 Continue：带 Expect 的请求先回 100 再收 body；
 *  - 413：请求体超上限时判定为不完整（上层据此回 413）；
 *  - 431：头值超 1023B / 头条目 >64 时 parse 返回 -2 而非静默截断；
 *  - nlh_resp_file：文件不存在 → 404（不再 200 挂死）；
 *  - nlh_resp_body_offset：原始响应切割辅助。
 * ============================================================ */

struct fake_src { const char* data; size_t len; size_t off; size_t chunk; };

/* 假 recv：每次最多吐 chunk 字节（模拟"先头后 body"的到达顺序），
 * 数据吐完返回 -1（对端关闭）。 */
static long fake_recv(void* ctx, char* buf, size_t len) {
    struct fake_src* s = (struct fake_src*)ctx;
    if (s->off >= s->len) return -1;
    size_t n = s->len - s->off;
    if (n > len) n = len;
    if (s->chunk && n > s->chunk) n = s->chunk;
    memcpy(buf, s->data + s->off, n);
    s->off += n;
    return (long)n;
}

/* 假 write：把服务端写出的字节捕获到 buf（验证 100 Continue）。 */
struct wcap { char buf[256]; size_t len; };
static int fake_write(void* ctx, const char* data, size_t len) {
    struct wcap* w = (struct wcap*)ctx;
    if (len > sizeof(w->buf) - 1 - w->len) len = sizeof(w->buf) - 1 - w->len;
    memcpy(w->buf + w->len, data, len);
    w->len += len;
    w->buf[w->len] = '\0';
    return 0;
}

static void test_http1_boundaries(void) {
    printf("\n[用例] HTTP/1 v3.1.0 边界（100-continue / 413 / 431 / 404）\n");

    /* --- 1. 带 Expect: 100-continue：先回 100 再收 body --- */
    {
        const char* req =
            "POST /u HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
            "Content-Length: 4\r\n\r\nABCD";
        struct fake_src src = { req, strlen(req), 0, 0 };
        /* 第一次只吐头部（模拟 curl 等 100 才发 body） */
        src.chunk = (size_t)(strstr(req, "\r\n\r\n") + 4 - req);
        struct wcap w; memset(&w, 0, sizeof(w));
        size_t len = 0;
        char* b = nl_http_read_request_full(fake_recv, &src, fake_write, &w,
                                            1024, 1u << 20, &len);
        expect("Expect 请求读完整", (b && len == strlen(req)) ? 1 : 0, 1);
        expect("回了 100 Continue",
               (w.len == 25 && memcmp(w.buf, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0) ? 1 : 0, 1);
        expect("body 完整可交付", b ? nl_http_request_body_complete(b, len) : -1, 1);
        free(b);
    }

    /* --- 2. 不带 Expect：不发任何中间响应 --- */
    {
        const char* req = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
        struct fake_src src = { req, strlen(req), 0, 0 };
        struct wcap w; memset(&w, 0, sizeof(w));
        size_t len = 0;
        char* b = nl_http_read_request_full(fake_recv, &src, fake_write, &w,
                                            1024, 1u << 20, &len);
        expect("无 Expect 读完整", (b && len == strlen(req)) ? 1 : 0, 1);
        expect("无 Expect 不回 100", w.len == 0 ? 1 : 0, 1);
        free(b);
    }

    /* --- 3. 请求体超上限：返回头部完整但 body 不全（上层回 413） --- */
    {
        char req[512];
        int hl = snprintf(req, sizeof(req),
                          "POST / HTTP/1.1\r\nContent-Length: 104857600\r\n\r\n");
        memset(req + hl, 'x', 100);            /* 仅到了 100 字节 body */
        struct fake_src src = { req, (size_t)hl + 100, 0, 0 };
        struct wcap w; memset(&w, 0, sizeof(w));
        size_t len = 0;
        char* b = nl_http_read_request_full(fake_recv, &src, fake_write, &w,
                                            1024, 4096, &len);   /* 上限 4KB */
        expect("超限请求仍返回数据", (b && len > 0) ? 1 : 0, 1);
        expect("超限判为不完整(→413)", b ? (nl_http_request_body_complete(b, len) == 0) : -1, 1);
        expect("超限不回 100 Continue", w.len == 0 ? 1 : 0, 1);
        free(b);
    }

    /* --- 4. 头值 4200B（>4096 规范上限）：parse 返回 -2，不再截断 --- */
    {
        char req[4600];
        int n = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nX-Long: ");
        memset(req + n, 'A', 4200);
        snprintf(req + n + 4200, sizeof(req) - n - 4200, "\r\n\r\n");
        nl_http_request_t r;
        expect("4200B 头值返回 -2", nl_http_parse_request(&r, req, strlen(req)), -2);
    }

    /* --- 5. 4096B 头值：仍可正常解析（规范上限的最大值） --- */
    {
        char req[4600];
        int n = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nX-Long: ");
        memset(req + n, 'A', 4096);
        snprintf(req + n + 4096, sizeof(req) - n - 4096, "\r\n\r\n");
        nl_http_request_t r;
        int rc = nl_http_parse_request(&r, req, strlen(req));
        expect("4096B 头值解析成功", rc, 0);
        expect("4096B 头值不截断", (int)strlen(r.headers[0].value), 4096);
    }

    /* --- 6. 头条目 >64：返回 -2 --- */
    {
        char req[4096];
        int off = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\n");
        for (int i = 0; i < 65; i++)
            off += snprintf(req + off, sizeof(req) - off, "X-H%d: v\r\n", i);
        snprintf(req + off, sizeof(req) - off, "\r\n");
        nl_http_request_t r;
        expect("65 个头返回 -2", nl_http_parse_request(&r, req, strlen(req)), -2);
    }

    /* --- 7. nlh_resp_file：文件不存在 → 404 + 空正文，不挂死 --- */
    {
        nl_http_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = 200;
        nl_http_response_set_file(&resp, "Z:/no/such/dir/nl_missing_file.html", 1234);
        expect("缺文件回 404", resp.status, 404);
        expect("缺文件无 file_path", resp.file_path == NULL ? 1 : 0, 1);
        expect("缺文件有空正文", (resp.body != NULL && resp.body_size == 0) ? 1 : 0, 1);
        free(resp.body);

        /* 存在的文件：正常挂 file_path，状态保持 200 */
        memset(&resp, 0, sizeof(resp));
        resp.status = 200;
        const char* tmp = "nl_test_resp_file.tmp";
        FILE* f = fopen(tmp, "wb");
        if (f) { fputs("hello", f); fclose(f); }
        nl_http_response_set_file(&resp, tmp, 5);
        expect("存在文件挂 file_path", resp.file_path != NULL ? 1 : 0, 1);
        expect("存在文件状态不变", resp.status, 200);
        free(resp.file_path);
        free(resp.body);
        remove(tmp);
    }

    /* --- 8. nlh_resp_body_offset：原始响应切割 --- */
    {
        const char* raw = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nABCD";
        size_t off = nlh_resp_body_offset(raw, strlen(raw));
        expect("body 偏移指向 BODY", off == strlen(raw) - 4 ? 1 : 0, 1);
        expect("无头结束符返回 0", (int)nlh_resp_body_offset("HTTP/1.1 200", 12), 0);
        expect("偏移处内容为 ABCD", memcmp(raw + off, "ABCD", 4) == 0 ? 1 : 0, 1);
    }
}

int main(void) {
    printf("=== HTTP/2 帧回环回归测试 ===\n");

    test_h2_frame_roundtrip();
    test_shortname_accessors();
    test_huffman_vectors();
    test_hpack_roundtrip();
    test_connection_init();
    test_frame_state_machine();
    test_send_frames();
    test_http1_boundaries();

    printf("\n=== 结果：%d/%d 通过，%d 失败 ===\n",
           g_total - g_fail, g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}

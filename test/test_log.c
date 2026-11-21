/*
 * test_log.c - 统一日志系统回归测试（第193轮）
 *
 * 覆盖：
 *   1. nl_log_get_level 默认值与 set_level 生效
 *   2. 级别过滤：低于当前级别的日志不进 callback
 *   3. callback 分发：收到级别与消息内容
 *   4. nl_debug_enable 后级别降到 DEBUG
 *   5. 文件 sink：写入内容、格式（毫秒时间戳/线程ID/级别）
 *   6. 文件轮转：超过 max_bytes 生成 path.1，keep 生效
 *   7. nl_log_close_file 后不再写文件
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netleaf.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        g_failures++; \
    } \
} while (0)

/* ---- callback 捕获 ---- */
static nl_log_level_t g_cb_level;
static char g_cb_msg[512];
static int g_cb_count;

static void test_cb(nl_log_level_t level, const char* message, void* userdata) {
    (void)userdata;
    g_cb_level = level;
    snprintf(g_cb_msg, sizeof(g_cb_msg), "%s", message);
    g_cb_count++;
}

/* ---- 文件内容读取 ---- */
static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

int main(void) {
    printf("=== LogSystemTest ===\n");

    /* 1. 默认级别 INFO */
    printf("\n-- level management --\n");
    CHECK(nl_log_get_level() == NL_LOG_INFO, "default level is INFO");

    nl_log_set_level(NL_LOG_WARN);
    CHECK(nl_log_get_level() == NL_LOG_WARN, "set_level(WARN) takes effect");

    /* 2/3. 级别过滤 + callback 分发 */
    nl_log_set_callback(test_cb, NULL);
    g_cb_count = 0;
    g_cb_msg[0] = '\0';

    nl_log_info("should be filtered");
    CHECK(g_cb_count == 0, "INFO filtered out when level=WARN");

    nl_log_warn("warn message %d", 42);
    CHECK(g_cb_count == 1, "WARN delivered to callback");
    CHECK(g_cb_level == NL_LOG_WARN, "callback receives correct level");
    CHECK(strcmp(g_cb_msg, "warn message 42") == 0, "callback message formatted");

    nl_log_error("boom");
    CHECK(g_cb_count == 2, "ERROR delivered to callback");

    /* 4. debug 模式 */
    nl_debug_enable(1);
    CHECK(nl_debug_is_enabled() == 1, "debug enabled");
    CHECK(nl_log_get_level() == NL_LOG_DEBUG, "debug_enable lowers level to DEBUG");
    /* debug_enable 自身会发一条 "Debug mode enabled"（INFO），随后 debug 又一条，
     * 故 callback 计数由 2 -> 4 */
    nl_log_debug("dbg %s", "now");
    CHECK(g_cb_count == 4, "DEBUG delivered after debug_enable");

    /* 5. 文件 sink 写入 */
    printf("\n-- file sink --\n");
    const char* log_path = "test_log_out.log";
    remove(log_path);
    remove("test_log_out.log.1");
    remove("test_log_out.log.2");
    remove("test_log_out.log.3");

    /* 关掉 callback，避免干扰；文件 sink 独立于 callback 验证过，此处测纯文件路径 */
    nl_log_set_callback(NULL, NULL);
    nl_log_set_level(NL_LOG_INFO);

    int rc = nl_log_set_file(log_path, 0, 0);
    CHECK(rc == NL_OK, "nl_log_set_file opens file");

    nl_log_info("file hello %d", 123);
    nl_log_error("file err");

    char* content = read_file(log_path);
    CHECK(content != NULL, "log file readable");
    if (content) {
        CHECK(strstr(content, "file hello 123") != NULL, "info line written to file");
        CHECK(strstr(content, "[INFO ]") != NULL, "level field present and padded");
        CHECK(strstr(content, "[ERROR]") != NULL, "error line written to file");
        CHECK(strstr(content, "[T") != NULL, "thread id present in file line");
        /* 第194轮格式：[级别] [T线程ID] [时间戳] 消息 —— 行首必须是级别 */
        CHECK(strncmp(content, "[INFO ] [T", 10) == 0, "line starts with level then tid");
        /* 文件 sink 永不着色：不得出现 ANSI 转义序列 */
        CHECK(strchr(content, '\033') == NULL, "file content has no ANSI escape");
        /* 毫秒时间戳：HH:MM:SS.mmm */
        int has_ms = 0;
        /* 直接找 ".mmm" 模式：秒后跟点和3位数字 */
        for (const char* q = content; *q; q++) {
            if (*q == ':' && q[1] >= '0' && q[1] <= '9' && q[2] >= '0' && q[2] <= '9' &&
                q[3] == ':' && q[6] == '.' &&
                q[7] >= '0' && q[7] <= '9' && q[8] >= '0' && q[8] <= '9' &&
                q[9] >= '0' && q[9] <= '9') {
                has_ms = 1;
                break;
            }
        }
        CHECK(has_ms, "millisecond timestamp present");
        free(content);
    }

    /* 7. 关闭后不再写 */
    nl_log_close_file();
    nl_log_info("after close");
    content = read_file(log_path);
    if (content) {
        CHECK(strstr(content, "after close") == NULL, "no writes after close_file");
        free(content);
    } else {
        CHECK(0, "file still exists after close (info goes to stderr only)");
    }

    /* 6. 轮转：max_bytes=400, keep=2 */
    printf("\n-- rotation --\n");
    remove(log_path);
    remove("test_log_out.log.1");
    remove("test_log_out.log.2");
    remove("test_log_out.log.3");

    rc = nl_log_set_file(log_path, 400, 2);
    CHECK(rc == NL_OK, "set_file with rotation params");

    for (int i = 0; i < 30; i++) {
        nl_log_warn("rotation payload line number %03d aaaaaaaaaaaaaaaaaaaa", i);
    }

    CHECK(file_exists(log_path), "current file exists after rotation");
    CHECK(file_exists("test_log_out.log.1"), "rotated file path.1 exists");
    CHECK(file_exists("test_log_out.log.2"), "rotated file path.2 exists (keep=2)");
    CHECK(!file_exists("test_log_out.log.3"), "path.3 not created (keep=2)");

    /* 当前文件不应超过 max_bytes 太多（允许单条日志的余量） */
    content = read_file(log_path);
    if (content) {
        size_t sz = strlen(content);
        printf("  [INFO] current file size = %zu bytes (max=400)\n", sz);
        CHECK(sz < 400 + 512, "current file bounded by max_bytes + one line");
        free(content);
    } else {
        CHECK(0, "current file readable");
    }

    nl_log_close_file();

    /* 边界：NULL path 关闭（重复关闭安全） */
    CHECK(nl_log_set_file(NULL, 0, 0) == NL_OK, "set_file(NULL) returns OK");

    /* 清理 */
    remove(log_path);
    remove("test_log_out.log.1");
    remove("test_log_out.log.2");
    remove("test_log_out.log.3");

    /* 恢复默认状态（进程退出前） */
    nl_log_set_level(NL_LOG_INFO);
    nl_debug_enable(0);

    printf("\n=== %s (%d failure(s)) ===\n", g_failures == 0 ? "ALL PASS" : "FAILED",
           g_failures);
    return g_failures == 0 ? 0 : 1;
}

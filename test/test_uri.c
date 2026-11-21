/*
 * test_uri.c — RFC 3986 URI 端点 + 物理文件隔离回归测试（第192轮）
 *
 * 覆盖：
 *   1) nl_url_decode / nl_url_encode（严格 percent 编解码）；
 *   2) nl_uri_normalize（剥 query/fragment、绝对形式、dot-segment、
 *      越根拒绝、反斜杠/控制字符拒绝）；
 *   3) nl_uri_resolve_file（根目录约束、越界拒绝、不存在文件放行给 404、
 *      缓冲溢出、符号链接逃逸[POSIX]）；
 *   4) nl_uri_is_under（前缀边界，/base 不匹配 /baseXXX）。
 * 路由匹配（nl_px_path_match）的规范化经由工程级 WebEngineTest 覆盖。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/nl_uri.h"

#if defined(_WIN32)
    #include <direct.h>
    #define NL_T_MKDIR(p) _mkdir(p)
#else
    #include <sys/stat.h>
    #include <unistd.h>
    #define NL_T_MKDIR(p) mkdir((p), 0755)
#endif

static int g_fail = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            printf("  [PASS] %s\n", (name));                                  \
        } else {                                                              \
            printf("  [FAIL] %s\n", (name));                                  \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* ---------------------------------------------------------------- */
static void test_decode(void) {
    printf("== nl_url_decode ==\n");
    char* s;

    s = nl_url_decode("a%20b");
    CHECK(s && strcmp(s, "a b") == 0, "a%20b -> 'a b'");
    free(s);

    s = nl_url_decode("%E4%B8%AD");
    CHECK(s && (unsigned char)s[0] == 0xE4 && (unsigned char)s[1] == 0xB8 &&
          (unsigned char)s[2] == 0xAD && s[3] == '\0',
          "%E4%B8%AD -> UTF-8 bytes");
    free(s);

    s = nl_url_decode("a+b");
    CHECK(s && strcmp(s, "a+b") == 0, "'+' stays literal (not form-encoded)");
    free(s);

    s = nl_url_decode("%G0");
    CHECK(s == NULL, "invalid hex %G0 -> NULL");
    free(s);

    s = nl_url_decode("%2");
    CHECK(s == NULL, "truncated %2 -> NULL");
    free(s);

    s = nl_url_decode("%00");
    CHECK(s == NULL, "%00 -> NULL");
    free(s);

    s = nl_url_decode("%0d%0a");
    CHECK(s == NULL, "%0d%0a (CRLF) -> NULL");
    free(s);
}

static void test_encode(void) {
    printf("== nl_url_encode ==\n");
    char* s = nl_url_encode("a b/c?");
    CHECK(s && strcmp(s, "a%20b%2Fc%3F") == 0, "'a b/c?' -> a%20b%2Fc%3F");
    free(s);

    const char* raw = "中文 path/1?x=1";
    char* enc = nl_url_encode(raw);
    CHECK(enc != NULL, "encode non-ascii");
    if (enc) {
        char* dec = nl_url_decode(enc);
        CHECK(dec && strcmp(dec, raw) == 0, "decode(encode(x)) == x");
        free(dec);
        free(enc);
    }

    char* e2 = nl_url_encode(NULL);
    CHECK(e2 == NULL, "encode(NULL) -> NULL");
}

/* ---------------------------------------------------------------- */
static void test_normalize(void) {
    printf("== nl_uri_normalize ==\n");
    char out[NL_URI_MAX];

    struct { const char* in, *want; } ok[] = {
        {"/a/b?x=1#f",      "/a/b"},
        {"/a/./b//c",       "/a/b/c"},
        {"/a/b/../c",       "/a/c"},
        {"/a/%2e%2e/b",     "/b"},        /* 解码在前：%2e%2e ≡ .. */
        {"/a%2fb",          "/a/b"},      /* %2F 解码为分隔符 */
        {"/",               "/"},
        {"/.",              "/"},
        {"/a/",             "/a/"},       /* 尾斜杠语义保留 */
        {"/a/b/../../c",    "/c"},
        {"http://h:8080/p?q", "/p"},      /* 绝对形式请求行（RFC 7230） */
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        int rc = nl_uri_normalize(ok[i].in, out, sizeof(out));
        char name[128];
        snprintf(name, sizeof(name), "normalize(\"%s\") = \"%s\"",
                 ok[i].in, ok[i].want);
        CHECK(rc == NL_URI_OK && strcmp(out, ok[i].want) == 0, name);
    }

    struct { const char* in; int want; } bad[] = {
        {"/../x",     NL_URI_ESCAPES},   /* 越根 */
        {"/a/../../x", NL_URI_ESCAPES},
        {"/%2e%2e/etc", NL_URI_ESCAPES}, /* 编码变体同样越根 */
        {"/a\\b",     NL_URI_EINVAL},    /* '\' 不属于 RFC 3986 路径 */
        {"/a%zz",     NL_URI_EINVAL},    /* 非法 hex */
        {"/a%00b",    NL_URI_EINVAL},    /* NUL */
        {"/a%0ab",    NL_URI_EINVAL},    /* 控制字符（防响应拆分） */
        {"",          NL_URI_EINVAL},
        {"abc",       NL_URI_EINVAL},    /* 非绝对路径且非绝对形式 */
        {"/a",        NL_URI_OK},        /* 正常（用于对照上面的失败） */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]) - 1; i++) {
        int rc = nl_uri_normalize(bad[i].in, out, sizeof(out));
        char name[128];
        snprintf(name, sizeof(name), "normalize(\"%s\") -> %d",
                 bad[i].in, bad[i].want);
        CHECK(rc == bad[i].want, name);
    }

#if defined(_WIN32)
    CHECK(nl_uri_normalize("/C:/x", out, sizeof(out)) == NL_URI_EINVAL,
          "Windows: '/C:/x' rejected (drive letter)");
    CHECK(nl_uri_normalize("/C%3A/x", out, sizeof(out)) == NL_URI_EINVAL,
          "Windows: encoded drive letter rejected");
#else
    CHECK(nl_uri_normalize("/C:/x", out, sizeof(out)) == NL_URI_OK,
          "POSIX: '/C:/x' allowed (':' is a legal filename byte)");
#endif

    /* 缓冲不足 */
    char tiny[8];
    CHECK(nl_uri_normalize("/aaaaaaaaaaaa", tiny, sizeof(tiny)) == NL_URI_ENOSPC,
          "small cap -> ENOSPC");
}

/* ---------------------------------------------------------------- */
static void test_resolve_file(void) {
    printf("== nl_uri_resolve_file / nl_uri_is_under ==\n");

    const char* root = "nl_uri_test_root";
    NL_T_MKDIR(root);
#if defined(_WIN32)
    _chdir(root);
#else
    chdir(root);
#endif
    FILE* f = fopen("hello.txt", "w");
    if (f) { fputs("hi", f); fclose(f); }

    char out[NL_URI_MAX];
    int rc;

    rc = nl_uri_resolve_file(".", "/hello.txt", out, sizeof(out));
    CHECK(rc == NL_URI_OK, "existing file -> OK");
    char absfile[NL_URI_MAX] = "";
    if (rc == NL_URI_OK) {
        CHECK(strstr(out, "hello.txt") != NULL, "resolved path ends with hello.txt");
        CHECK(strstr(out, "..") == NULL, "no dot segments in resolved path");
        memcpy(absfile, out, strlen(out) + 1);
    }

    rc = nl_uri_resolve_file(".", "/missing.txt", out, sizeof(out));
    CHECK(rc == NL_URI_OK, "missing file -> OK (caller maps to 404)");

    rc = nl_uri_resolve_file(".", "/../etc/passwd", out, sizeof(out));
    CHECK(rc == NL_URI_ESCAPES, "traversal -> ESCAPES (403)");

    rc = nl_uri_resolve_file(".", "/%2e%2e/etc/passwd", out, sizeof(out));
    CHECK(rc == NL_URI_ESCAPES, "encoded traversal -> ESCAPES (403)");

    rc = nl_uri_resolve_file(".", "/a%00b", out, sizeof(out));
    CHECK(rc == NL_URI_EINVAL, "NUL in path -> EINVAL (400)");

    rc = nl_uri_resolve_file(".", "/hello.txt?x=1", out, sizeof(out));
    CHECK(rc == NL_URI_OK && strstr(out, "hello.txt?") == NULL,
          "query stripped before file join");

    rc = nl_uri_resolve_file("nl_no_such_root_xyz", "/a", out, sizeof(out));
    CHECK(rc == NL_URI_EROOT, "nonexistent root -> EROOT (403)");

    char tiny[4];
    rc = nl_uri_resolve_file(".", "/hello.txt", tiny, sizeof(tiny));
    CHECK(rc == NL_URI_ENOSPC, "small out cap -> ENOSPC");

#if !defined(_WIN32)
    /* 符号链接逃逸：root/link -> /etc/passwd（或创建一个外部文件） */
    if (symlink("/etc/hosts", "escape_link") == 0) {
        rc = nl_uri_resolve_file(".", "/escape_link", out, sizeof(out));
        CHECK(rc == NL_URI_ESCAPES, "symlink escaping root -> ESCAPES");
        unlink("escape_link");
    } else {
        printf("  [SKIP] symlink creation failed\n");
    }
#endif

    /* is_under 边界（rootc 由首个 resolve 结果反推） */
    if (absfile[0]) {
        size_t fl = strlen(absfile) - strlen("/hello.txt");
        char rootc[NL_URI_MAX], nested[NL_URI_MAX], sib[NL_URI_MAX];
        memcpy(rootc, absfile, fl);
        rootc[fl] = '\0';
        snprintf(nested, sizeof(nested), "%s/sub/deep.txt", rootc);
        snprintf(sib, sizeof(sib), "%sX/hello.txt", rootc);   /* rootcX ≠ rootc */
        CHECK(nl_uri_is_under(rootc, absfile) == 1, "is_under: file in root");
        CHECK(nl_uri_is_under(rootc, nested) == 1,
              "is_under: nonexistent nested path under root (lexical)");
        CHECK(nl_uri_is_under(rootc, sib) == 0,
              "is_under: sibling prefix (rootcX) rejected");
        CHECK(nl_uri_is_under(rootc, "/etc/passwd") == 0,
              "is_under: outside rejected");
    }

#if defined(_WIN32)
    _chdir("..");
#else
    chdir("..");
#endif
    /* 清理 */
    remove("nl_uri_test_root/hello.txt");
#if defined(_WIN32)
    _rmdir("nl_uri_test_root");
#else
    rmdir("nl_uri_test_root");
#endif
}

/* ---------------------------------------------------------------- */
int main(void) {
    test_decode();
    test_encode();
    test_normalize();
    test_resolve_file();

    if (g_fail) {
        printf("\nURI test: %d FAILED\n", g_fail);
        return 1;
    }
    printf("\nURI test: all passed\n");
    return 0;
}

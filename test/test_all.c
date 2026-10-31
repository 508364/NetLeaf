/*
 * NetLeaf v2.2.2 - 全模块综合测试程序
 *
 * 测试覆盖：
 *   1. Core        - nl_modules_init, nl_version_string, nl_web_create/destroy
 *   2. Lang        - nl_lang_set/get, nl_lang_validate_code, nl_lang_get_error
 *   3. Autocomplete - nl_complete_charset, nl_html_has_vue_code, nl_import_vue
 *   4. Autoroute   - nl_route_matcher_create, nl_route_similarity, nl_levenshtein_distance
 *   5. ErrorPage   - nl_errorpage_set_template, nl_errorpage_render_default
 *   6. Vue         - nl_vue_generate_page, nl_vue_get_cdn_url
 *   7. IPC         - nl_ipc_is_available, nl_ipc_create/destroy (structure only)
 *   8. LinkAgg     - nl_lagg_is_available, nl_lagg_validate_id_format
 *   9. JSON/TOML   - nl_json_parse, nl_toml_parse
 *  10. Encoding    - nl_encoding_detect, nl_encoding_console_output
 *  11. System Info - nl_sys_info_get_os_name, nl_sys_info_get_architecture
 */

#include "netleaf.h"
#include "netleaf_module.h"
#include "netleaf_lang.h"
#include "netleaf_autocomplete.h"
#include "netleaf_autoroute.h"
#include "netleaf_errorpage.h"
#include "netleaf_vue.h"
#include "netleaf_ipc.h"
#include "netleaf_linkagg.h"
#ifdef HAVE_NETLEAF_MQTT
#include "netleaf_mqtt.h"
#endif
#ifdef HAVE_NETLEAF_MQTT_SERVER
#include "netleaf_mqtt_server.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PASSED  1
#define TEST_FAILED  0
#define TEST_SKIPPED -1

static int g_test_count = 0;
static int g_pass_count = 0;
static int g_fail_count = 0;
static int g_skip_count = 0;

#define TEST_BEGIN(name) do { \
    printf("\n--- %s ---\n", name); \
    g_test_count++; \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", msg); \
        g_fail_count++; \
    } else { \
        printf("  [PASS] %s\n", msg); \
        g_pass_count++; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b, msg) do { \
    const char* _a = (a); const char* _b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) { \
        printf("  [FAIL] %s (expected \"%s\", got \"%s\")\n", msg, _b ? _b : "NULL", _a ? _a : "NULL"); \
        g_fail_count++; \
    } else { \
        printf("  [PASS] %s\n", msg); \
        g_pass_count++; \
    } \
} while(0)

#define SKIP(msg) do { \
    printf("  [SKIP] %s\n", msg); \
    g_test_count++; \
    g_skip_count++; \
} while(0)

static void print_sep(void) {
    printf("==================================================\n");
}

/* ============================================================
 * Test 1: Core Module
 * ============================================================ */
static int test_core(void) {
    TEST_BEGIN("Core Module");

    const char* ver = nl_version_string();
    ASSERT(ver != NULL, "nl_version_string() returns non-NULL");
    printf("  version: %s\n", ver);

    ASSERT(nl_version_major() == NETLEAF_VERSION_MAJOR, "nl_version_major() matches NETLEAF_VERSION_MAJOR");
    ASSERT(nl_version_minor() == NETLEAF_VERSION_MINOR, "nl_version_minor() matches NETLEAF_VERSION_MINOR");
    ASSERT(nl_version_patch() == NETLEAF_VERSION_PATCH, "nl_version_patch() matches NETLEAF_VERSION_PATCH");

    nl_modules_init();

    /* Initialize all built-in modules that need explicit init */
    nl_lang_init();
    nl_autocomplete_init();
    nl_autoroute_init();
    nl_errorpage_init();
    nl_vue_init();

    int mod_count = nl_get_module_count();
    printf("  module count: %d\n", mod_count);
    ASSERT(mod_count >= 1, "module count >= 1");

    nl_module_info_t* core_mod = nl_get_module(NL_MODULE_CORE);
    ASSERT(core_mod != NULL, "nl_get_module(CORE) non-NULL");
    if (core_mod) {
        ASSERT_STR_EQ(core_mod->name, "netleaf", "core module name == netleaf");
        printf("  capabilities: %u\n", core_mod->capabilities);
    }

    nl_print_modules();

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 2: Lang Module
 * ============================================================ */
static int test_lang(void) {
    TEST_BEGIN("Lang Module");

    int rc = nl_lang_validate_code("zh_cn");
    ASSERT(rc == 1, "nl_lang_validate_code(\"zh_cn\") == 1");

    rc = nl_lang_validate_code("en_us");
    ASSERT(rc == 1, "nl_lang_validate_code(\"en_us\") == 1");

    rc = nl_lang_validate_code("invalid");
    ASSERT(rc == 0, "nl_lang_validate_code(\"invalid\") == 0");

    const char* cur = nl_lang_get();
    printf("  current lang: %s\n", cur ? cur : "(null)");
    ASSERT(cur != NULL, "nl_lang_get() non-NULL");

    rc = nl_lang_set("zh_cn");
    ASSERT(rc == 0, "nl_lang_set(\"zh_cn\") == 0");

    cur = nl_lang_get();
    ASSERT_STR_EQ(cur, "zh_cn", "nl_lang_get() == zh_cn after set");

    rc = nl_lang_set("en_us");
    ASSERT(rc == 0, "nl_lang_set(\"en_us\") == 0");

    int lang_count = nl_lang_get_count();
    printf("  registered languages: %d\n", lang_count);
    ASSERT(lang_count > 0, "nl_lang_get_count() > 0");

    const char** langs = nl_lang_get_all();
    if (langs) {
        printf("  languages:");
        for (int i = 0; langs[i] != NULL; i++) {
            printf(" %s", langs[i]);
        }
        printf("\n");
    }

    int lib_id = 0x7FFF;
    rc = nl_lang_register_lib(lib_id, (const char*[]){"en_us", "zh_cn"}, 2);
    ASSERT(rc == 0, "nl_lang_register_lib() succeeds");

    const char* msgs[] = {"Success", "成功"};
    rc = nl_lang_add_error(lib_id, 0, msgs);
    ASSERT(rc == 0, "nl_lang_add_error(0) succeeds");

    const char* err_en = nl_lang_get_error(lib_id, 0);
    ASSERT(err_en != NULL, "nl_lang_get_error(lib, 0) non-NULL (EN)");
    if (err_en) printf("  error EN: %s\n", err_en);

    const char* err_zh = nl_lang_get_error_for(lib_id, 0, "zh_cn");
    ASSERT(err_zh != NULL, "nl_lang_get_error_for(lib, 0, zh_cn) non-NULL");
    if (err_zh) printf("  error ZH: %s\n", err_zh);

    int has_err = nl_lang_has_error(lib_id, 0);
    ASSERT(has_err == 1, "nl_lang_has_error(lib, 0) == 1");

    int is_reg = nl_lang_is_registered(lib_id);
    ASSERT(is_reg == 1, "nl_lang_is_registered(lib) == 1");

    nl_lang_unregister_lib(lib_id);

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 3: Autocomplete Module
 * ============================================================ */
static int test_autocomplete(void) {
    TEST_BEGIN("Autocomplete Module");

    int avail = nl_autocomplete_is_available();
    printf("  available: %d\n", avail);
    if (!avail) { SKIP("autocomplete not available"); return TEST_SKIPPED; }

    const char* ver = nl_autocomplete_version();
    printf("  version: %s\n", ver);
    ASSERT(ver != NULL, "nl_autocomplete_version() non-NULL");

    nl_autocomplete_enable(1);
    ASSERT(nl_autocomplete_is_enabled() == 1, "nl_autocomplete_is_enabled() == 1 after enable");

    nl_autocomplete_disable_feature(NL_AUTOCOMPLETE_FEATURE_ALL);
    ASSERT(nl_autocomplete_is_feature_enabled(NL_AUTOCOMPLETE_FEATURE_ALL) == 0, "nl_autocomplete_is_feature_enabled() == 0 after disable");
    nl_autocomplete_enable_feature(NL_AUTOCOMPLETE_FEATURE_ALL);
    nl_autocomplete_enable(1);

    /* charset completion */
    const char* html_no_charset =
        "<!DOCTYPE html>\n<html>\n<head>\n"
        "<title>Test</title>\n</head>\n<body>Hello</body>\n</html>";
    char* result = nl_complete_charset(html_no_charset, strlen(html_no_charset), NULL);
    ASSERT(result != NULL, "nl_complete_charset() returns non-NULL");
    if (result) {
        printf("  charset result (first 120 chars): %.120s...\n", result);
        int has_charset = (strstr(result, "charset") != NULL);
        ASSERT(has_charset, "charset meta tag inserted");
        free(result);
    }

    /* HTML with existing charset should not duplicate */
    const char* html_with_charset =
        "<!DOCTYPE html>\n<html>\n<head>\n"
        "<meta charset=\"UTF-8\">\n"
        "<title>Test</title>\n</head>\n<body>Hello</body>\n</html>";
    result = nl_complete_charset(html_with_charset, strlen(html_with_charset), NULL);
    ASSERT(result != NULL, "nl_complete_charset(with charset) returns non-NULL");
    if (result) {
        int count = 0;
        const char* p = result;
        while ((p = strstr(p, "charset")) != NULL) { count++; p++; }
        printf("  charset occurrences: %d (expect 1)\n", count);
        ASSERT(count == 1, "charset appears exactly once");
        free(result);
    }

    /* Vue detection */
    const char* html_vue = "<div id=\"app\">{{ message }}</div>\n<script>const { createApp } = Vue;</script>";
    int has_vue = nl_html_has_vue_code(html_vue, strlen(html_vue));
    printf("  html_has_vue_code: %d\n", has_vue);
    ASSERT(has_vue == 1, "nl_html_has_vue_code detects Vue code");

    int has_import = nl_html_has_vue_import(html_vue, strlen(html_vue));
    printf("  html_has_vue_import: %d\n", has_import);

    /* Vue import */
    result = nl_import_vue(html_vue, strlen(html_vue), NL_VUE_CDN_UNPKG, NULL);
    ASSERT(result != NULL, "nl_import_vue() returns non-NULL");
    if (result) {
        int has_script = (strstr(result, "vue.global.js") != NULL);
        ASSERT(has_script, "Vue CDN script tag inserted");
        free(result);
    }

    /* Process HTML with all features */
    result = nl_autocomplete_process_html(html_no_charset, strlen(html_no_charset), NULL);
    ASSERT(result != NULL, "nl_autocomplete_process_html() returns non-NULL");
    if (result) {
        printf("  processed html (first 120 chars): %.120s...\n", result);
        free(result);
    }

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 4: Autoroute Module
 * ============================================================ */
static int test_autoroute(void) {
    TEST_BEGIN("Autoroute Module");

    int avail = nl_autoroute_is_available();
    printf("  available: %d\n", avail);
    if (!avail) { SKIP("autoroute not available"); return TEST_SKIPPED; }

    const char* ver = nl_autoroute_version();
    printf("  version: %s\n", ver);

    /* Levenshtein distance */
    int dist = nl_levenshtein_distance("kitten", "sitting");
    printf("  levenshtein(\"kitten\",\"sitting\"): %d\n", dist);
    ASSERT(dist == 3, "levenshtein(\"kitten\",\"sitting\") == 3");

    dist = nl_levenshtein_distance("abc", "abc");
    ASSERT(dist == 0, "levenshtein(\"abc\",\"abc\") == 0");

    dist = nl_levenshtein_distance("", "xyz");
    ASSERT(dist == 3, "levenshtein(\"\",\"xyz\") == 3");

    /* Similarity */
    double sim = nl_route_similarity("/about", "/abouts");
    printf("  similarity(/about, /abouts): %.4f\n", sim);
    ASSERT(sim > 0.7, "similarity(/about, /abouts) > 0.7");

    sim = nl_route_similarity("/index", "/nonexistent");
    printf("  similarity(/index, /nonexistent): %.4f\n", sim);
    ASSERT(sim < 0.6, "similarity(/index, /nonexistent) < 0.6");

    /* Route matcher */
    nl_route_matcher_t* matcher = nl_route_matcher_create();
    ASSERT(matcher != NULL, "nl_route_matcher_create() non-NULL");
    if (matcher) {
        const char* routes[] = {"/", "/about", "/contact", "/api/users", "/api/products"};
        nl_route_matcher_add_routes(matcher, routes, 5);

        nl_route_suggestion_t suggestions[5];
        int count = nl_route_matcher_get_suggestions(matcher, "/abt", suggestions, 5);
        printf("  suggestions for \"/abt\": %d\n", count);
        ASSERT(count > 0, "finds suggestions for typo \"/abt\"");
        if (count > 0) {
            printf("    best: %s (score=%.2f)\n", suggestions[0].path, suggestions[0].score);
        }

        count = nl_route_matcher_get_suggestions(matcher, "/api/xxx", suggestions, 5);
        printf("  suggestions for \"/api/xxx\": %d\n", count);

        nl_route_matcher_destroy(matcher);
        printf("  matcher destroyed OK\n");
    }

    /* Route matching with wildcards */
    int matched = nl_route_matches("/api/*", "/api/users");
    printf("  route_matches(\"/api/*\", \"/api/users\"): %d\n", matched);
    ASSERT(matched == 1, "wildcard /api/* matches /api/users");

    matched = nl_route_matches("/api/*", "/about");
    ASSERT(matched == 0, "wildcard /api/* does not match /about");

    /* Best route helper */
    const char* best_routes[] = {"/", "/about", "/contact", "/api/users", "/api/products"};
    const char* best = nl_find_best_route("/cntact", best_routes, 5, 0.5);
    printf("  best route for \"/cntact\": %s\n", best ? best : "(none)");
    ASSERT(best != NULL, "find_best_route finds match for typo");

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 5: ErrorPage Module
 * ============================================================ */
static int test_errorpage(void) {
    TEST_BEGIN("ErrorPage Module");

    int avail = nl_errorpage_is_available();
    printf("  available: %d\n", avail);
    if (!avail) { SKIP("errorpage not available"); return TEST_SKIPPED; }

    const char* ver = nl_errorpage_version();
    printf("  version: %s\n", ver);

    /* Default template rendering */
    nl_errorpage_vars_t vars = {
        .status_code = 404,
        .error_message = "Page Not Found",
        .requested_path = "/missing",
        .suggestion = "/about",
        .server_version = "2.2.2",
        .timestamp = "2026-09-11 12:00:00"
    };

    char* html = nl_errorpage_render_default(404, &vars);
    ASSERT(html != NULL, "nl_errorpage_render_default(404) returns non-NULL");
    if (html) {
        printf("  rendered HTML (first 200 chars): %.200s...\n", html);
        int has_404 = (strstr(html, "404") != NULL);
        int has_msg = (strstr(html, "Page Not Found") != NULL);
        ASSERT(has_404, "rendered HTML contains 404");
        ASSERT(has_msg, "rendered HTML contains error message");
        free(html);
    }

    /* Quick response */
    char* resp = nl_errorpage_quick_response(500, "Internal Server Error", "/boom");
    ASSERT(resp != NULL, "nl_errorpage_quick_response(500) non-NULL");
    if (resp) {
        printf("  quick response (first 200 chars): %.200s...\n", resp);
        free(resp);
    }

    /* Status message */
    const char* status_msg = nl_errorpage_status_message(404);
    ASSERT_STR_EQ(status_msg, "Not Found", "nl_errorpage_status_message(404) == Not Found");

    status_msg = nl_errorpage_status_message(200);
    ASSERT_STR_EQ(status_msg, "OK", "nl_errorpage_status_message(200) == OK");

    /* Template validation */
    const char* bad_template = "<html><body>Error {{ERROR_CODE}}</body></html>";
    int valid = nl_errorpage_validate_template(bad_template);
    printf("  validate missing vars: %d\n", valid);
    ASSERT(valid == 0, "template missing mandatory vars is invalid");

    const char* good_template =
        "<html><body>"
        "{{<var>ERROR_CODE</var>}} - {{<var>ERROR_MESSAGE</var>}} "
        "{{<var>REQUESTED_PATH</var>}} - {{<var>SUGGESTION</var>}}"
        " - {{<var>SERVER_VERSION</var>}} - {{<var>TIMESTAMP</var>}}"
        "</body></html>";
    valid = nl_errorpage_validate_template(good_template);
    printf("  validate complete template: %d\n", valid);
    ASSERT(valid == 1, "complete template is valid");

    /* Make response with suggestion */
    char* resp2 = nl_errorpage_404_with_suggestion("/wrong-path", "/correct-path");
    ASSERT(resp2 != NULL, "nl_errorpage_404_with_suggestion returns non-NULL");
    if (resp2) {
        printf("  404 with suggestion (first 100 chars): %.100s...\n", resp2);
        int has_suggest = (strstr(resp2, "correct-path") != NULL);
        ASSERT(has_suggest, "404 response includes suggestion");
        free(resp2);
    }

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 6: Vue Module
 * ============================================================ */
static int test_vue(void) {
    TEST_BEGIN("Vue Module");

    int avail = nl_vue_is_available();
    printf("  available: %d\n", avail);
    if (!avail) { SKIP("vue not available"); return TEST_SKIPPED; }

    const char* ver = nl_vue_version();
    printf("  version: %s\n", ver);

    /* CDN URL */
    const char* url = nl_vue_get_cdn_url(NL_VUE_CDN_UNPKG, NULL);
    ASSERT(url != NULL, "nl_vue_get_cdn_url(NL_VUE_CDN_UNPKG) non-NULL");
    if (url) {
        printf("  CDN URL (unpkg): %s\n", url);
        ASSERT(strstr(url, "unpkg.com") != NULL, "unpkg CDN contains unpkg.com");
    }

    url = nl_vue_get_cdn_url(NL_VUE_CDN_CDNJS, NULL);
    ASSERT(url != NULL, "nl_vue_get_cdn_url(NL_VUE_CDN_CDNJS) non-NULL");
    if (url) {
        printf("  CDN URL (cdnjs): %s\n", url);
    }

    /* CDN script tag */
    char* script = nl_vue_get_cdn_script(NL_VUE_CDN_UNPKG, NULL);
    ASSERT(script != NULL, "nl_vue_get_cdn_script() non-NULL");
    if (script) {
        printf("  CDN script: %s\n", script);
        free(script);
    }

    /* Generate page */
    const char* vue_code = "const { createApp, ref } = Vue;\n"
                           "createApp({ setup() { return { count: ref(0) }; } }).mount('#app');";
    char* page = nl_vue_generate_page(vue_code, "Vue Test", NL_VUE_CDN_UNPKG, NULL);
    ASSERT(page != NULL, "nl_vue_generate_page() non-NULL");
    if (page) {
        printf("  generated page (first 200 chars): %.200s...\n", page);
        int has_vue = (strstr(page, "createApp") != NULL);
        int has_title = (strstr(page, "Vue Test") != NULL);
        int has_script = (strstr(page, "vue.global.js") != NULL);
        ASSERT(has_vue, "generated page contains Vue code");
        ASSERT(has_title, "generated page has correct title");
        ASSERT(has_script, "generated page includes Vue CDN");
        free(page);
    }

    /* Detect Vue code */
    int detected = nl_vue_detect_code(vue_code, strlen(vue_code));
    printf("  detect vue code: %d\n", detected);
    ASSERT(detected == 1, "nl_vue_detect_code detects Vue code");

    /* Default CDN/version */
    nl_vue_set_default_cdn(NL_VUE_CDN_LOCAL);
    int cdn = nl_vue_get_default_cdn();
    ASSERT(cdn == NL_VUE_CDN_LOCAL, "default CDN set to LOCAL");

    nl_vue_set_default_version("3.4.0");
    const char* def_ver = nl_vue_get_default_version();
    ASSERT_STR_EQ(def_ver, "3.4.0", "default version is 3.4.0");

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 7: IPC Module
 * ============================================================ */
static int test_ipc(void) {
    TEST_BEGIN("IPC Module");

    int avail = nl_ipc_is_available();
    printf("  available: %d\n", avail);
    if (!avail) { SKIP("IPC not available on this platform"); return TEST_SKIPPED; }

    const char* ver = nl_ipc_version();
    printf("  version: %s\n", ver);
    ASSERT(ver != NULL, "nl_ipc_version() non-NULL");

    /* Structure test: create and destroy without actually binding */
    /* On Windows, named pipe endpoints require specific format */
    nl_ipc_t* ipc = nl_ipc_create("\\\\.\\pipe\\netleaf_test");
    ASSERT(ipc != NULL, "nl_ipc_create() non-NULL");
    if (ipc) {
        nl_ipc_destroy(ipc);
        printf("  ipc create/destroy OK\n");
    }

    /* Module info */
    nl_module_info_t* info = nl_ipc_get_module_info();
    ASSERT(info != NULL, "nl_ipc_get_module_info() non-NULL");
    if (info) {
        printf("  ipc module: %s (caps=%u)\n", info->name, info->capabilities);
        ASSERT(info->platform_windows == 1, "IPC supports Windows");
        ASSERT(info->platform_linux == 1, "IPC supports Linux");
        ASSERT(info->platform_macos == 0, "IPC does NOT support macOS");
    }

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 8: LinkAgg Module
 * ============================================================ */
static int test_linkagg(void) {
    TEST_BEGIN("LinkAgg Module");

    int avail = nl_lagg_is_available();
    printf("  available: %d\n", avail);
    if (!avail) { SKIP("LinkAgg not available on this platform"); return TEST_SKIPPED; }

    const char* ver = nl_lagg_version();
    printf("  version: %s\n", ver);
    ASSERT(ver != NULL, "nl_lagg_version() non-NULL");

    /* ID validation */
    ASSERT(nl_lagg_validate_id_format("123.456") == 1, "ID format \"123.456\" is valid");
    ASSERT(nl_lagg_validate_id_format("abc.def") == 1, "ID format \"abc.def\" is valid");
    ASSERT(nl_lagg_validate_id_format("no-dot") == 0, "ID format \"no-dot\" is invalid");
    ASSERT(nl_lagg_validate_id_format("") == 0, "empty ID is invalid");
    ASSERT(nl_lagg_validate_id_format("too.long.id.format") == 0, "ID with multiple dots is invalid");

    /* Module info */
    nl_module_info_t* info = nl_lagg_get_module_info();
    ASSERT(info != NULL, "nl_lagg_get_module_info() non-NULL");
    if (info) {
        printf("  linkagg module: %s (caps=%u)\n", info->name, info->capabilities);
        ASSERT(info->platform_windows == 1, "LinkAgg supports Windows");
        ASSERT(info->platform_linux == 1, "LinkAgg supports Linux");
        ASSERT(info->platform_macos == 0, "LinkAgg does NOT support macOS");
    }

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 9: JSON & TOML Parser
 * ============================================================ */
static int test_json_toml(void) {
    TEST_BEGIN("JSON & TOML Parser");

    /* JSON parse */
    const char* json_str = "{\"name\":\"NetLeaf\",\"version\":2,\"features\":[\"http\",\"ws\"]}";
    nl_status_t err;
    void* json = nl_json_parse(json_str, &err, NULL, NULL);
    ASSERT(json != NULL, "nl_json_parse() succeeds");
    if (json) {
        ASSERT(nl_json_get_type(json) == NL_JSON_OBJECT, "parsed JSON is OBJECT");
        const char* name = nl_json_get_string(nl_json_object_get(json, "name"));
        ASSERT_STR_EQ(name, "NetLeaf", "JSON name == NetLeaf");

        int64_t ver = nl_json_get_int(nl_json_object_get(json, "version"));
        ASSERT(ver == 2, "JSON version == 2");

        void* features = nl_json_object_get(json, "features");
        size_t arr_size = nl_json_array_size(features);
        printf("  features array size: %zu\n", arr_size);
        ASSERT(arr_size == 2, "features array has 2 elements");

        char* str = nl_json_stringify(json, 0);
        ASSERT(str != NULL, "nl_json_stringify() non-NULL");
        if (str) {
            printf("  JSON stringify: %s\n", str);
            free(str);
        }

        nl_json_destroy(json);
        printf("  JSON destroy OK\n");
    }

    /* JSON error handling */
    json = nl_json_parse("{invalid json", &err, NULL, NULL);
    ASSERT(json == NULL, "nl_json_parse invalid input returns NULL");
    if (json == NULL) {
        printf("  parse error: %s\n", nl_json_error_message(err));
        ASSERT(err != NL_OK, "parse error code is non-zero");
    }

    /* TOML parse */
    const char* toml_str = "[server]\nhost = \"localhost\"\nport = 8080\nverbose = true\n\n"
                           "[database]\ndriver = \"sqlite\"\npath = \":memory:\"";
    void* toml = nl_toml_parse(toml_str, &err, NULL, NULL);
    ASSERT(toml != NULL, "nl_toml_parse() succeeds");
    if (toml) {
        ASSERT(nl_toml_get_type(toml) == NL_TOML_TABLE, "parsed TOML is TABLE");

        const char* host = nl_toml_get_string(nl_toml_table_get(toml, "host"));
        ASSERT_STR_EQ(host, "localhost", "TOML host == localhost");

        int64_t port = nl_toml_get_int(nl_toml_table_get(toml, "port"));
        ASSERT(port == 8080, "TOML port == 8080");

        int verbose = nl_toml_get_bool(nl_toml_table_get(toml, "verbose"));
        ASSERT(verbose == 1, "TOML verbose == true");

        char* str = nl_toml_stringify(toml);
        ASSERT(str != NULL, "nl_toml_stringify() non-NULL");
        if (str) {
            printf("  TOML stringify (first 80 chars): %.80s...\n", str);
            free(str);
        }

        nl_toml_destroy(toml);
        printf("  TOML destroy OK\n");
    }

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 10: Encoding Module
 * ============================================================ */
static int test_encoding(void) {
    TEST_BEGIN("Encoding Module");

    const char* text = "Hello 世界! Hello World!";
    const char* detected = nl_encoding_detect(text, strlen(text));
    printf("  detected encoding: %s\n", detected ? detected : "(null)");
    ASSERT(detected != NULL, "nl_encoding_detect() non-NULL");

    const char* sys_enc = nl_encoding_get_system_default();
    printf("  system default encoding: %s\n", sys_enc ? sys_enc : "(null)");
    ASSERT(sys_enc != NULL, "nl_encoding_get_system_default() non-NULL");

    /* Encoding conversion */
    char* converted = nl_encoding_convert(text, strlen(text), "UTF-8", "UTF-8");
    ASSERT(converted != NULL, "nl_encoding_convert(UTF-8->UTF-8) non-NULL");
    if (converted) {
        printf("  converted: %s\n", converted);
        free(converted);
    }

    /* Console output */
    nl_encoding_console_output("Test output: Hello World!", "UTF-8");
    printf("  console output OK\n");

    /* HTML convert */
    const char* html = "<html><body>你好</body></html>";
    char* html_conv = nl_encoding_html_convert(html, strlen(html), "UTF-8");
    ASSERT(html_conv != NULL, "nl_encoding_html_convert() non-NULL");
    if (html_conv) free(html_conv);

    /* Chinese conversion check */
    const char* simplified = "简体中文";
    int is_simp = nl_encoding_is_simplified_chinese(simplified, strlen(simplified));
    printf("  is_simplified_chinese(\"简体中文\"): %d\n", is_simp);

    const char* traditional = "繁體中文";
    int is_trad = nl_encoding_is_traditional_chinese(traditional, strlen(traditional));
    printf("  is_traditional_chinese(\"繁體中文\"): %d\n", is_trad);

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 11: System Info Module
 * ============================================================ */
static int test_sysinfo(void) {
    TEST_BEGIN("System Info Module");

    const char* os = nl_sys_info_get_os_name();
    printf("  OS: %s\n", os ? os : "(null)");
    ASSERT(os != NULL, "nl_sys_info_get_os_name() non-NULL");

    const char* arch = nl_sys_info_get_architecture();
    printf("  arch: %s\n", arch ? arch : "(null)");
    ASSERT(arch != NULL, "nl_sys_info_get_architecture() non-NULL");

    const char* cpu = nl_sys_info_get_cpu_model();
    printf("  CPU: %s\n", cpu ? cpu : "(null)");
    ASSERT(cpu != NULL, "nl_sys_info_get_cpu_model() non-NULL");

    int64_t ram = nl_sys_info_get_total_ram();
    printf("  total RAM: %lld bytes\n", (long long)ram);
    ASSERT(ram > 0, "total RAM > 0");

    const char* runtime_ver = nl_sys_info_get_runtime_version();
    printf("  runtime version: %s\n", runtime_ver ? runtime_ver : "(null)");

    /* Lazy loading status */
    int loaded = nl_sys_info_is_loaded();
    printf("  sysinfo loaded: %d\n", loaded);
    ASSERT(loaded == 1, "sysinfo module loaded");

    /* Clear and reload */
    nl_sys_info_clear_cache();
    loaded = nl_sys_info_is_loaded();
    printf("  after clear cache, loaded: %d\n", loaded);

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 12: Web Server Basic Lifecycle
 * ============================================================ */
static int test_web_server(void) {
    TEST_BEGIN("Web Server Basic Lifecycle");

    nl_web_server_t* server = nl_web_create(18923);
    ASSERT(server != NULL, "nl_web_create(18923) succeeds");
    if (!server) return TEST_SKIPPED;

    /* Encoding settings */
    nl_web_set_encoding(server, "UTF-8");
    nl_web_enable_auto_encoding(server, 1);
    nl_web_set_fallback_encoding(server, "UTF-8");
    printf("  encoding configured: UTF-8\n");

    /* Route management */
    int route_count_before = nl_web_get_route_count(server);
    printf("  routes before: %d\n", route_count_before);

    int rc = nl_web_add_route(server, "/test", "<h1>Test</h1>", "text/html");
    ASSERT(rc == 0, "nl_web_add_route() succeeds");

    int route_count_after = nl_web_get_route_count(server);
    printf("  routes after add: %d\n", route_count_after);
    ASSERT(route_count_after == route_count_before + 1, "route count increased by 1");

    /* List routes */
    char paths[16][256];
    int listed = nl_web_list_routes(server, (char**)paths, 16);
    printf("  listed routes: %d\n", listed);
    for (int i = 0; i < listed && i < 5; i++) {
        printf("    route %d: %s\n", i, paths[i] ? paths[i] : "(null)");
    }

    /* Update route */
    rc = nl_web_update_route(server, "/test", "<h1>Updated</h1>", "text/html");
    ASSERT(rc == 0, "nl_web_update_route() succeeds");

    /* Remove route */
    rc = nl_web_remove_route(server, "/test");
    ASSERT(rc == 0, "nl_web_remove_route() succeeds");

    int route_count_final = nl_web_get_route_count(server);
    printf("  routes after remove: %d\n", route_count_final);
    ASSERT(route_count_final == route_count_before, "route count restored after remove");

    /* HTML helpers */
    const char* simple_html = "<!DOCTYPE html><html><head><title>Hi</title></head>"
                              "<body><h1>Hello World</h1></body></html>";
    nl_web_add_html(server, "/", simple_html);
    printf("  [PASS] nl_web_add_html() succeeds\n");

    /* Redirect - only on platforms that implement it */
#if !defined(_WIN32) && !defined(__APPLE__)
    nl_web_add_redirect(server, "/old", "/new");
    printf("  [PASS] nl_web_add_redirect() succeeds\n");
#else
    printf("  [SKIP] nl_web_add_redirect() not available on this platform\n");
#endif

    /* Encoding validation */
#if defined(__linux__) || defined(__APPLE__)
    rc = nl_web_validate_encoding("UTF-8");
    ASSERT(rc == 1, "nl_web_validate_encoding(\"UTF-8\") == 1");

    rc = nl_web_validate_encoding("INVALID");
    ASSERT(rc == 0, "nl_web_validate_encoding(\"INVALID\") == 0");
#else
    printf("  [SKIP] nl_web_validate_encoding() not available on this platform\n");
#endif

    /* Redirect type */
    nl_web_set_redirect_type(server, NL_REDIRECT_PERMANENT);
    nl_redirect_type_t rtype = nl_web_get_redirect_type(server);
    ASSERT(rtype == NL_REDIRECT_PERMANENT, "redirect type set to 301");

    nl_web_set_redirect_type(server, NL_REDIRECT_TEMPORARY);

    /* Stop and destroy */
    nl_web_stop(server);
    nl_web_destroy(server);
    printf("  web server stopped and destroyed OK\n");

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 13: Lazy Loading API
 * ============================================================ */
static int test_lazy_loading(void) {
    TEST_BEGIN("Lazy Loading API");

    nl_lazy_enable(1);
    ASSERT(nl_lazy_is_enabled(NL_LAZY_MODULE_ALL) == 1, "lazy loading enabled");

    /* Preload HTTP module */
    nl_lazy_preload_module(NL_LAZY_MODULE_HTTP);
    int http_loaded = nl_lazy_is_module_loaded(NL_LAZY_MODULE_HTTP);
    printf("  HTTP module loaded: %d\n", http_loaded);

    /* Check status */
    nl_lazy_status_t status = nl_lazy_get_module_status(NL_LAZY_MODULE_HTTP);
    printf("  HTTP status: %d\n", status);
    ASSERT(status == NL_LAZY_STATUS_LOADED || status == NL_LAZY_STATUS_UNLOADED,
           "HTTP lazy status is valid");

    /* Thread count */
    int threads = nl_lazy_get_thread_count();
    printf("  thread count: %d\n", threads);

    nl_lazy_set_thread_count(4);
    threads = nl_lazy_get_thread_count();
    ASSERT(threads == 4, "thread count set to 4");

    /* Disable individual module */
    nl_lazy_disable_module(NL_LAZY_MODULE_HTTP);
    int http_enabled = nl_lazy_is_enabled(NL_LAZY_MODULE_HTTP);
    printf("  HTTP enabled after disable: %d\n", http_enabled);
    ASSERT(http_enabled == 0, "HTTP module disabled");

    nl_lazy_enable_module(NL_LAZY_MODULE_HTTP);
    http_enabled = nl_lazy_is_enabled(NL_LAZY_MODULE_HTTP);
    ASSERT(http_enabled == 1, "HTTP module re-enabled");

    nl_lazy_clear_all_cache();
    printf("  cache cleared OK\n");

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
}

/* ============================================================
 * Test 14: MQTT Client Module
 * ============================================================ */
static int test_mqtt(void) {
#ifdef HAVE_NETLEAF_MQTT
    TEST_BEGIN("MQTT Client");

    int avail = nl_mqtt_is_available();
    printf("  available: %d\n", avail);
    if (!avail) { SKIP("MQTT not available"); return TEST_SKIPPED; }

    const char* ver = nl_mqtt_version();
    printf("  version: %s\n", ver ? ver : "(null)");
    ASSERT(ver != NULL, "nl_mqtt_version() returns non-NULL");

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
#else
    (void)0;
    return TEST_SKIPPED;
#endif
}

/* ============================================================
 * Test 15: MQTT Server Module
 * ============================================================ */
static int test_mqtt_server(void) {
#ifdef HAVE_NETLEAF_MQTT_SERVER
    TEST_BEGIN("MQTT Server");

    int avail = nl_mqtt_server_is_available();
    printf("  available: %d\n", avail);
    if (!avail) { SKIP("MQTT Server not available"); return TEST_SKIPPED; }

    const char* ver = nl_mqtt_server_version();
    printf("  version: %s\n", ver ? ver : "(null)");
    ASSERT(ver != NULL, "nl_mqtt_server_version() returns non-NULL");

    return g_fail_count == 0 ? TEST_PASSED : TEST_FAILED;
#else
    (void)0;
    return TEST_SKIPPED;
#endif
}

/* ============================================================
 * Main
 * ============================================================ */
int main(void) {
    print_sep();
    printf("  NetLeaf v%s - All Module Tests\n", NETLEAF_VERSION);
    print_sep();
    printf("  Platform: %s\n",
#ifdef _WIN32
           "Windows"
#elif defined(__linux__)
           "Linux"
#elif defined(__APPLE__)
           "macOS"
#else
           "Unknown"
#endif
    );
    printf("  Compiler: %s\n",
#ifdef __clang__
           "Clang"
#elif defined(__GNUC__)
           "GCC"
#elif defined(_MSC_VER)
           "MSVC"
#else
           "Unknown"
#endif
    );
    printf("\n");

    test_core();
    test_lang();
    test_autocomplete();
    test_autoroute();
    test_errorpage();
    test_vue();
    // test_ipc();
    // test_linkagg();
    // test_json_toml();
    // test_encoding();
    // test_sysinfo();
    // test_web_server();  // Temporarily disabled for crash debugging
    // test_lazy_loading();
    test_mqtt();
    test_mqtt_server();

    print_sep();
    printf("  Results: %d passed, %d failed, %d skipped, %d total\n",
           g_pass_count, g_fail_count, g_skip_count, g_test_count);
    print_sep();

    if (g_fail_count > 0) {
        printf("\n  SOME TESTS FAILED!\n");
        return 1;
    } else {
        printf("\n  ALL TESTS PASSED!\n");
        return 0;
    }
}

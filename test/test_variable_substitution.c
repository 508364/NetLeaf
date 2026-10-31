#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netleaf_lang.h"

static nl_script_result_t test_result = {1, "test output", 11, 0};

static nl_script_result_t* mock_exec(const char* script, const char* lang_code, int error_code, void* userdata) {
    (void)script; (void)lang_code; (void)error_code; (void)userdata;
    return &test_result;
}

// Dynamic variable provider: computes the value on each access.
static int mock_provider(const char* name, char* out, size_t out_size, void* userdata) {
    (void)name;
    snprintf(out, out_size, "dynamic-%s", (const char*)userdata);
    return 1;
}

int main(void) {
    int ok = 1;

    // Init
    if (nl_lang_init() != 0) { fprintf(stderr, "init failed\n"); return 1; }

    // String variable
    if (nl_lang_var_set("user", "Alice") != 0) { fprintf(stderr, "set string failed\n"); ok = 0; }
    if (strcmp(nl_lang_var_get("user"), "Alice") != 0) { fprintf(stderr, "get string failed\n"); ok = 0; }
    if (!nl_lang_var_exists("user")) { fprintf(stderr, "exists string failed\n"); ok = 0; }

    // Int variable
    if (nl_lang_var_set_int("age", 25) != 0) { fprintf(stderr, "set int failed\n"); ok = 0; }
    if (nl_lang_var_get_int("age", 0) != 25) { fprintf(stderr, "get int failed: %lld\n", (long long)nl_lang_var_get_int("age", 0)); ok = 0; }

    // Float variable
    if (nl_lang_var_set_float("price", 3.14) != 0) { fprintf(stderr, "set float failed\n"); ok = 0; }
    if (nl_lang_var_get_float("price", 0.0) < 3.13 || nl_lang_var_get_float("price", 0.0) > 3.15) { fprintf(stderr, "get float failed\n"); ok = 0; }

    // Bool variable
    if (nl_lang_var_set_bool("active", 1) != 0) { fprintf(stderr, "set bool failed\n"); ok = 0; }
    if (nl_lang_var_get_bool("active", 0) != 1) { fprintf(stderr, "get bool failed\n"); ok = 0; }

    // Replace
    char buf[256];
    const char* r = nl_lang_var_replace("Hello {{ user }}, you are {{ age }} years old.", buf, sizeof(buf));
    if (!r || strcmp(r, "Hello Alice, you are 25 years old.") != 0) {
        fprintf(stderr, "replace failed: %s\n", r ? r : "NULL");
        ok = 0;
    }

    // Condition eval
    int c1 = nl_lang_var_condition_eval("age > 18");
    int c2 = nl_lang_var_condition_eval("user == 'Alice'");
    int c3 = nl_lang_var_condition_eval("price >= 3.0");
    if (!c1 || !c2 || !c3) {
        fprintf(stderr, "condition eval failed: age>18=%d, user=='Alice'=%d, price>=3.0=%d\n", c1, c2, c3);
        ok = 0;
    }

    // Remove
    if (nl_lang_var_remove("user") != 0) { fprintf(stderr, "remove failed\n"); ok = 0; }
    if (nl_lang_var_exists("user")) { fprintf(stderr, "after remove still exists\n"); ok = 0; }
    if (nl_lang_var_get("user") != NULL) { fprintf(stderr, "after remove get not null\n"); ok = 0; }

    // Clear all
    nl_lang_var_set_int("x", 1);
    nl_lang_var_set_int("y", 2);
    nl_lang_var_clear_all();
    if (nl_lang_var_exists("x") || nl_lang_var_exists("y")) { fprintf(stderr, "clear all failed\n"); ok = 0; }

    // Script engine register / execute
    if (nl_lang_register_script_engine("mock", mock_exec, NULL) != 0) { fprintf(stderr, "register script engine failed\n"); ok = 0; }
    char result_buf[256];
    if (nl_lang_execute_script("mock", "echo hello", NULL, 0, result_buf, sizeof(result_buf)) != 0) {
        fprintf(stderr, "execute script failed\n"); ok = 0;
    } else if (strcmp(result_buf, "test output") != 0) {
        fprintf(stderr, "execute script bad result: %s\n", result_buf); ok = 0;
    }

    int cnt = 0;
    const char** engines = nl_lang_get_script_engines(&cnt);
    if (cnt != 1 || !engines || strcmp(engines[0], "mock") != 0) {
        fprintf(stderr, "get script engines failed\n"); ok = 0;
    }

    nl_lang_unregister_script_engine("mock");

    // === <var> tag format tests ===

    // Re-set variables for html replacement tests
    nl_lang_var_set("product_name", "NetLeaf");
    nl_lang_var_set_int("product_version", 240);
    nl_lang_var_set_float("price_usd", 9.99);
    nl_lang_var_set_bool("in_stock", 1);

    // Test nl.lang. prefix format
    char html_buf[512];
    const char* hr = nl_lang_var_replace_html(
        "Welcome to {{<var>nl.lang.product_name</var>}} v{{<var>nl.lang.product_version</var>}}!",
        html_buf, sizeof(html_buf));
    if (!hr || strcmp(hr, "Welcome to NetLeaf v240!") != 0) {
        fprintf(stderr, "html replace nl.lang. prefix failed: %s\n", hr ? hr : "NULL");
        ok = 0;
    }

    // Test bare <var> format (no prefix, still looks up lang vars)
    const char* hr2 = nl_lang_var_replace_html(
        "Price: ${{<var>price_usd</var>}}, In stock: {{<var>in_stock</var>}}",
        html_buf, sizeof(html_buf));
    if (!hr2 || strcmp(hr2, "Price: $9.99, In stock: true") != 0) {
        fprintf(stderr, "html replace bare <var> failed: %s\n", hr2 ? hr2 : "NULL");
        ok = 0;
    }

    // Test mixed content
    const char* hr3 = nl_lang_var_replace_html(
        "<p>{{<var>nl.lang.product_name</var>}} is a library.</p>",
        html_buf, sizeof(html_buf));
    if (!hr3 || strcmp(hr3, "<p>NetLeaf is a library.</p>") != 0) {
        fprintf(stderr, "html replace mixed content failed: %s\n", hr3 ? hr3 : "NULL");
        ok = 0;
    }

    // Test missing variable (should just not replace)
    const char* hr4 = nl_lang_var_replace_html(
        "Hello {{<var>nl.lang.nonexistent</var>}}!",
        html_buf, sizeof(html_buf));
    if (!hr4 || strcmp(hr4, "Hello !") != 0) {
        fprintf(stderr, "html replace missing var failed: %s\n", hr4 ? hr4 : "NULL");
        ok = 0;
    }

    // Test nl_lang_get_error_with_vars
    int lib_id = 0x7FFF;
    nl_lang_register_lib(lib_id, (const char*[]){"en_us"}, 1);
    nl_lang_var_set("err_user", "Bob");
    nl_lang_var_set_int("err_code", 404);
    nl_lang_set_error(lib_id, 100, "en_us", "{{<var>nl.lang.err_user</var>}} not found (code {{<var>nl.lang.err_code</var>}})");

    char err_buf[256];
    const char* err_msg = nl_lang_get_error_with_vars(lib_id, 100, err_buf, sizeof(err_buf));
    if (!err_msg || strcmp(err_msg, "Bob not found (code 404)") != 0) {
        fprintf(stderr, "get_error_with_vars failed: %s\n", err_msg ? err_msg : "NULL");
        ok = 0;
    }
    nl_lang_unregister_lib(lib_id);

    // === Dynamic variables (v2.4.1) ===
    if (nl_lang_var_set_provider("dyn", mock_provider, (void*)"A") != 0) {
        fprintf(stderr, "set_provider failed\n"); ok = 0;
    }
    if (!nl_lang_var_is_dynamic("dyn")) {
        fprintf(stderr, "dyn not reported dynamic\n"); ok = 0;
    }
    if (strcmp(nl_lang_var_get("dyn"), "dynamic-A") != 0) {
        fprintf(stderr, "dyn get failed\n"); ok = 0;
    }
    {
        char db[128];
        const char* dr = nl_lang_var_replace("value={{ dyn }}", db, sizeof(db));
        if (!dr || strcmp(dr, "value=dynamic-A") != 0) {
            fprintf(stderr, "dyn replace failed: %s\n", dr ? dr : "NULL"); ok = 0;
        }
    }

    // === External variables: environment binding (v2.4.1) ===
#if defined(_WIN32)
    _putenv_s("NETLEAF_TEST_ENV", "from-env");
#else
    setenv("NETLEAF_TEST_ENV", "from-env", 1);
#endif
    if (nl_lang_var_bind_env("env_var", "NETLEAF_TEST_ENV") != 0) {
        fprintf(stderr, "bind_env failed\n"); ok = 0;
    }
    if (!nl_lang_var_is_dynamic("env_var")) {
        fprintf(stderr, "env var not reported dynamic\n"); ok = 0;
    }
    if (strcmp(nl_lang_var_get("env_var"), "from-env") != 0) {
        fprintf(stderr, "env var get failed: %s\n", nl_lang_var_get("env_var")); ok = 0;
    }

    // === External variables: .env file import (v2.4.1) ===
    {
        const char* envfile = "netleaf_test_vars.env";
        FILE* f = fopen(envfile, "w");
        if (f) {
            fputs("# comment line\n", f);
            fputs("FILE_KEY=file-value\n", f);
            fputs("QUOTED=\"quoted value\"\n", f);
            fclose(f);
        }
        if (nl_lang_var_load_file(envfile, NULL) < 2) {
            fprintf(stderr, "load_file failed\n"); ok = 0;
        }
        if (strcmp(nl_lang_var_get("FILE_KEY"), "file-value") != 0) {
            fprintf(stderr, "load_file FILE_KEY failed\n"); ok = 0;
        }
        if (strcmp(nl_lang_var_get("QUOTED"), "quoted value") != 0) {
            fprintf(stderr, "load_file QUOTED failed\n"); ok = 0;
        }
        remove(envfile);
    }

    // === Table-driven error registration (v2.4.1) ===
    {
        NL_ERROR_BEGIN(test_errors)
            NL_ERROR(0,  "OK",    "成功")
            NL_ERROR(-1, "oops",  "出错")
        NL_ERROR_END
        nl_lang_register_errors(0x7FF0, test_errors);
        if (strcmp(nl_lang_get_error_for(0x7FF0, -1, "zh_cn"), "出错") != 0) {
            fprintf(stderr, "table error zh_cn failed\n"); ok = 0;
        }
        if (strcmp(nl_lang_get_error_for(0x7FF0, -1, "en_us"), "oops") != 0) {
            fprintf(stderr, "table error en_us failed\n"); ok = 0;
        }
        nl_lang_unregister_lib(0x7FF0);
    }

    nl_lang_shutdown();

    if (ok) printf("ALL TESTS PASSED\n");
    else fprintf(stderr, "SOME TESTS FAILED\n");
    return ok ? 0 : 1;
}

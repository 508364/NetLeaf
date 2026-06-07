/*
 * NetLeaf v2.0.0 - JSON/TOML 配置解析示例
 *
 * 编译:
 *   Windows: cl example_config.c /I ../include /link ../build/lib/Release/netleaf.lib ws2_32.lib
 *   Linux:   gcc example_config.c -I ../include -L ../build/lib -lnetleaf -lpthread -o example_config
 */

#include <stdio.h>
#include <stdlib.h>
#include "netleaf.h"

#ifdef _WIN32
#include <windows.h>
#define SLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP(ms) usleep((ms) * 1000)
#endif

/* JSON 解析示例 */
void json_example() {
    const char* json_str = "{\n"
        "  \"name\": \"NetLeaf\",\n"
        "  \"version\": 20000,\n"
        "  \"debug\": true,\n"
        "  \"host\": \"127.0.0.1\",\n"
        "  \"port\": 8080,\n"
        "  \"tags\": [\"network\", \"http\", \"web\"]\n"
        "}";
    
    printf("=== JSON 解析示例 ===\n");
    printf("输入 JSON:\n%s\n", json_str);
    
    nl_status_t error;
    int line, col;
    void* root = nl_json_parse(json_str, &error, &line, &col);
    
    if (!root) {
        printf("解析失败 (%d): %s\n", error, nl_json_error_message(error));
        printf("位置: 第 %d 行, 第 %d 列\n", line, col);
        return;
    }
    
    /* 读取字符串 */
    void* name_node = nl_json_object_get(root, "name");
    if (name_node) {
        const char* name = nl_json_get_string(name_node);
        printf("name: %s\n", name);
        nl_json_destroy(name_node);
    }
    
    /* 读取整数 */
    void* ver_node = nl_json_object_get(root, "version");
    if (ver_node) {
        int64_t version = nl_json_get_int(ver_node);
        printf("version: %lld\n", (long long)version);
        nl_json_destroy(ver_node);
    }
    
    /* 读取布尔值 */
    void* debug_node = nl_json_object_get(root, "debug");
    if (debug_node) {
        int debug = nl_json_get_bool(debug_node);
        printf("debug: %s\n", debug ? "true" : "false");
        nl_json_destroy(debug_node);
    }
    
    /* 读取数组 */
    void* tags = nl_json_object_get(root, "tags");
    if (tags) {
        size_t count = nl_json_array_size(tags);
        printf("tags: [");
        for (size_t i = 0; i < count; i++) {
            void* item = nl_json_array_get(tags, i);
            if (item) {
                const char* tag = nl_json_get_string(item);
                if (i > 0) printf(", ");
                printf("\"%s\"", tag);
            }
        }
        printf("]\n");
        nl_json_destroy(tags);
    }
    
    nl_json_destroy(root);
    printf("\n");
}

/* TOML 解析示例 */
void toml_example() {
    const char* toml_str = "\"name\" = \"NetLeaf\"\n"
        "\"version\" = 20000\n"
        "\"debug\" = true\n"
        "\"host\" = \"127.0.0.1\"\n"
        "\"port\" = 8080\n";
    
    printf("=== TOML 解析示例 ===\n");
    printf("输入 TOML:\n%s\n", toml_str);
    
    nl_status_t error;
    int line, col;
    void* root = nl_toml_parse(toml_str, &error, &line, &col);
    
    if (!root) {
        printf("解析失败 (%d): %s\n", error, nl_toml_error_message(error));
        return;
    }
    
    /* 读取值 */
    void* name_node = nl_toml_table_get(root, "name");
    if (name_node) {
        const char* name = nl_toml_get_string(name_node);
        printf("name: %s\n", name);
        nl_toml_destroy(name_node);
    }
    
    void* ver_node = nl_toml_table_get(root, "version");
    if (ver_node) {
        int64_t version = nl_toml_get_int(ver_node);
        printf("version: %lld\n", (long long)version);
        nl_toml_destroy(ver_node);
    }
    
    void* debug_node = nl_toml_table_get(root, "debug");
    if (debug_node) {
        int debug = nl_toml_get_bool(debug_node);
        printf("debug: %s\n", debug ? "true" : "false");
        nl_toml_destroy(debug_node);
    }
    
    nl_toml_destroy(root);
    printf("\n");
}

/* 文件解析示例 */
void file_example() {
    printf("=== 文件解析示例 ===\n");
    
    /* 尝试解析配置文件 */
    nl_status_t error;
    void* json = nl_json_parse_file("config.json", &error);
    if (json) {
        char* str = nl_json_stringify(json, 1);
        if (str && str[0] != '\0') {
            printf("JSON 文件内容:\n%s\n", str);
            free(str);
        }
        nl_json_destroy(json);
    } else {
        printf("无法读取 config.json: %s\n", nl_json_error_message(error));
    }
    
    printf("\n");
}

int main() {
    printf("========================================\n");
    printf("   NetLeaf v2.0.0 - 配置解析示例\n");
    printf("========================================\n\n");
    
    json_example();
    toml_example();
    file_example();
    
    printf("示例完成！\n");
    return 0;
}
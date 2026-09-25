#ifndef NETLEAF_MQTT_SERVER_LANG_H
#define NETLEAF_MQTT_SERVER_LANG_H

#include "netleaf_lang.h"

// Library ID for MQTT Server
#define NL_LIB_MQTT_SERVER 0x0009

// Supported languages for MQTT Server
static const char* nl_mqtt_server_languages[] = NL_LANG_CODES("en_us", "zh_cn");

// Error messages for each language
// Index 0: en_us, Index 1: zh_cn

static const char* nl_mqtt_server_msg_0[] = NL_ERROR_MSGS("Success", "成功");
static const char* nl_mqtt_server_msg_1[] = NL_ERROR_MSGS("Server initialization failed", "服务端初始化失败");
static const char* nl_mqtt_server_msg_2[] = NL_ERROR_MSGS("Socket bind failed", "Socket绑定失败");
static const char* nl_mqtt_server_msg_3[] = NL_ERROR_MSGS("Socket listen failed", "Socket监听失败");
static const char* nl_mqtt_server_msg_4[] = NL_ERROR_MSGS("Accept connection failed", "接受连接失败");
static const char* nl_mqtt_server_msg_5[] = NL_ERROR_MSGS("Memory allocation failed", "内存分配失败");
static const char* nl_mqtt_server_msg_6[] = NL_ERROR_MSGS("Too many clients", "客户端数量已达上限");
static const char* nl_mqtt_server_msg_7[] = NL_ERROR_MSGS("TLS initialization failed", "TLS初始化失败");
static const char* nl_mqtt_server_msg_8[] = NL_ERROR_MSGS("Server already running", "服务端已在运行");
static const char* nl_mqtt_server_msg_9[] = NL_ERROR_MSGS("Server not running", "服务端未运行");
static const char* nl_mqtt_server_msg_10[] = NL_ERROR_MSGS("Invalid topic", "无效主题");
static const char* nl_mqtt_server_msg_11[] = NL_ERROR_MSGS("Client not found", "客户端不存在");
static const char* nl_mqtt_server_msg_12[] = NL_ERROR_MSGS("Authentication failed", "认证失败");
static const char* nl_mqtt_server_msg_13[] = NL_ERROR_MSGS("Protocol error", "协议错误");
static const char* nl_mqtt_server_msg_14[] = NL_ERROR_MSGS("Packet too large", "数据包过大");
static const char* nl_mqtt_server_msg_15[] = NL_ERROR_MSGS("Topic wildcard invalid", "主题通配符无效");

// Helper function to register all errors
static inline int nl_mqtt_server_register_lang(void) {
    nl_lang_register_lib_name(NL_LIB_MQTT_SERVER, "mqtt_server");

    int result = nl_lang_register_lib(NL_LIB_MQTT_SERVER, nl_mqtt_server_languages, 2);
    if (result != 0) return result;

    nl_lang_add_error(NL_LIB_MQTT_SERVER, 0, nl_mqtt_server_msg_0);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -1, nl_mqtt_server_msg_1);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -2, nl_mqtt_server_msg_2);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -3, nl_mqtt_server_msg_3);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -4, nl_mqtt_server_msg_4);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -5, nl_mqtt_server_msg_5);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -6, nl_mqtt_server_msg_6);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -7, nl_mqtt_server_msg_7);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -8, nl_mqtt_server_msg_8);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -9, nl_mqtt_server_msg_9);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -10, nl_mqtt_server_msg_10);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -11, nl_mqtt_server_msg_11);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -12, nl_mqtt_server_msg_12);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -13, nl_mqtt_server_msg_13);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -14, nl_mqtt_server_msg_14);
    nl_lang_add_error(NL_LIB_MQTT_SERVER, -15, nl_mqtt_server_msg_15);

    return 0;
}

#define NL_MQTT_SERVER_REGISTER_LANG() nl_mqtt_server_register_lang()

#endif // NETLEAF_MQTT_SERVER_LANG_H

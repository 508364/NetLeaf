#ifndef NETLEAF_MQTT_LANG_H
#define NETLEAF_MQTT_LANG_H

#include "netleaf_lang.h"

// Library ID for MQTT
#define NL_LIB_MQTT 0x0008

// Supported languages for MQTT
static const char* nl_mqtt_languages[] = NL_LANG_CODES("en_us", "zh_cn");

// Error messages for each language
// Index 0: en_us, Index 1: zh_cn

static const char* nl_mqtt_msg_0[] = NL_ERROR_MSGS("Success", "成功");
static const char* nl_mqtt_msg_1[] = NL_ERROR_MSGS("Unknown error", "未知错误");
static const char* nl_mqtt_msg_2[] = NL_ERROR_MSGS("Memory allocation failed", "内存分配失败");
static const char* nl_mqtt_msg_3[] = NL_ERROR_MSGS("Invalid parameter", "无效参数");
static const char* nl_mqtt_msg_4[] = NL_ERROR_MSGS("Socket creation failed", "Socket创建失败");
static const char* nl_mqtt_msg_5[] = NL_ERROR_MSGS("Connection refused", "连接被拒绝");
static const char* nl_mqtt_msg_6[] = NL_ERROR_MSGS("Connection timeout", "连接超时");
static const char* nl_mqtt_msg_7[] = NL_ERROR_MSGS("Network error", "网络错误");
static const char* nl_mqtt_msg_8[] = NL_ERROR_MSGS("Parse error", "解析错误");
static const char* nl_mqtt_msg_9[] = NL_ERROR_MSGS("Protocol error", "协议错误");
static const char* nl_mqtt_msg_10[] = NL_ERROR_MSGS("MQTT version not supported", "MQTT版本不受支持");
static const char* nl_mqtt_msg_11[] = NL_ERROR_MSGS("Client ID not valid", "客户端ID无效");
static const char* nl_mqtt_msg_12[] = NL_ERROR_MSGS("Server unavailable", "服务器不可用");
static const char* nl_mqtt_msg_13[] = NL_ERROR_MSGS("Bad username or password", "用户名或密码错误");
static const char* nl_mqtt_msg_14[] = NL_ERROR_MSGS("Not authorized", "未授权");
static const char* nl_mqtt_msg_15[] = NL_ERROR_MSGS("Packet too large", "数据包过大");
static const char* nl_mqtt_msg_16[] = NL_ERROR_MSGS("Topic name invalid", "主题名称无效");
static const char* nl_mqtt_msg_17[] = NL_ERROR_MSGS("Subscribe failed", "订阅失败");
static const char* nl_mqtt_msg_18[] = NL_ERROR_MSGS("Already connected", "已连接");
static const char* nl_mqtt_msg_19[] = NL_ERROR_MSGS("Not connected", "未连接");

// Helper function to register all errors
static inline int nl_mqtt_register_lang(void) {
    nl_lang_register_lib_name(NL_LIB_MQTT, "mqtt");

    int result = nl_lang_register_lib(NL_LIB_MQTT, nl_mqtt_languages, 2);
    if (result != 0) return result;

    nl_lang_add_error(NL_LIB_MQTT, 0, nl_mqtt_msg_0);
    nl_lang_add_error(NL_LIB_MQTT, -1, nl_mqtt_msg_1);
    nl_lang_add_error(NL_LIB_MQTT, -2, nl_mqtt_msg_2);
    nl_lang_add_error(NL_LIB_MQTT, -3, nl_mqtt_msg_3);
    nl_lang_add_error(NL_LIB_MQTT, -4, nl_mqtt_msg_4);
    nl_lang_add_error(NL_LIB_MQTT, -5, nl_mqtt_msg_5);
    nl_lang_add_error(NL_LIB_MQTT, -6, nl_mqtt_msg_6);
    nl_lang_add_error(NL_LIB_MQTT, -7, nl_mqtt_msg_7);
    nl_lang_add_error(NL_LIB_MQTT, -8, nl_mqtt_msg_8);
    nl_lang_add_error(NL_LIB_MQTT, -9, nl_mqtt_msg_9);
    nl_lang_add_error(NL_LIB_MQTT, -10, nl_mqtt_msg_10);
    nl_lang_add_error(NL_LIB_MQTT, -11, nl_mqtt_msg_11);
    nl_lang_add_error(NL_LIB_MQTT, -12, nl_mqtt_msg_12);
    nl_lang_add_error(NL_LIB_MQTT, -13, nl_mqtt_msg_13);
    nl_lang_add_error(NL_LIB_MQTT, -14, nl_mqtt_msg_14);
    nl_lang_add_error(NL_LIB_MQTT, -15, nl_mqtt_msg_15);
    nl_lang_add_error(NL_LIB_MQTT, -16, nl_mqtt_msg_16);
    nl_lang_add_error(NL_LIB_MQTT, -17, nl_mqtt_msg_17);
    nl_lang_add_error(NL_LIB_MQTT, -18, nl_mqtt_msg_18);
    nl_lang_add_error(NL_LIB_MQTT, -19, nl_mqtt_msg_19);

    return 0;
}

#define NL_MQTT_REGISTER_LANG() nl_mqtt_register_lang()

#endif // NETLEAF_MQTT_LANG_H

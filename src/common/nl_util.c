#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* L3: Windows 侧使用 CRITICAL_SECTION + INIT_ONCE，最低支持到 Vista (0x0600)。 */
#if defined(_WIN32) && !defined(_WIN32_WINNT)
    #define _WIN32_WINNT 0x0600
#endif

/**
 * @file nl_util.c
 * @brief 平台无关公共工具层实现 (Platform-Independent Common Utilities)
 *
 * 本文件收敛以下工具实现：
 *  - 字符串：nl_strdup / nl_parse_enable_value / nl_strncasecmp /
 *            nl_strncasestr / nl_tolower / nl_string_tolower
 *  - 安全：  nl_sha1（纯 C）/ nl_base64_encode
 *  - 初始化：nl_once_init / nl_once_run
 *
 * 设计约束：
 *  - 零平台特定 API：不依赖 socket / winsock / MSVCRT 扩展，纯 C99。
 *  - 全部函数导出为 NL_API（在核心库中由 NL_EXPORTS 切换为 dllexport，
 *    消费端默认为 dllimport）。
 *  - 一次性守卫采用 8 槽固定容量；并发热路径上已就绪的调用零锁开销。
 */

#include "nl_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
    #include <windows.h>
    #include <windows.h>
#else
    #include <pthread.h>
#endif

/* ============================================================
 * 内部：字符串
 * ============================================================ */

NL_API char* nl_strdup(const char* s) {
    if (!s) return NULL;
#ifdef _WIN32
    return _strdup(s);
#else
    return strdup(s);
#endif
}

NL_API char* nl_strndup(const char* s, size_t n) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len > n) len = n;
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

NL_API int nl_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

NL_API char* nl_string_tolower(char* s) {
    if (!s) return NULL;
    for (char* p = s; *p; p++) {
        *p = (char)nl_tolower((unsigned char)*p);
    }
    return s;
}

NL_API int nl_parse_enable_value(const char* value) {
    if (!value) return 0;

    /* 数字串（如 "0"/"1"/"42"）直接按数值解析；其它走关键词比较 */
    size_t i;
    int all_digits = 1;
    if (value[0] != '\0') {
        for (i = 0; value[i]; i++) {
            if (value[i] < '0' || value[i] > '9') { all_digits = 0; break; }
        }
    } else {
        all_digits = 0;
    }
    if (all_digits) {
        return atoi(value);
    }

    /* 其他：转小写后做关键词比较 */
    char lower[16];
    for (i = 0; i < sizeof(lower) - 1 && value[i]; i++) {
        lower[i] = (char)nl_tolower((unsigned char)value[i]);
    }
    lower[i] = '\0';

    if (strcmp(lower, "true") == 0 || strcmp(lower, "on") == 0 ||
        strcmp(lower, "yes") == 0) {
        return 1;
    }
    if (strcmp(lower, "false") == 0 || strcmp(lower, "off") == 0 ||
        strcmp(lower, "no") == 0) {
        return 0;
    }
    return 0;
}

NL_API int nl_strncasecmp(const char* s1, const char* s2, size_t n) {
    if (!s1 || !s2) return 0;
    if (n == 0) return 0;
    while (n-- > 0) {
        int c1 = nl_tolower((unsigned char)*s1);
        int c2 = nl_tolower((unsigned char)*s2);
        int diff = c1 - c2;
        if (diff != 0) return diff;
        if (c1 == '\0') return 0;
        s1++;
        s2++;
    }
    return 0;
}

NL_API char* nl_strncasestr(const char* haystack, const char* needle, size_t len) {
    if (!haystack || !needle || needle[0] == '\0') return NULL;
    size_t needle_len = strlen(needle);
    if (needle_len > len) return NULL;
    const char* end = haystack + len - needle_len;
    for (const char* p = haystack; p <= end; p++) {
        if (nl_strncasecmp(p, needle, needle_len) == 0) {
            return (char*)p;
        }
    }
    return NULL;
}

/* ============================================================
 * 内部：SHA-1 / Base64
 * ============================================================ */

NL_API void nl_sha1(const char* input, size_t len, unsigned char* output) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    size_t original_len = len;
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    unsigned char* padded = (unsigned char*)calloc(padded_len, 1);
    if (!padded) return;

    memcpy(padded, input, len);
    padded[len] = 0x80;
    /* 末尾 64-bit 长度（大端） */
    uint64_t bit_len = (uint64_t)original_len * 8;
    for (int i = 0; i < 8; i++) {
        padded[padded_len - 8 + i] = (unsigned char)((bit_len >> (56 - i * 8)) & 0xFF);
    }

    for (size_t i = 0; i < padded_len; i += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; j++) {
            w[j] = ((uint32_t)padded[i + j * 4] << 24) |
                   ((uint32_t)padded[i + j * 4 + 1] << 16) |
                   ((uint32_t)padded[i + j * 4 + 2] << 8)  |
                   ((uint32_t)padded[i + j * 4 + 3]);
        }
        for (int j = 16; j < 80; j++) {
            uint32_t x = w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16];
            w[j] = (x << 1) | (x >> 31);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for (int j = 0; j < 80; j++) {
            uint32_t f, k;
            if (j < 20)       { f = (b & c) | ((~b) & d);            k = 0x5A827999; }
            else if (j < 40)  { f = b ^ c ^ d;                       k = 0x6ED9EBA1; }
            else if (j < 60)  { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;                       k = 0xCA62C1D6; }

            uint32_t temp = (((a << 5) | (a >> 27)) + f + e + k + w[j]);
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    free(padded);

    for (int i = 0; i < 4; i++) {
        output[i]      = (unsigned char)((h0 >> (24 - i * 8)) & 0xFF);
        output[i + 4]  = (unsigned char)((h1 >> (24 - i * 8)) & 0xFF);
        output[i + 8]  = (unsigned char)((h2 >> (24 - i * 8)) & 0xFF);
        output[i + 12] = (unsigned char)((h3 >> (24 - i * 8)) & 0xFF);
        output[i + 16] = (unsigned char)((h4 >> (24 - i * 8)) & 0xFF);
    }
}

NL_API void nl_base64_encode_into(const char* input, size_t len, char* output) {
    static const char* base64_table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t i = 0;
    int padding = 0;

    while (i < len) {
        uint32_t n = ((uint32_t)(unsigned char)input[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)(unsigned char)input[i + 1]) << 8;
        else padding++;
        if (i + 2 < len) n |= ((uint32_t)(unsigned char)input[i + 2]);
        else padding++;

        output[0] = base64_table[(n >> 18) & 0x3F];
        output[1] = base64_table[(n >> 12) & 0x3F];
        output[2] = (padding >= 2) ? '=' : base64_table[(n >> 6) & 0x3F];
        output[3] = (padding >= 1) ? '=' : base64_table[n & 0x3F];

        output += 4;
        i += 3;
    }
    *output = '\0';
}

/* 堆分配版本：netleaf.h 对外 API，内部复用上面的缓冲版本 */
NL_API char* nl_base64_encode(const char* input, size_t len) {
    if (!input) return NULL;

    size_t out_len = 4 * ((len + 2) / 3) + 1;
    char* out = (char*)malloc(out_len);
    if (!out) return NULL;

    nl_base64_encode_into(input, len, out);
    return out;
}

/* 堆分配版本：netleaf.h 对外 API，返回 NUL 结尾缓冲，调用方负责 free */
NL_API char* nl_base64_decode(const char* input, size_t* out_len) {
    if (!input) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    size_t in_len = strlen(input);

    /* 容忍末尾的 '=' 填充与空白 */
    size_t i = 0;
    while (i < in_len && (input[i] == ' ' || input[i] == '\n' ||
                          input[i] == '\r' || input[i] == '\t')) i++;
    in_len = strlen(input + i);
    while (in_len > 0 && (input[i + in_len - 1] == '=' ||
                          input[i + in_len - 1] == ' ')) in_len--;
    i += in_len;

    /* 估算输出上界（每 4 个 base64 字符对应 3 字节） */
    size_t est = in_len / 4 * 3 + 3;
    char* out = (char*)malloc(est + 1);
    if (!out) return NULL;

    int buf = 0, bits = 0, o = 0;
    for (size_t j = 0; j < in_len; j++) {
        unsigned char c = (unsigned char)input[i + j];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else { free(out); return NULL; }

        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (char)((buf >> bits) & 0xFF);
        }
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

/* ============================================================
 * 内部：一次性守卫
 * ============================================================ */
#define NL_ONCE_SLOTS 8

#ifdef _WIN32
    typedef struct nl_once_slot {
        INIT_ONCE once;
        BOOL inited;
    } nl_once_slot_t;

    static nl_once_slot_t g_once_slots[NL_ONCE_SLOTS] = {
        { INIT_ONCE_STATIC_INIT, 0 },
        { INIT_ONCE_STATIC_INIT, 0 },
        { INIT_ONCE_STATIC_INIT, 0 },
        { INIT_ONCE_STATIC_INIT, 0 },
        { INIT_ONCE_STATIC_INIT, 0 },
        { INIT_ONCE_STATIC_INIT, 0 },
        { INIT_ONCE_STATIC_INIT, 0 },
        { INIT_ONCE_STATIC_INIT, 0 },
    };

    static BOOL CALLBACK nl_once_caller(PINIT_ONCE once, PVOID param, PVOID* ctx) {
        (void)once; (void)ctx;
        /* 通过联合体做对象指针 <-> 函数指针转换，规避 -Wpedantic 告警 */
        union { void* obj; void (*fn)(void); } h = { .obj = param };
        if (h.fn) h.fn();
        return TRUE;
    }

    static int nl_once_execute(int index, void (*fn)(void)) {
        if (index < 0 || index >= NL_ONCE_SLOTS) return 0;
        nl_once_slot_t* slot = &g_once_slots[index];
        /* 热路径：已初始化直接返回，不进入临界区 */
        if (slot->inited) return 1;
        union { void* obj; void (*fn)(void); } h = { .fn = fn };
        InitOnceExecuteOnce(&slot->once, nl_once_caller, h.obj, NULL);
        slot->inited = 1;
        return 1;
    }

#else
    typedef struct nl_once_slot {
        pthread_mutex_t lock;
        int inited;
    } nl_once_slot_t;

    static nl_once_slot_t g_once_slots[NL_ONCE_SLOTS] = {
        [0]  = { PTHREAD_MUTEX_INITIALIZER, 0 },
        [1]  = { PTHREAD_MUTEX_INITIALIZER, 0 },
        [2]  = { PTHREAD_MUTEX_INITIALIZER, 0 },
        [3]  = { PTHREAD_MUTEX_INITIALIZER, 0 },
        [4]  = { PTHREAD_MUTEX_INITIALIZER, 0 },
        [5]  = { PTHREAD_MUTEX_INITIALIZER, 0 },
        [6]  = { PTHREAD_MUTEX_INITIALIZER, 0 },
        [7]  = { PTHREAD_MUTEX_INITIALIZER, 0 },
    };

    static int nl_once_execute(int index, void (*fn)(void)) {
        if (index < 0 || index >= NL_ONCE_SLOTS) return 0;
        nl_once_slot_t* slot = &g_once_slots[index];
        pthread_mutex_lock(&slot->lock);
        if (!slot->inited && fn) {
            fn();
            slot->inited = 1;
        }
        pthread_mutex_unlock(&slot->lock);
        return 1;
    }
#endif

NL_API int nl_once_init(int index, void (*init_fn)(void)) {
    return nl_once_execute(index, init_fn);
}

NL_API int nl_once_run(int index, void (*work_fn)(void)) {
    return nl_once_execute(index, work_fn);
}

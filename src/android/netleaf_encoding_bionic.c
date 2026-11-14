/*
 * 编码转码后端：Android Bionic 实现（minSdk 21）。
 *
 * Bionic 自 API 28 才提供 iconv_open/iconv/iconv_close、API 26 才提供
 * nl_langinfo；以 API 21 为目标构建时这些声明被 availability guard
 * 编译掉，低版本设备运行时也无此符号，故按目标 API 分支：
 *   - __ANDROID_API__ >= 28：使用 Bionic 内置 iconv（与 glibc 同路径）；
 *   - 否则（minSdk 21 默认）：无 iconv 回退——转码按"不支持"返回失败，
 *     上层回退原文本；系统编码默认 UTF-8（由调用方兜底）。
 *
 * 本文件替代原 netleaf_encoding_linux.c 中的 __ANDROID__ 分支。
 */
#include <stdlib.h>

#include "common/netleaf_encoding_backend.h"

#if defined(__ANDROID_API__) && __ANDROID_API__ >= 28

#include <iconv.h>
#include <langinfo.h>

void* nl_encoding_backend_open(const char* from, const char* to) {
    iconv_t cd = iconv_open(to, from);
    return (cd == (iconv_t)-1) ? NULL : (void*)cd;
}

int nl_encoding_backend_convert(void* cd, const char* input, size_t input_len,
                                char** output, size_t* output_len) {
    if (!cd) {
        return -1;
    }

    size_t buf_size = input_len * 4;
    *output = (char*)malloc(buf_size);
    if (!*output) {
        iconv_close((iconv_t)cd);
        return -1;
    }

    char* inbuf = (char*)input;
    char* outbuf = *output;
    size_t inbytesleft = input_len;
    size_t outbytesleft = buf_size;

    size_t result = iconv((iconv_t)cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
    *output_len = buf_size - outbytesleft;

    iconv_close((iconv_t)cd);

    if (result == (size_t)-1) {
        free(*output);
        *output = NULL;
        *output_len = 0;
        return -1;
    }

    return 0;
}

const char* nl_encoding_backend_default_codeset(void) {
    return nl_langinfo(CODESET);
}

#else /* __ANDROID_API__ < 28：minSdk 21 回退 */

void* nl_encoding_backend_open(const char* from, const char* to) {
    (void)from;
    (void)to;
    return NULL; /* 无 iconv：上层按"不支持该转码"回退原文本 */
}

int nl_encoding_backend_convert(void* cd, const char* input, size_t input_len,
                                char** output, size_t* output_len) {
    (void)cd;
    (void)input;
    (void)input_len;
    (void)output;
    (void)output_len;
    return -1;
}

const char* nl_encoding_backend_default_codeset(void) {
    return NULL; /* 无 nl_langinfo：调用方回退 UTF-8 */
}

#endif /* __ANDROID_API__ >= 28 */

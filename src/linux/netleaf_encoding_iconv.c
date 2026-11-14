/*
 * 编码转码后端：glibc/musl 的 iconv 实现。
 *
 * 从原 netleaf_encoding_linux.c 抽出，供 Linux（非 Android）构建编译；
 * Android 构建改用 src/android/netleaf_encoding_bionic.c。
 */
#ifndef __linux__
#error "This file should only be compiled on Linux"
#endif

#include <iconv.h>
#include <langinfo.h>
#include <stdlib.h>

#include "common/netleaf_encoding_backend.h"

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

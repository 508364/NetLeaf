/**
 * @file quictls_huffman.c
 * @brief HPACK / QPACK 霍夫曼编解码（RFC 7541 Appendix B；RFC 9204 同表）
 * @version 0.1.0
 *
 * 覆盖 257 个符号：0..255（字节）+ 256（EOS）。编码按 MSB-first 追加比特流，
 * 末尾不足一字节以 1 位（EOS 前缀）填充；解码按 MSB-first 逐位匹配，
 * 结束处允许最多 7 位全 1 填充。
 */

#include "netleaf_quictls.h"

#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* RFC 7541 Appendix B 霍夫曼码表（code, bits）；最后一项为 EOS */
static const struct { uint32_t code; uint8_t bits; } HUFF[257] = {
    { 0x1ff8, 13 }, { 0x7fffd8, 23 }, { 0xfffffe2, 28 }, { 0xfffffe3, 28 },
    { 0xfffffe4, 28 }, { 0xfffffe5, 28 }, { 0xfffffe6, 28 }, { 0xfffffe7, 28 },
    { 0xfffffe8, 28 }, { 0xffffea, 24 }, { 0x3ffffffc, 30 }, { 0xfffffe9, 28 },
    { 0xfffffea, 28 }, { 0x3ffffffd, 30 }, { 0xfffffeb, 28 }, { 0xfffffec, 28 },
    { 0xfffffed, 28 }, { 0xfffffee, 28 }, { 0xfffffef, 28 }, { 0xffffff0, 28 },
    { 0xffffff1, 28 }, { 0xffffff2, 28 }, { 0x3ffffffe, 30 }, { 0xffffff3, 28 },
    { 0xffffff4, 28 }, { 0xffffff5, 28 }, { 0xffffff6, 28 }, { 0xffffff7, 28 },
    { 0xffffff8, 28 }, { 0xffffff9, 28 }, { 0xffffffa, 28 }, { 0xffffffb, 28 },
    { 0x14, 6 }, { 0x3f8, 10 }, { 0x3f9, 10 }, { 0xffa, 12 },
    { 0x1ff9, 13 }, { 0x15, 6 }, { 0xf8, 8 }, { 0x7fa, 11 },
    { 0x3fa, 10 }, { 0x3fb, 10 }, { 0xf9, 8 }, { 0x7fb, 11 },
    { 0xfa, 8 }, { 0x16, 6 }, { 0x17, 6 }, { 0x18, 6 },
    { 0x0, 5 }, { 0x1, 5 }, { 0x2, 5 }, { 0x19, 6 },
    { 0x1a, 6 }, { 0x1b, 6 }, { 0x1c, 6 }, { 0x1d, 6 },
    { 0x1e, 6 }, { 0x1f, 6 }, { 0x5c, 7 }, { 0xfb, 8 },
    { 0x7ffc, 15 }, { 0x20, 6 }, { 0xffb, 12 }, { 0x3fc, 10 },
    { 0x1ffa, 13 }, { 0x21, 6 }, { 0x5d, 7 }, { 0x5e, 7 },
    { 0x5f, 7 }, { 0x60, 7 }, { 0x61, 7 }, { 0x62, 7 },
    { 0x63, 7 }, { 0x64, 7 }, { 0x65, 7 }, { 0x66, 7 },
    { 0x67, 7 }, { 0x68, 7 }, { 0x69, 7 }, { 0x6a, 7 },
    { 0x6b, 7 }, { 0x6c, 7 }, { 0x6d, 7 }, { 0x6e, 7 },
    { 0x6f, 7 }, { 0x70, 7 }, { 0x71, 7 }, { 0x72, 7 },
    { 0xfc, 8 }, { 0x73, 7 }, { 0xfd, 8 }, { 0x1ffb, 13 },
    { 0x7fff0, 19 }, { 0x1ffc, 13 }, { 0x3ffc, 14 }, { 0x22, 6 },
    { 0x7ffd, 15 }, { 0x3, 5 }, { 0x23, 6 }, { 0x4, 5 },
    { 0x24, 6 }, { 0x5, 5 }, { 0x25, 6 }, { 0x26, 6 },
    { 0x27, 6 }, { 0x6, 5 }, { 0x74, 7 }, { 0x75, 7 },
    { 0x28, 6 }, { 0x29, 6 }, { 0x2a, 6 }, { 0x7, 5 },
    { 0x2b, 6 }, { 0x76, 7 }, { 0x2c, 6 }, { 0x8, 5 },
    { 0x9, 5 }, { 0x2d, 6 }, { 0x77, 7 }, { 0x78, 7 },
    { 0x79, 7 }, { 0x7a, 7 }, { 0x7b, 7 }, { 0x7ffe, 15 },
    { 0x7fc, 11 }, { 0x3ffd, 14 }, { 0x1ffd, 13 }, { 0xffffffc, 28 },
    { 0xfffe6, 20 }, { 0x3fffd2, 22 }, { 0xfffe7, 20 }, { 0xfffe8, 20 },
    { 0x3fffd3, 22 }, { 0x3fffd4, 22 }, { 0x3fffd5, 22 }, { 0x7fffd9, 23 },
    { 0x3fffd6, 22 }, { 0x7fffda, 23 }, { 0x7fffdb, 23 }, { 0x7fffdc, 23 },
    { 0x7fffdd, 23 }, { 0x7fffde, 23 }, { 0xffffeb, 24 }, { 0x7fffdf, 23 },
    { 0xffffec, 24 }, { 0xffffed, 24 }, { 0x3fffd7, 22 }, { 0x7fffe0, 23 },
    { 0xffffee, 24 }, { 0x7fffe1, 23 }, { 0x7fffe2, 23 }, { 0x7fffe3, 23 },
    { 0x7fffe4, 23 }, { 0x1fffdc, 21 }, { 0x3fffd8, 22 }, { 0x7fffe5, 23 },
    { 0x3fffd9, 22 }, { 0x7fffe6, 23 }, { 0x7fffe7, 23 }, { 0xffffef, 24 },
    { 0x3fffda, 22 }, { 0x1fffdd, 21 }, { 0xfffe9, 20 }, { 0x3fffdb, 22 },
    { 0x3fffdc, 22 }, { 0x7fffe8, 23 }, { 0x7fffe9, 23 }, { 0x1fffde, 21 },
    { 0x7fffea, 23 }, { 0x3fffdd, 22 }, { 0x3fffde, 22 }, { 0xfffff0, 24 },
    { 0x1fffdf, 21 }, { 0x3fffdf, 22 }, { 0x7fffeb, 23 }, { 0x7fffec, 23 },
    { 0x1fffe0, 21 }, { 0x1fffe1, 21 }, { 0x3fffe0, 22 }, { 0x1fffe2, 21 },
    { 0x7fffed, 23 }, { 0x3fffe1, 22 }, { 0x7fffee, 23 }, { 0x7fffef, 23 },
    { 0xfffea, 20 }, { 0x3fffe2, 22 }, { 0x3fffe3, 22 }, { 0x3fffe4, 22 },
    { 0x7ffff0, 23 }, { 0x3fffe5, 22 }, { 0x3fffe6, 22 }, { 0x7ffff1, 23 },
    { 0x3ffffe0, 26 }, { 0x3ffffe1, 26 }, { 0xfffeb, 20 }, { 0x7fff1, 19 },
    { 0x3fffe7, 22 }, { 0x7ffff2, 23 }, { 0x3fffe8, 22 }, { 0x1ffffec, 25 },
    { 0x3ffffe2, 26 }, { 0x3ffffe3, 26 }, { 0x3ffffe4, 26 }, { 0x7ffffde, 27 },
    { 0x7ffffdf, 27 }, { 0x3ffffe5, 26 }, { 0xfffff1, 24 }, { 0x1ffffed, 25 },
    { 0x7fff2, 19 }, { 0x1fffe3, 21 }, { 0x3ffffe6, 26 }, { 0x7ffffe0, 27 },
    { 0x7ffffe1, 27 }, { 0x3ffffe7, 26 }, { 0x7ffffe2, 27 }, { 0xfffff2, 24 },
    { 0x1fffe4, 21 }, { 0x1fffe5, 21 }, { 0x3ffffe8, 26 }, { 0x3ffffe9, 26 },
    { 0xffffffd, 28 }, { 0x7ffffe3, 27 }, { 0x7ffffe4, 27 }, { 0x7ffffe5, 27 },
    { 0xfffec, 20 }, { 0xfffff3, 24 }, { 0xfffed, 20 }, { 0x1fffe6, 21 },
    { 0x3fffe9, 22 }, { 0x1fffe7, 21 }, { 0x1fffe8, 21 }, { 0x7ffff3, 23 },
    { 0x3fffea, 22 }, { 0x3fffeb, 22 }, { 0x1ffffee, 25 }, { 0x1ffffef, 25 },
    { 0xfffff4, 24 }, { 0xfffff5, 24 }, { 0x3ffffea, 26 }, { 0x7ffff4, 23 },
    { 0x3ffffeb, 26 }, { 0x7ffffe6, 27 }, { 0x3ffffec, 26 }, { 0x3ffffed, 26 },
    { 0x7ffffe7, 27 }, { 0x7ffffe8, 27 }, { 0x7ffffe9, 27 }, { 0x7ffffea, 27 },
    { 0x7ffffeb, 27 }, { 0xffffffe, 28 }, { 0x7ffffec, 27 }, { 0x7ffffed, 27 },
    { 0x7ffffee, 27 }, { 0x7ffffef, 27 }, { 0x7fffff0, 27 }, { 0x3ffffee, 26 },
    { 0x3fffffff, 30 }
};

int nl_h3_huffman_encode(const uint8_t* in, size_t in_len, uint8_t* out, size_t cap, size_t* out_len) {
    if (!out || !out_len || (!in && in_len)) return -1;
    uint64_t acc = 0;
    int nbits = 0;
    size_t op = 0;

    for (size_t i = 0; i < in_len; i++) {
        uint32_t code = HUFF[in[i]].code;
        int bits = HUFF[in[i]].bits;
        acc = (acc << bits) | (uint64_t)code;
        nbits += bits;
        while (nbits >= 8) {
            nbits -= 8;
            if (op >= cap) return -1;
            out[op++] = (uint8_t)(acc >> nbits);
        }
        acc &= (nbits > 0) ? (((uint64_t)1 << nbits) - 1) : 0;
    }

    if (nbits > 0) {
        int pad = 8 - nbits;
        acc = (acc << pad) | (((uint64_t)1 << pad) - 1);
        if (op >= cap) return -1;
        out[op++] = (uint8_t)acc;
    }

    *out_len = op;
    return 0;
}

int nl_h3_huffman_decode(const uint8_t* in, size_t in_len, uint8_t* out, size_t cap, size_t* out_len) {
    if (!out || !out_len || (!in && in_len)) return -1;
    if (in_len == 0) { *out_len = 0; return 0; }

    uint32_t cur = 0;
    int nbits = 0;
    size_t op = 0;

    for (size_t i = 0; i < in_len; i++) {
        for (int k = 7; k >= 0; k--) {
            cur = (cur << 1) | (uint32_t)((in[i] >> k) & 1);
            nbits++;
            int sym = -1;
            for (int s = 0; s < 257; s++) {
                if (HUFF[s].bits == (uint8_t)nbits && HUFF[s].code == cur) { sym = s; break; }
            }
            if (sym >= 0) {
                if (sym == 256) return -1;
                if (op >= cap) return -1;
                out[op++] = (uint8_t)sym;
                cur = 0;
                nbits = 0;
            } else if (nbits >= 30) {
                return -1;
            }
        }
    }

    if (nbits > 7) return -1;
    if (nbits > 0) {
        uint32_t ones = ((uint32_t)1 << nbits) - 1;
        if (cur != ones) return -1;
    }

    *out_len = op;
    return 0;
}

int nl_quictls_huffman_selftest(void) {
    static const uint8_t v1[] = { 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff };
    static const uint8_t v2[] = { 0xa8, 0xeb, 0x10, 0x64, 0x9c, 0xbf };
    static const uint8_t v3[] = { 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xa9, 0x7d, 0x7f };

    uint8_t buf[64];
    size_t n = 0;
    int rc;

    rc = nl_h3_huffman_encode((const uint8_t*)"www.example.com", 15, buf, sizeof(buf), &n);
    if (rc != 0 || n != sizeof(v1) || memcmp(buf, v1, sizeof(v1)) != 0) return 1;

    rc = nl_h3_huffman_encode((const uint8_t*)"no-cache", 8, buf, sizeof(buf), &n);
    if (rc != 0 || n != sizeof(v2) || memcmp(buf, v2, sizeof(v2)) != 0) return 2;

    rc = nl_h3_huffman_encode((const uint8_t*)"custom-key", 10, buf, sizeof(buf), &n);
    if (rc != 0 || n != sizeof(v3) || memcmp(buf, v3, sizeof(v3)) != 0) return 3;

    rc = nl_h3_huffman_decode(v1, sizeof(v1), buf, sizeof(buf), &n);
    if (rc != 0 || n != 15 || memcmp(buf, "www.example.com", 15) != 0) return 4;

    rc = nl_h3_huffman_decode(v2, sizeof(v2), buf, sizeof(buf), &n);
    if (rc != 0 || n != 8 || memcmp(buf, "no-cache", 8) != 0) return 5;

    rc = nl_h3_huffman_decode(v3, sizeof(v3), buf, sizeof(buf), &n);
    if (rc != 0 || n != 10 || memcmp(buf, "custom-key", 10) != 0) return 6;

    /* 7：encode "custom-value" → RFC 7541 C.6.3 向量（25 a8 49 e9 5b b8 e8 b4 bf） */
    {
        static const uint8_t v4[] = { 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xb8, 0xe8, 0xb4, 0xbf };
        rc = nl_h3_huffman_encode((const uint8_t*)"custom-value", 12, buf, sizeof(buf), &n);
        if (rc != 0 || n != sizeof(v4) || memcmp(buf, v4, sizeof(v4)) != 0) return 7;
        rc = nl_h3_huffman_decode(v4, sizeof(v4), buf, sizeof(buf), &n);
        if (rc != 0 || n != 12 || memcmp(buf, "custom-value", 12) != 0) return 8;
    }

    /* 9：往返 "NetLeaf" */
    rc = nl_h3_huffman_encode((const uint8_t*)"NetLeaf", 7, buf, sizeof(buf), &n);
    if (rc != 0) return 9;
    {
        uint8_t dec[64];
        size_t dn = 0;
        rc = nl_h3_huffman_decode(buf, n, dec, sizeof(dec), &dn);
        if (rc != 0 || dn != 7 || memcmp(dec, "NetLeaf", 7) != 0) return 10;
    }

    /* 11：空串 encode → 0 字节 */
    rc = nl_h3_huffman_encode((const uint8_t*)"", 0, buf, sizeof(buf), &n);
    if (rc != 0 || n != 0) return 11;

    /* 12：空串 decode → 0 字节 */
    rc = nl_h3_huffman_decode(buf, 0, buf, sizeof(buf), &n);
    if (rc != 0 || n != 0) return 12;

    /* 13：编码缓冲不足 → -1 */
    rc = nl_h3_huffman_encode((const uint8_t*)"www.example.com", 15, buf, 4, &n);
    if (rc != -1) return 13;

    /* 14：解码缓冲不足 → -1 */
    rc = nl_h3_huffman_decode(v1, sizeof(v1), buf, 4, &n);
    if (rc != -1) return 14;

    return 0;
}

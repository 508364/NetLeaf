/**
 * @file test_quictls.c
 * @brief 自研 QUIC-TLS 扩展（netleaf_quictls）回归测试
 *
 * 调用 nl_quictls_selftest_all()，覆盖：
 *   - RFC 9001 A.1/A.5 外部 KAT（Initial 密钥 / ChaCha20 短首部整包）
 *   - TLS 1.3 密钥调度
 *   - 握手状态机 + 证书/CertificateVerify
 *   - QUIC 握手 + HTTP/3 请求/响应
 *   - ACK / 丢包检测重传 / 拥塞 / RTT / PTO
 *   - 连接级 + 流级流控
 *   - QPACK（静态表 / 动态表 / 指令 / T=0 字段行）
 *
 * 全部通过返回 0；否则返回失败的自测个数。
 */
#include "netleaf_quic.h"

#include <stdio.h>

static int rc_of(const char* name, int (*f)(void)) {
    int rc = f();
    printf("[%s] %s => %d\n", name, rc==0?"PASS":"FAIL", rc);
    return rc;
}

int main(void) {
    int f1  = rc_of("rfc9001", nl_quic_rfc9001_selftest);
    int f2  = rc_of("tls13",   nl_quictls_tls13_selftest);
    int f3  = rc_of("hs",      nl_quictls_handshake_selftest);
    int f4  = rc_of("quic",    nl_quictls_quic_selftest);
    int f5  = rc_of("recovery",nl_quictls_recovery_selftest);
    int f6  = rc_of("flow",    nl_quictls_flowcontrol_selftest);
    int f7  = rc_of("qpack",   nl_quictls_qpack_selftest);
    int f8  = rc_of("huff",    nl_quictls_huffman_selftest);
    int f9  = rc_of("qpkstr",  nl_quictls_qpack_stream_selftest);
    int f10 = rc_of("qpkl",    nl_quictls_qpack_link_selftest);
    int f11 = rc_of("h3srv",   nl_quictls_h3server_selftest);
    int failures = (f1!=0)+(f2!=0)+(f3!=0)+(f4!=0)+(f5!=0)+(f6!=0)+(f7!=0)+(f8!=0)+(f9!=0)+(f10!=0)+(f11!=0);
    if (failures == 0) {
        printf("[PASS] quictls selftests: all 11 passed\n");
        return 0;
    }
    printf("[FAIL] quictls selftests: %d of 11 failed\n", failures);
    return 1;
}

/*
 * nl_px_io.c - 统一代理泵实现（平台无关）
 *
 * 与旧三平台实现逐位对齐（含半包余量 memmove 前移），保证字节级不发重、
 * 不发丢；差量只来自平台 ops 的 EAGAIN 判定。
 */
#include "nl_px_io.h"

#include <string.h>

int nl_px_pump(const nl_px_io_ops* io, intptr_t from, intptr_t to,
               char* buf, size_t cap, int* remain,
               nl_px_on_bytes on_bytes, void* ud) {
    if (!io || !io->recv_some || !io->send_some || !buf || cap == 0 || !remain)
        return -1;

    /* 1) 补发上次未发完的余量 */
    while (*remain > 0) {
        int w = io->send_some(to, buf, *remain);
        if (w > 0) {
            if (w < *remain) memmove(buf, buf + w, (size_t)(*remain - w));
            *remain -= w;
            continue;
        }
        if (w == NL_PX_IO_WANT) return 1;   /* 写满，等可写事件再补 */
        if (w == 0) return 0;               /* 对端关闭 */
        return -1;
    }

    /* 2) 读新数据 */
    int n = io->recv_some(from, buf, (int)cap);
    if (n == NL_PX_IO_WANT) return 1;
    if (n == 0) return 0;                   /* 对端 EOF */
    if (n < 0) return -1;

    if (n > (int)cap) n = (int)cap;
    /* 3) 旁路消费（解析器绝不修改 buf，转发必须字节级原样） */
    if (on_bytes) on_bytes(ud, buf, (size_t)n);

    *remain = n;
    while (*remain > 0) {
        int w = io->send_some(to, buf, *remain);
        if (w > 0) {
            if (w < *remain) memmove(buf, buf + w, (size_t)(*remain - w));
            *remain -= w;
            continue;
        }
        if (w == NL_PX_IO_WANT) return 1;   /* 写满，留待可写事件补发 */
        if (w == 0) return 0;
        return -1;
    }
    return 1;
}

/*
 * nl_px_io.h - 平台 IO 抽象（socket 收发 + 统一代理泵）
 *
 * 反代引擎三平台此前各写了一份逐行相同的 "先补发余量、再读新数据并转发"
 * 逻辑，仅 EAGAIN 判定不同。这里只抽象两个最小动作（收/发），
 * 泵送状态机（含半包余量前移、旁路喂解析器）由公共层统一实现。
 *
 * 语义约定（与反代会话层一致）：
 *   recv_some/send_some 返回 >0 字节数；0 = 对端关闭；NL_PX_IO_WANT = 暂不可用；
 *   NL_PX_IO_ERR = 硬错误。
 */
#ifndef NL_PX_IO_H
#define NL_PX_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NL_PX_IO_WANT (-2)
#define NL_PX_IO_ERR  (-1)

typedef struct nl_px_io_ops {
    int (*recv_some)(intptr_t fd, char* buf, int cap);
    int (*send_some)(intptr_t fd, const char* buf, int len);
} nl_px_io_ops;

/* 读到新数据时的回调（旁路解析 / 刷新活跃时间；data 只读，不得修改） */
typedef void (*nl_px_on_bytes)(void* ud, const char* data, size_t len);

/*
 * 非阻塞泵一次：
 *   1) 先补发上次未发完的 remain 字节（半包时把余量前移到 buf[0]）；
 *   2) 再从 from 读新数据，立即转发到 to；
 *   3) 读到的新数据同步交给 on_bytes（可为 NULL）。
 * 返回 1 = 可继续（暂不可用 / 已读满），0 = 对端 EOF，-1 = 错误。
 * remain 由调用方按连接持有，初始为 0。
 */
int nl_px_pump(const nl_px_io_ops* io, intptr_t from, intptr_t to,
               char* buf, size_t cap, int* remain,
               nl_px_on_bytes on_bytes, void* ud);

#ifdef __cplusplus
}
#endif

#endif /* NL_PX_IO_H */

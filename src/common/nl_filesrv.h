/*
 * nl_filesrv.h - 文件服务辅助（平台无关，一份实现）
 *
 * 第192轮去重：三平台 netleaf_*.c 中各有一份几乎相同的
 * get_file_extension / get_mime_type / read_file / send_http_response /
 * send_http_error / send_418_response / is_april_fools_day，
 * 现收敛为本模块。三平台通过 #define 别名映射旧名，调用点零改动。
 *
 * fd 统一为 intptr_t（Windows 内部转 SOCKET，POSIX 转 int）；
 * 统一返回 0（历史实现的返回值无任何调用方使用）。
 */
#ifndef NL_FILESRV_H
#define NL_FILESRV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 从文件名中提取扩展名（不含 '.'，写入 ext 且 NUL 结尾） */
void nl_filesrv_ext(const char* filename, char* ext, size_t cap);

/* 扩展名 → MIME 类型（未知时回退 application/octet-stream） */
const char* nl_filesrv_mime(const char* ext);

/*
 * 读取整个文件到内存（size+1 字节，末字节置 0，便于当 C 字符串用）。
 * 成功返回堆指针（调用方 free），失败返回 NULL。
 */
char* nl_filesrv_read(const char* path, size_t* out_size);

/* 发送 200 + Content-Type + 正文（Connection: close）。恒返回 0。 */
int nl_filesrv_send_response(intptr_t fd, const char* content_type,
                             const char* content, size_t len);

/* 发送错误页（HTML，Content-Length 与三平台旧实现保持一致）。恒返回 0。 */
int nl_filesrv_send_error(intptr_t fd, int status_code, const char* message);

/* 418 彩蛋页。恒返回 0。 */
int nl_filesrv_send_teapot(intptr_t fd);

/* 4 月 1 日（本地时区）返回 1，否则 0 */
int nl_filesrv_is_teapot_day(void);

#ifdef __cplusplus
}
#endif

#endif /* NL_FILESRV_H */

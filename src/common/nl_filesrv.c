/*
 * nl_filesrv.c - 文件服务辅助实现（平台无关）
 *
 * 内容与三平台旧 static 实现逐字节等价（teapot HTML 采用 Linux 多行版，
 * macOS 单行版为纯外观差异，已统一）；仅 fd 类型改为 intptr_t。
 */
#include "nl_filesrv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #define NLFS_SOCK(fd) ((SOCKET)(uintptr_t)(fd))
#else
    #include <sys/socket.h>
    #define NLFS_SOCK(fd) ((int)(fd))
#endif

void nl_filesrv_ext(const char* filename, char* ext, size_t cap) {
    if (!filename || !ext || cap == 0) return;
    ext[0] = '\0';
    const char* dot = strrchr(filename, '.');
    if (dot && *(dot + 1) != '\0') {
        snprintf(ext, cap, "%s", dot + 1);
    }
}

const char* nl_filesrv_mime(const char* ext) {
    if (!ext) return "application/octet-stream";

    /* 统一小写比较 */
    char lower[32];
    size_t i = 0;
    for (; ext[i] && i < sizeof(lower) - 1; i++) {
        char c = ext[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        lower[i] = c;
    }
    lower[i] = '\0';

    static const struct { const char* ext; const char* mime; } table[] = {
        {"html", "text/html"},
        {"htm",  "text/html"},
        {"css",  "text/css"},
        {"js",   "application/javascript"},
        {"json", "application/json"},
        {"xml",  "application/xml"},
        {"txt",  "text/plain"},
        {"png",  "image/png"},
        {"jpg",  "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif",  "image/gif"},
        {"bmp",  "image/bmp"},
        {"ico",  "image/x-icon"},
        {"svg",  "image/svg+xml"},
        {"webp", "image/webp"},
        {"mp3",  "audio/mpeg"},
        {"mp4",  "video/mp4"},
        {"pdf",  "application/pdf"},
        {"zip",  "application/zip"},
        {"gz",   "application/gzip"},
        {"wasm", "application/wasm"},
        {NULL, NULL}
    };

    for (int k = 0; table[k].ext; k++) {
        if (strcmp(lower, table[k].ext) == 0) return table[k].mime;
    }
    return "application/octet-stream";
}

char* nl_filesrv_read(const char* path, size_t* out_size) {
    if (out_size) *out_size = 0;
    if (!path) return NULL;

    FILE* file = fopen(path, "rb");
    if (!file) return NULL;

    /* 获取文件大小 */
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < 0) {
        fclose(file);
        return NULL;
    }

    /* 分配内存（size + 1 用于 NUL 终止符） */
    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }

    /* 读取文件内容 */
    size_t bytes_read = fread(buffer, 1, (size_t)size, file);
    fclose(file);

    if (bytes_read == 0 && size > 0) {
        free(buffer);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    if (out_size) *out_size = bytes_read;
    return buffer;
}

int nl_filesrv_send_response(intptr_t fd, const char* content_type,
                             const char* content, size_t len) {
    if (!content_type || !content) return 0;

    char header[4096];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              content_type, len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return 0;

    send(NLFS_SOCK(fd), header, (size_t)header_len, 0);
    if (len > 0) {
        send(NLFS_SOCK(fd), content, len, 0);
    }
    return 0;
}

int nl_filesrv_send_error(intptr_t fd, int status_code, const char* message) {
    char body[1024];
    int body_len = snprintf(body, sizeof(body),
                            "<html><body><h1>%d %s</h1></body></html>",
                            status_code, message);
    if (body_len < 0) body_len = 0;
    if ((size_t)body_len >= sizeof(body)) body_len = (int)sizeof(body) - 1;

    char header[4096];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: text/html\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status_code, message,
                              strlen(message) + 32);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return 0;

    send(NLFS_SOCK(fd), header, (size_t)header_len, 0);
    send(NLFS_SOCK(fd), body, (size_t)body_len, 0);
    return 0;
}

int nl_filesrv_send_teapot(intptr_t fd) {
    const char* teapot_html =
        "<html>\n"
        "<head>\n"
        "<title>418 I'm a teapot</title>\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; text-align: center; padding: 50px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); }\n"
        ".teapot { font-size: 100px; margin-bottom: 20px; }\n"
        ".container { background: white; border-radius: 16px; padding: 40px; box-shadow: 0 10px 40px rgba(0,0,0,0.2); }\n"
        "h1 { color: #8B4513; }\n"
        "p { color: #666; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<div class=\"container\">\n"
        "<div class=\"teapot\">🫖</div>\n"
        "<h1>418 I'm a teapot</h1>\n"
        "<p>This server is a teapot, not a coffee machine!</p>\n"
        "<p>Happy April Fools' Day! ☕</p>\n"
        "</div>\n"
        "</body>\n"
        "</html>";

    char response[8192];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 418 I'm a teapot\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "X-Teapot: Yes, this is a teapot!\r\n"
        "\r\n"
        "%s",
        strlen(teapot_html),
        teapot_html);
    if (len < 0) return 0;
    if ((size_t)len > sizeof(response)) len = (int)sizeof(response);
    send(NLFS_SOCK(fd), response, (size_t)len, 0);
    return 0;
}

int nl_filesrv_is_teapot_day(void) {
    time_t now = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    if (localtime_s(&tmv, &now) != 0) return 0;
#else
    if (!localtime_r(&now, &tmv)) return 0;
#endif
    return tmv.tm_mon == 3 && tmv.tm_mday == 1; /* 4 月 1 日 */
}

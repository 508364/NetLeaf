/*
 * nl_uri.c - RFC 3986 URI 端点处理 + 物理文件隔离（平台无关，一份实现）
 *
 * 设计与返回码见 nl_uri.h。要点：
 *   - 先解码后归一：`%2e%2e%2f` 与 `..` 同义，编码变体无法绕过段规范化；
 *   - 越根 `..` 直接拒绝（RFC 5.2.4 会丢弃越根前缀，本实现更严：
 *     越界请求是明确的攻击特征，按 403 处理，与三平台旧 realpath 校验
 *     的 403 语义一致）；
 *   - 物理隔离先 canonical root 再拼接，已存在文件做 canonical 二次校验
 *     （POSIX realpath 解析符号链接；Windows 用句柄最终路径解析 junction）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nl_uri.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600
    #endif
    #include <windows.h>
#else
    #include <limits.h>
    #include <errno.h>
#endif

/* 段栈上限（每段至少 2 字节 "a/"，8192 上限内远够用；超限按 ENOSPC 拒绝） */
#define NL_URI_MAX_SEG 512

/* ============================================================
 * 基础：hex / 分隔符 / 前缀边界
 * ============================================================ */
static int nl__hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* path 是否以 base 开头且处于路径边界（下一字符为分隔符或结尾），
 * 避免 /base 误匹配 /baseXXX。Windows 侧大小写不敏感（盘符/文件系统）。 */
static int nl__under(const char* base, const char* path) {
    size_t bl = strlen(base);
    if (bl == 0) return 0;
#if defined(_WIN32)
    if (_strnicmp(path, base, bl) != 0) return 0;
    if (base[bl - 1] == '\\' || base[bl - 1] == '/') return 1;
    return path[bl] == '\0' || path[bl] == '\\' || path[bl] == '/';
#else
    if (strncmp(path, base, bl) != 0) return 0;
    if (base[bl - 1] == '/') return 1;
    return path[bl] == '\0' || path[bl] == '/';
#endif
}

/* canonical 化：POSIX realpath（解析符号链接，要求路径已存在）；
 * Windows _fullpath（纯词法，存在与否均可）。返回 1 成功。 */
static int nl__canonical(const char* p, char* out, size_t cap) {
#if defined(_WIN32)
    return _fullpath(out, p, (int)cap) != NULL;
#else
    (void)cap;
    return realpath(p, out) != NULL;
#endif
}

/* 去掉尾部分隔符（根 "/" 本身除外） */
static void nl__strip_tail_sep(char* p) {
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) p[--n] = '\0';
}

/* ============================================================
 * 公共 API：percent-decode / encode（netleaf.h 悬空声明的实现）
 * ============================================================ */
NL_API char* nl_url_decode(const char* input) {
    if (!input) return NULL;
    size_t n = strlen(input);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '%') {
            if (i + 2 >= n) { free(out); return NULL; }      /* 截断的 %XX */
            int h1 = nl__hexv((unsigned char)input[i + 1]);
            int h2 = nl__hexv((unsigned char)input[i + 2]);
            if (h1 < 0 || h2 < 0) { free(out); return NULL; } /* 非法十六进制 */
            c = (unsigned char)((h1 << 4) | h2);
            i += 2;
        }
        if (c == 0 || c < 0x20 || c == 0x7f) { free(out); return NULL; } /* NUL / 控制字符 */
        out[w++] = (char)c;
    }
    out[w] = '\0';
    return out;
}

NL_API char* nl_url_encode(const char* input) {
    if (!input) return NULL;
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(input);
    char* out = (char*)malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)input[i];
        int unres = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '_' || c == '~';
        if (unres) {
            out[w++] = (char)c;
        } else {
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 0x0f];
        }
    }
    out[w] = '\0';
    return out;
}

/* ============================================================
 * remove_dot_segments（原地，RFC 3986 §5.2.4 变体）
 * 输入保证：以 '/' 开头、NUL 结尾、无 '%'、无控制字符、无 '\'。
 * 规则：折叠空段与 "."；".." 弹出上一段，栈空即越根：
 *       clamp=0 → 返回 NL_URI_ESCAPES（全局端点语义）；
 *       clamp=1 → 忽略该 ".."（第203轮：端点内根目录语义，见
 *       nl_uri_normalize_clamp）；
 *       保留原输入的尾部 '/' 语义。
 * 注意：输出 w 恒 <= 读指针 r。首段无需补 '/'（输入自带前导 '/'）；
 *       后续段的分隔符写在 w 处 —— 该位置正是输入中已被消费的 '/'，
 *       同值覆盖不破坏数据；w == r 时跳过拷贝（原实现先写 '/' 再从
 *       已被覆盖的位置 memcpy，产生 "//" 与垃圾字节）。
 * ============================================================ */
static int nl__remove_dot_segments(char* s, int clamp) {
    size_t len = strlen(s);
    uint16_t seg_pos[NL_URI_MAX_SEG];   /* 各段起始（分隔符位）在输出中的下标 */
    int nseg = 0;
    int trailing = (len > 1 && s[len - 1] == '/');
    const char* r = s + 1;              /* 读指针（跳过首 '/'） */
    const char* endp = s + len;
    char* w = s + 1;                    /* 写指针：恒 <= 读指针 */

    while (r < endp) {
        const char* sl = (const char*)memchr(r, '/', (size_t)(endp - r));
        const char* segend = sl ? sl : endp;
        size_t seglen = (size_t)(segend - r);

        if (seglen == 0 || (seglen == 1 && r[0] == '.')) {
            /* 空段（"//"）或 "."：折叠 */
        } else if (seglen == 2 && r[0] == '.' && r[1] == '.') {
            if (nseg == 0) {
                /* 越根：clamp=0 拒绝（攻击特征 → 403）；
                 * clamp=1 忽略（端点即根，../ 保持在端点内） */
                if (!clamp) return NL_URI_ESCAPES;
            } else {
                nseg--;
                w = s + seg_pos[nseg];              /* 弹出上一段 */
            }
        } else {
            if (nseg >= NL_URI_MAX_SEG) return NL_URI_ENOSPC;
            seg_pos[nseg++] = (uint16_t)(w - s);    /* 段起点（含分隔符位） */
            if (w > s + 1) *w++ = '/';              /* 首段免分隔符 */
            if (w != r) memmove(w, r, seglen);
            w += seglen;
        }
        r = segend;
        if (r < endp) r++;                          /* 跳过分隔符 */
    }

    if (w == s + 1) {
        s[0] = '/';                                 /* "/." "/.." 归为 "/" */
        s[1] = '\0';
    } else {
        if (trailing) *w++ = '/';
        *w = '\0';
    }
    return NL_URI_OK;
}

/* ============================================================
 * 规范化主体（clamp=0 全局端点语义 / clamp=1 端点内根目录语义）
 * ============================================================ */
static int nl__normalize_ex(const char* uri, char* out, size_t cap, int clamp) {
    if (!uri || !out || cap < 2) return NL_URI_EINVAL;

    const char* p = uri;
    if (p[0] != '/') {
        /* 绝对形式请求行（RFC 7230 §5.3.2）：scheme://authority/path → path */
        if (!((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')))
            return NL_URI_EINVAL;
        size_t i = 1;
        for (;;) {
            char c = p[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
                i++;
                continue;
            }
            break;
        }
        if (!(p[i] == ':' && p[i + 1] == '/' && p[i + 2] == '/'))
            return NL_URI_EINVAL;
        const char* slash = strchr(p + i + 3, '/');
        if (!slash) return NL_URI_EINVAL;           /* "http://host" 无 path */
        p = slash;
    }

    /* 剥离 ?query 与 #fragment */
    const char* end = p;
    while (*end && *end != '?' && *end != '#') end++;
    size_t rawlen = (size_t)(end - p);
    if (rawlen == 0) return NL_URI_EINVAL;
    if (rawlen + 1 > cap) return NL_URI_ENOSPC;

    /* percent-decode + 字节校验 → out（解码只缩不涨，out 可与 uri 别名） */
    size_t w = 0;
    for (size_t i = 0; i < rawlen; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '%') {
            if (i + 2 >= rawlen) return NL_URI_EINVAL;
            int h1 = nl__hexv((unsigned char)p[i + 1]);
            int h2 = nl__hexv((unsigned char)p[i + 2]);
            if (h1 < 0 || h2 < 0) return NL_URI_EINVAL;
            c = (unsigned char)((h1 << 4) | h2);
            i += 2;
        }
        if (c == 0 || c < 0x20 || c == 0x7f) return NL_URI_EINVAL;  /* NUL / 控制字符 */
        if (c == '\\') return NL_URI_EINVAL;                        /* RFC 3986 不含 '\' */
#if defined(_WIN32)
        if (c == ':') return NL_URI_EINVAL;                         /* 盘符 / NTFS ADS */
#endif
        out[w++] = (char)c;
    }
    if (w == 0) return NL_URI_EINVAL;
    out[w] = '\0';
    if (out[0] != '/') return NL_URI_EINVAL;

    return nl__remove_dot_segments(out, clamp);
}

NL_API int nl_uri_normalize(const char* uri, char* out, size_t cap) {
    return nl__normalize_ex(uri, out, cap, 0);
}

/* 第203轮：端点内根目录语义 —— 与 nl_uri_normalize 相同，但越根 ".."
 * 不报 ESCAPES 而是忽略（/web/../../x → /x，保持在端点根内）。
 * 用于"端点指向目录"（nl_web_add_dir）的余段解析。 */
NL_API int nl_uri_normalize_clamp(const char* uri, char* out, size_t cap) {
    return nl__normalize_ex(uri, out, cap, 1);
}

/* ============================================================
 * nl_uri_resolve_file —— 物理文件隔离主入口
 * ============================================================ */
NL_API int nl_uri_resolve_file(const char* root, const char* url_path,
                               char* out, size_t out_cap) {
    if (!root || !url_path || !out || out_cap < 2) return NL_URI_EINVAL;

    /* 1) 端点规范化（剥 query/fragment、解码、dot-segment、越根拒绝） */
    char norm[NL_URI_MAX];
    int rc = nl_uri_normalize(url_path, norm, sizeof(norm));
    if (rc != NL_URI_OK) return rc;

    /* 2) canonical 根目录（root 必须可解析，否则属配置错误） */
    char rootc[NL_URI_MAX];
    if (!nl__canonical(root, rootc, sizeof(rootc))) return NL_URI_EROOT;
#if defined(_WIN32)
    /* _fullpath 是纯词法的：不存在的 root 也能拼出路径 → 显式查存在性 */
    {
        DWORD wa = GetFileAttributesA(rootc);
        if (wa == INVALID_FILE_ATTRIBUTES || !(wa & FILE_ATTRIBUTE_DIRECTORY))
            return NL_URI_EROOT;
    }
#endif
    nl__strip_tail_sep(rootc);

    /* 3) 拼接：norm 已保证无 ".."、无 '\'、以 '/' 开头，词法上不可能逃逸 */
    int n = snprintf(out, out_cap, "%s%s", rootc, norm);
    if (n < 0 || (size_t)n >= out_cap) return NL_URI_ENOSPC;

    /* 4) canonical 二次校验（已存在文件解析符号链接 / junction） */
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (GetFileAttributesExA(out, GetFileExInfoStandard, &ad)) {
        HANDLE h = CreateFileA(out, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h == INVALID_HANDLE_VALUE) return NL_URI_EROOT;
        char fbuf[NL_URI_MAX];
        DWORD flen = GetFinalPathNameByHandleA(h, fbuf, (DWORD)sizeof(fbuf),
                                               FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (flen == 0 || flen >= sizeof(fbuf)) return NL_URI_EROOT;
        const char* fp = fbuf;
        if (strncmp(fp, "\\\\?\\UNC\\", 8) == 0) fp += 7;       /* \\?\UNC\ → \ */
        else if (strncmp(fp, "\\\\?\\", 4) == 0) fp += 4;
        if (!nl__under(rootc, fp)) return NL_URI_ESCAPES;
    } else {
        /* 不存在：词法前缀校验（norm 无 ..，恒过；保留兜底） */
        char lex[NL_URI_MAX];
        if (!_fullpath(lex, out, (int)sizeof(lex))) return NL_URI_ESCAPES;
        if (!nl__under(rootc, lex)) return NL_URI_ESCAPES;
    }
#else
    char fin[NL_URI_MAX];
    if (realpath(out, fin)) {
        if (!nl__under(rootc, fin)) return NL_URI_ESCAPES;
    } else if (errno == ENOENT || errno == ENOTDIR) {
        /* 文件（或其父目录）不存在：交由调用方回 404；
         * norm 无 .. → out 词法上必在 rootc 之下 */
        if (!nl__under(rootc, out)) return NL_URI_ESCAPES;
    } else {
        return NL_URI_EROOT;                        /* EACCES / ELOOP / ... */
    }
#endif
    return NL_URI_OK;
}

/* ============================================================
 * nl_uri_is_under —— 拼接后二次校验（如目录 index.html 追加）
 * ============================================================ */
NL_API int nl_uri_is_under(const char* root, const char* abs_path) {
    if (!root || !abs_path || !root[0] || !abs_path[0]) return 0;

    char rootc[NL_URI_MAX];
    if (!nl__canonical(root, rootc, sizeof(rootc))) return 0;
    nl__strip_tail_sep(rootc);

    char fin[NL_URI_MAX];
    if (!nl__canonical(abs_path, fin, sizeof(fin))) {
        /* 不存在：按词法路径校验（调用方传入的通常已是 root 下拼接结果） */
        if (strlen(abs_path) >= sizeof(fin)) return 0;
        memcpy(fin, abs_path, strlen(abs_path) + 1);
    }
    return nl__under(rootc, fin);
}

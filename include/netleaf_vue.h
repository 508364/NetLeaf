#ifndef NETLEAF_VUE_H
#define NETLEAF_VUE_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
    #ifdef NL_VUE_EXPORTS
        #define NL_VUE_API __declspec(dllexport)
    #else
        #define NL_VUE_API __declspec(dllimport)
    #endif
#else
    #define NL_VUE_API
#endif

#define NL_VUE_VERSION "2.2.2"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NL_VUE_CDN_UNPKG = 0,
    NL_VUE_CDN_CDNJS,
    NL_VUE_CDN_JSDELIVR,
    NL_VUE_CDN_LOCAL
} nl_vue_cdn_type_t;

NL_VUE_API int nl_vue_init(void);
NL_VUE_API void nl_vue_shutdown(void);
NL_VUE_API int nl_vue_is_available(void);
NL_VUE_API const char* nl_vue_version(void);

NL_VUE_API void nl_vue_set_default_version(const char* version);
NL_VUE_API const char* nl_vue_get_default_version(void);
NL_VUE_API void nl_vue_set_default_cdn(nl_vue_cdn_type_t cdn_type);
NL_VUE_API nl_vue_cdn_type_t nl_vue_get_default_cdn(void);

// 本地文件支持
NL_VUE_API void nl_vue_set_local_path(const char* path);
NL_VUE_API int nl_vue_load_from_file(const char* filepath);
NL_VUE_API const char* nl_vue_get_local_content(void);
NL_VUE_API int nl_vue_has_local_content(void);
NL_VUE_API const char* nl_vue_get_local_path(void);
NL_VUE_API void nl_vue_clear_local_content(void);

NL_VUE_API char* nl_vue_generate_page(const char* vue_code, const char* title, 
                                       nl_vue_cdn_type_t cdn_type, const char* version);
NL_VUE_API char* nl_vue_generate_page_inline(const char* vue_code, const char* title, const char* vue_filepath);
NL_VUE_API char* nl_vue_generate_page_with_vars(const char* vue_code, const char* title,
                                                 const char** vars, const char** values, int count,
                                                 nl_vue_cdn_type_t cdn_type, const char* version);
NL_VUE_API const char* nl_vue_get_cdn_url(nl_vue_cdn_type_t cdn_type, const char* version);
NL_VUE_API char* nl_vue_get_cdn_script(nl_vue_cdn_type_t cdn_type, const char* version);
NL_VUE_API char* nl_vue_get_local_script(const char* vue_filepath);

NL_VUE_API int nl_vue_detect_code(const char* html, size_t html_len);
NL_VUE_API int nl_vue_detect_import(const char* html, size_t html_len);

NL_VUE_API char* nl_vue_add_import(const char* html, size_t html_len, 
                                    nl_vue_cdn_type_t cdn_type, const char* version);

// 预制页面（唯一的系统信息页面）
NL_VUE_API char* nl_vue_generate_sysinfo(const char* title);
NL_VUE_API char* nl_vue_generate_sysinfo_inline(const char* title, const char* vue_filepath);

#ifdef __cplusplus
}
#endif

#endif

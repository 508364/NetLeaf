<img src="Logo.svg" width="40" height="40" align="left"> 

# NetLeaf v2.2.2

High-performance cross-platform network library supporting TCP/UDP/HTTP/HTTP2/HTTP3, and inline HTML/Vue reactive web server.

**Platform Support:**
- ✅ Windows (IOCP) - Full support
- ✅ Linux (epoll) - Full support
- ✅ macOS (kqueue) - Full support

## Features

- ✅ **Cross-platform**: Windows / Linux / macOS
- ✅ **Multi-architecture**: x86, x64, ARM, ARM64, RISC-V, etc.
- ✅ **Protocols**: HTTP/1.1, HTTP/2, HTTP/3 (QUIC), WebSocket, TCP, UDP
- ✅ **Web Server**: Built-in HTML/Vue.js support with server-side variable replacement
- ✅ **Data Parsing**: JSON + TOML
- ✅ **Lazy Loading**: All components support lazy loading
- ✅ **System Info**: OS/Architecture/CPU/RAM/Runtime information
- ✅ **Multi-threading**: Configurable thread pool (1-256 threads)
- ✅ **Dynamic Encoding**: Auto encoding negotiation and transcoding (UTF-8, GBK, Big5, etc.)
- ✅ **Auto-complete**: Automatic charset/Vue import
- ✅ **Auto-route**: Route suggestions for 404 pages
- ✅ **Custom Error Pages**: Template-based error pages with variables
- ✅ **IPC Communication**: Inter-process communication (Windows Named Pipe, Linux Unix Domain Socket)
- ✅ **Link Aggregation**: Same-port load balancing with multiple backends
- ✅ **Unified Module Interface**: Centralized module management via nl_module_info_t
- ✅ **ASan Support**: AddressSanitizer memory detection
- ✅ **File Hot Reload**: Modify external HTML/Vue/JSON files and see changes on refresh
- ✅ **HTTP Redirect**: Configurable 301/302 redirects with runtime switching
- ✅ **Smart Detection**: `nl_web_add_html/vue/json` auto-detects URL/file/static content
- ✅ **Runtime Route Management**: Add, remove, query, and update routes while server is running

## Quick Start

```c
#include "netleaf.h"

int main() {
    // Enable auto cleanup on exit (recommended)
    nl_web_set_auto_cleanup(1);
    
    // Create and start web server in one step
    nl_web_server_t* server = nl_web_create(8080);
    
    // Add HTML page
    nl_web_add_html(server, "/", "<h1>Hello NetLeaf!</h1>");
    
    // Keep server running
    while (1) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
    }
    
    return 0;
}
```

## Installation

### Windows

Download prebuilt libraries from [releases](releases/) or build from source:

```cmd
build_all.bat
```

### Linux/WSL

```bash
chmod +x build_all.sh
./build_all.sh
```

## API Reference

### Core Functions

```c
// Create and start web server
nl_web_server_t* nl_web_create(int port);

// Destroy web server
void nl_web_destroy(nl_web_server_t* server);

// Stop server by port
void nl_web_stop_by_port(int port);

// Set auto cleanup on exit
void nl_web_set_auto_cleanup(int enable);
```

### System Information (Lazy Loading)

```c
const char* nl_sys_info_get_os_name(void);
const char* nl_sys_info_get_architecture(void);
const char* nl_sys_info_get_cpu_model(void);
int64_t nl_sys_info_get_total_ram(void);
const char* nl_sys_info_get_runtime_version(void);
void nl_sys_info_set_ram_unit(int unit); // NL_RAM_UNIT_DECIMAL or NL_RAM_UNIT_BINARY
```

### Lazy Loading

```c
void nl_lazy_enable(int enable);
void nl_lazy_preload_module(int module);
void nl_lazy_stop_module(int module);
void nl_lazy_set_thread_count(int count); // 1-256 threads
```

### Dynamic Encoding API

```c
// Enable auto encoding negotiation
void nl_web_enable_auto_encoding(nl_web_server_t* server, int enable);

// Convert encoding between formats
char* nl_encoding_convert(const char* input, size_t len, 
                          const char* src_enc, const char* dst_enc);

// Detect string encoding
const char* nl_encoding_detect(const char* input, size_t len);

// Get system default encoding
const char* nl_encoding_get_system_default(void);
```

## Gitee Repository

```
https://gitee.com/x508364/NetLeaf
```

## License

MIT License

## Authors

- 508364

## Version

2.2.2

## Optional Modules

The following modules are separated from the main NetLeaf library, sharing the same version number (v2.2.2) and built together by default.

### 1. Auto-complete Module (netleaf_autocomplete)

**Features:**
- **Charset Auto-complete**: Automatically adds `<meta charset>` and `<meta name="viewport">` tags based on configured encoding
- **Vue Auto-import**: Detects Vue code without import and automatically adds CDN link
- **Flexible Enable**: Supports `1/0`, `true/false`, `on/off`, `yes/no` (case-insensitive)
- **Individual Control**: Charset and Vue features can be enabled/disabled separately

**Platform Support:**
- ✅ Windows
- ✅ Linux
- ✅ macOS

**Usage:**
```c
#include "netleaf_autocomplete.h"

int main() {
    nl_autocomplete_init();
    nl_autocomplete_set_encoding("UTF-8");
    nl_autocomplete_enable_ex("true");
    
    char* result = nl_autocomplete_process_html(html, strlen(html), NULL);
    free(result);
    return 0;
}
```

**Build Option:** `BUILD_AUTOCOMPLETE=ON` (default)

---

### 2. Auto-route Module (netleaf_autoroute)

**Features:**
- **Smart Route Matching**: Uses Levenshtein distance algorithm
- **Multi-strategy Scoring**: Path segment matching, prefix sharing, segment count
- **Wildcard Support**: `*` and `**` patterns
- **Flexible Enable**: Supports `1/0`, `true/false`, `on/off`, `yes/no`

**Platform Support:**
- ✅ Windows
- ✅ Linux
- ✅ macOS

**Usage:**
```c
#include "netleaf_autoroute.h"

int main() {
    nl_autoroute_init();
    nl_autoroute_enable_ex("true");
    
    nl_route_matcher_t* matcher = nl_autoroute_get_global_matcher();
    nl_route_matcher_add_route(matcher, "/api/users");
    
    char* suggestion = nl_route_matcher_find_similar(matcher, "/api/user", 0.4);
    free(suggestion);
    return 0;
}
```

**Build Option:** `BUILD_AUTOROUTE=ON` (default)

---

### 3. ErrorPage Module (netleaf_errorpage)

**Features:**
- **Template System**: Custom HTML error page templates
- **Required Variables**: `{{ERROR_CODE}}`, `{{ERROR_MESSAGE}}`, `{{REQUESTED_PATH}}`, `{{SERVER_VERSION}}`, `{{TIMESTAMP}}`
- **Conditional Blocks**: `{{#if SUGGESTION}}...{{/if}}`
- **Standalone**: Cannot be used by other modules; if not loaded, error pages won't work

**Platform Support:**
- ✅ Windows
- ✅ Linux
- ✅ macOS

**Usage:**
```c
#include "netleaf_errorpage.h"

int main() {
    nl_errorpage_init();
    nl_errorpage_enable_ex("true");
    
    nl_errorpage_set_template(404, custom_template);
    
    nl_errorpage_vars_t vars = {
        .status_code = 404,
        .error_message = "Not Found",
        .requested_path = "/badpath",
        .suggestion = "/correctpath",
        .server_version = "MyApp v1.0"
    };
    
    char* response = nl_errorpage_make_response(404, &vars);
    free(response);
    return 0;
}
```

**Required Variables:**
| Variable | Description |
|----------|-------------|
| `{{ERROR_CODE}}` | HTTP status code |
| `{{ERROR_MESSAGE}}` | Status description |
| `{{REQUESTED_PATH}}` | Request path |
| `{{SERVER_VERSION}}` | Server version |
| `{{TIMESTAMP}}` | Error timestamp |

**Build Option:** `BUILD_ERRORPAGE=ON` (default)

---

### 4. IPC Module (netleaf_ipc)

**Features:**
- **Inter-process Communication**: Windows Named Pipe and Linux Unix Domain Socket support
- **Server/Client Architecture**: Supports server-side listening and client connections
- **Cross-process Data Transfer**: Efficient data exchange between processes
- **Thread-safe Design**: Built with thread safety in mind

**Platform Support:**
- ✅ Windows (Named Pipe)
- ✅ Linux (Unix Domain Socket)
- ❌ macOS (Not supported)

**Usage:**
```c
#include "netleaf_ipc.h"

int main() {
    // Check platform support
    if (!nl_ipc_is_available()) {
        return 1;
    }
    
    nl_ipc_init();
    
#ifdef _WIN32
    const char* path = "\\\\.\\pipe\\my_pipe";
#else
    const char* path = "/tmp/my_socket";
#endif
    
    nl_ipc_server_t* server = nl_ipc_server_create(path);
    nl_ipc_server_start(server);
    
    // Keep running...
    while (1) sleep(1);
    
    nl_ipc_server_destroy(server);
    nl_ipc_shutdown();
    return 0;
}
```

**Build Option:** `BUILD_IPC=ON` (default)

---

### 5. LinkAgg Module (netleaf_linkagg)

**⚠️ Beta Version**

**Features:**
- **Same-port Load Balancing**: Single port listening with multiple backend forwarding
- **Load Balancing Strategies**: Round Robin, Random, Least Connections, Weighted Round Robin
- **Backend Support**: HTTP and IPC backends
- **Dependency**: Requires IPC module

**Platform Support:**
- ✅ Windows (Requires IPC)
- ✅ Linux (Requires IPC)
- ❌ macOS (Not supported - depends on IPC)

**Usage:**
```c
#include "netleaf_linkagg.h"

int main() {
    if (!nl_linkagg_is_available()) {
        return 1;
    }
    
    nl_linkagg_init();
    
    nl_linkagg_t* la = nl_linkagg_create(8080);
    nl_linkagg_set_strategy(la, NL_LINKAGG_STRATEGY_ROUND_ROBIN);
    
    nl_linkagg_add_http_backend(la, "127.0.0.1", 8081, 1);
    nl_linkagg_add_http_backend(la, "127.0.0.1", 8082, 1);
    
    nl_linkagg_start(la);
    
    while (1) sleep(1);
    
    nl_linkagg_destroy(la);
    nl_linkagg_shutdown();
    return 0;
}
```

**Build Option:** `BUILD_LINKAGG=ON` (default)

---

### 6. Lang Module (netleaf_lang)

**Features:**
- **Multi-language Support**: Unlimited languages (en_us, zh_cn, ja_jp, ko_kr, etc.)
- **Language Code Format**: Enforced `xx_xx` format (case-insensitive)
- **Separate Registration**: Register languages and error messages separately
- **Custom Error Codes**: Support for registering custom error codes
- **Multi-file Support**: Load multiple language files per library
- **Shared Files**: Multiple libraries can share translation files
- **Duplicate Detection**: Prevent duplicate error code registration
- **Async Loading**: Support for asynchronous loading

**Platform Support:**
- ✅ Windows
- ✅ Linux
- ✅ macOS

**Usage:**
```c
#include "netleaf_lang.h"

int main() {
    // Set language
    nl_lang_set("zh_cn");
    
    // Get error message
    const char* msg = nl_lang_get_error(NL_LIB_LINKAGG, -9);
    // Output: "不接入，ID被占用"
    
    // Switch to English
    nl_lang_set("en_us");
    msg = nl_lang_get_error(NL_LIB_LINKAGG, -9);
    // Output: "ID already in use, not connected"
    
    return 0;
}
```

**Build Option:** `BUILD_LANG=ON` (default)

---

### 7. Vue Module (netleaf_vue)

**Features:**
- **Vue CDN Configuration**: Support for unpkg, cdnjs, jsdelivr, and local Vue files
- **Vue Code Detection**: Auto-detect Vue code patterns in HTML
- **Auto Import**: Automatically add Vue CDN import when Vue code is detected
- **HTML Generation**: Generate complete HTML pages with Vue integration
- **Predefined Components**: Counter, Dashboard, and Form components
- **Variable Substitution**: Support for template variable replacement

**Platform Support:**
- ✅ Windows
- ✅ Linux
- ✅ macOS

**Usage:**
```c
#include "netleaf_vue.h"

int main() {
    nl_vue_init();
    
    // Generate a counter page
    char* html = nl_vue_generate_counter("My Counter");
    // Serve the HTML...
    free(html);
    
    // Generate a page with custom Vue code
    const char* vue_code = "<div>{{ message }}</div>";
    char* page = nl_vue_generate_page(vue_code, "My Page", NL_VUE_CDN_UNPKG, "3.4.0");
    // Serve the page...
    free(page);
    
    nl_vue_shutdown();
    return 0;
}
```

**Build Option:** `BUILD_VUE=ON` (default)
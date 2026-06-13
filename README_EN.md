<img src="Logo.svg" width="40" height="40" align="left"> 

# NetLeaf v2.2.0

High-performance cross-platform network library supporting TCP/UDP/HTTP/HTTP2/HTTP3, and inline HTML/Vue reactive web server.

**Platform Support:**
- ✅ Windows (IOCP) - Full support
- ✅ Linux (epoll) - Full support
- 🔶 macOS (kqueue) - **Initial support** (NEW in v2.2.0)

## Features

- ✅ **Cross-platform**: Windows / Linux / macOS 🔶
- ✅ **Multi-architecture**: x86, x64, ARM, ARM64, RISC-V, etc.
- ✅ **Protocols**: HTTP/1.1, HTTP/2, HTTP/3 (QUIC), WebSocket, TCP, UDP
- ✅ **Web Server**: Built-in HTML/Vue.js support with server-side variable replacement
- ✅ **Data Parsing**: JSON + TOML
- ✅ **Lazy Loading**: All components support lazy loading
- ✅ **System Info**: OS/Architecture/CPU/RAM/Runtime information
- ✅ **Multi-threading**: Configurable thread pool (1-256 threads)
- ✅ **Dynamic Encoding**: Auto encoding negotiation and transcoding (UTF-8, GBK, Big5, etc.)
- 🔶 **Auto-complete**: Automatic charset/Vue import (v2.2.0)
- 🔶 **Auto-route**: Route suggestions for 404 pages (v2.2.0)
- 🔶 **Custom Error Pages**: Template-based error pages with variables (v2.2.0)

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

2.2.0

## Optional Modules

The following modules are separated from the main NetLeaf library, sharing the same version number (v2.2.0) and built together by default.

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
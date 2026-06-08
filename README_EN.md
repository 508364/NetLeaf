# NetLeaf v2.1.6

High-performance cross-platform network library supporting TCP/UDP/HTTP/HTTP2/HTTP3, and inline HTML/Vue reactive web server.

![NetLeaf Logo](Logo.svg)

## Features

- ✅ **Cross-platform**: Windows / Linux
- ✅ **Multi-architecture**: x86, x64, ARM, ARM64, RISC-V, etc.
- ✅ **Protocols**: HTTP/1.1, HTTP/2, HTTP/3 (QUIC), WebSocket, TCP, UDP
- ✅ **Web Server**: Built-in HTML/Vue.js support with server-side variable replacement
- ✅ **Data Parsing**: JSON + TOML
- ✅ **Lazy Loading**: All components support lazy loading
- ✅ **System Info**: OS/Architecture/CPU/RAM/Runtime information
- ✅ **Multi-threading**: Configurable thread pool (1-256 threads)
- ✅ **Dynamic Encoding**: Auto encoding negotiation and transcoding (UTF-8, GBK, Big5, etc.)

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

2.1.6
<img src="Logo.svg" width="40" height="40" align="left">

# NetLeaf v3.0.0

High-performance cross-platform network library (Windows/Linux/macOS/Android) built on the **NL Extension System**. Supports TCP/UDP, HTTP/1.1 + HTTP/2, WebSocket, and MQTT v5.0, with a fully asynchronous web engine, TLS/SSL encryption, smart routing, and multi-language extensions.

**Platform Support:**
- ✅ Windows (IOCP) - Full support
- ✅ Linux (epoll) - Full support
- ✅ macOS (kqueue) - Full support
- ✅ Android - Core library (arm64-v8a / armeabi-v7a / x86_64)

## Features

- ✅ **Cross-platform**: Windows / Linux / macOS / Android (core)
- ✅ **Multi-architecture**: x86, x64, ARM, ARM64, RISC-V, etc.
- ✅ **Protocols**: TCP, UDP, HTTP/1.1, HTTP/2, WebSocket, MQTT v3.1.1/v5.0 (HTTP/3 QUIC is a separate optional `quic` extension)
- ✅ **Async Web Engine (v3.0.0)**: Data-driven event engine with streaming body (64KB chunks) + chunked encoding, SO_REUSEPORT multi-instance, 30s idle timeout scan (slowloris protection)
- ✅ **sendfile Zero-copy (v3.0.0)**: Static file routes via Linux `sendfile` / macOS BSD `sendfile` / Windows `TransmitFile` — measured 3~6x speedup, -97% memory
- ✅ **HTTP/2 Complete (v3.0.0)**: HPACK dynamic tables + RFC 7541 Huffman codec, stream state machine, RST_STREAM/GOAWAY
- ✅ **HTTPS/2 (v3.0.0)**: `nlh_tls_server_*` TLS server with ALPN h2 negotiation, H2 over TLS
- ✅ **TLS/SSL**: `netleaf_tls2` (mbedTLS 2.28.10 LTS, TLS 1.0-1.3) and `netleaf_tls3` (mbedTLS 3.6.x, TLS 1.2/1.3)
- ✅ **Configurable Concurrency**: Linux/macOS worker pool / Windows full IOCP completion threads (1..64); Windows UDP 16-slot receive ring prevents burst packet loss
- ✅ **Web Server**: Built-in HTML/Vue.js support with server-side variable replacement, file hot reload, 301/302 redirects
- ✅ **Smart Routing / i18n / Vue**: 404 route suggestions, multi-language error messages, backend Vue component generation
- ✅ **IPC / Link Aggregation**: Named Pipe / Unix Socket IPC, same-port load balancing
- ✅ **Lazy Loading & System Info**: All components support lazy loading; OS/Architecture/CPU/RAM/Runtime queries
- ✅ **Dynamic Encoding**: Auto encoding negotiation and transcoding (UTF-8, GBK, Big5, etc.)
- ✅ **Dynamic Extension System**: Runtime loading, dependency management, lifecycle control, hot reload

## Project Layout

```
NetLeaf/
├── include/            Public headers
│   ├── netleaf.h, netleaf_module.h, nl_util.h
│   ├── optimize/       netleaf_http.h, netleaf_websocket.h, netleaf_optimize.h
│   └── netleaf_{tls2,tls3,quic,mqtt,mqtt_server,mqtt_tls,lang,vue,vue_assets,ipc,linkagg,autocomplete,autoroute,errorpage}.h
├── src/
│   ├── common/         nl_util.c (shared utilities)
│   ├── http/           common HTTP logic + nlh_* shortname layer
│   ├── quic/           QUIC/H3 extension (BUILD_QUIC)
│   ├── tls/(tls2) tls3/ mqtt/ lang/ vue/ ipc/ linkagg/ autocomplete/ autoroute/ errorpage/
│   └── windows/ linux/ macos/  platform backends (+ optimize/)
├── third-party/        mbedtls (2.28.10 LTS), mbedtls3 (3.6.x)
├── examples/           example_all_features.c, plugin_example/, plugin_template/, extension_showcase/
├── test/               ctest suites + bench_web benchmark tool
├── wiki/               Full documentation
└── build_all.sh / build_all.bat / build_macos.sh
```

**Libraries built (all SHARED):** `netleaf_core`, `netleaf_https`, `netleaf_tls2`, `netleaf_tls3`, `netleaf_quic` (optional), `netleaf_mqtt`, `netleaf_mqtt_server`, `netleaf_lang`, `netleaf_ipc`, `netleaf_linkagg`, `netleaf_vue`, `netleaf_autocomplete`, `netleaf_autoroute`, `netleaf_errorpage`.

## Build Requirements

- CMake 3.20+
- C compiler: MSVC / Clang / GCC (MSYS2/MinGW or LLVM-MinGW recommended on Windows)
- Android: NDK r25+ (cross-compile, see [docs/android_build.md](docs/android_build.md))
- mbedTLS sources are bundled in `third-party/mbedtls/` and `third-party/mbedtls3/` — no extra install needed

## Building

### CMake Options

| Option | Default | Description |
| --- | --- | --- |
| `BUILD_MQTT` / `BUILD_MQTT_SERVER` | ON | MQTT client / server |
| `BUILD_TLS3` | ON | Build `netleaf_tls3` (mbedTLS 3.x, TLS 1.2/1.3) |
| `BUILD_TLS` | OFF | Build `netleaf_tls2` (mbedTLS 2.28.10); mutually exclusive with `BUILD_TLS3` |
| `BUILD_QUIC` | OFF | Separate QUIC/H3 extension (core library has no QUIC) |
| `BUILD_LANG` | ON | Multi-language error messages |
| `BUILD_TESTS` / `BUILD_EXAMPLES` | OFF | Tests / examples |
| `NETLEAF_ENABLE_LTO` | OFF | LTO (adds `-flto`/`/GL /LTCG` only) |

### Standard Build (Windows)

```bash
cmake -B build -G "MinGW Makefiles" ^
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang.exe ^
    -DCMAKE_MAKE_PROGRAM=C:\msys64\mingw64\bin\make.exe ^
    -DBUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure

# macOS one-shot script
chmod +x build_macos.sh && ./build_macos.sh

# Windows one-shot script
build_all.bat
```

## Quick Start

```c
#include "netleaf.h"
#include "optimize/netleaf_http.h"
#include <stdio.h>

/* Handler: plain C function pointer.
 * NOTE: *response MUST be heap-allocated (malloc/calloc/realloc);
 * string literals / static buffers will crash when the server frees it. */
static void on_http(const nl_http_request_t* req, nl_http_response_t* resp, void* ud) {
    (void)ud;
    if (strcmp(nlh_req_path(req), "/api/hello") == 0) {
        nlh_resp_status(resp, 200);
        nlh_resp_header(resp, "Content-Type", "application/json");
        nlh_resp_body(resp, "{\"message\": \"Hello, NetLeaf!\"}", 38);
    } else {
        nlh_resp_status(resp, 404);
        nlh_resp_body(resp, "Not Found", 9);
    }
}

int main(void) {
    nl_http_server_t* server = nlh_server_create(8080);
    if (!server) return 1;
    nlh_server_set_handler(server, on_http, NULL);
    nlh_server_start(server);
    nlh_server_stop(server);
    nlh_server_destroy(server);
    return 0;
}
```

> **Shortname layer (`nlh_*`)**: HTTP/H2 APIs have one-to-one short aliases
> (`nlh_server_create` / `nlh_req_method` / `nlh_resp_body` …) with identical signatures,
> exported from the core library. Long names (`nl_http_*` / `nl_http2_*`) remain but are deprecated.

### Reverse Proxy / Mixed Routes (Async Engine, Beta)

```c
#include "netleaf.h"

int main(void) {
    nl_web_server_t* server = nl_web_create(8080);
    nl_web_add_proxy(server, "/api/",  "http://127.0.0.1:9000"); // HTTP forward
    nl_web_add_proxy(server, "/cache/", "tcp://127.0.0.1:6379"); // TCP passthrough
    nl_web_add_html_file(server, "/docs/", "./docs/index.html");  // hot reload + sendfile
    nl_web_start(server);  // async engine auto-starts on proxy routes
    nl_web_stop(server);
    nl_web_destroy(server);
    return 0;
}
```

### MQTT Client

```c
#include "netleaf.h"
#include "netleaf_mqtt.h"

void on_connect(int rc, void* ud) { (void)ud; printf("connected: %d\n", rc); }
void on_message(const char* topic, const void* payload, size_t len,
                int qos, int retain, void* ud) { (void)ud; /* ... */ }

int main(void) {
    nl_mqtt_client_t* client = nl_mqtt_create();
    nl_mqtt_connect_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.client_id     = "netleaf_client_001";
    opts.keep_alive    = 60;
    opts.clean_session = 1;

    nl_mqtt_connect(client, "broker.hivemq.com", 1883, &opts, on_connect);
    nl_mqtt_subscribe(client, "netleaf/#", &(nl_mqtt_sub_opts_t){.qos = 1}, on_message, NULL);

    while (nl_mqtt_get_status(client) == NL_MQTT_CONNECTED) {
        nl_mqtt_maintain(client, 1000);
    }
    nl_mqtt_destroy(client);
    return 0;
}
```

## Extension System

Runtime dynamic extension framework: auto-discovery (drop into `extensions/`, loaded by `nl_modules_init()`), dependency declaration (`nl_extension_dependency_t`, registration returns -2 if a required dependency is missing), lifecycle and hot-reload management. See [wiki/Extension.md](wiki/Extension.md) and `examples/plugin_example/`, `examples/plugin_template/`.

```c
#include "netleaf_module.h"

NL_EXTENSION_DEFINE(my_ext, "My Extension", "1.0.0", "Author",
    "My custom extension", "Windows,Linux,MacOS",
    NL_CAP_THREAD_SAFE, my_init, my_shutdown, my_is_available, my_version);

nl_extension_init_all();
int count = nl_extension_find_by_capability(NL_CAP_SERVER, servers, 10);
nl_extension_reload_all();
nl_extension_shutdown_all();
```

## Testing

```bash
ctest --test-dir build --output-on-failure
```

Registered suites: `ModulesTest` / `FullTest` / `VariableSubstitutionTest` / `AllModulesTest` / `MqttFramesTest` / `HttpFramesTest` / `MqttServerLoopbackTest` / `WebEngineTest` / `QuicTlsTest` (with `BUILD_QUIC`). Benchmark tool `test/bench_web.c` covers CONTENT/FILE/PROXY/slow-client scenarios.

## Documentation

Full documentation lives in [wiki/](wiki/Home.md) (Quickstart, Installation, Features, API Reference, Examples, Troubleshooting, plus TLS/MQTT/Vue/IPC/Lang/LinkAgg/Extension topics). Full changelog: [CHANGELOG.md](CHANGELOG.md).

## What's New in v3.0.0 (2026-10-31)

- **Web engine full async refactor (P1-P12)**: `nl_web_engine_t` event engine with streaming body/chunked, multi-instance, 30s idle scan; benchmarked (PROXY beats legacy worker pool, 10MB file RSS -97%); sendfile/TransmitFile zero-copy (3.3~5.9x); macOS/Windows parity; 9 integration tests (ASan/TSan clean)
- **HTTP/2 completion**: HPACK dual dynamic tables + RFC 7541 Huffman, stream state machine, RST_STREAM/GOAWAY (47 assertions)
- **HTTPS/2**: `nlh_tls_server_*` TLS server + ALPN h2 dispatch
- **Windows full IOCP**: `nl_server_*` worker pool retired — AcceptEx pre-post + completion threads; UDP 16-slot receive ring
- **QUIC/H3 split** into a separate `quic` extension (`BUILD_QUIC` default OFF)

## Version History

| Version | Date | Highlights |
| --- | --- | --- |
| 3.0.0 | 2026-10-31 | Async web engine (sendfile zero-copy, multi-instance, test suite); HTTP/2 completion; HTTPS/2; Windows full IOCP + UDP receive ring; QUIC split out |
| 2.4.2 | 2026-09-30 | HTTPS/TLS refactor (`netleaf_https` split, tls2/tls3); MQTT on by default; `nl_util`; H2/QUIC frame fixes; Android build support |
| 2.4.1 | 2026-09-24 | Lang error-code i18n, dynamic/external variables; warning cleanup; full-platform cross builds |
| 2.4.0 | 2026-09-19 | MQTT v5.0 full protocol; TLS/SSL integration (mbedTLS); extension system API expansion |
| 2.2.2 | 2026-09-09 | Extension library system, file hot reload, HTTP redirects, runtime route management |
| 2.2.1 | 2026-09-01 | IPC, link aggregation, multi-language error messages |
| 2.2.0 | 2026-08-25 | macOS support; Auto-complete / Auto-route / ErrorPage modules |
| 2.1.0 | 2026-08-20 | Route correction, dynamic encoding, lazy loading |
| 2.0.0 | 2026-08-15 | Modular architecture |

## License

BSD 3-Clause License - see [LICENSE](LICENSE)

## Acknowledgements

| Project | Purpose | License | Repository |
| --- | --- | --- | --- |
| **mbedTLS** | TLS/SSL library | Apache 2.0 / GPL v2.0 | [Mbed-TLS/mbedTLS](https://github.com/Mbed-TLS/mbedTLS) |
| **MQTT Specification** | MQTT v5.0 protocol spec | EPL-2.0 / EDL 1.0 | [mqtt.org](https://mqtt.org/) |
| **Paho MQTT C** | MQTT C client reference | EPL-2.0 / EDL 1.0 | [eclipse/paho.mqtt.c](https://github.com/eclipse/paho.mqtt.c) |

> ⚠️ TLS 1.0 / 1.1 are deprecated by [RFC 8996](https://www.rfc-editor.org/rfc/rfc8996) and must not be used in production. Set `min_proto` to `NL_TLS2_PROTO_TLS1_2` or higher. See `include/netleaf_tls2.h`, `include/netleaf_tls3.h`, and [wiki/TLS.md](wiki/TLS.md).

Issues and Pull Requests are welcome!

***

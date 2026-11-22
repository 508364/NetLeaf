<img src="Logo.svg" width="40" height="40" align="left">

# NetLeaf v3.1.0

High-performance cross-platform network library (Windows/Linux/macOS/Android) built on the **NL Extension System**. Supports TCP/UDP, HTTP/1.1 + HTTP/2, WebSocket, and MQTT v5.0, with a fully asynchronous web engine, TLS/SSL encryption, smart routing, and multi-language extensions.

**Platform Support:**
- ✅ Windows (IOCP) - Full support
- ✅ Linux (epoll) - Full support
- ✅ macOS (kqueue) - Full support
- ✅ Android - Core library (arm64-v8a / armeabi-v7a / x86_64)

## Features

- ✅ **Cross-platform**: Windows / Linux / macOS / Android (core)
- ✅ **Multi-architecture**: x86, x64, ARM, ARM64, RISC-V, etc.
- ✅ **Protocols**: TCP, UDP, HTTP/1.1, HTTP/2, WebSocket, MQTT v3.1.1/v5.0 (HTTP/3 QUIC is a separate optional `quic` extension — **discontinued, no longer updated**)
- ✅ **Unified logging (v3.0.1)**: single platform-independent core shared by all three platforms — millisecond timestamps + thread ID + padded level field (`[INFO ] [T12345] [2026-11-20 17:30:45.123] message`), per-level ANSI colors on consoles (DEBUG cyan / INFO green / WARN yellow / ERROR red, honors `NO_COLOR`), optional file sink with size-based rotation (`nl_log_set_file`)
- ✅ **Async Web Engine (v3.0.0)**: Data-driven event engine with streaming body (64KB chunks) + chunked encoding, SO_REUSEPORT multi-instance, 30s idle timeout scan (slowloris protection)
- ✅ **sendfile Zero-copy (v3.0.0)**: Static file routes via Linux `sendfile` / macOS BSD `sendfile` / Windows `TransmitFile` — measured 3~6x speedup, -97% memory
- ✅ **HTTP/2 Complete (v3.0.0)**: HPACK dynamic tables + RFC 7541 Huffman codec, stream state machine, RST_STREAM/GOAWAY
- ✅ **HTTPS/2 (v3.0.0)**: `nlh_tls_server_*` TLS server with ALPN h2 negotiation, H2 over TLS
- ✅ **TLS/SSL**: `netleaf_tls2` (mbedTLS 2.28.10 LTS, TLS 1.0-1.3) and `netleaf_tls3` (mbedTLS 3.6.x, TLS 1.2/1.3)
- ✅ **Configurable Concurrency**: Linux/macOS worker pool requires an explicit `nl_server_start_workers()` call before `nl_server_start()` (default is plain epoll/kqueue, in-place handling); Windows full IOCP completion threads (1..64); Windows UDP 16-slot receive ring prevents burst packet loss
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
│   ├── quic/           QUIC/H3 extension (discontinued, no longer updated)
│   ├── tls/(tls2) tls3/ mqtt/ lang/ vue/ ipc/ linkagg/ autocomplete/ autoroute/ errorpage/
│   └── windows/ linux/ macos/  platform backends (+ optimize/)
├── third-party/        mbedtls (2.28.10 LTS), mbedtls3 (3.6.x)
├── examples/           example_all_features.c, plugin_example/, plugin_template/, extension_showcase/
├── test/               ctest suites + bench_web benchmark tool
├── wiki/               Full documentation
└── build_all.sh / build_all.bat / build_macos.sh
```

**Libraries built (all SHARED):** `netleaf_core`, `netleaf_https`, `netleaf_tls2`, `netleaf_tls3`, `netleaf_quic` (optional, discontinued), `netleaf_mqtt`, `netleaf_mqtt_server`, `netleaf_lang`, `netleaf_ipc`, `netleaf_linkagg`, `netleaf_vue`, `netleaf_autocomplete`, `netleaf_autoroute`, `netleaf_errorpage`.

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
| `BUILD_QUIC` | OFF | Separate QUIC/H3 extension (core library has no QUIC) — **discontinued, no longer updated** |
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

**Directory (prefix) forwarding `nl_web_add_proxy_dir`** (nginx `location` semantics) —
forward an entire directory to an upstream; match priority is **exact > longest prefix**:

```c
nl_web_add_proxy_dir(server, "/api/",    "http://127.0.0.1:9000");     // full-path passthrough
nl_web_add_proxy_dir(server, "/api/v2/", "http://127.0.0.1:9000/v2/"); // strip prefix + join URI
```

- **Upstream without URI**: the full original path (including query) is forwarded
  unchanged (equivalent to nginx `proxy_pass http://backend;`);
- **Upstream with URI**: the matched prefix is stripped and the URI appended, query
  preserved (`/api/foo?a=1` → upstream `/v1/foo?a=1`, equivalent to nginx
  `proxy_pass http://backend/v1/;`);
- The prefix boundary is controlled by the trailing slash (`/api/` does not match
  `/apifoo`); directory routes mix freely with exact `nl_web_add_proxy` routes —
  exact matches always win.

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

Registered suites: `ModulesTest` / `FullTest` / `VariableSubstitutionTest` / `AllModulesTest` / `MqttFramesTest` / `HttpFramesTest` / `MqttServerLoopbackTest` / `WebEngineTest` / `LogSystemTest` / `QuicTlsTest` (with `BUILD_QUIC`; QUIC is discontinued). Benchmark tool `test/bench_web.c` covers CONTENT/FILE/PROXY/slow-client scenarios.

## Documentation

Full documentation lives in [wiki/](wiki/Home.md) (Quickstart, Installation, Features, API Reference, Examples, Troubleshooting, plus TLS/MQTT/Vue/IPC/Lang/LinkAgg/Extension topics). Full changelog: [CHANGELOG.md](CHANGELOG.md).

## What's New in v3.1.0 (2026-11-22)

- **`nlh_*` HTTP protocol fixes**: added the `100 Continue` handshake (curl uploads >1MB no longer idle-wait 1s — 4MB measured 23ms); oversized header fields (value >4096B per spec / >64 entries) now get `431` instead of silent truncation; requests over the 64MiB total limit get a clean `413` instead of an early corrupt `200`; fixed request-header scanning skipping the last header line (`Content-Length` was always missed)
- **`nlh_resp_file` returns 404 on missing files**: immediate `404 + empty body` (previously `200 + headers` then 0-byte hang, measured 655s)
- **Plaintext HTTP client**: `nlh_client_connect(host, port, NULL)` now works without TLS; `nlh_client_request` documents `out_buf` as the raw response and a new `nlh_resp_body_offset()` helper splits off the body
- **`nlh_server_enable_tls` misuse guard**: marked deprecated and logs a runtime error pointing to `nlh_tls_server_*`
- **Version 3.1.0**; ctest 11/11 on both platforms (new HttpFramesTest boundary case group)

## What's New in v3.0.1 (2026-11-21)

- **Unified logging**: the three per-platform logging copies (~120 lines each) merged into one platform-independent core `src/common/nl_logsys.c` (~360 lines removed); millisecond timestamps, thread IDs, padded level fields; new file sink with size-based rotation (`nl_log_set_file` / `nl_log_close_file`) and level query (`nl_log_get_level`); per-level ANSI colors on consoles (honors `NO_COLOR`); bare `fprintf` calls in `http_tls` now go through `nl_log_error`; new regression test `LogSystemTest` (11/11 on both Windows and Linux)
- **QUIC/H3 discontinued**: the `quic` extension is frozen due to unfixable protocol-stack defects — no further updates as of 2026-11-21 (source retained, `BUILD_QUIC` remains OFF by default)

## What's New in v3.0.1 (2026-11-08)

- **LinkAgg reverse data pool**: link aggregation now uses a host-pulls model — requests are queued per endpoint, the host calls `nl_lagg_pull()` / `nl_lagg_request_respond()`; client waits time out, retry once, then mark the endpoint unhealthy and return 503 (auto-recover when the host resumes)
- **TLS2/TLS3 non-blocking async**: new `nl_tls2_set_nonblocking()` / `nl_tls3_set_nonblocking()` — handshake/I/O return `WANT_READ`/`WANT_WRITE` for your event loop; fixes `handshake()` always returning INVALID_STATE
- **Shared core extraction**: the three per-platform LinkAgg copies merged into one `netleaf_linkagg_core.c` (~47KB smaller); `nl_lagg_start()` is now non-blocking
- **Windows named pipes in byte mode**: 64KB buffer, arbitrary payload sizes; new regression test `test_linkagg_pull`
- **HTTP/2 & HTTPS stability fixes (2026-11-13)**: >16KB in-memory responses over HTTPS/1.1 and large H2 responses no longer truncate/hang (partial-write loops on all write paths); `nlh_resp_file` now streams files as H2 DATA frames (was 200 with empty body); H2 streams are unlinked and freed right after END_STREAM/CLOSED (fixes cumulative-request OOM on long-lived connections); batched H2 frame writes reduce per-request TLS syscalls and raise QPS
- **Android split-out (minSdk 21)**: platform backends moved into `src/android/` (Bionic iconv gated on API 28, runtime version string); verified cross-builds for all three ABIs (NDK r30)
- **Cross-protocol reverse-proxy fixes, 5 defects (2026-11-15)**: `.so` suffix off-by-one that prevented Linux/macOS extension auto-loading; extension discovery now derives the module name from the filename first and no longer probes `get_module_info` (struct mismatch → segfault); `dlsym` results are validated against `library_id` so `libnetleaf_https.so` can no longer register as `lang`; `nl_proxy_connect_upstream` now uses the new `conn_wrap` TLS contract (fixes HTTPS/H2 upstreams always returning 502); removed the H2-side HPACK capacity-update write that desynchronized decoding
- **HPACK dynamic table capacity-update decoding**: `hpack_decode_header` implements RFC 7541 §6.3 capacity updates, so the header block no longer desyncs and `:path` is preserved
- **Worker pool explicit enablement (2026-11-15)**: the Linux/macOS worker pool no longer starts by default (plain epoll/kqueue); even with a configured concurrency value you must call the new `nl_server_start_workers()` before `nl_server_start()` to build the pool; fixes a double `pthread_join` segfault on `nl_server_stop()` + `nl_server_destroy()`

## What's New in v3.0.0 (2026-10-31)

- **Web engine full async refactor (P1-P12)**: `nl_web_engine_t` event engine with streaming body/chunked, multi-instance, 30s idle scan; benchmarked (PROXY beats legacy worker pool, 10MB file RSS -97%); sendfile/TransmitFile zero-copy (3.3~5.9x); macOS/Windows parity; 9 integration tests (ASan/TSan clean)
- **HTTP/2 completion**: HPACK dual dynamic tables + RFC 7541 Huffman, stream state machine, RST_STREAM/GOAWAY (47 assertions)
- **HTTPS/2**: `nlh_tls_server_*` TLS server + ALPN h2 dispatch
- **Windows full IOCP**: `nl_server_*` worker pool retired — AcceptEx pre-post + completion threads; UDP 16-slot receive ring
- **QUIC/H3 split** into a separate `quic` extension (`BUILD_QUIC` default OFF)

## Version History

| Version | Date | Highlights |
| --- | --- | --- |
| 3.1.0 | 2026-11-22 | `nlh_*` HTTP protocol fixes (100-continue, 431 oversized headers, 413 total limit, last-header scan); `nlh_resp_file` 404 on missing file; plaintext HTTP client (`cfg=NULL`) + `nlh_resp_body_offset` response splitter; deprecated `nlh_server_enable_tls` misuse guard |
| 3.0.1 | 2026-11-08 | LinkAgg reverse data pool (host pulls, timeout retry, health marking); TLS2/TLS3 non-blocking async; shared core `netleaf_linkagg_core.c`; Windows named pipes byte mode; TLS handshake INVALID_STATE fix; HTTP/2/HTTPS large-response truncation & stream-leak OOM fixes, Android backend split-out (2026-11-13); cross-protocol reverse-proxy fixes, HPACK capacity-update decoding, Linux/macOS worker pool now gated behind `nl_server_start_workers()` (2026-11-15); unified logging (ms timestamps/thread IDs/colors/file rotation), QUIC/H3 discontinued (2026-11-21) |
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

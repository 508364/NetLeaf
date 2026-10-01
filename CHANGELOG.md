# Changelog

所有重要变更将记录在此文件中。

遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/) 规范。

## [2.4.2](https://github.com/508364/NetLeaf/compare/v2.4.1...v2.4.2)

### 新增

#### QUIC-TLS 自研扩展库 `netleaf_quictls`（实验性）— HTTP/3 基石

- **新扩展库**：CMake 选项 `BUILD_QUICTLS`（默认 OFF，需 `BUILD_TLS3=ON`）；在 mbedTLS 原语之上
  **自研** QUIC-TLS（RFC 9001），不依赖 mbedTLS 的 TLS 记录层（mbedTLS 3.6 无 QUIC API）。
- **能力**：
  - 密码学与包保护：HKDF / QUIC v1 Initial 密钥 / 长·短首部 AEAD 保护 + 首部保护（AES-GCM + ChaCha20）/
    包号编解码 / 首部保护后**动态读取 pn_len**。
  - 握手：TLS 1.3 密钥调度与握手状态机（X25519 ECDHE + transcript + Finished）、
    **Certificate + CertificateVerify**（ECDSA P-256）与证书链校验、**ALPN `h3`**、KeyUpdate、**quic ku keystore 更新**。
  - QUIC：变长整数、CRYPTO/STREAM 帧、Initial/Handshake/1-RTT 数据包；**可靠性**（ACK、
    packet-threshold + 时间阈值丢包检测与重传、NewReno 拥塞、RTT/PTO、ECN、持久拥塞）；
    连接级 + 流级**流控**。
  - HTTP/3：SETTINGS/HEADERS/DATA 帧；**QPACK**（静态表全集 + 动态表 + 编解码器流 + 霍夫曼 +
    动态表↔字段块联动）。
  - **可运行 HTTP/3 服务端**：`nlh3_server_*` / `nlh3_client_request[_ex]`（UDP 回环、多连接、
    客户端证书校验、真实丢包下 **PTO 重传**）。
- **验证**：`test/test_quictls.c`（ctest `QuicTlsTest`）一次运行 **11 个自测，全部通过**；
  含 **RFC 9001 A.1 / A.2 / A.3 / A.5 外部 KAT**，以及握手+证书、QUIC+H3、可靠性、流控、QPACK 端到端自测。
- **模块接入**：新增模块类型 `NL_MODULE_QUICTLS=10` 与能力位 `NL_CAP_QUIC=1<<12`。
- **平台**：Linux 全量构建 + ctest 通过；Windows(MinGW) 全部源文件**编译成目标文件 0 error**；macOS 为纯 POSIX（预期可用，未实编）。
  H3 服务端/客户端经**薄可移植层**支持 Windows（Winsock2 socket + `CreateThread`）；Windows **实机运行未验证**（本环境无 Windows mbedTLS / Wine）。
- **范围**：本扩展属 **HTTP/3**；**HTTP/2 与 HTTPS 服务端接线顺延至 2.4.3**，本版不涉及。
- **待办（可选）**：RFC 8448（TLS 1.3 握手轨迹）KAT。

### 变更

#### 反向代理升级为数据驱动异步并行事件引擎 — **Beta**

> ⚠️ **Beta 阶段**：API 与行为可能在后续版本调整；三平台实现已完成并通过语法校验。

- **数据驱动状态表**：Web 反向代理不再为每连接建对象，连接状态编码为
  8 位无状态字节（`pr_state`）+ 按设备动态定容的整数 ring buffer
  （`pr_fd_client` / `pr_fd_upstream` / `pr_route_idx` / `pr_hbuf` /
  `pr_hbuf_out` / `pr_hdr_len` / `pr_remain` / `pr_free`），单线程一次
  泵多个连接，彻底摆脱"并发 ≈ worker 数"瓶颈。
- **混合路由支持**（v2.4.2 新特性）：同一 Web 服务器可同时启用 proxy + static
  + content + redirect + file 路由，引擎内统一处理，消除早期"proxy 路由激活
  后其它路由返回 501 Not Proxy"的限制。
- **I/O 多路复用后端（按平台自动选择）**：
  - Linux：`epoll`（`event.data.u32` 编码 idx + 方向位，`epoll_wait` 1000ms 退避）。
  - macOS：`kqueue`（`kevent.udata` 编码 idx + 方向位，`kevent` 1s 退避）。
  - Windows：`IOCP`（内核完成端口，无 `FD_SETSIZE` 上限），`select` 作为兼容回退。
- **极低占用**：listen / 客户端 / 上游 fd 全非阻塞（`O_NONBLOCK` /
  `ioctlsocket(FIONBIO)`）；空闲退避超时，无连接时近乎零 CPU；`nl_web_start`
  检测到 proxy 路由时**单线程引擎直接拉起、不分配 worker 池**，无 proxy 路由
  回退原 worker 池，行为不变。
- **防半包丢数据**：`pr_remain` 记录每方向"已收待发余量"，先补发上次
  `EAGAIN` / `WSAEWOULDBLOCK` 未发完部分再读新数据。
- **ring 容量按设备动态调整**：三平台统一——`逻辑核数 × 64`（下限 64、上限 4096）。
  Windows 采用 IOCP 完成端口，**不受 `FD_SETSIZE` 限制**，4096 为工程保护值而非 IOCP 硬上限。
- **上游协议分流**：`tcp://` 纯字节透传；`http://` / `https://` 重组
  request line + `Host` / `X-Forwarded-For` / `Connection: close` 头并透传
  原始 header / body。
- **生命周期安全**：`nl_web_stop` / `nl_web_destroy` 增加 pr ring 兜底清理
  （close 残留连接 + 逐字段 free 置 NULL），防 `TerminateThread` 路径泄漏。

#### HTTPS 正式纳入 CMake 构建 + 扩展自动依赖检查 + Lang 软依赖解耦 (v2.4.2)

- **HTTPS 主库独立成** **`netleaf_http`** **目标**：为避免 `netleaf_core → netleaf_tls → netleaf_core`
  循环依赖，将使用 TLS 的 `netleaf_http_<plat>.c` 从核心库剥离，仅在 `BUILD_TLS=ON` 时构建，
  定义 `NL_HTTPS_ENABLE` 并链接 `netleaf_core` + `netleaf_tls` + mbedTLS；`BUILD_TLS=OFF` 时
  不生成该目标，HTTPS API 退化为桩返回 `NL_HTTP_TLS_UNAVAILABLE(-100)`。

- **运行时门控**：三平台 `nl_http_server_enable_tls` / `nl_http_client_connect` 首行加
  `nl_tls_is_available()` 检查，仅 TLS 库存在时才允许 HTTPS。

- **POSIX 特性宏修复**：`-std=c99` 下 glibc `netdb.h` 把 `getaddrinfo/struct addrinfo` 藏在
  `_POSIX_C_SOURCE` 之后，Linux/macOS HTTP 源顶部补 `#define _POSIX_C_SOURCE 200809L` +
  `#define _DEFAULT_SOURCE`，否则 HTTPS 实编报 `storage size of 'hints' isn't known`。

- **扩展加载前自动依赖检查**：`nl_extension_register` 注册阶段自动调用
  `nl_extension_check_dependencies(info, 1)`，必需依赖不满足直接返回 `-2`；`auto_load_extension_file`
  复用该路径，实现「加载前自动校验、缺依赖不加载」。

- **`netleaf_lang`** **软依赖解耦**：`BUILD_LANG` 门控 `add_subdirectory(src/lang)`，各扩展对 lang
  改为「`BUILD_LANG=ON` 才链接」的可选链接；`NL_LANG_ENABLE` 经 lang 目标 PUBLIC 传播，未构建时
  各扩展 lang 调用点退化为 no-op。

#### TLS 扩展拆分为 tls2 / 新增 tls3（mbedTLS 3.x）

- 原 TLS 扩展重命名为 **`netleaf_tls2`**（底层 mbedTLS 2.28.10 LTS，覆盖 TLS 1.0–1.3）；
  新增 **`netleaf_tls3`**（底层 mbedTLS 3.x，覆盖 TLS 1.2/1.3），两库共享模块常量
  `NL_MODULE_TLS = 9` / `NL_CAP_TLS = 1<<11`，宿主在两库共存时优先 `tls3`。
- 语言库 id 由各模块 lang 头本地持有（`NL_LIB_TLS2` / `NL_LIB_TLS3`），不放入全局
  `netleaf_lang.h`；`netleaf_module.h` 中失效的 `NL_MODULE_TLS_INFO` 别名死代码块已移除。
- 新增 `src/tls3/CMakeLists.txt`（`netleaf_tls3` 目标，引入 `third-party/mbedtls3`，
  缺源码时 `FATAL_ERROR` 提示下载 mbedTLS ≥3.6）；顶层 `CMakeLists.txt` 新增 `BUILD_TLS3`
  选项与 `BUILD_TLS` 互斥保护（同名 CMake 目标 `mbedtls/mbedx509/mbedcrypto` 冲突，
  单次构建仅用一个 mbedTLS 版本）；`netleaf_http` 按 `BUILD_TLS3` 选链 `netleaf_tls3`。
- `.gitignore` 新增 `third-party/mbedtls3/`。

#### TLS3 构建验证 + mbedTLS 3.6 API 移植 (v2.4.2 后续补完)

- **http 层 tls3 双后端抽象**：三个 `netleaf_http_{linux,macos,windows}.c` 按
  `NL_HTTPS_USE_TLS3` 宏定义 `NL_HTTPS_TLS_*` 抽象块，切换底层 TLS 后端
  （tls3 / tls2），修复 `NL_HTTPS_TLS_OK` 枚举常量大小写不一致（`nl_tls3_OK` →
  `NL_TLS3_OK`、`nl_tls2_OK` → `NL_TLS2_OK`）。
- **`netleaf_tls3.c` mbedTLS 2.28 → 3.6 移植（12 处）**：
  - `mbedtls_pk_parse_keyfile` 由 3 参扩为 5 参（新增 `f_rng`/`p_rng`）
  - `mbedtls_ssl_cache_init` 由 4 参改为 1 参 + `mbedtls_ssl_cache_set_max_entries`
  - `mbedtls_ssl_conf_session_cache` 参数顺序调整（`p_cache` 前移）
  - `mbedtls_ssl_conf_ciphersuites` 返回 void（去 ret 检查）
  - `mbedtls_net_set_nonblocking` → `mbedtls_net_set_nonblock`（单参数）
  - `mbedtls_ssl_handshake_server`/`_client` → 统一 `mbedtls_ssl_handshake`
  - `mbedtls_ssl_session_id_reset` 已移除，session 复用检测简化为 `reused = 0`
  - `mbedtls_ssl_set_timer` → `mbedtls_ssl_set_timer_cb`
  - `mbedtls_x509_string_subject` 已移除，改用 `mbedtls_x509_dn_gets`
  - `mbedtls_ssl_get_ciphersuite` 返回 `const char*`（直接取名称，去 `list_ciphersuites` 循环）
  - ALPN 参数 `char **` → `(const char **)`
- **mbedTLS v3.6.7 拉取**：`third-party/mbedtls3/` 含 `framework` 子模块
  （mbedTLS 3.4+ 将 framework 拆为独立子模块，浅克隆需 `git submodule update --init`）。
- **CMake 默认 `BUILD_TLS3=ON`**：顶层选项已默认 ON，构建脚本
  `build_all.sh` / `build_macos.sh` 同步 `-DBUILD_TLS3=ON`。
- **编译验证（Windows MinGW64）**：`-G "MinGW Makefiles"` + `gcc 16.1.0`，
  `BUILD_TLS3=ON` 下 `netleaf_tls3` + `netleaf_http` 实编通过，
  产物 `libnetleaf_tls3.dll`（1.0 MB）+ `libnetleaf_http.dll`（79 KB）。

#### v2.4.2 close-bug & epoll 修复（修复与改进计划执行）

- **BUG-002 handler 响应内存约定**（`include/netleaf.h`）：在 `nl_http_handler_t` 上方补充
  所有权约定说明——`*response` 必须堆分配（malloc/calloc/realloc），传字符串字面量 / 静态
  缓冲会在服务端 `free(response)` 时崩溃；无响应时置 `*response = NULL`，附正确示例。
- **BUG-001 HTTP 写完不关 fd → CLOSE-WAIT**：
  - linux（`netleaf_linux.c` write 分支）写完响应后先 `epoll_ctl(EPOLL_CTL_DEL)`（容错
    `EBADF`）再 `close(fd)`；
  - macos（kqueue close 自动 detach）写完直接 `close(fd)`，并补 `n<0` 错误分支；
  - windows 阻塞 accept + IOCP 无事件循环，N/A。
- **BUG-101 忽略 epoll 挂起事件**（linux）：epoll ADD 掩码加入
  `EPOLLRDHUP | EPOLLHUP | EPOLLERR`，并在 handler 前新增挂起检测块
  （命中则 `epoll_ctl(DEL)` + `close(fd)` + `continue`）。
- **BUG-102 close 后再 `epoll_ctl(DEL)` → EBADF**（linux）：`n==0` 与 `n<0` 两条错误路径
  反转为先 `epoll_ctl(EPOLL_CTL_DEL)`（容错 `EBADF`）再 `close(fd)`。
- **BUG-201 UDP handler 缺少对端地址**：新增
  `nl_udp_message_handler_v2` 类型 + `nl_server_set_udp_handler_v2`（三平台 struct 字段与
  setter 已落地），UDP recv 分支用 `inet_ntop` + `ntohs` 暴露 `peer_addr`/`peer_port`，
  未设 v2 时回退 v1。
- **BUG-301 websocket 入库**：`CMakeLists.txt` 已将 `netleaf_websocket_linux.c` 纳入
  `NL_OPTIMIZE_COMMON_SOURCES`，现状已满足，无需改动。
- **BUG-304 编译器告警根除**（`src/windows/netleaf_windows.c` +
  `netleaf_sysinfo_windows.c`）：
  - 根除 `-Wstringop-truncation`（5 + 1 处）：`strncpy` 满容量填充改为
    `snprintf(dst, N, "%s", src)`（必写终止符）或 `memcpy`+`strlen` 有界拷贝；
    `RegQueryValueEx` 改按实读字节数补终止符再 `snprintf`。
  - 根除 `-Wmisleading-indentation`（2 处）：`nl_json_parse`/`nl_toml_parse` 尾部
    同行两 if 拆分。
  - 根除 `-Wunused-function`：删除冗余 `toml_parse_int`（数值解析已走
    `toml_parse_float` + `d == (int64_t)d` 判别）。
  - 根除 `-Wunused-parameter`：`toml_parse_float` 补 `(void)err;`。
  - 保留 3 处良性 `-Wformat-truncation`（路径拼接 / key-value 截断的
    `snprintf`，安全截断无越界）；完整 `-Werror` 可用 `-Wno-format-truncation`
    抑制后启用。

#### 头文件类型冲突修复：`nl_http_method_t` 重定义 (v2.4.2)

- **问题**：`include/netleaf.h` 与 `include/optimize/netleaf_http.h` 各自定义了同名
  类型 `nl_http_method_t`（前者枚举 `NL_METHOD_*`：GET/POST/PUT/DELETE/PATCH/HEAD/OPTIONS；
  后者枚举 `NL_HTTP_*`：GET/POST/PUT/DELETE/HEAD/OPTIONS/PATCH/UNKNOWN）。同一编译单元
  同时包含两个头会触发 `error: conflicting types for 'nl_http_method_t'`；
  README「基本 HTTP 服务器」示例（同时 include 两头）即无法编译。此前仅在
  `netleaf_https_ext.c` 以"不同时包含两头"的方式规避。
- **修复**：将 `include/optimize/netleaf_http.h` 的方法枚举更名为 `nlh_http_method_t`
  （与优化层 `nl_http_*` / `nlh_*` 短名体系一致），`include/netleaf.h` 的
  `nl_http_method_t` 保持不变。同步更新 `nl_http_request_get_method` /
  `nlh_req_method` / `nl_http_parse_method` 的返回类型及 `struct nl_http_request::method`
  字段（`src/http/netleaf_http_internal.h`、`netleaf_http_common.c`、`netleaf_http_shortnames.c`）。
- **兼容性**：使用 `NL_HTTP_*` 常量的用户代码不受影响；仅在源码中显式写
  `nl_http_method_t` 指代优化层枚举的用户需改用 `nlh_http_method_t`。修复后两个头
  可安全同包包含，`netleaf_https_ext.c` 中原"避免重定义"的规避注释同步更正。

#### HTTPS 独立成库 + 三平台公共 HTTP 逻辑去重 + LTO 选项 (v2.4.2 后续)

- **HTTPS 拆出独立库 `netleaf_https`**：将原单一 `netleaf_http` 目标拆为两个 CMake 目标，
  实现"HTTPS 独立成库、HTTP 仍独立可用"：
  - **`netleaf_http`（纯 HTTP，不依赖 TLS）**：编译 `src/http/netleaf_http_common.c` +
    平台 `netleaf_http_<plat>.c`；不再链接 `netleaf_tls3/tls2`/mbedtls，仅链接
    `netleaf_core`（Win32 另加 `ws2_32 bcrypt`，供 QUIC 连接 ID 的 `BCryptGenRandom`）。
  - **`netleaf_https`（HTTPS/TLS，独立库）**：新增
    `src/http/netleaf_http_tls_<plat>.c`（windows/linux/macos 各一），链接
    `netleaf_http` + `netleaf_tls3`(或 tls2) + mbedtls，承载
    `nl_http_server_enable_tls` / `nl_http_client_connect` / `nl_http_client_request`
    / `nl_http_client_close`。
  - **ABI 拆分**：`netleaf_http` 的平台 .c 定义不含 TLS 字段的 `struct nl_http_server`；
    `netleaf_https` 的同名结构含 `tls_cfg`/`tls_enabled` 字段并承载全部 TLS API，
    两库在各自编译/链接边界内自洽。
- **三平台公共 HTTP 逻辑去重**：将 linux/macos/windows 三份重复的 ~40 个纯逻辑函数
  提取到平台无关的 `src/http/netleaf_http_common.c` + 共享头
  `src/http/netleaf_http_internal.h`；平台 .c 经 `nl_http_io_t` 回调表
  （`write_h2_frame` / `udp_send`）解耦平台 socket/pthread，并统一
  `nl_h2_frame_header` 位域。
- **`MAX_PATH` 冲突修复**：内部常量 `MAX_PATH`(4096) 与 Windows 系统
  `MAX_PATH`(260) 冲突，改名 `NL_HTTP_MAX_PATH`。
- **LTO 选项（BUG-303）**：新增 `option(NETLEAF_ENABLE_LTO "..." OFF)`；启用后
  GCC/Clang 仅加 `-flto`（MinGW 另加 `-fno-fat-lto-objects`）、MSVC 加 `/GL /LTCG`，
  **不额外提升优化等级**（不再追加 `-O3` / `/O2 /Ob3`，沿用 CMake 各 build type
  默认优化等级），避免对下游用户产生非预期的等级抬升。
- **编译验证（Windows MinGW64）**：`cmake -G "MinGW Makefiles" -DBUILD_TLS3=ON`，
  生成 `libnetleaf_http.dll`（纯 HTTP，不含 TLS 符号）+ `libnetleaf_https.dll`
  （导出 HTTPS/TLS API），`objdump` 确认符号分布符合预期。
- **性能占用优化（内存/栈）**：
  - hpack 动态表瘦身：`hpack_context.dynamic_table[4096]`（≈4.7 MB，且从未被
    读写）由值内嵌改为指针 + 按需分配，新增 `hpack_free` 并接入
    `h3_connection_free`；每个 H2 线程栈与每条 H3 堆连接各削减 ≈4.7 MB，
    `hpack_init` 不再 memset 4.7 MB。
  - H3 回包栈缓冲复用：`h3_process_stream_frame` 原每请求栈上分配两个
    64 KB 缓冲（共 128 KB），改用连接已有 64 KB 堆 `send_buffer` 复用，
    并对 body 长度加上界检查。
- **性能优化编译验证**：`libnetleaf_http` / `libnetleaf_https` 在 Windows MinGW64
  重新编译通过。

- **HTTP/2 帧协议补全**（`netleaf_http_common.c`）：
  - `HEADERS` 帧检查 `END_HEADERS`(0x04)：置位即解析，未置位累积进
    `pending_headers_block`（`realloc` 增长）等待 `CONTINUATION` 补全。
  - 新增 `CONTINUATION` 帧：按流累积至 `END_HEADERS` 后一次性解析。
  - 新增 `PUSH_PROMISE` 帧：尊重 `enable_push`，读取预留流 ID（含 PRIORITY
    4 字节偏移）创建预留流并解析 promise 头块。
  - 抽出共享 helper `h2_apply_header_block`（HEADERS/CONTINUATION/PUSH_PROMISE
    三处复用 HPACK 解码循环）；`struct nl_http2_connection` 新增
    `pending_headers_block/len/stream` 三字段。
- **QUIC 传输层帧补全**（RFC 9000，`h3_process_stream_frame`）：在 H3 应用帧同一
  switch 处补 `MAX_DATA` / `MAX_STREAM_DATA` / `MAX_STREAMS_BIDI/UNI` / `ACK` /
  `PING` / `CONNECTION_CLOSE` / `HANDSHAKE_DONE`；`h3_process_quic_packet` 短头路径
  改为解析 QUIC STREAM 帧（varint frame_type/length/stream_id/offset + FIN），
  正确分发至传输帧或 H3 应用帧。加密采用可插拔设计：纯 `netleaf_http` 维持
  Initial→ESTABLISHED 演示态机，真实 1-RTT TLS 1.3 握手由 `netleaf_https` 的
  `tls3`（mbedtls）承载。
- **QUIC/H2 补全编译验证**：`libnetleaf_http` / `libnetleaf_https` 在 Windows MinGW64
  `--clean-first` 重编通过，`netleaf_http_common.c` 无新增告警。

#### 平台无关公共工具层 `nl_util` 抽取（公共代码去重，方便维护）

- **新增公共工具模块** `include/nl_util.h` + `src/common/nl_util.c`（编入 `netleaf_core`，
  随核心库导出，供全部扩展与平台实现共享）：
  - 字符串：`nl_strdup`（Windows→`_strdup`/POSIX→`strdup`）、`nl_parse_enable_value`
    （"1/0/true/on/yes/…" 归一为 0/1）、`nl_strncasecmp`、`nl_strncasestr`、
    `nl_tolower`、`nl_string_tolower`。
  - 安全：`nl_sha1`（纯 C，WebSocket `Sec-WebSocket-Accept` 用）、
    `nl_base64_encode_into`（写入调用方缓冲，输出长度已知场景）。
  - 初始化：`nl_once_init` / `nl_once_run`（线程安全一次性守卫，Windows 用
    `INIT_ONCE`、POSIX 用 `pthread_mutex_t`，8 槽固定容量，热路径零锁开销）。
- **补齐既有 `netleaf.h` 声明**：`nl_base64_encode`（堆分配、2 参）与
  `nl_base64_decode`（堆分配）此前在 `netleaf.h` 有声明但无实现，现于 `nl_util.c`
  落地，`nl_base64_encode` 内部复用 `nl_base64_encode_into`；同时新增
  `nl_base64_encode_into` 避免与既有 2 参签名命名冲突。
- **替换各扩展中的重复拷贝**（去重收敛到 `nl_util`）：
  - `src/netleaf_module.c`、`src/vue/netleaf_vue.c`、`src/autoroute/netleaf_autoroute.c`、
    `src/errorpage/netleaf_errorpage.c`、`src/autocomplete/netleaf_autocomplete.c`、
    `src/autocomplete/netleaf_charset.c`、`src/autocomplete/netleaf_autocomplete_vue.c`、
    `src/ipc/netleaf_ipc.c`：移除本地 `#define strdup _strdup`、静态
    `parse_enable_value` / `strncasecmp` / `strcasestr` / `tolower` 拷贝，改调
    `nl_util` 对应函数（公共 API 头文件保留同名薄封装）。
  - `src/{windows,linux,macos}/optimize/netleaf_websocket_*.c`：三平台字节一致的
    `simple_sha1` / `base64_encode` 改为调 `nl_sha1` / `nl_base64_encode_into`。
  - 注：`mqtt`（可选模块，默认 OFF）暂保留本地 `strdup` 宏，控制本轮改动面。
- **编译验证**：Windows MinGW64 `cmake --build` 全目标（`netleaf_core` +
  各扩展 + `netleaf_http`/`netleaf_https`）通过，退出码 0；清理
  `nl_autocomplete_tolower` 未用函数告警。

#### 版本号升级 2.4.1 → 2.4.2

- 主版本号 `NETLEAF_VERSION` 升级为 `2.4.2`，`NL_MODULE_VERSION` 同步为 `2.4.2`。

- 各扩展版本号宏同步：`NL_LANG_VERSION`、`NL_IPC_VERSION`、`NL_LINKAGG_VERSION`、
  `NL_VUE_VERSION`、`NL_AUTOROUTE_VERSION`、`NL_AUTOCOMPLETE_VERSION`、`NL_ERRORPAGE_VERSION`
  均升级为 `2.4.2`。

- CMake `project(... VERSION ...)`、`NETLEAF_VERSION_STRING`、构建脚本 `VERSION` 均同步为 `2.4.2`。

- `build_macos.sh` 打包补 `include/optimize/` 头文件拷贝（`netleaf_http.h`、
  `netleaf_optimize.h`、`netleaf_websocket.h`），x86\_64 与 arm64 两个打包块均已补。

#### 编译验证（WSL Debian + osxcross）

| 平台           | 命令                                                       | 结果                                                             |
| ------------ | -------------------------------------------------------- | -------------------------------------------------------------- |
| Linux 全量     | `cmake -DBUILD_TLS=ON` + `cmake --build . -j4`           | **通过**，生成 `libnetleaf_http.so` + `libnetleaf_tls.so`           |
| Linux 关闭 TLS | `cmake`（默认 `BUILD_TLS=OFF`）                              | **通过**，不生成 `netleaf_http`                                      |
| macOS 全量实编   | `bash build_macos.sh`（osxcross x86\_64 + arm64）          | **通过**，生成 `releases/NetLeaf-2.4.2-macos-{x86_64,arm64}.tar.gz` |
| Windows 静态   | `x86_64-w64-mingw32-gcc -fsyntax-only -DNL_HTTPS_ENABLE` | **通过**                                                         |

#### HTTP 短名转接层 + 动态优化 + QUIC varint 边界修复 + H2/QUIC 回环测试 (v2.4.2 后续)

- **HTTP 短名转接层（`nlh_*`）**：为过长的 HTTP/H2/H3 接口名提供一一对应的
  短名转接层，降低调用冗余：
  - `include/optimize/netleaf_http.h`：全部原长名接口（`nl_http_*` /
    `nl_http2_*` / `nl_http3_*` / `nl_http_client_*` /
    `nl_http_server_enable_tls`）统一标记 `NL_HTTP_DEPRECATED`，调用仍可用
    但产生编译告警；新增一一对应的短名块（`nlh_server_*` / `nlh_h2_*` /
    `nlh_h3_*` / `nlh_req_*` / `nlh_resp_*`，TLS 门控下另含
    `nlh_server_enable_tls` / `nlh_client_*`），无告警，为推荐用法。
  - `NL_HTTP_DEPRECATED` 跨平台宏：MSVC/MinGW 走 `__declspec(deprecated)`，
    GCC/Clang 走 `__attribute__((deprecated))`，其余编译器空宏。
  - **内部构建免告警**：`NL_HTTP_INTERNAL_BUILD` 守卫——库自身 `.c`
    （`netleaf_http_shortnames.c` 及三平台 / tls 实现）在 include 公共头前
    `#define NL_HTTP_INTERNAL_BUILD`，将 `NL_HTTP_DEPRECATED` 置空，仅对
    外部用户调用产生告警，消除库内部自触发告警噪声。
  - `src/http/netleaf_http_shortnames.c`：实现全部 `nlh_*` 转发器（仅
    转发到原长名，不改变 ABI），随核心库 `netleaf_core` 编译导出；
    TLS 门控短名仅在 `NL_HTTPS_ENABLE` 构建（`netleaf_https` 目标）实现。

- **动态/性能优化（栈缓冲堆化）**：继续"能动态的都动态"，且**不提升
  优化等级**（沿用 `-O2`，未追加 `-O3` / `/Ob3`）：
  - `h2_send_headers`：原两个栈上 H2 帧缓冲（HEADERS + DATA，共
    `2×(9 + H2_MAX_FRAME_SIZE)` 字节）合并为单个 `malloc` 堆缓冲，
    削减大帧下的栈占用；写帧长度改用 `H2_MAX_FRAME_SIZE` 常量
    （修复原 `sizeof(frame)` 误用指针尺寸问题）。
  - `h3_process_stream_frame` Initial 响应：每请求栈上 64 KB 双缓冲改为
    复用连接堆 `send_buffer`，body 长度加上界检查。

- **QUIC varint 编码边界修复（真实 BUG）**：`quic_write_varint` 的 8 字节
  档原先未设上界，`value ≥ 2^62` 时落入 8 字节档会溢出 62 位表示域
  （QUIC varint 实际可表示 `0..2^62-1`，prefix=3 占用 2 位）。修复为
  `value < (1ULL << 62) && len >= 8`，超范围值返回 0（失败）。

- **H2/QUIC 端对端回环回归测试 `test_http_frames`**：
  - 新增 `test/test_http_frames.c`（CMake 注册 `test_http_frames` +
    `add_test(HttpFramesTest)`），直接 `#include "netleaf_http_common.c"`
    自包含纯逻辑；覆盖 H2 帧编解码回环（SETTINGS/HEADERS/DATA/PING/
    GOAWAY/WINDOW_UPDATE/CONTINUATION/PUSH_PROMISE）、QUIC varint
    多值回环与 `2^62` 超范围边界、以及 `nlh_*` 短名与原长名访问器
    一致性（空数组合法、`2^62` 应失败）。
  - 补 `quic_generate_conn_id` 平台桩（该函数定义于平台 .c，非公共层）。
  - 运行结果：**16/16 通过，0 失败**。

- **编译/测试验证（Windows MinGW64）**：
  - `cmake --build build-tls3-new --target netleaf_core netleaf_https -j4`
    通过（exit 0），内部 deprecated 告警已清零。
  - `ctest`：`HttpFramesTest` / `MqttFramesTest` / `VariableSubstitutionTest`
    通过（3 通过）；`test_modules` / `test_full` / `test_all` 因 MQTT
    未纳入该构建配置而"未运行"（既有 `-lnetleaf_mqtt` 链接缺失，与本
    轮改动无关）。

#### MQTT 默认开启 + 测试 CMake 拆分 + 文档 Wiki 补全 (v2.4.2 后续)

- **MQTT 默认开启**：`BUILD_MQTT` 与 `BUILD_MQTT_SERVER` 的 `option(...)`
  默认值从 `OFF` 改为 `ON`。构建脚本（`build_all.sh` / `build_macos.sh`）
  原本即显式传 `-DBUILD_MQTT=ON`，此变更仅使默认 CMake 配置与脚本行为
  一致，消除 `test_modules` / `test_full` / `test_all` 因 MQTT 库缺失而
  "未运行"的问题。

- **测试 CMake 拆分至 `test/CMakeLists.txt`**：
  - 将顶层 `CMakeLists.txt` 中 70 余行的测试注册块（`test_modules`、
    `test_full`、`test_variable_substitution`、`test_all`、
    `test_mqtt_frames`、`test_http_frames`、`test_mqtt_server_loopback`）
    整体迁移至 `test/CMakeLists.txt`，顶层改为
    `add_subdirectory(test)`。
  - `test/CMakeLists.txt` 引入 `NL_TEST_MQTT_LINKS` 变量
    （`$<$<BOOL:${BUILD_MQTT}>:netleaf_mqtt>` 等 generator expression），
    按 `BUILD_MQTT` / `BUILD_MQTT_SERVER` 开关动态裁剪
    `test_modules` / `test_full` / `test_all` 的链接列表，彻底消除
    未开启 MQTT 时 `-lnetleaf_mqtt` 链接缺失问题。
  - 验证：`BUILD_MQTT=ON + BUILD_MQTT_SERVER=ON + BUILD_TESTS=ON`
    构建后 `ctest` **7/7 全部通过**（含 `MqttServerLoopbackTest`，
    26.72s）。

- **文档 / Wiki 补全**：
  - `docs/extension_showcase_api_guide.md`：版本号 2.4.1 → 2.4.2；
    补充 `nl_extension_register` `-2` 返回码、
    `nl_extension_clear_metadata()`、`nl_module_get_platforms()`、
    自动加载符号发现约定（已知符号表 + 文件名推导 +
    通用入口 `nl_get_extension_info`）。
  - `docs/extension_tutorial.md`：版本号 2.4.1 → 2.4.2；修正
    §19 断链（`examples/plugin_template` → `examples/plugin_example`）；
    补充线程安全说明（`registry_mutex` / `metadata_mutex`）、
    `NL_PLUGIN_EXPORT` 宏。
  - `docs/plugin_development.md`：版本号 2.4.0 → 2.4.2；
    新增「插件描述符」章节，覆盖
    `nl_plugin_get_descriptor` / `nl_plugin_get_descriptor_into`、
    `nl_plugin_get_all` free 契约、`nl_plugin_discover` 副作用、
    `nl_plugin_discover_all` 澄清、`nl_plugin_search` 说明、
   - 事件订阅上限 16、`nl_plugin_enable_sandbox` 占位标注、
    `nl_plugin_validate` 校验内容。

#### H1 核心服务端线程池（多用户并发优化）(v2.4.2 后续)

- **可配置线程池工作模型**：将核心 `nl_server_*` TCP/HTTP/WebSocket 服务由
  「单线程事件循环」升级为「监听线程（accept 循环）+ N 个 worker 线程」，
  支持多用户并发请求；UDP 路径保持单线程（`recvfrom` 单线程）。
- **新增公开 API**（落到全部 3 个平台）：
  - `nl_socket_option_t` 新增枚举值 `NL_OPT_CONCURRENCY`：worker 池大小
    （合法范围 1..64，默认 4，仅 TCP 有效）。
  - 便捷函数 `int nl_server_set_concurrency(nl_server_t* server, int concurrency)`
    （`<=0` 或 `>64` 返回 `NL_EINVAL`）；同步在 `nl_server_set_option` /
    `nl_server_get_option` 增加 `case NL_OPT_CONCURRENCY`。
- **线程池设计**：
  - 监听线程 accept 成功后将 fd 放入带锁 FIFO 环形队列（容量
    `MAX_WORKER_QUEUE 1024`）；队列满则阻塞等待 `queue_not_full` 条件变量，
    worker 空闲则等待 `queue_not_empty`。
  - worker 读取模型：`poll()` 带 30s 空闲超时（`idle_ms=30000`）的
    keep-alive 读 → `parse_http_request` → `handler` → 写回响应 → `nl_buffer_clear`；
    读 0 或 idle 超时则退出。
  - 每连接读写线程安全：复用 `nl_buffer_t` 自带 mutex（POSIX
    `pthread_mutex_t`、Windows `CRITICAL_SECTION`）。
  - 平台同步原语：POSIX（Linux/macOS）`pthread_t*` + `pthread_mutex_t` +
    两个 `pthread_cond_t`；Windows `HANDLE*`（`CreateThread`）+
    `CRITICAL_SECTION` + 两个自动复位事件 + `WaitForSingleObject`。
  - 优雅停机 join 顺序：`running=0` → close 队列中未处理 fd →
    broadcast/SetEvent → join 所有 worker → 释放队列/句柄 → close listen fd
    → join listener 线程。
- **变更文件**：`include/netleaf.h`、`src/linux/netleaf_linux.c`、
  `src/macos/netleaf_macos.c`、`src/windows/netleaf_windows.c`（Windows 文件
  补齐缺失的 `parse_http_request`）。
- **编译 / 测试验证**：
  - Linux（WSL Debian gcc）：`libnetleaf.so` 实编通过，`nm -D` 确认
    `nl_server_set_concurrency` / `nl_server_start` / `nl_server_stop` 导出；
    并发冒烟测试 32/32（16 worker）成功；`set_concurrency(0)`/`(99)` 正确
    返回 `NL_EINVAL(-10)`。
  - Windows（LLVM-MinGW Clang/MinGW）：`libnetleaf.dll` 实编通过，符号导出
    确认；`build_all-Clang.bat x64` 成功。
  - macOS（osxcross x86_64 + arm64）：实编通过，生成
    `releases/NetLeaf-2.4.2-macos-{x86_64,arm64}.tar.gz`。
  - `ctest`（`build_x64`，Release）：**7/7 全部通过**（含
    `MqttServerLoopbackTest` 27.68s），线程池改动无回归。
- **构建脚本版本号统一**：4 个一键构建脚本（`build_all-MSVC.bat` /
  `build_all-Clang.bat` / `build_all.sh` / `build_macos.sh`）`VERSION` 均确认为
  `2.4.2`；各脚本按平台边界运行（Windows 脚本仅 Windows，Linux/macOS 在 WSL
  内分别跑 `build_all.sh` / `build_macos.sh`）。
  - `build_all.sh`（Linux，WSL）：13 架构（amd64/i686/arm/arm64/riscv64/
    powerpc/powerpc64/powerpc64le/mips/mipsel/mips64/mips64el/s390x）全部
    实编通过（含 amd64 原生 + TLS3/HTTPS）。
  - `build_macos.sh`（macOS，WSL osxcross）：x86_64 + arm64 实编通过。
  - `build_all-Clang.bat`（Windows，LLVM-MinGW）：x64 实编通过；WSL 段因
    发行版非 Ubuntu 按设计安全跳过。

***

#### Android（移动端）构建支持 (v2.4.2 后续)

- **CMake Android 平台分支**：`CMakeLists.txt` 新增 `ANDROID` 平台识别（`CMAKE_SYSTEM_NAME=Android`），
  复用 `src/linux/` 源文件（Bionic 兼容），链接策略按 Bionic 调整——`dl` / `iconv` 已并入 libc，
  移除这两个链接项并追加 `log`（`__android_log`）；核心库 `netleaf_core` 统一由该分支覆盖。
- **Bionic 兼容修复**：
  - `src/linux/netleaf_sysinfo_linux.c`：glibc 版本探测在 Bionic 下无 `__GLIBC__` /
    `__GLIBC_MINOR__` 宏，改用 `__ANDROID__` 分支输出 `Bionic (NDK)`，否则链接期告警。
  - `src/linux/netleaf_http.c`：`getrandom()` / `<sys/random.h>` 在 Bionic 不可用，
    Android 分支改用 `arc4random_buf` 等价实现，避免编译失败。
  - `flock` / `epoll_create1` 等 POSIX 扩展经核对在 NDK Bionic 可用，无需降级。
- **双构建脚本**：
  - `build_android.sh`（Linux / WSL）：自动探测 WSL `/mnt/e` 下的 NDK，驱动交叉编译。
  - `build_android.bat`（Windows）：按 `ANDROID_NDK_HOME` → 环境变量 → 常见路径顺序定位 NDK，
    支持 `aarch64 / armeabi-v7a / x86_64 / all` 三 ABI，自适应 `ninja / nmake / make`。
  - 默认参数：`minSdk 21`、`c++_shared`、NEON 开启、`Release`。
- **文档**：新增 `docs/android_build.md`（NDK 安装、三 ABI 矩阵、Android Studio jniLibs 集成、
  Bionic 兼容性说明、llvm-readelf 验证方法）。
- **范围**：Android 走「复用 `src/linux/` + 链接项调整 + 两处 Bionic 宏修复」路径，
  未新增独立 `src/android/` 目录；HTTPS / TLS3 等扩展按需经 CMake 开关纳入。
  **扩展系统（NL Extension）暂不支持 Android 运行时**，但其源码与构建配置完整保留，
  后续按扩展逐个跟进。当前 Android 覆盖范围为**核心库**（`netleaf_core`），
  各扩展共享库在 Android 目标下仍按 Linux 语义构建。

***

## [2.4.1](https://github.com/508364/NetLeaf/compare/v2.4.0...v2.4.1) - 2026-09-24

### 变更

#### Lang：错误码接入多语言 + 变量系统支持动态/外部变量 (v2.4.1)

- 新增表驱动错误注册基础设施：`NL_ERROR_BEGIN / NL_ERROR / NL_ERROR_END` 宏与
  `nl_lang_register_errors()` / `nl_lang_register_errors_ex()`，一次注册中英双语错误表

- 让此前"只声明、未生效"的模块真正接入 Lang：autoroute / autocomplete / errorpage / ipc
  （补充初始化注册调用与对 `netleaf_lang` 的链接依赖）；连同 linkagg / tls / mqtt / mqtt\_server，
  各模块返回码/错误码均纳入多语言

- 变量增强（动态变量与外部变量）：

  - `nl_lang_var_set_provider()` / `nl_lang_var_is_dynamic()`：provider 按需计算变量值（锁外调用）

  - `nl_lang_var_bind_env()`：变量绑定环境变量，访问时实时读取

  - `nl_lang_var_load_env()` / `nl_lang_var_load_file()`：批量导入环境变量与 .env 文件

  - `{{name}}` 与 `{{<var>name</var>}}` 替换、`get_int/float/bool` 均支持动态/外部变量

#### TLS 扩展：迁移到 mbedTLS 2.28.10 LTS，支持 TLS 1.0 - 1.3

- 内置 mbedTLS 由 3.5.2 更换为 **2.28.10 LTS**，以真正支持 **TLS 1.0 / 1.1 / 1.2 / 1.3**
  （mbedTLS 3.x 已从协议内核移除 TLS 1.0/1.1，无法通过配置开启）

- `src/tls/netleaf_tls.c`、`src/mqtt/netleaf_mqtt_tls.c` 适配 2.28 API：
  协议版本改用 `MBEDTLS_SSL_MINOR_VERSION_1..4` 映射、SSL 错误码调整、
  `mbedtls_pk_parse_keyfile` 三参签名、`nl_tls_is_active` 改用 `MBEDTLS_SSL_HANDSHAKE_OVER`、
  `nl_tls_handshake` 使用 `net_context.fd` 绑定套接字

- 构建仅生成 mbedTLS 静态库，并在构建目录生成用户配置以启用
  `MBEDTLS_SSL_PROTO_TLS1_3_EXPERIMENTAL`

- `nl_tls_config_t` 的 `min_proto` / `max_proto` 现覆盖 TLS 1.0 - TLS 1.3 全区间

### 修复

#### Windows 多架构构建修复 (LLVM-MinGW / Clang)

- 移除构建脚本中不被 MinGW 支持的 `-target` 编译参数（`build_all-Clang.bat`、`build_tls.bat`）

- 清理源码根目录残留的 in-source 构建产物（`CMakeCache.txt`、`CMakeFiles/`、`Makefile`、
  `bin/`、`lib/`、`mbedtls/` 等），并在 `.gitignore` 中忽略以防误提交

- 修正 CMake 关闭 mbedTLS 测试套件的配置（改用 CACHE FORCE，不再编译 mbedTLS 自带测试）

- TLS 扩展补充链接 `netleaf_lang`；MQTT 直接链接 mbedTLS 库，解决未定义符号

- 适配 mbedTLS 2.28 在 CMake 4.0 下的兼容性（`CMAKE_POLICY_VERSION_MINIMUM=3.5`）

- 修复 mbedTLS 2.28 在 ARM64/Windows 上 `timing.c` 的 `gettimeofday` 未声明问题

- 修复 `netleaf_mqtt.c` 中 `strndup` 与 UCRT 头文件冲突，以及 Windows 下 `EAGAIN` 未定义

- 修复 `test_variable_substitution.c` 中 GCC 专属嵌套函数（Clang 不兼容）

- 更新 `test_all.c` 版本断言以对照 `NETLEAF_VERSION_*` 宏

- `include/netleaf.h` 补全 JSON/TOML/Encoding/System Info/Web/Lazy/版本 等
  已实现但未声明的公共 API

#### 编译告警清理与健壮性修复 (v2.4.1)

- 修复 Linux 上 `strdup` 隐式声明（指针截断隐患）：`src/netleaf_module.c` 顶部 `_GNU_SOURCE`

- 修复 `vue` 内联模板格式串中裸 `%` 导致的非法转换符（未知行为）

- 初始化 MQTT 报文 ID 变量（`netleaf_mqtt.c` 6 处 `pid = 0`）

- 检查 `fread`/`write` 返回值（`netleaf_vue.c`、`src/linux/netleaf_linux.c`）

- 为 `netleaf_mqtt` 目标定义 `NL_MQTT_TLS_STATIC`，消除同 DLL 内 dllimport（LNK4217）

- 消除大量编译告警：`NL_EXTENSION_DEFINE` 多余分号、文件末尾缺换行、winsock2 顺序、
  `DWORD` 用 `%d`、`int main()` 无原型、`_environ` 重声明等

### 构建验证

- Windows x64 / i686 / arm64 三架构全部构建成功

- 一键脚本（`build_all-Clang.bat`）完成全平台交叉编译并打包：
  Windows(x64/x86/arm64)、Linux(amd64/i686/arm/arm64/mips/mipsel/mips64/mips64el/
  powerpc/powerpc64/powerpc64le/riscv64/s390x)、macOS(x86\_64/arm64) 全部成功

- 版本号统一为 `2.4.1`（含 `build_macos.sh`、`build_all-MSVC.bat` 修正）

- TLS 配置探针确认 TLS 1.0 / 1.1 / 1.2 / 1.3 均已启用

- 四个测试程序（test\_all / test\_full / test\_modules / test\_variable\_substitution）全部通过

### 安全提示

- TLS 1.0 / 1.1 已被 RFC 8996 废弃且不再安全，仅建议用于兼容老旧对端；
  生产环境建议将最小版本设置为 TLS 1.2 及以上

### 文档

- 在 `README.md` / `README_EN.md` / `wiki`（Home、Features、TLS）中补充安全说明：
  **不推荐使用 TLS 1.0 / 1.1**，并说明 NetLeaf 的 TLS 能力由**独立的 TLS 扩展库**（`netleaf_tls`）负责；
  如确需使用请将 `min_proto` 设置为 `NL_TLS_PROTO_TLS1_2` 或更高

- 将文档"架构说明"中的字符画（ASCII 图）改为 **Mermaid 图表**，便于阅读与维护
  （涉及 `README.md`、`wiki/TLS.md`、`wiki/Features.md`、`wiki/LinkAgg.md`）

## [2.4.0](https://github.com/508364/NetLeaf/compare/v2.2.2-17891424...v2.4.0) - 2026-09-19

### 新增功能

#### TLS/SSL 内置扩展 (v2.4.0)

**mbedTLS 集成：**

- 创建 `include/netleaf_tls.h` - TLS/SSL 扩展头文件

- 创建 `src/tls/netleaf_tls.c` - TLS 基础实现

- 创建 `src/mqtt/netleaf_mqtt_tls.c` - MQTT TLS 集成实现

- 内置 mbedTLS 源码于 `third-party/mbedTLS/`

- 支持 TLS 1.0 - TLS 1.3 协议版本

- 完整的证书管理：CA 证书、客户端证书、私钥

- 支持证书验证和 peer 认证

- 随机数生成器（entropy + CTR-DRBG）

- 创建 TLS 构建脚本：`build_tls.bat` (Windows) 和 `build_tls.sh` (Linux/macOS)

**API 函数：**

- `nl_tls_create()` / `nl_tls_destroy()` - 创建/销毁 TLS 上下文

- `nl_tls_configure()` - 配置证书和选项

- `nl_tls_set_verify()` - 设置验证选项

- `nl_tls_handshake()` - 执行 TLS 握手

- `nl_tls_send()` / `nl_tls_recv()` - TLS 收发数据

- `nl_tls_close()` / `nl_tls_is_active()` - 连接管理

- `nl_tls_get_peer_cert()` - 获取对端证书信息

- `nl_tls_strerror()` - 错误信息

- `nl_tls_init()` / `nl_tls_version()` / `nl_tls_is_available()` - 模块信息

**MQTT TLS 集成：**

- `nl_mqtt_tls_create()` / `nl_mqtt_tls_destroy()`

- `nl_mqtt_tls_configure()` / `nl_mqtt_tls_handshake()`

- `nl_mqtt_tls_send()` / `nl_mqtt_tls_recv()`

- `nl_mqtt_tls_close()` / `nl_mqtt_tls_is_active()`

- `nl_mqtt_tls_strerror()`

**构建系统更新：**

- CMakeLists.txt 支持内置 mbedTLS 编译

- `BUILD_TLS=ON` 构建选项

- `BUILD_MQTT=ON` 同时启用 MQTT + TLS

- 自动链接 mbedtls、mbedx509、mbedcrypto 库

**开源项目来源：**

- mbedTLS（当前内置 2.28.10 LTS）: <https://github.com/Mbed-TLS/mbedTLS> (Apache 2.0 / GPL v2.0)

- MQTT Specification: <https://mqtt.org/> (EPL-2.0 / EDL 1.0)

- Paho MQTT C: <https://github.com/eclipse/paho.mqtt.c> (EPL-2.0 / EDL 1.0)

#### MQTT 模块增强（v2.4.0）:

#### Lang 模块增强（v2.4.0）

**变量替换系统：**

- `nl_lang_var_set/set_int/set_float/set_bool` — 设置字符串/整数/浮点/布尔变量

- `nl_lang_var_get/get_int/get_float/get_bool` — 获取变量值

- `nl_lang_var_exists/remove/clear_all` — 查询、删除、清空所有变量

- `nl_lang_var_replace` — 模板替换，支持 `{{VAR_NAME}}` 语法，自动 trim 空白

- 最多支持 128 个并发变量，线程安全（mutex 保护）

**条件表达式求值：**

- `nl_lang_var_condition_eval` — 支持 `== != >= <= > <` 运算符

- 兼容数值比较和字符串比较（自动检测类型）

- 支持字面量（数字、单引号字符串、`true`/`false`/`null`）和变量引用

**脚本引擎回调接口：**

- `nl_lang_register_script_engine` / `nl_lang_unregister_script_engine` — 注册/注销脚本引擎

- `nl_lang_execute_script` — 执行脚本并返回结果

- `nl_lang_get_script_engines` — 获取已注册引擎列表

- 支持最多 8 个脚本引擎，便于后续接入 Lua/Python/JavaScript

**版本与能力更新：**

- Lang 版本号更新为 `2.4.0`

- 新增能力标志：`NL_LANG_CAP_VARIABLES (1<<4)`, `NL_LANG_CAP_SCRIPTING (1<<5)`, `NL_LANG_CAP_CONDITIONALS (1<<6)`

- 模块描述更新："Multi-language error message translation with variable substitution and scripting"

#### NL扩展系统全面增强 (v2.4.0)

**扩展独立化重构：**

- 所有扩展改为独立的动态库文件，支持运行时热加载和卸载

- 统一命名规范：`get_extension_info()` 函数替代旧式符号查找

- 扩展自动发现机制优化，支持标准符号名和旧版兼容名

**扩展生命周期管理 API（新增）：**

- `nl_extension_init()` / `nl_extension_shutdown()` - 单个扩展的初始化和关闭

- `nl_extension_force_shutdown()` - 强制关闭扩展（跳过安全检查）

- `nl_extension_is_initialized()` / `nl_extension_is_running()` - 状态查询

- `nl_extension_get_state()` - 获取完整模块状态

**扩展信息查询 API（新增）：**

- `nl_extension_get_name()` / `nl_extension_get_author()` / `nl_extension_get_description()` - 便捷信息获取

- `nl_extension_get_caps()` / `nl_extension_supports_platform()` - 能力和平台查询

**批量操作 API（新增）：**

- `nl_extension_init_all()` / `nl_extension_shutdown_all()` / `nl_extension_force_shutdown_all()` - 批量生命周期管理

**扩展搜索 API（新增）：**

- `nl_extension_find_by_capability()` - 按能力筛选扩展

- `nl_extension_find_by_platform()` - 按平台筛选扩展

- `nl_extension_find_by_name_pattern()` - 按名称模式匹配

**热重载支持：**

- `nl_extension_reload()` - 重载单个扩展

- `nl_extension_reload_all()` - 重载所有扩展

**元数据管理：**

- `nl_extension_get_metadata()` / `nl_extension_set_metadata()` - 扩展元数据的读写

### 修改

#### 版本号更新

- 主版本号和核心库版本更新为 `2.4.0`

- 所有扩展模块版本同步更新为 `2.4.0`

- 更新所有头文件和源文件的版本注释

#### 头文件更新

- `netleaf_module.h`: 新增 v2.4.0 扩展管理 API 声明，完整生命周期管理、搜索、元数据接口

- `netleaf.h`: 更新版本号宏，添加 `NETLEAF_VERSION_MAJOR/MINOR/PATCH`

- 所有扩展头文件版本统一更新为 `2.4.0`

### 移除

- 无移除项

### 修复

- 修复 `strings.h` 头文件包含错误（原为 `strings.b`）

***

## [2.2.2-17891424](https://github.com/508364/NetLeaf/compare/v2.2.2...v2.2.2-17891424) - 2026-09-12

### 新增功能

#### NL扩展系统正式命名

- 将"动态扩展功能"正式命名为 **NL扩展系统** (NL Extension System)

- 更新所有相关文档和注释

- 新增 `NL_SYSTEM_NAME` 宏定义

#### MQTT 完整协议支持

- 添加 `netleaf_mqtt.h` 头文件（MQTT v3.1.1 完整协议）

- 添加 `src/mqtt/` 模块实现目录

- 支持 QoS 0/1/2 消息质量等级

- 支持主题通配符 (`+`, `#`)

- 支持 CONNECT/PUBLISH/SUBSCRIBE/UNSUBSCRIBE 等全部 14 种消息类型

- 支持遗嘱消息 (Last Will)

- 支持保留消息 (Retained Messages)

#### 扩展系统健壮性增强

- 新增 `NL_CAP_EXT_SYSTEM` 能力标志

- 所有模块宏自动附加 `NL_CAP_EXT_SYSTEM` 标志

- 扩展描述长度验证（最多 50 个中文字符）

- 平台字符串解析增强（大小写不敏感）

- 自动分配 `library_value`（从 1 开始，0 保留）

### 修改

#### 版本更新

- 版本号从 `2.2.2` 更新为 `2.2.2-17891424`

- 更新 `NL_MODULE_VERSION` 宏

- 更新所有头文件和源文件的版本注释

#### 头文件更新

- `netleaf_module.h`: 重写文档，标注为 NL扩展系统

- `netleaf.h`: 简化 API，添加 MQTT 基础类型定义

- 新增 `NL_MODULE_MQTT` 模块类型

- 新增 `NL_MODULE_MQTT_INFO` 宏定义

#### 构建系统更新

- 更新 CMakeLists.txt 版本为 `2.2.2-17891424`

- 添加 MQTT 模块构建选项 `BUILD_MQTT`

- 更新构建输出路径配置

### 移除

- 无移除项

### 修复

- 修复 MQTT 模块链接错误（移除对未导出符号的引用）

- 修复构建配置中的依赖链接问题

## [2.2.2](https://github.com/508364/NetLeaf/compare/v2.2.1...v2.2.2) - 2026-09-09

### 新增功能

#### 跨平台构建优化

- 支持 Windows (x86\_64, i686, arm64)

- 支持 Linux (x86\_64, arm64)

- 支持 macOS (x86\_64, arm64)

#### 模块化架构

- 将 IPC、LinkAgg 等功能移至独立模块

- 实现延迟加载机制

- 添加模块依赖管理

### 修改

#### 文件重命名

- `libnetleaf.dll` → `libnetleaf_core.dll` (内部)

- `libnetleaf.exe` → `libnetleaf_core.exe` (测试程序)

#### 构建配置

- 更新 CMakeLists.txt

- 优化编译器标志

- 添加平台特定源文件

## [2.2.1](https://github.com/508364/NetLeaf/compare/v2.2.0...v2.2.1) - 2026-09-01

### 修复

- 修复 Windows 下链接器错误

- 修复 macOS 编译警告

- 修复 Linux 构建脚本

## [2.2.0](https://github.com/508364/NetLeaf/compare/v2.1.0...v2.2.0) - 2026-08-25

### 新增功能

- 添加 WebSocket 服务端和客户端支持

- 添加 HTTP 服务器路由修正功能

- 支持 Windows/MacOS/Linux 三平台构建

### 修改

- 重构网络层抽象

- 优化事件循环性能

- 改进错误处理机制

## [2.1.0](https://github.com/508364/NetLeaf/compare/v2.0.0...v2.1.0) - 2026-08-20

### 新增功能

- 添加智能路由修正功能

- 添加路由匹配建议

- 支持路由参数自动解析

### 修改

- 重构路由引擎

- 优化正则表达式性能

- 改进错误页面生成

## [2.0.0](https://github.com/508364/NetLeaf/compare/v1.0.0...v2.0.0) - 2026-08-15

### 重大变更

- 重构为模块化架构

- 分离核心库与功能模块

- 添加动态扩展系统

- 引入异步 I/O 模型

### 新增功能

- 多语言错误消息支持

- Vue.js 后端渲染支持

- 自定义错误页模板

- 进程间通信 (IPC)

- 链接聚合与负载均衡

### 移除

- 移除旧的同步 API

- 移除单线程模型支持

## [1.0.0](https://github.com/508364/NetLeaf/releases/tag/v1.0.0) - 2026-07-01

### 初始版本

- 基础 TCP/UDP 服务器

- 基础 HTTP 处理

- 简单的路由系统

- Windows 平台支持

***

**注意**: 从 v2.0.0 开始采用语义化版本控制， breaking changes 会在主版本号升级时出现。

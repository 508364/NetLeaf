# NetLeaf 版本历史

## v3.1.0-11/28-2b（2026-11-28）

### HTTP TRACE / CONNECT 方法支持

- 核心枚举 `nl_http_method_t` 新增 `NL_METHOD_TRACE` / `NL_METHOD_CONNECT`，优化层 `nlh_http_method_t` 新增 `NL_HTTP_TRACE` / `NL_HTTP_CONNECT`，`nl_http_parse_method()` 识别两种方法；三平台 legacy 路径 method 字符串解析同步补齐（macOS 顺带补齐缺失的 HEAD/OPTIONS）
- **TRACE 回显（RFC 9110 §9.3.1）**：引擎在 body 剥离**之前**取原始请求字节（剥离会 memmove 掉头块），构造 `200 OK` + `Content-Type: message/http`，body = 请求行+头，`Connection` 随状态机 KA 位；发送后走统一收口，TRACE 后同连接可继续复用；本地路由 404 时按路径正常拒绝
- **CONNECT 真隧道（方案A + 白名单）**：新增 `nl_web_connect_allow(server, host, port)`（最多 16 条、port=0 任意端口、**空名单默认拒绝全部**）；CONNECT 分支插在路由查找之前（`host:port` 查表必 404），按白名单校验（不区分大小写）→ 403；建连失败 502；成功回 `200 Connection Established` 后进入原始字节双向泵（复用现有隧道机），隧道结束双端 close 回收
- 测试：新增用例 o（TRACE 200/message/http/回显请求行与自定义头/keep-alive 复用，8 项断言）、用例 p（本地 echo 服务器隧道往返 + 白名单 403 拒绝）

### WSS（WebSocket over TLS）

- 新增 `nl_ws_server_enable_tls(server, cert_file, key_file)`：独立 WS 服务器可选 TLS（PEM 路径由调用方传入，不硬编码密钥）；未 enable 时与明文行为完全一致
- 新增平台无关运行时后端 `src/common/nl_ws_tls.c/.h`：核心库不直接链接 TLS 扩展（tls3/tls2 反向链接核心，硬链会循环依赖/Windows 构建死锁），改为 dlopen/LoadLibrary 加载 `netleaf_tls3`/`netleaf_tls2` 并解析符号（tls3 优先回退 tls2）；每连接一个服务端 TLS ctx（10s 握手超时），统一 `nl_ws_io_recv/send` 在 conn==NULL 时走明文
- 三平台 WS 线程接线：accept 后 TLS 握手 → HTTP 升级握手 → frame 读写全部走 TLS，关闭前释放 ctx（明文为 no-op）；mbedTLS 单次写 16KB 上限在发送侧循环处理
- 顺带修复 macOS WS 客户端线程野指针（原把已 free 的 fd 指针强转 server），改用 `ws_thread_arg_t` 传递 server+fd（与 Linux 对齐）
- 实测：Linux 自签证书 + `openssl s_client` TLS 握手成功；明文路径 ctest 回归无变化

### 内存优化

- 核实三平台 `pr_free_slot` 回收本就齐全、空载无每槽预分配、`pr_hbuf_out` 本就按需——非泄漏
- **`pr_outbuf` 8KB**：accept 处预分配改为本地路由构造响应时首次懒分配（Linux/macOS）
- **`pr_up_buf` 64KB**：proxy 分发处预分配改为首次收到上游数据时才分配（Linux/macOS）——上游未响应的连接（含 HTTPS 隧道等待期）从 ~72KB/条 降至 ~16KB/条
- 实测（Linux）：空载 1.9MB → 1.79MB；2000 连接 5776KB → 5716KB（本地路由场景差异小，收益主要在 proxy 场景）

### TCP/UDP 通用服务器性能

- **TCP_NODELAY**：Linux/macOS `nl_server` accept 后（worker 池与 legacy 单线程两路径）补设，小响应不再受 Nagle 延迟；Windows AcceptEx 路径已有
- **每连接 malloc 消除**：Linux/macOS worker 路径请求缓冲改 worker 线程级复用、legacy 路径改 listener 线程复用（顺带删除 Linux 死代码缓冲）；Windows UDP 16 槽接收环每包 `malloc/free` 改栈缓冲，消除分配失败丢包分支
- worker 污锁经核实已是最短临界区（入队/出队各一次、处理期不持锁），不合并（有丢唤醒风险）；nl_server 无静态文件 read+send 路径，sendfile 不适用

### Linux 编译警告清理

- `nl_px_local.c` const 丢弃（原地规范化确认可写后强转）；`netleaf_linux.c` `nl_web_open_listen` 第 5 参误传 `&opt` 指针（**真 bug**，改传值）；toml 序列化 switch 补 `NL_TOML_NULL`/default
- `nl_px_h2.c` 4 处 `%s` 格式截断改 `strnlen+memcpy` 有界拷贝；H2 preface 字面量 25B 塞 24B 数组改尺寸 +1；路径/上游拼接 3 处改手写有界拼接（放不下走 404/截断）
- Windows `__declspec(thread)` 在 MinGW 下告警 → `NL_WIN_TLS` 宏按 `_MSC_VER` 分流
- 实测：Linux 构建 0 警告 0 错误；Linux/Windows 双平台全量 ctest **11/11**

### Web 引擎 keep-alive 连接复用与稳定性修复

修复引擎线程模型压测报告中的全部 5 项缺陷（三平台 Linux/Windows/macOS 统一状态机）：

- **keep-alive 语义修正（缺陷1）**：新增公共层 `nl_px_req_keepalive()` / `nl_px_req_content_length()` 判定请求 framing；本地路由统一等完整头后按 ka 设 `PR_ST_KA` 位、剥离 body（未到则记 DRAIN），响应完成走 `pr_conn_finish` 收口——按 KA 位真复用或关闭，响应头 `Connection` 与实际行为一致；错误响应（>=400）固定 `Connection: close`
- **流式响应后连接状态污染（缺陷2）**：SERVE_FILE/SERVE_OUT/proxy 各完成点统一走 finish 收口，文件路由响应完后同连接下一请求不再返回 400；body 分段转发补 `*hlen=0` 防跨段累加
- **连接耗尽 fd 泄漏（缺陷3）**：Linux HUP/ERR 路径两端 fd 均 close+epoll DEL（原上游错误时客户端 fd 泄漏）；macOS EV_ERROR 双端收口、pump 补 `rc==0` 处理、空闲扫描回收前 EV_DELETE
- **引擎线程饥饿（缺陷4）**：新增线程本地路由预查缓存（`g_pr_pre_route`/`g_pr_pre_path`，Linux/macOS `__thread`、Windows `__declspec(thread)`），plan_build 免二次拿路由表锁；Windows dispatch 两次临界区合并为一次
- **proxy 复用（方案E）**：分发时保留 KA 位，完成点（上游 DONE 且无剩余）上游归池 + 客户端按 KA 位复用/关；管道化残留 Linux 递归 dispatch 处理，Windows/macOS 拒绝复用直接关（顺序请求不受影响）
- **DRAIN 阶段**：请求 body 未收完先精确计数收完（Linux 独立事件分支、Windows `pr_post_drain_recv` 按 8KB 分段 post、macOS 同构），再定复用或关闭，防带未读数据 close 触发 RST 冲掉响应
- **测试（缺陷5）**：`test_web_engine.c` 新增长连接用例——l（同连接二次请求 + 404 真关断言）、m（1MB FILE 后复用非 400，Windows SKIP）、n（POST body 延后到达 DRAIN 后复用），断言 75→80
- 实测：Linux/Windows 双平台全量 ctest **11/11**；keep-alive 压测全轮 fail=0，QPS 随引擎线程 1→2→4 递增（15632→23026→25020），500 并发长连接无耗尽；macOS 改动完成代码自查（无本机工具链未编译）

## v3.1.0（2026-11-22）

### 目录/文件端点

- 新增 `nl_web_add_dir(server, prefix, target)`：把 URL 前缀注册为**端点**，端点指向磁盘**目录或文件**，目标即端点眼中的根目录——`/web` 指向 `~/net/web` 时，对该端点而言 `~/net/web` 就是 `/`，`../` 上跳也保持在该目录内（越根 `..` 忽略不逃逸）
- 路由类型新增 `NL_ROUTE_TYPE_DIR`（三平台 `nl_route_t` 与公共层 `NL_PX_LOCAL_DIR`），结构零新增字段：`path` 存端点前缀、`file_path` 存磁盘根、`is_prefix=1`
- **三阶段路由查找**：① 字面精确（剥 query 不解码）→ ② DIR 字面前缀（最长优先，边界必须是 `/` 或结尾，`/web` 不吞 `/webx`）→ ③ 规范化匹配；优先级 精确 > DIR 前缀 > 其它前缀
- **公共层 lookup 统一改传原始 path**（`nl_px_local_dispatch` / `nl_px_local_plan_build`）：规范化会吃掉端点前缀（`/web/../p` → `/p`），否则 DIR 字面前缀无法命中；`norm_ok` 降级为校验信号——未命中且规范化失败仍 400/404，命中 DIR 则放行进端点内 clamp
- **端点内 clamp 规范化** `nl_uri_normalize_clamp`（`nl_uri.c/.h`）：与全局 `nl_uri_normalize` 同源，`clamp=1` 时越根 `..` 忽略不报 `ESCAPES`；配合 `%` 重编码防双重解码、`nl_uri_resolve_file` realpath 二次校验
- **端点指向文件**：端点即该文件，任意余段（`/single/anything/deep?x=1`）均服务此文件
- **错误映射**：端点根下目标不存在 → 404（`PL_DIR_ENOENT`）；`ESCAPES/EROOT` → 403；ENOSPC → 500；其余规范化错误 → 400
- **注册约定**：前缀去尾 `/`（`/web/` → `/web`）、拒绝根 `/` 与非绝对前缀（防吞掉所有路由）；DIR 路由不参与第③阶段规范化匹配
- **legacy 同步发送路径**（三平台 `nl_handle_client`，`NETLEAF_LEGACY_WORKER_POOL` 下才编译）：normalize 失败改为延迟拒绝（记 `norm_ok` 用原始 path 继续查表）、hash 未命中回退中 DIR 单独按原始 path 字面前缀匹配且优先于其它前缀、命中后复用 `nl_px_local_plan_build` 同步发文件体
- 测试：`test_web_engine.c` 新增 `case_dir_endpoint`（13 项断言：目录根 index、端点内文件/子目录、`/d/../page.html` 与 `/d/../../page.html` clamp 200、越界 `../` 拿端点外诱饵文件 404 且无泄露、文件端点任意余段、`/dx` 不被 `/d` 吞、404、HEAD 无 body、非法端点注册被拒）
- 实测：Windows mingw x64 与 Linux（WSL）双平台构建通过，ctest **11/11** 全绿

### HTTP 客户端/服务端缺陷修复

- `nlh_client_connect` 支持明文 HTTP：`cfg=NULL` 时走非 TLS 路径，不再误报失败
- 修复 64MiB 请求体上限提前约 200 字节判定完成（请求体没收完就回包）的问题
- 新增 `Expect: 100-continue`（100 Continue）支持
- 请求头单个值 1023 字节静默截断 → 按需扩容，超限返回 431（后微调为规范上限 4096）
- `nlh_resp_file` 文件不存在时返回 404，不再返回 200 且连接挂死
- `nlh_server_enable_tls` 桩函数误用风险：补导出符号警告与文档说明
- `nlh_client_request` 输出缓冲区响应切割行为补充说明/辅助

### 头值上限微调与栈占用治理

- `MAX_HEADER_VALUE` 按规范从 1024 调整为 **4096**，超限回 431 不截断；`quic_h3_legacy.c` 同名宏同步
- 初版曾将 `headers[64]×4097B` 整体作为内嵌缓冲（结构体膨胀至约 270KB）并把六个平台服务循环/H2 回包处的 req/resp 改为堆分配；**后续澄清修正**：头值缓冲不再内嵌，改为**按需 `malloc(strlen+1)` 动态分配**（`struct nl_http_header.value` 由数组改为 `char*`），结构体回落到约 9KB，4096 仅作为超限判定值
- 新增释放契约 `nl_http_headers_free()`（`NL_HTTP_INT_API` 导出）：所有 req/resp/stream 销毁前统一释放已分配头值；覆盖六个平台服务循环、H2 两处 resp、H2 流销毁、QUIC/H3 流与回包处（quic 扩展为自包含副本，布局与释放逻辑同步）；HPACK 动态表结构保持不变

### 预构建包库命名与依赖自洽修正

- Linux/macOS 预构建包**不再收录符号链接**：只取真实文件，主文件去版本号重命名为单一名（`libnetleaf.so.3.1.0` → `libnetleaf.so`、`libnetleaf.3.1.0.dylib` → `libnetleaf.dylib`），取代早前的 `cp -Pf` 保链接方案
- Linux 打包用 patchelf 同步包内所有库的 **SONAME / NEEDED / RUNPATH**（NEEDED `libnetleaf.so.3` → `libnetleaf.so`、系统库不动、RUNPATH 改 `$ORIGIN`），否则扩展在干净机器上按旧 SONAME 找不到主库
- **patchelf 缺失回退**：按源文件原 SONAME 在包内补建相对符号链接（`libnetleaf.so.3` → `libnetleaf.so` 等），保证用本包编译 `-lnetleaf` 与运行时 NEEDED 解析不失败（此时 RUNPATH 仍为构建机路径，使用需 `LD_LIBRARY_PATH` 指向包内 `lib/bin`）；回退路径已实测：纯净环境 ldd/dlopen、带符号引用的编译+运行均通过
- macOS 对称修正：用 `install_name_tool` 把包内 dylib 的 **id 与依赖引用**统一改写为 `@rpath/<无版本名>`（含构建机绝对路径引用），系统库自动跳过；工具解析兼容 macOS 主机裸名与 osxcross `<triplet>-` 前缀
- 实测：Linux/ macOS x86_64 打包（有 patchelf/install_name_tool 时）包内 0 符号链接；Linux 纯净环境（`env -i`）ldd/dlopen 扩展通过，无 `LD_LIBRARY_PATH` 时靠 `$ORIGIN` 亦可加载；无 patchelf 回退包亦通过 ldd/dlopen/编译+运行验证

### 版本与构建

- 全部构建脚本版本号统一为 3.1.0（Linux/macOS/Windows MSVC/Windows LLVM-MinGW/Android）
- 两个演示程序（example_server / example_extension）改为可选且默认关闭：`BUILD_EXAMPLES=OFF`（CMake 默认值及所有构建脚本、wiki 文档同步）
- Linux/macOS 预构建包符号链接去重：`cp -f` 改为 `cp -Pf`，`libnetleaf.so` / `.so.N` / `.so.N.N.N` 三份相同内容不再重复占空间（zip 无此问题，Windows 不受影响；**后改为不收链接 + 主文件重命名 + 依赖同步，见上节**）
- 预构建包目录结构重排，包内含顶层目录 `NetLeaf-<版本>-<平台>-<arch>/`：
  - 根：README.md、README_EN.md、LICENSE、netleaf.pc
  - `lib/bin/`：共享库（**`.a` 静态库不再入包**）
  - `lib/cmake/NetLeaf/`：CMake package config
  - `include/`：公共头文件平铺（原 `include/optimize/` 下头文件一并平铺）
- 新增 pkg-config 与 CMake 包配置生成与分发：
  - `cmake/netleaf.pc.in` → 构建时生成 `netleaf.pc`，打包到包根（`prefix=${pcfiledir}` 相对定位，`PKG_CONFIG_PATH=<包根> pkg-config --cflags --libs netleaf`；`libdir=${prefix}/lib/bin`、`includedir=${prefix}/include`）
  - `cmake/NetLeafConfig.cmake.in` + `write_basic_package_version_file` → 生成 `NetLeafConfig.cmake` / `NetLeafConfigVersion.cmake`，打包到 `lib/cmake/NetLeaf/`（`CMAKE_PREFIX_PATH=<包根> find_package(NetLeaf REQUIRED)`，提供 `NetLeaf::netleaf` 及各扩展导入目标）
  - 生成逻辑位于根 `CMakeLists.txt`（输出到 `<build>/pkgmeta/`），各平台打包脚本自动收录

## v3.0.1（2026-11-21）

### 统一日志系统升级

**三平台去重：**
- linux/windows/macos 各 ~120 行重复日志实现（独立级别状态、格式化输出、回调分发、debug 模式）整体下沉为单份平台无关核心 `src/common/nl_logsys.c`（净删约 360 行）；`linux_log`/`windows_log`/`macos_log` 调用点（17/49/12 处）全部改走 `nl_log`

**输出格式增强（级别在前）：**
- `[级别] [T线程ID] [YYYY-MM-DD HH:MM:SS.mmm] 消息`——毫秒级时间戳（旧为秒级）、线程 ID（Linux `gettid` / macOS `pthread_threadid_np` / Windows `GetCurrentThreadId`）、级别定宽对齐 `[INFO ]`

**ANSI 颜色（仅 stderr）：**
- 按级别着色级别字段：DEBUG 青 / INFO 绿 / WARN 黄 / ERROR 红
- 仅控制台启用（Windows 显式开启 `ENABLE_VIRTUAL_TERMINAL_PROCESSING`）；重定向到文件/管道或设置 `NO_COLOR` 环境变量时自动禁用；文件 sink 永不着色（保证落盘内容可被 grep/解析）

**新增 API：**
- `nl_log_set_file(path, max_bytes, keep)` 文件落盘 + 按大小轮转（`path → path.1 → … → path.keep`，兼容 Windows rename 不覆盖语义）；`nl_log_close_file()`；`nl_log_get_level()`

**并发安全：**
- 文件写入/轮转由互斥锁保护（Win `INIT_ONCE`+`CRITICAL_SECTION` / POSIX `PTHREAD_MUTEX_INITIALIZER`），逐条 `fflush` 崩溃不丢日志

**裸 fprintf 收敛：**
- `src/http/netleaf_http_tls_linux.c` 3 处 TLS 失败输出（TLS_create/configure/handshake）改走 `nl_log_error`（受级别过滤/回调/落盘统一管理）

**新增测试：**
- `test/test_log.c`（ctest `LogSystemTest`）：级别过滤、回调分发、debug 模式、文件写入格式断言（毫秒时间戳/线程ID/定宽级别/无 ANSI 转义）、轮转链（`path.1`/`path.2` 存在且 `path.3` 不存在）、close 后不再写
- Windows/Linux 双平台 ctest **11/11 通过**；bench（`engineN A 256×8`）落在 37k~41k 波动区间内，无回归

### QUIC/H3 停止支持（2026-11-21）

- `quic` 扩展因协议栈缺陷无法修复，自 2026-11-21 起**停止支持、不再更新**
- 源码保留，`BUILD_QUIC` 默认 OFF，`QuicTlsTest` 仅在 `BUILD_QUIC=ON` 时注册

**变更说明：**
- stderr 日志格式变为 `[级别] [T线程ID] [时间戳] 消息`（原为 `[时间戳] [级别] 消息`），依赖旧格式的日志解析脚本需适配
- 三平台此前的时间戳格式差异（Windows 与 POSIX 不同）已统一

## v3.0.1（2026-11-08）

### LinkAgg 反向数据池

- 链路聚合改为“宿主主动拉取”：接入请求按端点入池，`nl_lagg_pull()` 拉取、`nl_lagg_request_respond()` 回送；客户端等待超时后重试一次，仍超时标记端点异常并返回 503，宿主恢复拉取后自动转健康
- 新增 `nl_lagg_get_backend_health` / `nl_lagg_set_pull_timeout` / `nl_lagg_get_pool_endpoint` / `nl_lagg_set_pool_service`
- 跨进程数据池服务（FETCH/RESP 协议），Windows 命名管道改为字节模式 + 64KB 缓冲，服务端 accept 改用已连接实例并为下一次 accept 预创建实例
- 三平台重复实现合并为 `src/linkagg/netleaf_linkagg_core.c` 单份共享核心（删除 linux/windows/macos 三个平台副本，减少约 47KB 重复代码）；`nl_lagg_start()` 改为非阻塞启动监听线程
- 新增回归测试 `test/test_linkagg_pull.c`：正常拉取/响应、超时 503 + 健康标记、恢复健康

### TLS2/TLS3 异步模式

- 新增 `nl_tls2_set_nonblocking` / `nl_tls3_set_nonblocking`（含 `is_nonblocking`）
- 异步模式下握手/收发不再内部 select 等待，直接返回 `WANT_READ` / `WANT_WRITE`，由调用方事件循环驱动；同步行为保持不变
- 修复 `handshake()` 因 `setup_done` 未置位而始终返回 INVALID_STATE 的问题

### HTTP/2 / HTTPS 稳定性修复与安卓独立化

**修复:**
- HTTPS/1.1 内存体响应 >16KB 被截断并挂死：三平台 `nlh_tls_write_cb` 改为循环处理部分写（mbedTLS 单次写约 16KB 上限）
- HTTP/2 大响应只发一帧（16,384 B）就卡死：六个写帧实现（TLS 三平台 `io_tls_write_h2_frame` + 明文三平台 `io_write_h2_frame`）全部改为循环写，杜绝部分写截断
- `nlh_resp_file` 在 H2 下返回 200 但 body 为 0：`h2_send_headers` 支持 `file_path` 文件流式 DATA 帧（fopen/fread 分块，EOF 帧带 END_STREAM，两处 handler 调用点补 `free(file_path)`）
- H2 流泄漏可稳定 OOM（单连接 10 万请求 ≈ 890 MB）：新增 `h2_release_stream` 摘链释放；DATA/WINDOW_UPDATE/RST_STREAM 改为“只查不建”，防止迟到帧把已释放流重新分配导致再次泄漏
- H2 流释放语义：`END_STREAM` 发出且状态 `CLOSED` 后立即摘链释放（无 body 响应在 HEADERS 后即释放）；`test_frame_state_machine` 重写为新语义

**性能:**
- `h2_send_headers` 攒批发帧（HEADERS+DATA 同缓冲，约 49KB 批上限，攒满/结束一次写出），减少每请求 TLS 多次小写，提升 QPS

**安卓独立化（minSdk 21）:**
- 平台差异后端拆分：`src/common/netleaf_encoding_backend.h` + `src/linux/netleaf_encoding_iconv.c`（glibc）+ `src/android/netleaf_encoding_bionic.c`（Bionic iconv 按 `__ANDROID_API__ >= 28` 启用，21–27 回退无转码）；运行库版本串同样拆分为 `netleaf_sysinfo_version_libc.c` / `netleaf_sysinfo_version_bionic.c`
- `netleaf_encoding_linux.c` / `netleaf_sysinfo_linux.c` 去除平台分支，`src` 树 `__ANDROID__` 编译分支清零；CMake `elseif(ANDROID)` / `elseif(UNIX)` 源列表分别选文件
- `build_android.sh` / `build_android.bat` 三 ABI（arm64-v8a / armeabi-v7a / x86_64）实测构建通过（NDK r30 / Clang 21，`ANDROID_PLATFORM=android-21`）

**验证:**
- Windows `ctest` 9/9 通过、`test_http_frames` 52/52 断言；Android 三 ABI 构建通过

### 跨协议反代修复与 worker 池显式启用

**跨协议反代 5 项缺陷修复:**
- 扩展自动加载 `.so` 后缀判断差一（`len-4` 比较 3 字符 `".so"`）→ 新增 `nl_has_suffix()`，Linux/macOS 扩展现在可正常自动加载
- 扩展发现顺序调整为**文件名派生优先**（`nl_ext_module_from_filename` → `nl_<mod>_get_extension_info`，校验 `library_id` 与文件名一致）→ 已知符号表兜底；候选列表彻底移除 `*_get_module_info`（其返回结构与 `nl_extension_info_t` 布局不同，强转导致段错误）
- `dlsym` 依赖树误命中修复：`nl_ext_try_symbol()` 校验 `library_id`，加载 `libnetleaf_https.so` 不再因依赖树中 `libnetleaf_lang.so` 的符号而误注册为 `lang`
- `nl_proxy_connect_upstream` 改用新 TLS 契约 `conn_wrap/conn_read/conn_write/conn_free`（`nl_px_tls_contract_get/missing`），不再要求已废弃的 `api.handshake`；worker 路径 HTTPS 改走 `nl_px_tls_tunnel_run`（此前 100% 打成 502）
- `nl_px_h2.c` 移除两处 `0x20` HPACK 动态表容量更新写入（H2 服务端解码器原先不认，整块错位导致 `:path` 丢失）

**HPACK 动态表容量更新解码:**
- `hpack_decode_header` 支持 RFC 7541 §6.3 动态表容量更新（`001xxxxx` 5 位前缀无符号整数）：识别并收缩动态表；空 name 表示该指令非头字段，调用方 `continue` 跳过

**worker 池改为显式启用:**
- Linux/macOS worker 池**默认不启动**，`nl_server_start()` 走纯 epoll/kqueue 就地处理；即使调用 `nl_server_set_concurrency()` / `NL_OPT_CONCURRENCY` 配置了池大小，未显式调用新公开 API `nl_server_start_workers(nl_server_t*)` 则池不运行
- 新增 `nl_server_start_workers()`：在 `nl_server_start()` 之前调用以启用 worker 池（`worker_pool_enabled` 字段门控；UDP 协议不建池）；Windows 为 IOCP 完成线程，该函数为空操作
- 修复 `nl_server_stop()` + `nl_server_destroy()` 双重 `pthread_join` 同一线程导致的段错误（stop 后 `thread_id` 置 0，destroy 判空才 join）

## v3.0.0

> 本版本为大版本升级（2.4.2 → 3.0.0），涵盖 Web 引擎全异步重构（P1-P12）、HTTP/2 协议层补完、HTTPS/2、Windows 通用服务器全 IOCP 化等。版本号升级同步 19 个文件（CMakeLists、netleaf.h、9 个模块头 15 处版本宏、3 个内部头副本、docs/*.md、PROJECT_STRUCTURE.md）。

### Web 引擎（P1-P12，全异步重构）

**引擎架构（P1-P7，Linux 主实现，macOS/Windows 对齐）:**
- 新增 `nl_web_engine_t` 全异步引擎：数据驱动事件循环 + 每连接状态机，输出全部走 outbuf + EPOLLOUT（Linux）/ kevent（macOS）/ IOCP（Windows）异步续写
- 流式 body：MEM/FILE 均 64KB 分块发送，未知长度内容走 chunked 编码，本地路由不再整块分配 64KB 输出缓冲
- P6 多实例：Linux/macOS `SO_REUSEPORT` 多引擎（每引擎独立事件循环与连接池），Windows 为多 IOCP 消费线程
- P7 空闲超时扫描：默认 30s 空闲连接回收（slowloris 防护），500ms 节流扫描
- `force_legacy_pool` 保留为回退开关：有 PROXY 路由时走引擎，无 proxy 路由或强制时回退 worker 池（Windows 通用服务器 `nl_server_*` 路径已彻底退役 worker 池，见下）

**P8 压测基线（bench_web，三模式对比）:**
- 修复引擎多实例 running flag 竞态（QPS 1-2 → 19048，engineN 4 线程）
- 修复 `realpath()` 缓冲区溢出（`abs_path[512]` → `PATH_MAX`，glibc `_FORTIFY_SOURCE` 崩溃）
- 修复 bench 工具慢读模式 recv 覆盖 bug
- 结论：engineN 在 PROXY 场景超过 legacy 池（11044 vs 10638 QPS），10MB 文件场景 RSS 从 165MB 降至 4.3MB（-97%）

**P10 sendfile 零拷贝:**
- 文件路由 BODY_SENDFILE 零拷贝：Linux `sendfile()` / macOS BSD `sendfile()` / Windows `TransmitFile`；文件 0<sz≤10MB 走零拷贝，>10MB 返回 500
- EAGAIN 时记录偏移注册可写事件续传；chunked 模式强制回落分块
- 实测（FILE 1MB：engine1 4.8x / engineN 3.3x；FILE 10MB：engine1 5.9x / engineN 4.9x 提升）
- 修复 SERVE_FILE EAGAIN 时 outbuf 未排空即拉新块覆写未发数据的 bug

**P11 macOS/Windows 引擎对齐:**
- macOS（+543/-306）：引擎子结构抽取、SO_REUSEPORT 多实例、空闲扫描、BSD sendfile、stop 释放路径防 double-close
- Windows（+251/-104）：P7 空闲扫描、conn_id 竞态防护（slot 复用时丢弃过期完成）、TransmitFile、多 IOCP 消费线程（`pr_loop_cs` 临界区串行化）

**P12 测试体系:**
- 新增 9 个集成用例（`test/test_web_engine.c`）：CONTENT / FILE / REDIRECT / PROXY / HEAD / 404 / 连续请求 / slowloris / 客户端断开
- Linux 修复 4 处：HEAD RFC 7231 body 抑制、pr_body_offset 内存泄漏、`nl_web_stop` 先 join 再 close(epfd)、TSan running 读改 `__atomic_load_n` 三处
- Windows 修复 2 处：IOCP 引擎与 worker 池双路径 CONTENT/HEAD 响应修复
- 验证：ASan/TSan 23/23 通过（0 泄漏 0 竞争）、Windows 17/17、ctest 回归全过

**API 补齐（基线遗留）:**
- Linux/macOS 补齐：`nl_web_validate_encoding` / `nl_web_set_redirect_type` / `nl_web_get_redirect_type`
- Windows 补齐：`nl_web_add_html_file` / `nl_web_add_redirect_302`

### HTTP/2 协议层补完

- RFC 7541 Huffman 编解码完整实现
- HPACK 双向动态表（编码/解码各一张），修复 `hpack_decode_header` 字面量长度混读 BUG
- 流状态机 `NL_H2_ST_*`，新增 `h2_send_rst_stream` / `h2_send_goaway`
- 修复 `h2_connection_cleanup` HPACK 动态表泄漏
- `test_http_frames` 扩至 47 断言全部通过

### HTTPS/2（TLS 服务端 + H2 over TLS）

- 新增 `nlh_tls_server_*` 系列 API：TLS 服务端 + ALPN 协商，h2 命名协议分流至 HTTP/2 处理路径
- 修复 enable_tls 配置越界写

### Windows 通用服务器全 IOCP 化（worker 池彻底废弃）

- `nl_server_*`（TCP/HTTP/WEBSOCKET）改为 AcceptEx 预 post + 每连接 OVERLAPPED WSARecv + `GetQueuedCompletionStatus` 完成线程驱动
- 连接槽位状态机 `FREE→ACTIVE→PROCESSING→FREE`（`CLOSING` 等 aborted 完成回收），上限 1024 连接，30s 空闲断开
- `NL_OPT_CONCURRENCY` / `nl_server_set_concurrency` 语义改为 **IOCP 完成线程数**（1..64，默认 4；Linux/macOS 仍为 worker 池）
- UDP 改为 **16 槽 IOCP 接收环**（多条 outstanding WSARecvFrom）：handler 同步执行期间其余槽位仍由内核异步填充，消除 handler 慢时突发丢包；槽位内存活到 closesocket 之后释放
- 修复 AcceptEx 地址缓冲传 NULL+0 / lpBytesReceived 指向栈变量导致的 10014 (WSAEFAULT) 与栈损坏

### 其它

- QUIC/H3 拆出独立 `quic` 扩展（`BUILD_QUIC` 默认 OFF），主库只保留 HTTP/1.1 + HTTP/2

## v2.4.2

**新增:**
- HTTPS 拆出独立库 `netleaf_https`（纯 HTTP 的 `netleaf_http` 不依赖 TLS）
- TLS 扩展拆分：`netleaf_tls2`（mbedTLS 2.28.10）与 `netleaf_tls3`（mbedTLS 3.x，TLS 1.2/1.3）
- MQTT 默认开启：`BUILD_MQTT` / `BUILD_MQTT_SERVER` 默认 `OFF → ON`
- 平台无关公共工具层 `nl_util`；HTTP 短名转接层 `nlh_*`
- MQTT 服务端 + 回环回归测试

**变更:**
- 测试 CMake 拆分至 `test/CMakeLists.txt`；三平台公共 HTTP 逻辑去重（`netleaf_http_common.c`）
- 扩展注册自动依赖检查；`netleaf_lang` 软依赖解耦
- LTO 选项 `NETLEAF_ENABLE_LTO`；H2 动态表指针化（每连接削减 ≈4.7 MB）

**修复:**
- QUIC varint 编码 8 字节档缺上界溢出；H2 帧协议补全 END_HEADERS/CONTINUATION/PUSH_PROMISE
- QUIC 传输层帧补全（RFC 9000）；mbedTLS 3.6 API 移植（12 处）
- HTTP 写完不关 fd → CLOSE-WAIT；UDP handler 缺对端地址（新增 `nl_udp_message_handler_v2`）

## v2.4.1

**Lang 模块增强:**
- 错误码接入多语言：`NL_ERROR_BEGIN/NL_ERROR/NL_ERROR_END` 宏与 `nl_lang_register_errors()`
- 动态变量：`nl_lang_var_set_provider()` / `nl_lang_var_is_dynamic()`
- 外部变量：`nl_lang_var_bind_env()` / `nl_lang_var_load_env()` / `nl_lang_var_load_file()`

**质量与构建:**
- 清理编译告警，修复 MQTT 报文 ID 未初始化等隐患
- 一键脚本全平台交叉编译：Windows(x64/x86/arm64)、Linux(13 架构)、macOS(x86_64/arm64)

## v2.4.0

**Lang 模块增强:**
- 变量替换 `nl_lang_var_*`，模板语法 `{{VAR_NAME}}`，条件表达式（`== != >= <= > <`）
- 脚本引擎回调 `nl_lang_register_script_engine` / `nl_lang_execute_script`（可接入 Lua/Python/JS）

**TLS/SSL 扩展:**
- 内置 mbedTLS 2.28.10 LTS；`nl_tls_*` 通用 API；TLS 1.0-1.3；MQTT TLS 集成；`-DBUILD_TLS=ON`

**MQTT v5.0:**
- 完整协议支持（CONNECT/PUBLISH/SUBSCRIBE/UNSUBSCRIBE，QoS 0/1/2，5.0 属性编解码，主题通配符，遗嘱消息）

**扩展系统:**
- 生命周期/状态/信息查询、批量操作、搜索过滤、热重载、元数据管理 API
- `nl_module_register` 等补 `NL_API` 导出装饰符，修复扩展链接找不到符号

## v2.2.2

**平台支持:**
- ✅ Windows (IOCP) - 完全支持
- ✅ Linux (epoll) - 完全支持
- ✅ macOS (kqueue) - 完全支持（所有模块）

**重要变更 - 静态库支持:**
- ⚠️ **静态库已不再支持** - 由于扩展库需要动态链接以共享全局状态
- 如需静态链接，请使用 v2.2.1 或更早版本
- 所有模块现在仅构建为动态库（DLL/SO）

**新增功能 - 扩展库系统:**
- **功能**: 支持第三方开发者创建动态扩展库
- **文件**: `include/netleaf_module.h`, `src/netleaf_module.c`
- **特性**:
  - 扩展库定义结构 `nl_extension_info_t`
  - 用户定义字段：库名称、库ID、版本、作者、详情（可选，限制50个中文字符）
  - 平台支持字符串：`"Windows,Linux,MacOS"` 或 `"all"`（大小写不限，顺序不限）
  - `library_value` 由主库自动分配，用户无需设置
  - 扩展库自动加载：放入 `extensions/` 目录，`nl_modules_init()` 时自动加载
  - 扩展库注册/注销 API
  - 查询 API：`nl_extension_get_value_by_id()` / `nl_extension_get_id_by_value()`
  - 描述长度验证（最多50个中文字符）
- **示例**: `examples/plugin_example/`
- **文档**: `wiki/Extension.md`

**新增功能 - Web服务器文件热加载 (v2.2.2):**
- **功能**: 支持从外部文件加载HTML/Vue/JSON，修改后刷新立即生效
- **API**:
  - `nl_web_add_html_file(server, path, file_path)` - 从文件加载HTML
  - `nl_web_add_vue_file(server, path, file_path)` - 从文件加载Vue
  - `nl_web_add_json_file(server, path, file_path)` - 从文件加载JSON
- **特性**:
  - 每次请求时重新读取文件内容
  - 最大支持10MB文件
  - 自动获取文件绝对路径
  - 返回值：`NL_OK`/`NL_EINVAL`/`NL_EFILE`/`NL_ENOMEM`
- **平台**: Windows/Linux/macOS（全平台支持）

**新增功能 - HTTP重定向 (v2.2.2):**
- **功能**: 支持配置临时重定向（302）和永久重定向（301），可在运行时切换
- **API**:
  - `nl_web_add_redirect(server, path, target_url)` - 添加重定向
  - `nl_web_set_redirect_type(server, type)` - 运行时设置重定向类型（301/302）
  - `nl_web_get_redirect_type(server)` - 查询当前重定向类型
- **特性**:
  - 支持内部重定向（同服务器路径）
  - 支持外部重定向（完整URL）
  - 默认302临时重定向，可运行时切换为301永久重定向
  - 返回标准HTTP响应头
- **平台**: Windows/Linux/macOS（全平台支持）

**新增功能 - 智能检测 (v2.2.2):**
- **功能**: `nl_web_add_html`、`nl_web_add_vue`、`nl_web_add_json` 自动识别内容类型
- **特性**:
  - URL (`http://`/`https://`) → 自动创建重定向路由
  - 文件路径 → 自动创建热更新路由
  - 静态内容 → 直接作为内联内容处理
- **平台**: Windows/Linux/macOS（全平台支持）

**新增功能 - 运行时动态路由管理 (v2.2.2):**
- **功能**: 在服务器运行期间动态添加、移除、查询和更新路由
- **API**:
  - `nl_web_add_route(server, path, content, content_type)` - 添加静态内容路由
  - `nl_web_remove_route(server, path)` - 移除指定路径的路由
  - `nl_web_get_route_count(server)` - 获取当前路由数量
  - `nl_web_list_routes(server, paths, max_paths)` - 列出所有路由路径
  - `nl_web_update_route(server, path, content, content_type)` - 更新路由内容
- **特性**:
  - 线程安全：使用互斥锁保护路由操作
  - 支持运行时动态修改路由，无需重启服务器
  - 仅支持静态内容类型路由的更新（不支持重定向和文件路由）
- **平台**: Windows/Linux/macOS（全平台支持）

**移除的公开API:**
- 布尔值转换API（`nl_bool_from_string`等）已改为内部静默处理，不再对外暴露

**新增功能 - Vue 后端支持 (netleaf_vue):**
- **功能**: Vue.js 后端支持和 HTML 生成模块
- **文件**: `include/netleaf_vue.h`, `src/vue/netleaf_vue.c`
- **特性**:
  - Vue CDN 配置（unpkg, cdnjs, jsdelivr, local）
  - Vue 代码检测和自动导入
  - HTML 页面生成（带 Vue CDN）
  - 预定义组件生成（系统信息、性能监控）
  - 变量替换支持
- **平台**: Windows/Linux/macOS（全平台支持）

**构建流程简化:**
- `build_all.bat` / `build_all.sh` - 直接运行即可构建
- 自动创建 `extensions/` 目录
- 版本号更新为 2.2.2

**多语言错误消息增强:**
- 支持一个库指定多个同语言文件
- 支持多库共享翻译文件（需显式声明）
- 错误码重复注册检测
- 异步加载支持优化

**模块系统优化:**
- 新增 Vue 模块类型 `NL_MODULE_VUE`
- 新增扩展库管理 API
- 统一的模块注册和管理
- 跨平台兼容性改进

**Bug 修复:**
- 修复 Linux 构建警告（`strdup` 隐式声明、未使用参数）
- 修复 macOS 链接器选项问题（`-fstack-protector-strong` 仅 Linux）
- 修复插件加载中的 `va_list` 未声明问题
- 修复跨平台类型兼容性问题（`HMODULE*` 替换为 `nl_plugin_handle_t*`）
- 修复枚举重复定义问题（`nl_lazy_status_t` → `nl_module_lazy_status_t`）

## v2.2.1

**平台支持:**
- ✅ Windows (IOCP) - 完全支持
- ✅ Linux (epoll) - 完全支持
- ✅ macOS (kqueue) - 主库功能完全支持

**新增功能 - IPC通讯服务 (netleaf_ipc):**
- **功能**: 进程间通讯服务，支持 Windows Named Pipe 和 Linux Unix Domain Socket
- **文件**: `src/ipc/`
  - `netleaf_ipc.c` - 核心模块
  - `src/windows/netleaf_ipc_windows.c` - Windows 实现
  - `src/linux/netleaf_ipc_linux.c` - Linux 实现
- **特性**:
  - 支持服务端监听和客户端连接
  - 跨进程数据传输
  - 线程安全设计
- **平台**: Windows/Linux（macOS暂不支持）

**新增功能 - 同端口链路聚合 (netleaf_linkagg):**
- **功能**: 单端口监听，将请求转发到多个后端（HTTP/IPC）
- **文件**: `src/linkagg/`
  - `netleaf_linkagg.c` - 核心模块
  - `src/windows/netleaf_linkagg_windows.c` - Windows 实现
  - `src/linux/netleaf_linkagg_linux.c` - Linux 实现
- **特性**:
  - 负载均衡策略: Round Robin, Random, Least Connections, Weighted Round Robin
  - 支持 HTTP 和 IPC 后端
  - 同端口路由聚合
- **平台**: Windows/Linux（macOS暂不支持）

**统一模块接口:**
- **功能**: 所有独立模块通过统一接口进行管理
- **文件**: `include/netleaf_module.h`, `src/netleaf_module.c`
- **特性**:
  - 模块信息结构 `nl_module_info_t`
  - 模块注册/注销 API
  - 模块版本、能力、平台支持查询
  - 统一的模块管理 API

**多语言错误消息 (netleaf_lang):**
- **功能**: 统一的多语言错误消息翻译库
- **文件**: `include/netleaf_lang.h`, `src/lang/netleaf_lang.c`
- **特性**:
  - 无限语言支持（en_us, zh_cn, ja_jp, ko_kr 等）
  - 语言代码格式强制 `xx_xx`（忽略大小写）
  - 分开注册语言和错误消息
  - 自定义语言代码和错误码注册
  - 多文件支持（一个库多个语言文件）
  - 多库共享文件（需显示声明）
  - 错误码重复检测
  - 异步加载支持
  - 跨平台：Windows / Linux / macOS
- **平台**: ✅ Windows / ✅ Linux / ✅ macOS

**Bug修复:**
- 修复边缘触发 epoll 循环读取问题
- 修复 socket 双重关闭问题
- 修复内存泄漏问题（Levenshtein栈分配、g_global_matcher释放）
- 修复缓冲区溢出问题
- 修复线程安全问题（原子操作、线程安全时间函数）

**优化:**
- 启用 LTO 链接时优化
- 移除 usleep(100ms) 忙等待，改用 poll
- Bubble sort 替换为 qsort
- 添加安全加固编译标志
- 构建系统优化（OBJECT库避免重复编译）

**构建系统更新:**
- CMake选项: `BUILD_IPC=ON`, `BUILD_LINKAGG=ON`
- ASan支持: `BUILD_ASAN=ON`

## v2.2.0

**平台支持:**
- ✅ Windows (IOCP) - 完全支持
- ✅ Linux (epoll) - 完全支持
- 🔶 macOS (kqueue) - **初步支持**

**新增功能 - macOS平台:**
- `src/macos/` 目录下的完整实现
- kqueue替代epoll作为事件机制
- iconv编码转换（与Linux兼容）
- pthread懒加载机制
- sysctl系统信息获取

**附加模块 (从主库分离，与主库共用版本号):**

### Auto-complete (netleaf_autocomplete)
- **功能**: 自动补全charset标签和自动引入Vue库
- **文件**: `src/autocomplete/`
  - `netleaf_autocomplete.c` - 核心模块
  - `netleaf_charset.c` - Charset自动补全
  - `netleaf_vue_import.c` - Vue自动引入
- **特性**:
  - Charset自动补全：根据统一编码格式添加 `<meta charset>` 和 `<meta name="viewport">`
  - Vue自动引用：检测Vue代码但无引用时自动从CDN引入
  - 灵活启用方式：支持 `true/false`, `on/off`, `yes/no`, `1/0`
  - 单独控制：charset和Vue功能可独立启用/禁用
- **平台**: Windows/Linux/macOS 全部支持

### Auto-route (netleaf_autoroute)
- **功能**: 404时自动查找相近端点并在错误页面提示
- **文件**: `src/autoroute/`
  - `netleaf_autoroute.c` - 核心模块
  - `netleaf_route_matcher.c` - 路由匹配算法
- **特性**:
  - Levenshtein距离算法计算路径相似度
  - 多策略评分：路径段匹配、前缀共有、段数相同
  - 通配符支持：`*` 和 `**`
- **平台**: Windows/Linux/macOS 全部支持

### ErrorPage (netleaf_errorpage)
- **功能**: 支持自定义错误页面模板，强制预留变量区域
- **文件**: `src/errorpage/`
  - `netleaf_errorpage.c` - 完整实现
- **特性**:
  - 独立standalone模块，其他模块不可使用其功能
  - 模板必须预留变量：`{{ERROR_CODE}}`, `{{ERROR_MESSAGE}}`, `{{REQUESTED_PATH}}`, `{{SERVER_VERSION}}`, `{{TIMESTAMP}}`
  - 支持 `{{#if SUGGESTION}}` 条件块
  - 可与Auto-route联动显示路由建议
- **平台**: Windows/Linux/macOS 全部支持

**主库API更新:**
- `nl_web_server_set_error_page()` - 设置自定义错误页面模板
- `nl_web_server_enable_error_suggestions()` - 启用路由建议
- `nl_render_error_page()` - 渲染错误页面
- `nl_make_error_response()` - 生成HTTP错误响应

**构建系统更新:**
- CMake选项: `BUILD_AUTOCOMPLETE=ON`, `BUILD_AUTOROUTE=ON`, `BUILD_ERRORPAGE=ON`
- 所有附加模块默认一起构建，共用主库版本号
- macOS交叉编译toolchain: `cmake/osxcross.cmake`

## v2.1.6

**新增功能 - 编码动态适配:**
- `nl_web_enable_auto_encoding(server, enable)` - 启用/禁用自动编码协商
- `nl_web_is_auto_encoding_enabled(server)` - 检查是否启用自动编码
- `nl_web_set_fallback_encoding(server, encoding)` - 设置回退编码
- `nl_web_get_negotiated_encoding(server)` - 获取协商后的编码
- `nl_encoding_convert(input, len, src_enc, dst_enc)` - 编码转换函数
- `nl_encoding_detect(input, len)` - 自动检测字符串编码
- `nl_encoding_get_system_default()` - 获取系统默认编码

**编码支持:**
- UTF-8
- GBK
- GB2312
- GB18030
- Big5
- ISO-8859-1
- US-ASCII
- UTF-16

**全场景支持:**
- ✅ 控制台输出
- ✅ Web响应
- ✅ HTML页面
- ✅ Vue组件
- ✅ JSON响应
- ✅ TOML响应

**特性:**
- 自动解析客户端Accept-Charset请求头
- 运行时自动转码响应内容
- 双向编码转换（任意编码之间互相转换）
- 跨平台支持（Windows/Linux）
- 懒加载机制：功能拉起使用后自动下线，减少内存占用
- 智能优化：仅对非ASCII字符进行转码，减少性能开销

**编译器编码修复:**
- 默认使用UTF-8编码编译
- 解决中文乱码问题

## v2.1.5

**新增功能:**
- `nl_sys_info_get_os_name()` - 获取操作系统名称
- `nl_sys_info_get_architecture()` - 获取系统架构
- `nl_sys_info_get_cpu_model()` - 获取CPU型号
- `nl_sys_info_get_total_ram()` - 获取总内存（字节）
- `nl_sys_info_get_runtime_version()` - 获取运行库版本
- `nl_sys_info_set_ram_unit(unit)` - 设置RAM进制（1000/1024）
- `nl_sys_info_clear_cache()` - 清除缓存（用于重新加载）
- **懒加载机制**: 系统信息仅在首次调用时加载
- **安全增强**: `nl_web_stop_by_port()` 只能停止本库启动的端口

**RAM进制配置:**
- Linux 默认: 1000进制（符合SI标准）
- Windows 默认: 1024进制（传统二进制）
- 可通过 `nl_sys_info_set_ram_unit(NL_RAM_UNIT_DECIMAL)` 或 `nl_sys_info_set_ram_unit(NL_RAM_UNIT_BINARY)` 切换

**安全更新:**
- 只能停止由本库启动的端口，防止误操作其他进程的端口

**懒加载支持:**
- `nl_lazy_enable(enable)` - 全局启用/禁用懒加载
- `nl_lazy_enable_module(module)` - 启用指定模块的懒加载
- `nl_lazy_disable_module(module)` - 禁用指定模块的懒加载
- `nl_lazy_is_enabled(module)` - 检查模块是否启用懒加载
- `nl_lazy_clear_all_cache()` - 清除所有模块的缓存
- `nl_lazy_preload_module(module)` - 预加载指定模块

**支持懒加载的模块:**
- HTTP (`NL_LAZY_MODULE_HTTP`)
- WebSocket (`NL_LAZY_MODULE_WEBSOCKET`)
- TCP (`NL_LAZY_MODULE_TCP`)
- UDP (`NL_LAZY_MODULE_UDP`)
- TOML (`NL_LAZY_MODULE_TOML`)
- JSON (`NL_LAZY_MODULE_JSON`)
- SysInfo (`NL_LAZY_MODULE_SYSINFO`)

**模块启停与状态管理:**
- `nl_lazy_stop_module(module)` - 停止指定模块并释放资源
- `nl_lazy_get_module_status(module)` - 获取模块状态（未加载/加载中/已加载/停止中/已停止）
- `nl_lazy_is_module_loaded(module)` - 检查模块是否已加载

**多线程优化:**
- `nl_lazy_set_thread_count(count)` - 设置线程池大小（1-256）
- `nl_lazy_get_thread_count()` - 获取当前线程池大小
- 默认线程数: 4
- 支持根据应用场景调整：IO密集型推荐较大线程数，CPU密集型推荐CPU核心数

## v2.1.0

**新增功能:**
- `nl_web_create(port)` - 创建即启动Web服务器
- `nl_web_stop_by_port(port)` - 按端口号停止服务器
- `nl_web_set_auto_cleanup(enable)` - 程序退出时自动清理所有服务器
- `nl_web_set_encoding()` - 设置响应编码
- `nl_web_add_html_with_vars()` - 带变量的HTML页面
- `nl_web_add_vue_with_vars()` - 带变量的Vue页面
- `nl_debug_enable()` - 启用调试模式
- `nl_log_debug()`, `nl_log_info()`, `nl_log_warn()`, `nl_log_error()` - 日志函数
- 自动检测交叉编译工具链
- 异步并发优化，完整的异步IO支持

**Bug修复:**
- 修复服务器端口冲突问题（重复创建相同端口返回现有实例）
- 修复内存泄漏风险（通过`atexit`注册自动清理）
- 修复多线程安全问题（全局服务器列表添加互斥锁保护）

**安全更新:**
- 完善的空指针检查
- 缓冲区溢出防护
- 字符串格式化安全

## v2.0.0

- 初始版本发布
- 支持HTTP/1.1, HTTP/2, HTTP/3
- WebSocket支持
- TCP/UDP基础通信
- 文件服务器功能
- JSON/TOML解析器
- 路由配置API

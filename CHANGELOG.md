# NetLeaf 版本历史

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
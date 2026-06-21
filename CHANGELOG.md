# NetLeaf 版本历史

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
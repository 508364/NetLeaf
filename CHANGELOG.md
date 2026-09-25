# Changelog

所有重要变更将记录在此文件中。

遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/) 规范。

## [2.4.1] - 2026-09-24

### 变更

#### Lang：错误码接入多语言 + 变量系统支持动态/外部变量 (v2.4.1)
- 新增表驱动错误注册基础设施：`NL_ERROR_BEGIN / NL_ERROR / NL_ERROR_END` 宏与
  `nl_lang_register_errors()` / `nl_lang_register_errors_ex()`，一次注册中英双语错误表
- 让此前"只声明、未生效"的模块真正接入 Lang：autoroute / autocomplete / errorpage / ipc
  （补充初始化注册调用与对 `netleaf_lang` 的链接依赖）；连同 linkagg / tls / mqtt / mqtt_server，
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
  powerpc/powerpc64/powerpc64le/riscv64/s390x)、macOS(x86_64/arm64) 全部成功
- 版本号统一为 `2.4.1`（含 `build_macos.sh`、`build_all-MSVC.bat` 修正）
- TLS 配置探针确认 TLS 1.0 / 1.1 / 1.2 / 1.3 均已启用
- 四个测试程序（test_all / test_full / test_modules / test_variable_substitution）全部通过

### 安全提示
- TLS 1.0 / 1.1 已被 RFC 8996 废弃且不再安全，仅建议用于兼容老旧对端；
  生产环境建议将最小版本设置为 TLS 1.2 及以上

### 文档
- 在 `README.md` / `README_EN.md` / `wiki`（Home、Features、TLS）中补充安全说明：
  **不推荐使用 TLS 1.0 / 1.1**，并说明 NetLeaf 的 TLS 能力由**独立的 TLS 扩展库**（`netleaf_tls`）负责；
  如确需使用请将 `min_proto` 设置为 `NL_TLS_PROTO_TLS1_2` 或更高
- 将文档"架构说明"中的字符画（ASCII 图）改为 **Mermaid 图表**，便于阅读与维护
  （涉及 `README.md`、`wiki/TLS.md`、`wiki/Features.md`、`wiki/LinkAgg.md`）

## [2.4.0] - 2026-09-19

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
- mbedTLS（当前内置 2.28.10 LTS）: https://github.com/Mbed-TLS/mbedTLS (Apache 2.0 / GPL v2.0)
- MQTT Specification: https://mqtt.org/ (EPL-2.0 / EDL 1.0)
- Paho MQTT C: https://github.com/eclipse/paho.mqtt.c (EPL-2.0 / EDL 1.0)

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

---

## [2.2.2-17891424] - 2026-09-12

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

## [2.2.2] - 2026-09-09

### 新增功能

#### 跨平台构建优化
- 支持 Windows (x86_64, i686, arm64)
- 支持 Linux (x86_64, arm64)
- 支持 macOS (x86_64, arm64)

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

## [2.2.1] - 2026-09-01

### 修复

- 修复 Windows 下链接器错误
- 修复 macOS 编译警告
- 修复 Linux 构建脚本

## [2.2.0] - 2026-08-25

### 新增功能

- 添加 WebSocket 服务端和客户端支持
- 添加 HTTP 服务器路由修正功能
- 支持 Windows/MacOS/Linux 三平台构建

### 修改

- 重构网络层抽象
- 优化事件循环性能
- 改进错误处理机制

## [2.1.0] - 2026-08-20

### 新增功能

- 添加智能路由修正功能
- 添加路由匹配建议
- 支持路由参数自动解析

### 修改

- 重构路由引擎
- 优化正则表达式性能
- 改进错误页面生成

## [2.0.0] - 2026-08-15

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

## [1.0.0] - 2026-07-01

### 初始版本

- 基础 TCP/UDP 服务器
- 基础 HTTP 处理
- 简单的路由系统
- Windows 平台支持

---

**注意**: 从 v2.0.0 开始采用语义化版本控制， breaking changes 会在主版本号升级时出现。

[Unreleased]: https://github.com/508364/NetLeaf/compare/v2.4.1...HEAD
[2.4.1]: https://github.com/508364/NetLeaf/compare/v2.4.0...v2.4.1
[2.4.0]: https://github.com/508364/NetLeaf/compare/v2.2.2-17891424...v2.4.0
[2.2.2-17891424]: https://github.com/508364/NetLeaf/compare/v2.2.2...v2.2.2-17891424
[2.2.2]: https://github.com/508364/NetLeaf/compare/v2.2.1...v2.2.2
[2.2.1]: https://github.com/508364/NetLeaf/compare/v2.2.0...v2.2.1
[2.2.0]: https://github.com/508364/NetLeaf/compare/v2.1.0...v2.2.0
[2.1.0]: https://github.com/508364/NetLeaf/compare/v2.0.0...v2.1.0
[2.0.0]: https://github.com/508364/NetLeaf/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/508364/NetLeaf/releases/tag/v1.0.0

# NetLeaf 版本历史

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
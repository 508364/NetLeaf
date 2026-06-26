<img src="./Logo.svg" width="50" height="50" align="left"> 

### **NetLeaf v2.2.2**
高性能跨平台网络库，支持TCP/UDP/HTTP/HTTP2/HTTP3，以及内联HTML/Vue响应式Web服务器

**平台支持:**
- ✅ Windows (IOCP) - 完全支持
- ✅ Linux (epoll) - 完全支持
- ✅ macOS (kqueue) - 完全支持

**仓库地址:**
- GitHub: [https://github.com/508364/NetLeaf](https://github.com/508364/NetLeaf)
- Gitee: [https://gitee.com/x508364/NetLeaf](https://gitee.com/x508364/NetLeaf)

## 快速开始

### Windows

在 Visual Studio Developer Command Prompt 中运行：

```cmd
build_all.bat
```

或指定架构：

```cmd
build_all.bat x86
build_all.bat arm64
```

### Linux/WSL

```bash
chmod +x build.sh
./build.sh
```

或指定架构：

```bash
./build.sh x86
./build.sh arm
./build.sh arm64
```

### 全架构构建

```bash
# Linux - 自动检测可用交叉编译工具链
./build_all_linux.sh

# Windows
build_all_windows.bat
```

## 输出位置

构建完成后：

```
build/
├── bin/Release/netleaf.dll (Windows) | libnetleaf.so (Linux)
└── lib/Release/netleaf.lib (Windows) | libnetleaf.a (Linux)
```

## 使用示例

### 1. 一行代码启动Web服务器

```c
#include "netleaf.h"

int main() {
    nl_serve_dashboard(8080, "My Dashboard");
    
    while (1) {
        Sleep(1000);  // Windows
        // sleep(1);  // Linux
    }
    return 0;
}
```

### 2. 内联HTML

```c
#include "netleaf.h"

int main() {
    nl_web_server_t* server = nl_web_create(8080);
    nl_web_add_html(server, "/", 
        "<!DOCTYPE html>"
        "<html><body>"
        "<h1>Hello NetLeaf!</h1>"
        "</body></html>");
    nl_web_start(server);
    
    while (1) Sleep(1000);
    nl_web_destroy(server);
    return 0;
}
```

### 3. 变量硬编码（新增）

**变量在服务器端替换，用户F12看不到原始变量名：**

```c
#include "netleaf.h"

int main() {
    nl_web_server_t* server = nl_web_create(8080);
    
    // 设置编码
    nl_web_set_encoding(server, "UTF-8");
    
    // 定义变量
    const char* vars[] = {"username", "user_id"};
    const char* values[] = {"张三", "12345"};
    
    // 添加带变量的页面
    nl_web_add_html_with_vars(server, "/profile",
        "<h1>欢迎, {{<var>username</var>}}</h1>"
        "<p>用户ID: {{<var>user_id</var>}}</p>",
        vars, values, 2);
    
    nl_web_start(server);
    while (1) Sleep(1000);
    return 0;
}
```

**输出到客户端：**
```html
<h1>欢迎, 张三</h1>
<p>用户ID: 12345</p>
```

### 4. 使用预设组件

```c
// 计数器
nl_web_add_counter(server, "/", "Counter Demo");

// 数据面板
nl_web_add_dashboard(server, "/dashboard", "Analytics");

// 表单
const char* fields[] = {"name", "email", "message"};
nl_web_add_form(server, "/contact", "Contact Us", fields, 3);
```

### 5. TCP服务器

```c
#include "netleaf.h"
#include <stdio.h>

void on_data(const char* data, size_t len, char** response, size_t* response_size, void* user_data) {
    *response = (char*)malloc(len + 1);
    memcpy(*response, data, len);
    (*response)[len] = '\0';
    *response_size = len;
}

int main() {
    nl_server_t* server = nl_server_create(NL_PROTO_TCP, 8080);
    nl_serve(8080, on_data, NULL);
    
    while (1) Sleep(1000);
    return 0;
}
```

## 更新日志

**查看完整版本历史:** [CHANGELOG.md](CHANGELOG.md)

## Wiki - 最佳实践

### 推荐配置

#### 1. 启用自动清理（强烈推荐）

为了防止程序退出时内存泄漏，建议在程序初始化时启用自动清理功能：

```c
#include "netleaf.h"

int main() {
    // 启用程序退出时自动清理所有服务器资源
    nl_web_set_auto_cleanup(1);
    
    // 创建服务器（创建即启动）
    nl_web_server_t* server = nl_web_create(8080);
    
    // ... 添加路由和业务逻辑 ...
    
    while (1) {
        // 主循环
    }
    
    // 程序退出时会自动调用 nl_web_destroy 清理所有服务器
    return 0;
}
```

**为什么推荐启用：**
- ✅ 防止内存泄漏
- ✅ 自动释放所有服务器资源（socket、线程、内存）
- ✅ 确保程序优雅退出
- ✅ 特别适合守护进程或服务类应用

**默认行为：**
- 默认关闭（`nl_web_set_auto_cleanup(0)`）
- 如需手动管理，使用 `nl_web_destroy(server)` 或 `nl_web_stop_by_port(port)`

#### 2. 多服务器管理

```c
// 创建多个服务器
nl_web_server_t* server1 = nl_web_create(8080);
nl_web_server_t* server2 = nl_web_create(8081);

// 按端口停止指定服务器
nl_web_stop_by_port(8080);

// 程序退出时自动清理所有剩余服务器
```

#### 3. 编译选项

**Debug模式（开发阶段）：**
```c
nl_debug_enable();  // 启用详细日志输出
```

**Release模式（生产环境）：**
```c
// 默认关闭调试日志，仅输出错误信息
```

#### 4. 系统信息组件（懒加载）

获取非敏感的系统信息，采用懒加载机制，仅在首次调用时加载。

```c
#include "netleaf.h"

int main() {
    // 设置RAM进制（可选，默认平台相关）
    nl_sys_info_set_ram_unit(NL_RAM_UNIT_BINARY);  // 1024进制
    
    // 获取系统信息（首次调用时懒加载）
    const char* os_name = nl_sys_info_get_os_name();
    const char* arch = nl_sys_info_get_architecture();
    const char* cpu = nl_sys_info_get_cpu_model();
    int64_t ram = nl_sys_info_get_total_ram();  // 字节数
    const char* runtime = nl_sys_info_get_runtime_version();
    
    printf("OS: %s\n", os_name);
    printf("Architecture: %s\n", arch);
    printf("CPU: %s\n", cpu);
    printf("Total RAM: %lld bytes\n", (long long)ram);
    printf("Runtime: %s\n", runtime);
    
    // 清除缓存（如需重新加载）
    nl_sys_info_clear_cache();
    
    return 0;
}
```

**RAM进制说明：**
- **NL_RAM_UNIT_DECIMAL (1000)**: Linux默认，符合SI标准（1 KB = 1000 bytes）
- **NL_RAM_UNIT_BINARY (1024)**: Windows默认，传统二进制（1 KB = 1024 bytes）

**安全性：**
- 仅获取非敏感信息：系统名称、架构、CPU型号、RAM总量、运行库版本
- 不收集任何个人信息或敏感数据

#### 5. 全局懒加载配置

所有组件支持懒加载机制，仅在首次使用时才加载，减轻启动时的性能消耗。

```c
#include "netleaf.h"

int main() {
    // 全局启用懒加载（默认已启用）
    nl_lazy_enable(1);
    
    // 禁用特定模块的懒加载（立即加载）
    nl_lazy_disable_module(NL_LAZY_MODULE_HTTP);
    
    // 启用特定模块的懒加载
    nl_lazy_enable_module(NL_LAZY_MODULE_WEBSOCKET);
    
    // 检查模块是否启用懒加载
    if (nl_lazy_is_enabled(NL_LAZY_MODULE_TCP)) {
        printf("TCP模块懒加载已启用\n");
    }
    
    // 预加载指定模块（提前加载，避免首次调用延迟）
    nl_lazy_preload_module(NL_LAZY_MODULE_ALL);
    
    // 清除所有模块缓存（重新触发懒加载）
    nl_lazy_clear_all_cache();
    
    return 0;
}
```

**支持懒加载的模块:**

| 模块 | 常量 | 说明 |
|------|------|------|
| HTTP | `NL_LAZY_MODULE_HTTP` | HTTP服务器组件 |
| WebSocket | `NL_LAZY_MODULE_WEBSOCKET` | WebSocket组件 |
| TCP | `NL_LAZY_MODULE_TCP` | TCP通信组件 |
| UDP | `NL_LAZY_MODULE_UDP` | UDP通信组件 |
| TOML | `NL_LAZY_MODULE_TOML` | TOML解析组件 |
| JSON | `NL_LAZY_MODULE_JSON` | JSON解析组件 |
| SysInfo | `NL_LAZY_MODULE_SYSINFO` | 系统信息组件 |
| 全部 | `NL_LAZY_MODULE_ALL` | 所有组件 |

**懒加载API:**
- `nl_lazy_enable(enable)` - 全局启用/禁用懒加载
- `nl_lazy_enable_module(module)` - 启用指定模块的懒加载
- `nl_lazy_disable_module(module)` - 禁用指定模块的懒加载
- `nl_lazy_is_enabled(module)` - 检查模块是否启用懒加载
- `nl_lazy_clear_all_cache()` - 清除所有模块的缓存
- `nl_lazy_preload_module(module)` - 预加载指定模块
- `nl_lazy_stop_module(module)` - 停止指定模块（释放资源）
- `nl_lazy_get_module_status(module)` - 获取模块状态
- `nl_lazy_is_module_loaded(module)` - 检查模块是否已加载
- `nl_lazy_set_thread_count(count)` - 设置线程池大小（1-256）
- `nl_lazy_get_thread_count()` - 获取当前线程池大小

**模块状态:**

| 状态 | 常量 | 说明 |
|------|------|------|
| 未加载 | `NL_LAZY_STATUS_UNLOADED` | 模块尚未加载 |
| 加载中 | `NL_LAZY_STATUS_LOADING` | 模块正在加载 |
| 已加载 | `NL_LAZY_STATUS_LOADED` | 模块已加载完成 |
| 停止中 | `NL_LAZY_STATUS_STOPPING` | 模块正在停止 |
| 已停止 | `NL_LAZY_STATUS_STOPPED` | 模块已停止 |

**使用示例 - 模块启停:**

```c
#include "netleaf.h"

int main() {
    // 预加载HTTP模块
    nl_lazy_preload_module(NL_LAZY_MODULE_HTTP);
    
    // 检查模块状态
    if (nl_lazy_is_module_loaded(NL_LAZY_MODULE_HTTP)) {
        printf("HTTP模块已加载\n");
    }
    
    // 设置线程池大小（优化多线程性能）
    nl_lazy_set_thread_count(8);
    printf("线程池大小: %d\n", nl_lazy_get_thread_count());
    
    // 使用后停止模块，释放资源
    nl_lazy_stop_module(NL_LAZY_MODULE_HTTP);
    
    return 0;
}
```

**多线程优化:**

懒加载系统支持线程池配置，默认使用4个线程。根据应用场景调整线程数可以获得更好的性能：
- **IO密集型应用**: 推荐设置较大线程数（如CPU核心数的2倍）
- **CPU密集型应用**: 推荐设置为CPU核心数

#### 6. 编码格式支持

支持多种字符编码格式，防止数据返回时出现字符崩溃问题。

```c
#include "netleaf.h"

int main() {
    nl_web_server_t* server = nl_web_create(8080);
    
    // 设置响应编码
    nl_web_set_encoding(server, NL_ENCODING_UTF8);
    // 或使用其他编码：NL_ENCODING_GBK, NL_ENCODING_BIG5, NL_ENCODING_GB2312等
    
    // 验证编码格式
    if (nl_web_validate_encoding(NL_ENCODING_GBK)) {
        nl_web_set_encoding(server, NL_ENCODING_GBK);
    }
    
    return 0;
}
```

**支持的编码格式：**
| 编码 | 常量 | 说明 |
|------|------|------|
| UTF-8 | `NL_ENCODING_UTF8` | Unicode（默认） |
| GBK | `NL_ENCODING_GBK` | 简体中文 |
| GB2312 | `NL_ENCODING_GB2312` | 简体中文（旧版） |
| GB18030 | `NL_ENCODING_GB18030` | 中文国家标准 |
| ISO-8859-1 | `NL_ENCODING_ISO8859_1` | 西欧语言 |
| US-ASCII | `NL_ENCODING_US_ASCII` | ASCII |
| UTF-16 | `NL_ENCODING_UTF16` | Unicode（16位） |
| Big5 | `NL_ENCODING_BIG5` | 繁体中文 |

#### 6. 错误处理与警告

启用警告功能，获得更详细的错误提示。

```c
#include "netleaf.h"

int main() {
    // 启用警告功能
    nl_web_enable_warnings(1);
    
    nl_web_server_t* server = nl_web_create(8080);
    if (!server) {
        nl_warning_t warn;
        if (nl_web_get_last_warning(&warn)) {
            printf("警告: %s\n", nl_warning_message(warn));
        }
    }
    
    return 0;
}
```

**警告类型：**
| 警告 | 说明 |
|------|------|
| `NL_WARN_PORT_IN_USE` | 端口已被占用 |
| `NL_WARN_INVALID_ENCODING` | 无效的编码格式 |
| `NL_WARN_MEMORY_LIMIT` | 内存使用接近限制 |
| `NL_WARN_CONNECTION_LIMIT` | 连接数达到上限 |
| `NL_WARN_INVALID_CONFIG` | 无效的配置参数 |
| `NL_WARN_BUFFER_OVERFLOW` | 缓冲区溢出风险 |

#### 9. 内联HTML/Vue支持

### 10. 文件服务

```c
// 一行代码服务静态文件
nl_serve_files("./public", 8080);
```

### 11. API路由

```c
nl_router_t* router = nl_router_create();
nl_router_add_route(router, "/api/hello", NL_METHOD_GET, my_handler, NULL);
nl_router_set_static_dir(router, "./public");
nl_router_serve(router, 8080);
```

## 构建脚本

### Windows

```cmd
build_all.bat
```

自动构建 x64、x86、ARM64 架构并打包为ZIP格式。

### Linux/WSL

```bash
chmod +x build_all.sh
./build_all.sh
```

自动检测已安装的交叉编译工具链并构建所有可用架构。

## 链接你的项目

### Windows (MSVC)

```cmd
cl myapp.c /I include /link build/lib/Release/netleaf.lib ws2_32.lib
```

### Linux (GCC)

```bash
gcc myapp.c -I include -L build/lib -lnetleaf -lpthread -o myapp
```

## 功能特性

✅ **TCP/UDP** - 完整的网络协议支持  
✅ **HTTP/1.1, HTTP/2, HTTP/3** - 全HTTP协议栈  
✅ **内联HTML/Vue** - 直接在代码中嵌入响应式界面  
✅ **预设组件** - 计数器、面板、表单等开箱即用  
✅ **现代CSS** - 美观的渐变和动画效果  
✅ **跨平台** - Windows (IOCP) + Linux (epoll) + macOS (kqueue) 🔶  
✅ **宽可用库** - 简洁统一的输出结构  
✅ **无外部依赖** - 仅使用系统API  
✅ **多架构支持** - x86, x64, ARM, ARM64

## API参考

### Web服务器API

```c
// 创建服务器
nl_web_server_t* nl_web_create(int port);
void nl_web_destroy(nl_web_server_t* server);
int nl_web_start(nl_web_server_t* server);
void nl_web_stop(nl_web_server_t* server);

// 添加路由
void nl_web_add_html(nl_web_server_t* server, const char* path, const char* html);
void nl_web_add_vue(nl_web_server_t* server, const char* path, const char* vue_code);
void nl_web_add_json(nl_web_server_t* server, const char* path, const char* json);

// 预设组件
void nl_web_add_counter(nl_web_server_t* server, const char* path, const char* title);
void nl_web_add_dashboard(nl_web_server_t* server, const char* path, const char* title);
void nl_web_add_form(nl_web_server_t* server, const char* path, const char* title, const char** fields, int field_count);

// 一行代码启动
int nl_serve_html(int port, const char* html);
int nl_serve_vue(int port, const char* vue_code);
int nl_serve_dashboard(int port, const char* title);
```

更多API请查看 `include/netleaf.h`。

## 附加模块

以下模块是从主库分离出来的独立库，与主库共用版本号(v2.2.2)，构建时一起构建。

### 1. Auto-complete 模块 (netleaf_autocomplete)

**功能：** 自动补全内联HTML/Vue代码中缺失的编码声明，以及自动引入Vue库。

**特性：**
- **Charset自动补全**: 根据程序设置的统一编码格式自动添加 `<meta charset>` 和 `<meta name="viewport">` 标签
- **Vue自动引用**: 检测Vue代码但没有Vue引用时，自动从CDN引入Vue库
- **独立功能控制**: charset和Vue功能可以单独启用/禁用
- **灵活启用方式**: 支持 `1/0`、`true/false`、`on/off`、`yes/no`（大小写不敏感）

**平台支持：**
- ✅ Windows - 使用系统API
- ✅ Linux - 使用系统API
- ✅ macOS - 使用系统API

**使用示例：**
```c
#include "netleaf_autocomplete.h"

int main() {
    // 初始化模块
    nl_autocomplete_init();
    
    // 设置统一编码格式
    nl_autocomplete_set_encoding("UTF-8");
    
    // 启用模块（支持字符串）
    nl_autocomplete_enable_ex("true");  // 或 "on", "yes", "1"
    
    // 单独控制功能
    nl_autocomplete_enable_feature(NL_AUTOCOMPLETE_FEATURE_CHARSET);  // 启用charset
    nl_autocomplete_disable_feature(NL_AUTOCOMPLETE_FEATURE_VUE);     // 禁用Vue
    
    // 处理HTML
    const char* html = "<html><body>Hello</body></html>";
    char* result = nl_autocomplete_process_html(html, strlen(html), NULL);
    
    // result: <html><head><meta charset="UTF-8">...</head><body>Hello</body></html>
    
    free(result);
    return 0;
}
```

**构建选项：** `BUILD_AUTOCOMPLETE=ON` (默认)

---

### 2. Auto-route 模块 (netleaf_autoroute)

**功能：** 404时自动查找相近端点并在错误页面提示。

**特性：**
- **智能路由匹配**: 使用Levenshtein距离算法计算路径相似度
- **多策略评分**: 考虑路径段匹配、前缀共有、段数相同等因素
- **通配符支持**: 支持 `*` 和 `**` 通配符模式
- **灵活启用方式**: 支持 `1/0`、`true/false`、`on/off`、`yes/no`

**平台支持：**
- ✅ Windows - 完全支持
- ✅ Linux - 完全支持
- ✅ macOS - 完全支持

**使用示例：**
```c
#include "netleaf_autoroute.h"

int main() {
    // 初始化模块
    nl_autoroute_init();
    
    // 启用模块
    nl_autoroute_enable_ex("true");
    
    // 获取全局路由匹配器
    nl_route_matcher_t* matcher = nl_autoroute_get_global_matcher();
    
    // 添加已知路由
    nl_route_matcher_add_route(matcher, "/api/users");
    nl_route_matcher_add_route(matcher, "/api/products");
    nl_route_matcher_add_route(matcher, "/dashboard");
    
    // 查找相似路由
    char* suggestion = nl_route_matcher_find_similar(matcher, "/api/user", 0.4);
    // suggestion: "/api/users" (相似度0.75)
    
    free(suggestion);
    return 0;
}
```

**构建选项：** `BUILD_AUTOROUTE=ON` (默认)

---

### 3. ErrorPage 模块 (netleaf_errorpage)

**功能：** 支持自定义错误页面模板，强制预留变量区域，可与Auto-route联动。

**特性：**
- **模板系统**: 支持自定义HTML模板
- **变量替换**: 必须预留 `{{ERROR_CODE}}`、`{{ERROR_MESSAGE}}`、`{{REQUESTED_PATH}}` 等变量
- **条件块**: 支持 `{{#if SUGGESTION}}...{{/if}}` 条件块
- **独立运行**: 模块未加载时错误页面功能不生效

**重要声明：**
- 该模块独立且standalone
- 其他模块不能使用此模块的功能
- 如果该模块未编译/加载，错误页面功能将不可用

**平台支持：**
- ✅ Windows - 完全支持
- ✅ Linux - 完全支持
- ✅ macOS - 完全支持

**使用示例：**
```c
#include "netleaf_errorpage.h"

int main() {
    // 初始化模块
    nl_errorpage_init();
    
    // 启用模块
    nl_errorpage_enable_ex("true");
    
    // 设置自定义模板（必须包含所有必需变量）
    const char* template = 
        "<html><head><title>{{ERROR_CODE}} {{ERROR_MESSAGE}}</title></head>"
        "<body><h1>{{ERROR_CODE}} {{ERROR_MESSAGE}}</h1>"
        "<p>Path: {{REQUESTED_PATH}}</p>"
        "{{#if SUGGESTION}}<p>Did you mean: <a href=\"{{SUGGESTION}}\">{{SUGGESTION}}</a></p>{{/if}}"
        "<footer>Server: {{SERVER_VERSION}} at {{TIMESTAMP}}</footer></body></html>";
    
    nl_errorpage_set_template(404, template);
    
    // 生成错误响应
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

**必需变量：**
| 变量 | 说明 |
|------|------|
| `{{ERROR_CODE}}` | HTTP状态码 |
| `{{ERROR_MESSAGE}}` | 状态码描述 |
| `{{REQUESTED_PATH}}` | 请求路径 |
| `{{SERVER_VERSION}}` | 服务器版本 |
| `{{TIMESTAMP}}` | 错误时间戳 |

**可选变量：**
| 变量 | 说明 |
|------|------|
| `{{SUGGESTION}}` | Auto-route提供的路由建议 |

**构建选项：** `BUILD_ERRORPAGE=ON` (默认)

### 4. IPC 模块 (netleaf_ipc)

**功能：** 进程间通讯服务，支持 Windows Named Pipe 和 Linux Unix Domain Socket。

**特性：**
- 服务端监听和客户端连接
- 跨进程数据传输
- 线程安全设计
- 数据回调、连接/断开回调

**平台支持：**
- ✅ Windows - Named Pipe
- ✅ Linux - Unix Domain Socket
- ❌ macOS - 不支持

**路径格式：**
- Windows: `\\.\pipe\<name>`
- Linux: 文件系统路径（如 `/tmp/ipc_socket`）

**构建选项：** `BUILD_IPC=ON` (默认)

### 5. LinkAgg 模块 (netleaf_linkagg)

**功能：** 同端口链路聚合，单端口监听转发到多个后端。

**特性：**
- 负载均衡策略: Round Robin, Random, Least Connections, Weighted Round Robin
- 支持 HTTP 和 IPC 后端
- 同端口路由聚合
- 后端数量限制: 512个（索引 0-511）
- 端口 ID 格式: `xxx.xxx`

**平台支持：**
- ✅ Windows - 需要 IPC 模块支持
- ✅ Linux - 需要 IPC 模块支持
- ❌ macOS - 不支持（依赖 IPC 模块）

**状态：** Beta 版本

**构建选项：** `BUILD_LINKAGG=ON` (默认)

### 6. Lang 模块 (netleaf_lang)

**功能：** 多语言错误消息翻译库。

**特性：**
- 无限语言支持（en_us, zh_cn, ja_jp, ko_kr 等）
- 语言代码格式强制 `xx_xx`（忽略大小写）
- 分开注册语言和错误消息
- 自定义语言代码和错误码注册
- 多文件支持（一个库多个语言文件）
- 多库共享文件（需显示声明）
- 错误码重复检测
- 异步加载支持

**平台支持：**
- ✅ Windows - 完全支持
- ✅ Linux - 完全支持
- ✅ macOS - 完全支持

**使用示例：**
```c
#include "netleaf_lang.h"

int main() {
    // 设置语言
    nl_lang_set("zh_cn");  // 或 "en_us", "ja_jp" 等
    
    // 获取错误消息
    const char* msg = nl_lang_get_error(NL_LIB_LINKAGG, -9);
    printf("错误: %s\n", msg);
    // 输出: "不接入，ID被占用"
    
    // 分开注册语言
    nl_lang_register_language(NL_LIB_LINKAGG, "ja_jp");
    nl_lang_set_error(NL_LIB_LINKAGG, -9, "ja_jp", "IDが使用中");
    
    return 0;
}
```

**构建选项：** `BUILD_LANG=ON` (默认)

### 7. Vue 模块 (netleaf_vue)

**功能：** Vue.js 后端支持和 HTML 生成。

**特性：**
- Vue CDN 配置（unpkg、cdnjs、jsdelivr、local）
- Vue 代码检测和自动导入
- HTML 页面生成（带 Vue CDN）
- 预定义组件（Counter、Dashboard、Form）
- 变量替换支持

**平台支持：**
- ✅ Windows - 完全支持
- ✅ Linux - 完全支持
- ✅ macOS - 完全支持

**使用示例：**
```c
#include "netleaf_vue.h"

int main() {
    // 初始化模块
    nl_vue_init();
    
    // 生成计数器页面
    char* counter = nl_vue_generate_counter("计数器");
    if (counter) {
        printf("%s\n", counter);
        free(counter);
    }
    
    // 生成仪表盘页面
    char* dashboard = nl_vue_generate_dashboard("监控面板");
    if (dashboard) {
        printf("%s\n", dashboard);
        free(dashboard);
    }
    
    // 清理
    nl_vue_shutdown();
    return 0;
}
```

**构建选项：** `BUILD_VUE=ON` (默认)

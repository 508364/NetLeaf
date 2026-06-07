# 问题排查 | Troubleshooting

常见问题与解决方案。

---

## 目录 | Table of Contents

- [Windows 构建问题](#windows-构建问题)
- [Linux 构建问题](#linux-构建问题)
- [运行时问题](#运行时问题)
- [性能问题](#性能问题)
- [获取帮助](#获取帮助)

---

## Windows 构建问题

### 错误：找不到 Visual Studio 生成器

**问题：**
```
CMake Error: Could not find generator "Visual Studio 17 2022"
```

**解决方案：**
确保安装了 Visual Studio 2022 并包含 "使用 C++ 的桌面开发" 工作负载。

---

### 错误：端口被占用

**问题：**
```
bind failed: Address already in use
```

**解决方案：**
检查端口 8080 是否被其他程序占用：
```powershell
netstat -ano | findstr :8080
```
终止占用端口的进程或使用其他端口。

---

### 警告：snprintf 格式警告

**问题：**
```
warning C4476: "snprintf": 格式说明符中的类型字段字符 ";" 未知
```

**解决方案：**
确保在嵌入 HTML/CSS 的字符串中正确转义百分号 `%` 为 `%%`。

---

## Linux 构建问题

### 错误：找不到 realpath

**问题：**
```
error: implicit declaration of function 'realpath'
```

**解决方案：**
确保在代码文件顶部定义 `_GNU_SOURCE`：
```c
#define _GNU_SOURCE
#include <stdlib.h>
```

---

### 错误：缺少依赖

**问题：**
```
fatal error: sys/socket.h: No such file or directory
```

**解决方案：**
安装 build-essential：
```bash
sudo apt install build-essential
```

---

### 警告：未使用的参数/变量

**问题：**
```
warning: unused parameter 'user_data'
```

**解决方案：**
添加 `(void)` 显式忽略：
```c
(void)user_data;
```

---

## 运行时问题

### 服务器无法访问

**检查清单：**
1. 确认服务器正在运行
2. 检查防火墙设置
3. 确认监听地址是 `0.0.0.0` 或正确的网络接口
4. 检查是否有其他程序占用了端口

---

### WebSocket 连接失败

**可能原因：**
- 路径不匹配
- 缺少 Sec-WebSocket-Key
- 网络代理干扰

---

### JSON 解析错误

**常见问题：**
- 缺少引号
- 尾随逗号
- 注释（标准 JSON 不支持注释）

---

## 性能问题

### 高 CPU 使用率

**优化建议：**
1. 减少日志级别
2. 使用连接池
3. 启用 HTTP/2 多路复用
4. 检查是否有死循环

---

### 内存泄漏

**排查方法：**
1. 确保所有 `nl_create_*` 都有对应的 `nl_destroy_*`
2. 检查 JSON/TOML 对象是否正确释放
3. 使用工具如 Valgrind (Linux) 或 Dr. Memory (Windows)

---

## 获取帮助

如果问题仍然存在：

1. 查看项目 [GitHub Issues](https://github.com/solo-agent/NetLeaf/issues)
2. 搜索是否有类似问题已解决
3. 创建新 Issue 时请提供：
   - 操作系统版本
   - 编译器版本
   - 完整的错误日志
   - 最小可复现代码

---

[返回主页](./Home)


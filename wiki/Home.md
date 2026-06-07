# NetLeaf Wiki | NetLeaf 百科

[![Version](https://img.shields.io/badge/version-2.0.0-blue.svg)](https://github.com/solo-agent/NetLeaf)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-orange.svg)](#)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](../LICENSE)

---

## Navigation | 导航

- [快速入门](#快速入门--quick-start)
- [功能介绍](#功能介绍--features)
- [API 参考](#api-参考--api-reference)
- [安装指南](#安装指南--installation)
- [问题排查](#问题排查--troubleshooting)
- [示例代码](#示例代码--examples)
- [贡献指南](#贡献指南--contributing)

---

## 关于 NetLeaf | About NetLeaf

NetLeaf 是一个高性能、跨平台、轻量级的 C 网络库，专为快速构建现代化网络应用而设计。它提供完整的 HTTP 协议栈（HTTP/1.1、HTTP/2、HTTP/3）、WebSocket 支持、TCP/UDP 套接字、JSON/TOML 解析等功能，同时具有响应式 Web UI，方便调试和管理。

### 核心特性 | Key Features

- 🚀 **高性能**: 事件驱动架构，Linux 使用 epoll，Windows 使用 IOCP
- 🌐 **跨平台**: 原生支持 Windows (x86/x64/ARM64) 和 Linux
- 📦 **完整协议栈**: HTTP/1.1、HTTP/2、HTTP/3、WebSocket
- 🔒 **安全**: 内置 TLS 支持，线程安全
- 📝 **JSON/TOML 解析**: 内置解析器，无需额外依赖
- 🎨 **Web UI**: 响应式管理界面，Vue.js + Tailwind CSS
- 🛠️ **轻量级**: 无外部依赖，易于集成

### 支持的平台 | Supported Platforms

| 平台 | 架构 | 状态 |
|------|------|------|
| Windows | x86 (Win32) | ✅ 正式支持 |
| Windows | x64 | ✅ 正式支持 |
| Windows | ARM64 | ✅ 正式支持 |
| Linux | x64 | ✅ 正式支持 |
| Linux | ARM64 | ✅ 计划中 |

---

## 快速入门 | Quick Start

### 1. 安装 | Installation

查看 [安装指南](./Installation) 获取详细说明。

### 2. 第一个示例 | First Example

```c
#include "netleaf.h"
#include <stdio.h>

int main() {
    // 一行代码启动文件服务器
    nl_serve_files("./public", 8080);
    
    while (1) {
        Sleep(1000); // Windows
        // sleep(1); // Linux
    }
    return 0;
}
```

---

## 目录导航 | Table of Contents

| 文档 | 描述 |
|------|------|
| [Home](./Home) | 本页面 - 项目总览 |
| [Quickstart](./Quickstart) | 快速上手指南 |
| [Installation](./Installation) | 详细安装说明 |
| [Features](./Features) | 功能详细介绍 |
| [API Reference](./API-Reference) | 完整 API 文档 |
| [Troubleshooting](./Troubleshooting) | 常见问题与解决方案 |
| [Examples](./Examples) | 示例代码集合 |
| [Contributing](./Contributing) | 贡献指南 |

---

## License | 许可证

MIT License - 详见 [LICENSE](../LICENSE)

---

## 相关链接 | Related Links

- [GitHub Repository](https://github.com/solo-agent/NetLeaf)
- [问题反馈](https://github.com/solo-agent/NetLeaf/issues)
- [最新版本](https://github.com/solo-agent/NetLeaf/releases)


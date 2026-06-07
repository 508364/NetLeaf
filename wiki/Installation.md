# 安装指南 | Installation

本页面提供 NetLeaf 在各平台上的详细安装说明。

---

## 目录 | Table of Contents

- [Windows 安装](#windows-安装)
- [Linux 安装](#linux-安装)
- [集成到项目](#集成到项目)
- [验证安装](#验证安装)

---

## Windows 安装

### 系统要求 | System Requirements

- **操作系统**: Windows 10 1809+, Windows 11, Windows Server 2019+
- **编译器**: Visual Studio 2022 (Community/Professional/Enterprise)
- **构建工具**: CMake 3.15 或更高版本
- **架构支持**: x86 (Win32), x64, ARM64

### 安装步骤 | Installation Steps

#### 1. 安装 Visual Studio 2022

下载并安装 [Visual Studio 2022](https://visualstudio.microsoft.com/)，确保安装以下组件：
- 使用 C++ 的桌面开发 (Desktop development with C++)
- CMake tools for Visual Studio

#### 2. 安装 CMake

下载并安装 [CMake](https://cmake.org/download/)，或使用 Visual Studio 内置的 CMake。

#### 3. 克隆项目 | Clone the Repository

```powershell
git clone https://github.com/solo-agent/NetLeaf.git
cd NetLeaf
```

#### 4. 构建项目 | Build the Project

**方式一：使用批处理脚本 (推荐)**

```powershell
# 构建所有架构 (x86, x64, ARM64)
.\build_all_windows.bat

# 或构建单个架构
.\build.bat x64       # x64
.\build.bat x86       # x86 (Win32)
.\build.bat arm64     # ARM64
```

**方式二：手动 CMake**

```powershell
# x64
mkdir build_x64
cd build_x64
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

---

## Linux 安装

### 系统要求 | System Requirements

- **操作系统**: Ubuntu 20.04+, Debian 11+, CentOS 8+, 或其他现代 Linux 发行版
- **编译器**: GCC 8+ 或 Clang 10+
- **构建工具**: CMake 3.15+, Make
- **架构支持**: x64, ARM64 (计划中)

### 安装步骤 | Installation Steps

#### 1. 安装依赖 | Install Dependencies

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install -y build-essential cmake git
```

**CentOS/RHEL:**
```bash
sudo yum install -y gcc gcc-c++ make cmake git
```

#### 2. 克隆项目 | Clone the Repository

```bash
git clone https://github.com/solo-agent/NetLeaf.git
cd NetLeaf
```

#### 3. 构建项目 | Build the Project

**方式一：使用 Shell 脚本 (推荐)**

```bash
chmod +x build_all_linux.sh
./build_all_linux.sh
```

**方式二：手动 CMake**

```bash
mkdir build_linux
cd build_linux
cmake ..
make -j$(nproc)
```

**使用 WSL (Windows Subsystem for Linux):**

```powershell
.\build_wsl.sh
```

---

## 集成到项目 | Integrate into Your Project

### 使用 CMake (推荐)

将 NetLeaf 作为子模块或复制到您的项目中，然后在您的 `CMakeLists.txt` 中添加：

```cmake
# 添加 NetLeaf 子目录
add_subdirectory(path/to/NetLeaf)

# 链接到您的目标
target_link_libraries(your_target PRIVATE netleaf_static)
# 或使用动态库
target_link_libraries(your_target PRIVATE netleaf_shared)
```

### 直接链接库文件

构建 NetLeaf 后，将以下文件添加到您的项目中：

| 平台 | 包含目录 | 库文件 |
|------|---------|--------|
| Windows x64 | `include/` | `build_x64/lib/Release/netleaf.lib` |
| Windows x86 | `include/` | `build_x86/lib/Release/netleaf.lib` |
| Windows ARM64 | `include/` | `build_arm64/lib/Release/netleaf.lib` |
| Linux x64 | `include/` | `build_wsl/lib/libnetleaf.a` |

---

## 验证安装 | Verify Installation

### 运行测试 | Run Tests

构建完成后，运行简单服务器验证：

```powershell
# Windows
cd build_x64
.\bin\Release\simple_server.exe

# Linux
cd build_wsl
./bin/simple_server
```

访问 http://localhost:8080 确认服务器正常运行。

### 编译您的第一个程序 | Compile Your First Program

创建一个简单的测试文件 `test.c`：

```c
#include "netleaf.h"
#include <stdio.h>

int main() {
    nl_server_t* server = nl_create_server(8080);
    if (!server) {
        printf("Failed to create server\n");
        return 1;
    }
    
    nl_add_get_route(server, "/", "text/plain", "Hello NetLeaf!");
    
    printf("Server running on http://localhost:8080\n");
    nl_start_server(server);
    
    return 0;
}
```

编译并运行：

**Windows (MSVC):**
```powershell
cl test.c /I include /link build_x64/lib/Release/netleaf.lib
.\test.exe
```

**Linux (GCC):**
```bash
gcc test.c -I include -L build_wsl/lib -lnetleaf -o test
./test
```

---

## 常见问题 | FAQ

### Q: 构建时提示找不到 CMake？
**A:** 确保 CMake 已安装并添加到系统 PATH 环境变量中。

### Q: Linux 上编译错误提示缺少头文件？
**A:** 确保安装了完整的 build-essential 或开发工具包。

### Q: 如何使用调试版本？
**A:** 将 CMake 构建配置改为 Debug：
```powershell
cmake --build . --config Debug
```

---

[返回主页](./Home)


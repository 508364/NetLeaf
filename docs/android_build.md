# NetLeaf Android 构建指南

NetLeaf v3.0.1 支持 Android（Bionic）交叉编译。复用 Linux/epoll 风格的 POSIX 源，
CMake 通过 `elseif(ANDROID)` 分支自动切换编译选项、源文件列表与核心库链接。

> **范围说明**：Android 目前覆盖**核心库**（`netleaf_core`，含 TCP/UDP、HTTP、
> WebSocket、优化层）。一键脚本仅构建 `netleaf_core` 目标（`--target netleaf_core`），
> 不产出扩展共享库；扩展系统（NL Extension，如 `lang`/`ipc`/`linkagg`/`vue`/
> `autocomplete`/`autoroute`/`errorpage`/`tls3`）的源码与构建配置完整保留，
> 其运行时 Android 适配暂未跟进，后续按扩展逐个落地。

## 支持矩阵

| 项 | 取值 |
|---|---|
| 目标 ABI | `arm64-v8a`（aarch64，推荐）、`armeabi-v7a`（32 位 ARM）、`x86_64`（模拟器） |
| minSdkVersion | 21（Android 5.0） |
| 编译器 | NDK clang（r25+，自带 CMake toolchain；已实测 r30 / Clang 21） |
| STL | 未显式设置（纯 C 项目，NDK 默认 c++_static，不引入 STL 运行时） |
| NEON | 默认开启（`ANDROID_ARM_NEON=ON`） |
| 产物 | `libnetleaf.so`（netleaf_core 共享库）+ 公共头文件（stage-lib） |

## 前置条件

1. **Android NDK**（r25+）。安装方式：
   ```bat
   :: 通过 sdkmanager（需要已安装 Android SDK command-line tools）
   sdkmanager "ndk;27.2.12479018"
   ```
   或手动下载解压，记住其目录路径。

2. **CMake**（3.20+，建议 4.x）。
3. **构建工具**：`ninja`（首选）或 `nmake` / `make`。
4. **Java**（仅 sdkmanager 安装 NDK 时需要）。

## 一键构建

### Windows

```bat
build_android.bat            :: 构建全部三 ABI
build_android.bat aarch64    :: 仅 arm64-v8a
build_android.bat armeabi-v7a
build_android.bat x86_64
```

脚本会自动探测 NDK 路径（优先级：环境变量 `ANDROID_NDK` / `ANDROID_NDK_HOME` /
`ANDROID_NDK_ROOT` / `NDK_HOME` / `NDK_ROOT` → `<ANDROID_HOME>\ndk` /
`<ANDROID_SDK_ROOT>\ndk` → `%LOCALAPPDATA%\Android\Sdk\ndk` 等常见安装位置；
若指向 SDK 根目录则自动选版本号最大的 `ndk\<版本>` 子目录）。产物写入
`build-android\<abi>\stage-lib\`。

### Linux / macOS（含 WSL）

```bash
./build_android.sh          # 构建全部三 ABI
./build_android.sh aarch64  # 仅 aarch64
```

脚本按 `ANDROID_NDK` / `ANDROID_NDK_HOME` / `ANDROID_NDK_ROOT` / `NDK_HOME` /
`NDK_ROOT` 环境变量探测，其次 `<ANDROID_HOME>/ndk`、`<ANDROID_SDK_ROOT>/ndk`、
`$HOME/Android/Sdk/ndk`、`$HOME/Library/Android/sdk/ndk` 等候选路径，
并自动选取版本号最大的 NDK 子目录。

## 手动 CMake（不依赖脚本）

```bash
# 示例：仅编 arm64-v8a
mkdir -p build-android/arm64-v8a && cd build-android/arm64-v8a
cmake -G "Ninja" -S ../../.. -B . \
      -DCMAKE_TOOLCHAIN_FILE="<NDK>/build/cmake/android.toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-21 \
      -DANDROID_STL=c++_shared \
      -DANDROID_ARM_NEON=ON \
      -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF \
      -DBUILD_TLS=OFF -DBUILD_TLS3=ON -DBUILD_MQTT=OFF -DBUILD_MQTT_SERVER=OFF \
      -DBUILD_LANG=ON -DBUILD_LINKAGG=ON -DBUILD_VUE=OFF -DBUILD_AUTOCOMPLETE=ON \
      -DBUILD_AUTOROUTE=OFF -DBUILD_ERRORPAGE=ON -DBUILD_IPC=ON -DBUILD_HTTPS=OFF
ninja netleaf_core
```

## 在 Android Studio 工程中集成

把 `stage-lib` 下的内容复制到 Android 工程的 `jniLibs/` 与 `cpp/`：

```
app/
├─ src/main/
│  ├─ jniLibs/arm64-v8a/libnetleaf.so     # 共享库
│  ├─ jniLibs/armeabi-v7a/libnetleaf.so
│  ├─ jniLibs/x86_64/libnetleaf.so
│  └─ cpp/
│     ├─ include/                         # 公共头文件（来自 stage-lib，含 optimize/）
```

在工程 CMakeLists 中按 `ANDROID_ABI` 预导入对应的 `libnetleaf.so` 并链接
（`add_library(netleaf SHARED IMPORTED)` + `set_target_properties(... IMPORTED_LOCATION ...)`）。

## Bionic 兼容性说明

Android 的 Bionic libc 与 glibc 差异通过**平台后端拆分**处理（minSdk 21，`src` 树
已无 `__ANDROID__` 编译分支）：

- **转码后端**：接口定义在 `src/common/netleaf_encoding_backend.h`
  （`nl_encoding_backend_open/convert/default_codeset`）：
  - Linux（glibc）：`src/linux/netleaf_encoding_iconv.c`（iconv_open/iconv/nl_langinfo）；
  - Android：`src/android/netleaf_encoding_bionic.c` —— `__ANDROID_API__ >= 28` 时启用
    Bionic iconv（iconv_open/iconv/iconv_close 自 API 28 起可用），21–27 回退为
    不转码（open 返回 NULL、codeset 返回 NULL，上层自动回退 UTF-8）。
  - `netleaf_encoding_linux.c` 只调用后端接口，不再包含 iconv/langinfo 平台分支。
- **运行库版本串**：`src/common/netleaf_sysinfo_version.h` +
  `src/linux/netleaf_sysinfo_version_libc.c`（glibc 版本号）/
  `src/android/netleaf_sysinfo_version_bionic.c`（"Bionic (NDK)"），
  `netleaf_sysinfo_linux.c` 中原 `__ANDROID__` 分支已移除。
- **随机数**：`netleaf_http_linux.c` 保留无条件 `#include <sys/random.h>`
  （NDK r25+ 提供该头文件，实测 NDK r30 编译通过）；主库已无 `getrandom()` 调用
  （v3.0.0 起 QUIC/H3 拆分至独立 `quic` 扩展，连接 ID 生成改用 mbedTLS CTR_DRBG；
  该扩展已于 2026-11-21 停止支持，不再更新）。
- 核心库链接：`dl`/`iconv` 在 Bionic 已并入 libc，`ANDROID` 分支仅显式链
  `Threads::Threads` 与 `log`（Bionic `__android_log` 由 liblog 提供）。
- `epoll_create1`（API 21+）/`flock`/`/proc/cpuinfo` 均受 Bionic 支持。

各扩展 CMakeLists 中 `if(UNIX)` 链接 `Threads::Threads`、`elseif(UNIX)` 指向
Linux 源文件，Android 会自动命中，无需单独改动。

## 验证

一键脚本仅构建 `netleaf_core` 目标，构建成功后 `build-android/<abi>/stage-lib/` 应包含：

```
libnetleaf.so          # 核心库
*.h                    # 全部公共头（含 optimize/ 子目录）
```

实测记录（2026-11-13）：NDK 30.0.16248370（r30，Clang 21）、`ANDROID_PLATFORM=android-21`，
`build_android.bat all` 下 arm64-v8a / armeabi-v7a / x86_64 三 ABI 全部构建通过。

用 NDK 的 `llvm-readelf` 检查产物架构与导出符号：

```bash
<NDK>/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf \
  -d build-android/arm64-v8a/lib/libnetleaf.so | grep -E "SONAME|NEEDED"
```

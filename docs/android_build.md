# NetLeaf Android 构建指南

NetLeaf v2.4.2 支持 Android（Bionic）交叉编译。复用 Linux/epoll 风格的 POSIX 源，
CMake 通过 `elseif(ANDROID)` 分支自动切换编译选项、源文件列表与核心库链接。

> **范围说明**：Android 目前覆盖**核心库**（`netleaf_core`，含 TCP/UDP、HTTP、
> WebSocket、优化层）。**扩展系统（NL Extension，如 `lang`/`ipc`/`linkagg`/`vue`/
> `autocomplete`/`autoroute`/`errorpage` 各扩展共享库）暂不支持 Android 运行时**，
> 但其源码与构建配置完整保留，后续按扩展逐个跟进。因此 Android 目标下核心库
> 可正常编译链接，扩展共享库仍按 Linux 语义构建，运行时尚不具备 Android 适配。

## 支持矩阵

| 项 | 取值 |
|---|---|
| 目标 ABI | `arm64-v8a`（aarch64，推荐）、`armeabi-v7a`（32 位 ARM）、`x86_64`（模拟器） |
| minSdkVersion | 21（Android 5.0） |
| 编译器 | NDK clang（r25+，自带 CMake toolchain） |
| STL | `c++_shared` |
| NEON | 默认开启（`ANDROID_ARM_NEON=ON`） |
| 产物 | `libnetleaf*.so`（共享库）+ 头文件 + CMakeLists.txt |

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

脚本会自动探测 NDK 路径（`ANDROID_NDK_HOME` → `C:\Android\Sdk\ndk` → `E:\AndroidSdk\ndk`，
以及其中的版本化子目录）。产物写入 `build-android\<abi>\stage-lib\`。

### Linux / macOS（含 WSL）

```bash
./build_android.sh          # 构建全部三 ABI
./build_android.sh aarch64  # 仅 aarch64
```

脚本在 Linux/WSL 下探测 `/mnt/e/AndroidSdk/ndk`、`/mnt/c/AndroidSdk/ndk`、
`~/Android/Sdk/ndk`、`/opt/android-ndk-*` 等候选路径。

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
│  └─ cpp/
│     ├─ CMakeLists.txt                    # 来自 stage-lib
│     ├─ include/*.h                       # 头文件
```

`CMakeLists.txt` 的 `ANDROID` 分支会按 `ANDROID_ABI` 自动选择对应 `libnetleaf.so`。

## Bionic 兼容性说明

Android 的 Bionic libc 与 glibc 差异已在代码层处理：

- `netleaf_sysinfo_linux.c`：`__GLIBC__`/`__GLIBC_MINOR__` 在 Bionic 未定义，
  已加 `__ANDROID__` 分支显示为 "Bionic (NDK)"。
- `netleaf_http_linux.c`：Bionic 不提供 `getrandom()`/`<sys/random.h>`，
  QUIC 连接 ID 生成在 `__ANDROID__` 下改为直接读 `/dev/urandom`，含 `rand()` 兜底。
- 核心库链接：`dl`/`iconv` 在 Bionic 已并入 libc，`ANDROID` 分支仅显式链
  `Threads::Threads` 与 `log`（Bionic `__android_log` 由 liblog 提供）。
- `epoll_create1`（API 21+）/`flock`/`/proc/cpuinfo` 均受 Bionic 支持。

各扩展 CMakeLists 中 `if(UNIX)` 链接 `Threads::Threads`、`elseif(UNIX)` 指向
Linux 源文件，Android 会自动命中，无需单独改动。

## 验证

构建成功后 `build-android/<abi>/stage-lib/` 应包含：

```
libnetleaf.so          # 核心库
libnetleaf_<ext>.so    # 各扩展（lang/linkagg/autocomplete/errorpage/ipc/tls3）
*.h                    # 全部公共头
CMakeLists.txt
```

用 NDK 的 `llvm-readelf` 检查产物架构与导出符号：

```bash
<NDK>/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf \
  -d build-android/arm64-v8a/lib/libnetleaf.so | grep -E "SONAME|NEEDED"
```

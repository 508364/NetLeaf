# 项目目录整理报告

**整理时间**: 2026-09-18 15:50 Beijing time
**状态**: 已完成

---

## 执行的操作

### 1. 文件迁移
- 将 `examples/test_*.c` 迁移到 `test/` 目录
- 将 `examples/plugin_example/test_plugin_api.c` 迁移到 `test/` 目录

### 2. 文件处理
- 尝试删除 `nul` 文件（Windows 保留设备名，无法删除）
- 将 `nul` 添加到 `.gitignore` 中忽略

### 3. .gitignore 更新
添加以下内容：
- `nul` - Windows 保留设备名
- `build_all-Clang.bat` - Clang 交叉编译脚本
- `build_all-MSVC.bat` - MSVC 构建脚本
- `src/mqtt/` - MQTT 模块（开发中）
- `include/netleaf_mqtt.h` - MQTT 头文件（开发中）

---

## 整理后的目录结构

### 根目录文件
```
.gitattributes
.gitignore
build_all-Clang.bat      (已忽略)
build_all-MSVC.bat       (已忽略)
build_all.sh
build_macos.sh
CHANGELOG.md
CMakeLists.txt
LICENSE
Logo.svg
README.md
README_EN.md
nul                      (已忽略)
```

### 核心目录
```
.github/
docs/
examples/
    plugin_example/
    plugin_template/
    hfile_example/
frontend-examples/
include/
src/
test/                    (新增测试文件)
wiki/                    (已忽略)
extensions/
Project-Record/          (已忽略)
```

### 测试文件位置
```
test/
├── test_netleaf.c
├── test_all.c
├── test_full.c
├── test_modules.c
├── test_no_web.c
├── test_simple.c
├── test_web.c
└── test_plugin_api.c
```

---

## 注意事项

1. `nul` 文件无法删除，已通过 `.gitignore` 忽略
2. 所有测试文件已统一到 `test/` 目录
3. 插件开发模板位于 `examples/plugin_template/`
4. MQTT 模块是开发中功能，已添加到 `.gitignore`

---

*报告生成时间: 2026-09-18 15:50 Beijing time*

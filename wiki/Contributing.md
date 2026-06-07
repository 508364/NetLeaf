# 贡献指南 | Contributing

欢迎贡献 NetLeaf！

---

## 目录 | Table of Contents

- [代码规范](#代码规范)
- [提交规范](#提交规范)
- [开发流程](#开发流程)

---

## 代码规范 | Coding Standards

### C 语言风格

- 使用 4 空格缩进
- 大括号不换行
- 函数名使用小写加下划线
- 宏定义使用全大写加下划线
- 私有函数前缀 `nl_`

```c
void nl_some_function(void) {
    if (condition) {
        do_something();
    }
}
```

### 提交规范 | Commit Guidelines

使用 Conventional Commits 格式：

```
<type>(<scope>): <subject>

<description>
```

**Type:

- feat: 新功能
- fix: 修复
- docs: 文档
- style: 格式
- refactor: 重构
- test: 测试
- chore: 构建/工具

---

## 开发流程 | Development Workflow

1. Fork 项目
2. 创建特性分支 (`git checkout -b feature/amazing-feature`)
3. 提交更改 (`git commit -m 'feat: add some amazing feature'`)
4. 推送到分支 (`git push origin feature/amazing-feature`)
5. 开启 Pull Request

---

[返回主页](./Home)


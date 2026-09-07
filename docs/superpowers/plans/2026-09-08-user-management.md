# 用户管理与密码设置实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 替换用户管理和系统设置占位页，提供可测试的账号、角色和密码维护能力。

**Architecture:** `UserService` 统一执行管理员校验、账号更新、密码哈希和审计日志；两个页面分别服务管理员维护和当前用户自助改密。

**Tech Stack:** C++17、Qt 6 Widgets/SQL、SQLite、PBKDF2-SHA256、Qt Test。

## Global Constraints

- 只有启用的管理员可新增、编辑用户和重置密码。
- 系统始终至少保留一个启用的管理员。
- 密码继续使用现有 `PasswordHasher` 加盐哈希，不保存明文。
- Debug 测试版本的管理员改密允许当前密码留空，Release 不允许。

---

### Task 1: 用户服务与测试

- [x] 实现用户新增、角色/状态更新、管理员重置密码和本人改密。
- [x] 实现用户名格式、密码长度、重复用户名和最后管理员保护。
- [x] 添加权限、密码哈希与错误路径自动化测试。

### Task 2: 页面与主窗口集成

- [x] 实现用户列表、查询、新增、编辑、启停和重置密码页面。
- [x] 实现当前用户信息、数据库路径和修改密码页面。
- [x] 替换两个占位页，更新 CMake 并完成全量构建。

### Task 3: 验证与发布

- [x] 运行全部 8 项测试并确认通过。
- [x] 更新 README 与开发进度。
- [x] 提交并推送 GitHub `main`。

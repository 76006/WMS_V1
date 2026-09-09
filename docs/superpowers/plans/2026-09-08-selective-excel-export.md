# Selective Excel Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为所有查询和记录页面的表格清单提供统一的 Excel 全部导出与选中行导出能力。

**Architecture:** 新建无业务依赖的 `TableExcelExport` 控制器，从 `QTableView/QTableWidget` 的模型和在线单元格控件提取可见列及数据，再调用现有 `XlsxExporter` 生成 `.xlsx`。主窗口在注册页面时自动安装表格右键菜单，并在顶部提供当前页面统一导出入口，因此现有及后续页面无需重复实现导出逻辑。

**Tech Stack:** C++17、Qt 6 Widgets、现有 OpenXML `XlsxExporter`、CMake

## Global Constraints

- 支持导出当前清单全部记录或用户选中的行。
- 只导出可见列，隐藏的数据库 ID 不进入 Excel。
- 查询页面导出当前筛选结果；多清单页面先选择需要导出的清单。
- 保持 `BUILD_TESTING=OFF`，不恢复测试文件，不执行构建或测试。

---

### Task 1: 通用表格数据提取与 Excel 写出

**Files:**
- Create: `src/ui/widgets/TableExcelExport.h`
- Create: `src/ui/widgets/TableExcelExport.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `TableExcelExport::install(QWidget *, const QString &)` 和 `TableExcelExport::exportPage(QWidget *, const QString &, QWidget *)`。

- [ ] 从模型表头生成 Excel 表头，只保留未隐藏列。
- [ ] 从普通模型数据及 `QComboBox/QLineEdit/QSpinBox/QDateEdit/QCheckBox` 在线控件读取显示值。
- [ ] 根据当前选区生成唯一行号列表，支持“全部”和“仅选中行”。
- [ ] 调用 `XlsxExporter::writeSingleSheet`，采用中文工作表名、时间戳默认文件名和现有统一样式。

### Task 2: 所有页面自动安装导出能力

**Files:**
- Modify: `src/ui/MainWindow.cpp`

**Interfaces:**
- Consumes: `TableExcelExport::install` 和 `TableExcelExport::exportPage`。

- [ ] 主窗口每注册一个业务页面时，为页面内已创建的所有 `QTableView` 自动安装右键导出菜单。
- [ ] 顶部“导出Excel”按钮导出当前页面；存在多个可见清单时显示清单选择栏。
- [ ] 选中行存在时提供“仅导出选中行”和“导出全部”，未选择行时默认导出全部。

### Task 3: 源码复核

**Files:**
- Review: `src/ui/widgets/TableExcelExport.cpp`
- Review: `src/ui/MainWindow.cpp`

**Interfaces:**
- Verifies: 查询清单、近期业务记录、统计明细以及在线表格均由同一入口覆盖。

- [ ] 检查空清单、隐藏列、重复表名和无选中行时的提示路径。
- [ ] 按用户要求仅做源码检查，不调用 CMake、编译器或测试程序。

# Sidebar Readability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore a consistently dark left navigation area with readable menu text and clearly distinguishable interaction states.

**Architecture:** Give the sidebar scroll area and navigation container stable object names in `MainWindow`, then target those objects from the shared Qt stylesheet. Extend the existing UI smoke path to verify that the named sidebar surfaces are constructed, while visual rules remain centralized in `resources/styles.qss`.

**Tech Stack:** C++17, Qt 6.10 Widgets, Qt Style Sheets, CMake, CTest

## Global Constraints

- Keep the sidebar width, menu ordering, permission behavior, and page layout unchanged.
- Use `#172033` for the full sidebar background and `#E2E8F0` for normal navigation text.
- Use 14px text at weight 500, `#26344D` for hover, `#2563EB` for checked, and `#64748B` for disabled navigation.
- Preserve visible keyboard focus with a `#93C5FD` border.
- Do not change business logic or database behavior.

---

### Task 1: Fix sidebar surfaces and navigation states

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/ui/MainWindow.cpp`
- Modify: `resources/styles.qss`
- Test: existing CTest target `WmsUiSmoke`

**Interfaces:**
- Produces: child widgets named `sidebarScroll` and `sidebarNav`.
- Consumes: the existing `--ui-smoke-test` command-line path and `[nav="true"]` button property.

- [ ] **Step 1: Strengthen the UI smoke contract**

After constructing `MainWindow` in `--ui-smoke-test` mode, require both named sidebar surfaces:

```cpp
const bool sidebarReady = window.findChild<QWidget *>(QStringLiteral("sidebarScroll"))
                       && window.findChild<QWidget *>(QStringLiteral("sidebarNav"));
return sidebarReady ? 0 : 1;
```

- [ ] **Step 2: Run the smoke test and verify failure**

Run:

```powershell
cmake --build build-qt610 -j 4
ctest --test-dir build-qt610 --output-on-failure -R WmsUiSmoke
```

Expected: `WmsUiSmoke` fails because the two object names do not exist yet.

- [ ] **Step 3: Name the sidebar surfaces**

In `MainWindow::buildUi`, name the existing widgets without changing their hierarchy:

```cpp
scroll->setObjectName(QStringLiteral("sidebarScroll"));
navWidget->setObjectName(QStringLiteral("sidebarNav"));
```

Remove the inline transparent background from `navWidget` so the shared stylesheet is the single source of truth.

- [ ] **Step 4: Apply high-contrast sidebar styling**

Add explicit scroll-area, viewport, and navigation-container rules and update the navigation button states:

```css
QScrollArea#sidebarScroll,
QScrollArea#sidebarScroll QWidget#qt_scrollarea_viewport,
QWidget#sidebarNav {
    background: #172033;
    border: none;
}

QPushButton[nav="true"] {
    color: #E2E8F0;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 10px 14px;
    text-align: left;
    margin: 1px 8px;
    font-size: 14px;
    font-weight: 500;
}

QPushButton[nav="true"]:hover { background: #26344D; color: white; }
QPushButton[nav="true"]:checked { background: #2563EB; color: white; font-weight: 600; }
QPushButton[nav="true"]:focus { border-color: #93C5FD; }
QPushButton[nav="true"]:disabled { color: #64748B; background: transparent; border-color: transparent; }
```

Place the disabled navigation rule after the generic `QPushButton:disabled` rule so navigation-specific colors win.

- [ ] **Step 5: Build and run all tests**

Run:

```powershell
cmake --build build-qt610 -j 4
ctest --test-dir build-qt610 --output-on-failure
```

Expected: all 5 test targets pass.

- [ ] **Step 6: Verify the rendered sidebar**

Launch `IceBeautyWms.exe` and confirm the brand area, scroll viewport, navigation container, and lower empty area are all `#172033`; normal menu text is readable; selected, hover, disabled, and keyboard-focus states remain visually distinct; text is not clipped at 100% Windows scaling.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp src/ui/MainWindow.cpp resources/styles.qss docs/superpowers/plans/2026-09-07-sidebar-readability.md
git commit -m "fix: improve sidebar navigation contrast"
```

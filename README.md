# LyCode

<div align="center">
  <img src="assets/icons/linux/hicolor/256x256/apps/lycode.png" alt="LyCode" width="128" height="128" />
</div>

LyCode 是 **Qt6 / C++ 原生桌面编码代理**：工作区与会话管理、流式对话、工具调用与权限确认、子 Agent、后台任务、MCP、Skills、语法高亮。

- **UI**：Qt Widgets（不是 QML——Widgets 对自绘对话流与工具卡片更可控）
- **构建**：CMake + Ninja，C++20，产物是**单一可执行文件**
- **本地优先**：配置、会话与日志都落在 `~/.lycode/`，不经过任何中转服务

## 常用命令

| 目标 | 命令 |
| --- | --- |
| 配置 | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo` |
| 构建 | `cmake --build build` |
| 运行 | `./build/lycode` |
| 打开 debug 日志 | `./build/lycode --verbose` |
| 测试 | `ctest --test-dir build --output-on-failure` |
| 安装 | `cmake --install build --prefix ~/.local` |

## 环境要求

- CMake ≥ 3.21、Ninja（或 Make）
- 支持 C++20 的编译器（GCC 12+ / Clang 15+ / MSVC 2022）
- Qt **6.4+**，需要模块 `Core`、`Gui`、`Widgets`、`Network`、`Sql`、`Concurrent`（测试另需 `Test`）
  - 6.5 起有 `QStyleHints::colorScheme()`，6.4 走调色板回退

Debian / Ubuntu / Deepin：

```bash
sudo apt install qt6-base-dev qt6-base-dev-tools qt6-tools-dev cmake ninja-build g++
```

## 构建与运行

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/lycode
```

未显式指定 `CMAKE_BUILD_TYPE` 时默认 `RelWithDebInfo`。构建产物只有 `build/lycode` 一个可执行文件：业务逻辑放在 `OBJECT` 库 `lycode_lib` 里，让测试与可执行文件共享同一份编译结果，同时不产生归档文件。测试目标默认开启，可用 `-DLYCODE_BUILD_TESTS=OFF` 关闭。

命令行目前只有 `--verbose` 一个参数，用来打开 debug 级日志。

### 首次使用

1. 在左侧选择**工作区**（项目目录）。
2. 打开 **设置 → Provider** 新增 Provider：Anthropic Messages 填 `https://api.anthropic.com`，或选 OpenAI 兼容填网关地址；模型列表每行一个模型 id。
3. 在输入框下方选择**模式**、**模型**与**思考等级**，输入任务后 `Enter` 发送（`Shift+Enter` 换行）。

会话模式：`plan`（只读，副作用工具直接拒绝）、`build`（默认，读写与命令都要确认）、`edit`（写文件免确认）、`yolo`（全部放行）。各项能力的详细用法与边界见 [docs/FEATURES.md](docs/FEATURES.md)。

## 配置与数据目录

数据都在 `~/.lycode/qt/` 下：

```
settings.json    应用配置（Provider、主题、字号、最近工作区）
sessions.db      会话与消息（SQLite，WAL 模式）
logs/lycode.log  日志
```

| 变量 | 用途 |
| --- | --- |
| `LYCODE_DATA_BASE_DIR` | 覆盖数据根目录（默认 `~/.lycode`），数据写入其下的 `qt/` |

## 安装

```bash
cmake --install build --prefix ~/.local
```

会安装 `bin/lycode` 与 `share/icons/hicolor/<size>x<size>/apps/lycode.png`。Windows 上图标编进可执行文件资源（`.ico` + `.rc`），窗口图标与文件图标同源。仓库目前不含 `.desktop` 文件，桌面环境需要的话请自行添加。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

12 个套件，全部使用 `QT_QPA_PLATFORM=offscreen`，不需要显示服务。覆盖领域模型、权限链、Markdown、Agent 主循环（真实 HTTP + SSE）、MCP（拉起的真实子进程）、Skills、Diff、语法高亮、文件查看器与界面级端到端；套件清单见 `tests/CMakeLists.txt`。

Windows CI 只跑其中 10 个：`test_agent_runtime` 与 `test_ui_flow` 依赖 POSIX shell。

## 仓库结构

| 路径 | 职责 |
| --- | --- |
| `src/core` | 领域模型（`Types`）、JSON 辅助、日志、ID 生成 |
| `src/model` | Provider 抽象、SSE 增量解析、Anthropic / OpenAI 兼容流式客户端 |
| `src/tools` | 工具契约（metadata 驱动）、注册表、12 个内置工具、统一 diff、后台任务注册表 |
| `src/mcp` | MCP stdio 客户端与管理器（远端工具适配成普通工具） |
| `src/skills` | Skill 发现与解析（`SKILL.md` / frontmatter） |
| `src/agent` | Agent 主循环、turn 相位机、权限门、系统提示词组装 |
| `src/storage` | SQLite 会话持久化 |
| `src/ui` | 主题令牌、Markdown 渲染、对话流、工具卡片、对话框、文件查看器、主窗口 |
| `tests` | 12 个测试套件；`support/` 下是假网关与假 MCP 服务器（后者是被拉起的真子进程） |
| `assets` | 图标资源树（Linux hicolor PNG、Windows ICO + RC）与内置模型目录 |
| `docs` | 功能与状态文档 |

`src/ui/MainWindow.*` 同时是**组装点**：它持有存储、注册表与运行时并把它们连起来。当前只有一个窗口与一个运行时，额外的 Application 层只会让所有权变模糊。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/FEATURES.md](docs/FEATURES.md) | 各项功能的用法与边界：后台任务、子 Agent、MCP、Skills、图片、Diff、侧边栏… |
| [docs/STATUS.md](docs/STATUS.md) | 功能状态、已知未实现与能力边界 |

## 许可

见仓库根目录的 `LICENSE`（当前仓库尚未包含该文件）。

# ZCode Qt

ZCode 的 **Qt6 / C++ 原生桌面实现**。基于同目录下的 [ZCode-npm](../ZCode-npm)（TypeScript + Electron 版）的设计规范与 Agent 语义重写。

- **UI**：Qt Widgets（不是 QML —— 本机只安装了 `qt6-base-dev`，且 Widgets 对自绘对话流／工具卡片更可控）
- **构建**：CMake + Ninja，C++20
- **Agent 运行时**：纯 C++（不依赖 Node）

---

## 与 npm 版的关系

npm 版是一个约 90 万行的 TypeScript monorepo。Qt 版**不是**它的包装层，而是用 C++ 重新实现了同一套产品语义。

重写时严格保留的东西：

| 维度 | 处理方式 |
| --- | --- |
| **词表** | 会话模式 `plan/build/edit/yolo/auto`、工具状态六态、权限决策 `allow/deny/escalate/modify`、风险四档、失败原因码等全部照抄，保证术语与文案不分叉 |
| **工具策略** | 完全由声明式 `ToolMetadata` 驱动（`readOnly`/`destructive`/`concurrentSafe`/`sideEffectScope`/`riskLevel`/`needsApproval`/`alwaysAsk`/`stopTurnOnSuccess`）。调度器与权限服务**从不按工具名猜测行为** |
| **权限判定** | 保留固定判定链：显式规则 → bypass → plan 只读 → 只读直通 → 模式策略 → 询问用户 |
| **设计令牌** | 直接取原版生效主题 `zai-light`/`zai-dark` 的 CSS 变量值（`packages/ui/src/styles.css`），字号走 `text-ui-*` 阶梯 |
| **系统提示词** | 分段顺序固定（身份 → 环境 → 工具规范 → 模式约束 → 项目说明），且**工具 schema 只走 provider 的 `tools` 字段，不镜像进提示词** |

有意偏离的东西：

| 维度 | npm 版 | Qt 版 | 原因 |
| --- | --- | --- | --- |
| 对话数据模型 | V4 `row` + `snapshot` + `delta`（扁平行 + 日志 + 窗口） | `Message` + `Part` 树 | V4 的 row/delta 机制是为**跨进程 wire 的断线重放**服务的。Qt 版把运行时与 UI 放在同一进程，没有 wire，重放机制没有收益，只会引入无谓复杂度 |
| 进程边界 | Electron main ↔ Host ↔ CLI 三层 | 单进程 | 同上 |
| 增量投递 | logEpoch + seq + 快照/增量重放 | Qt 信号 | 同进程内直接用信号，无需序列号与重放 |
| 输入排队 | `CommandInbox` 串行 admission | UI 层职责 | 运行时只保证"一次只有一个 turn"，排队交给 UI（当前 UI 直接拒绝运行中的提交） |

更详细的取舍记录见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

---

## 构建

### 依赖

- CMake ≥ 3.21、Ninja（或 Make）
- 支持 C++20 的编译器（GCC 12+ / Clang 15+ / MSVC 2022）
- Qt **6.5+**，需要模块：`Core`、`Gui`、`Widgets`、`Network`、`Sql`、`Concurrent`、`Test`

Debian / Ubuntu / Deepin：

```bash
sudo apt install qt6-base-dev qt6-base-dev-tools qt6-tools-dev cmake ninja-build g++
```

### 编译

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/zcode-qt
```

`--verbose` 打开 debug 级日志。日志与数据都在 `~/.zcode/qt/`（可用 `ZCODE_DATA_BASE_DIR` 覆盖根目录）：

```
~/.zcode/qt/settings.json   应用配置（Provider、主题、字号、最近工作区）
~/.zcode/qt/sessions.db     会话与消息（SQLite，WAL 模式）
~/.zcode/qt/logs/zcode-qt.log
```

### 测试

```bash
ctest --test-dir build --output-on-failure
```

6 个套件、159 项断言，全部使用 `QT_QPA_PLATFORM=offscreen`，不需要显示服务。

| 套件 | 断言 | 覆盖 |
| --- | ---: | --- |
| `test_types` | 97 | 领域模型、全部枚举词表的双向转换、JSON 往返、未知枚举降级、路径/权限策略函数 |
| `test_permission_gate` | 16 | 权限判定链、规则匹配（前缀 / `*` / `prefix:*` / allow 优先）、异步裁决、取消收尾、重复请求幂等 |
| `test_markdown` | 19 | 标题、围栏代码块、行内代码与强调规则顺序、表格、任务列表、HTML 转义、不安全链接 scheme 拦截 |
| `test_agent_runtime` | 16 | **真实 HTTP + SSE**（本地假网关）驱动完整 Agent 循环：纯文本轮、只读工具免确认、Bash 权限放行/拒绝、plan 模式拦截、中断、未知工具、HTTP 401 |
| `test_ui_flow` | 3 | **界面级端到端**：构造真实 MainWindow → 输入 → 点击发送 → 等待权限弹窗 → 点击"允许一次" → 断言对话流、工具卡片与会话落盘；另断言思考等级下拉可见且默认档位正确、上下文分母用的是覆盖值 |
| `test_app_config` | 6 | 配置持久化：全字段 JSON 往返、模型能力覆盖的写入/删除、思考档位（含 `off` 这个合法空串）、真实落盘往返与首次启动的默认值 |

`test_ui_flow` 会用 `QWidget::grab()` 把界面截图写到 `build/tests/ui-screenshots/`（权限弹窗、对话流、展开的工具卡片），作为渲染效果的证据。这些截图每次跑测试都会重新生成。

测试通过公开的 Qt 控件 API 驱动（`findChild` + `click()`），没有为测试在生产代码里留后门。

#### 验证过程中发现并修复的缺陷

这些都不是编译期能发现的，记录下来是因为它们说明了为什么值得写端到端测试：

1. **`findProjectRoot` 死循环**。`QDir::cleanPath("/..")` 在 Qt 里返回 `"/.."` 而不是收敛到根，于是"路径向上走、与自身比较"的终止条件永不成立。该函数在每次组装系统提示词时都会调用，在没有 `.git` 的目录树下会让应用直接卡死。改用 `QDir::cdUp()`（它在根上正确返回 false）并加了深度上限兜底。
2. **主窗口从未注册内置工具**。`ToolRegistry` 默认构造是空的，漏掉 `createWithBuiltins()` 的表现是模型收到零个工具、所有工具调用报 `tool_not_found`，没有任何启动期报错。
3. **正文完全不可见**。`QTextBrowser` 的高度是手动算一次再 `setFixedHeight` 的，但构造期文档还没按最终宽度排版，`document()->size()` 返回 0，控件被永久锁成 4px。改为由 `documentSizeChanged` 驱动高度。
4. **用户中断被当成故障**。用户点"停止"时 provider 以"请求已取消"收尾，原先被映射为 `TurnResult::Failed`（UI 报错、会话进入 Error）。现在识别 `abortRequested_` 并归为 `Interrupted`。
5. **合成的工具结果轮被显示成用户消息**。工具输出已经渲染在工具卡片里，重复展示会多出一条用户从未说过的"你"的消息。新增 `Message::modelOnly`（对应 npm 的 `visibility: "model-only"`）并在对话流中跳过。
6. **新建会话无法落盘**（`NOT NULL constraint failed: session.title`）。默认构造的 `QString` 是 *null* 而不只是空，`QSQLITE` 会把它绑成 SQL NULL。为 NOT NULL 文本列加了统一的归一绑定。
7. **日志过滤规则被 Qt 静默忽略**。`zcode.*=info` 不是合法语法（type 只能是 debug/info/warning/critical），整条规则被判为 malformed，结果是非 verbose 模式也输出 debug 日志。
8. **工具 schema 一度被复制进系统提示词**。这违反"schema 只走 provider 的 `tools` 字段"的既定约束，会造成两处定义漂移。已改为只写跨工具的行为准则。
9. **`prefix:*` 规则匹配过窄**。`stripTrailingWildcard` 原来只剥裸 `*`，导致 `npm:*` 变成 `npm:`，匹配不到 `npm install`。现在同时识别 `:*` 写法。
10. **侧边栏工作区行被裁切**（用户报告："左上角新建会话上面显示不全"）。原因是我把 `QVBoxLayout` 塞进了 `QPushButton` 来承载「名称 + 路径」两行，而 `QPushButton` 的 `sizeHint` 只按自身文本计算，子控件被裁掉。改为：按钮只承担名称与键盘可达性，路径另用一个可点标签承载，并按标签宽度做中间省略。
11. **会话列表项被撑成十几行高**。消息正文是 Markdown 源，直接当摘要塞进列表项会带出 `###`、列表符号和换行。改为统一经 `Markdown::toPlainPreview` 折叠成单行纯文本。

12. **模型能力表格的表头退化成列号**。`QTableWidget::clear()` 会连**表头文字一起清空**，只在构造函数里设一次 `setHorizontalHeaderLabels()`，第一次 reload 之后表头就变成 1/2/3。改为把表头设置抽成函数，构造与每次 reload 都调用。
13. **模型能力表格一打开就横向滚动**。五列固定宽度之和超过了设置页右栏可用宽度，"默认档位"那列根本看不到。同时收窄了 Provider 列表、加宽对话框、并把 ` tokens` 后缀从输入框移到分组提示里（单位由提示承载，宽度还给"模型"列）。

14. **改完"上下文窗口"后工具条仍显示旧值**（用户报告："我设置了 1000000 的上下文，上方显示的是 128k"）。覆盖本身生效了，但**没有任何东西触发重新测量**——`refreshContextUsage()` 只在新建会话、载入会话、turn 结束时调用。于是界面一直显示旧分母，只有等下一个回合结束才更新，看起来就是"设置没生效"。修法：新增 `AgentRuntime::remeasureContext()`，设置保存后与 `setModel()` 里都调用它；另外在**没有会话**时用有效模型元信息兜底显示分母，让用户一改就能看到反馈。这条已加回归测试，并验证过"去掉修复即失败"。

15. **重启后打不开之前的会话**（用户报告："之前执行过的对话，关闭软件后无法打开"）。`onSessionSelected` 在 `loadSession()` **之后**才调用 `conversation_->clear()`，而 `loadSession()` 会为每条历史消息发 `messageAdded` 并由窗口塞进对话流——于是刚载入的历史被立刻抹掉，点开会话是空的。改为"先清空视图，再让 runtime 驱动填充"，并让新建会话走同一顺序，使这条不变式只有一种写法。
16. **消息控件的几何是默认值、一条都画不出来**。`QWidget::setParent()` 会把控件置为隐藏，而 `QLayout` 会**直接跳过隐藏的控件**（不给它布局几何）。消息"存在"（`messageCount()` 正确）但界面全空。动态创建的控件必须显式 `show()`。
17. **消息之间出现大片空白**。`QAbstractScrollArea` 的默认 `sizeHint()` 是 `256x192`；只要竖向策略里没有 `ShrinkFlag`，Qt 的 `qSmartMinSize` 就取 `max(sizeHint, minimumSizeHint)` 当最小高度——192 会盖过 `setFixedHeight()`（实测内容只有 23 却占 192）。而且高度依赖宽度（换行数随宽度变），`sizeHint()` 在布局期被问到时文档还没拿到最终宽度，存在循环依赖。最终用 `heightForWidth()` 按目标宽度量高度（带宽度缓存，避免每次布局都做一次 HTML 往返）。

### 一个测试上的坑

`test_ui_flow` 截图时**必须从整窗截图裁剪**，不能用 `childWidget->grab()`：单独渲染子控件时 Qt 拿不到祖先级样式表的解析结果，控件会退回系统浅色配色，得到一张与真实外观完全不符的图片。我一开始就是这么截的，结果侧边栏看起来是浅色主题，差点误判。

---

## 首次使用

1. 启动后用左侧的**工作区**行选择项目目录。
2. 打开 **设置 → Provider**，新增一个 Provider：
   - **Anthropic Messages**：Base URL 填 `https://api.anthropic.com`，API Key 填 `sk-ant-...`
   - **OpenAI 兼容**：Base URL 填 `https://api.openai.com/v1`（或自建网关地址），API Key 按需
   - **模型列表**每行填一个模型 id，例如 `claude-sonnet-4-20250514`
3. 在输入框下方那一行选择**模式**（左下）、**模型**与**思考等级**（发送按钮左侧）。
4. 在底部输入框描述任务，`Enter` 发送，`Shift+Enter` 换行。

会话模式的含义：

| 模式 | 行为 |
| --- | --- |
| `plan` | 只读。带副作用的工具会被**直接拒绝**（不弹窗），产出计划供审阅 |
| `build` | 默认。读写文件与执行命令都需要确认 |
| `edit` | 文件写入自动放行，执行命令仍需确认 |
| `yolo` | 全部放行。请自行判断风险 |

### 工具并行调度

同一次模型响应里可能要求多个工具调用。调度规则照搬 npm 的 `ToolScheduler` + `batch-runner`：

```
调用序列（模型给出的顺序）:  Read   Grep   Write   Read   Edit
分组结果:                    └── 组 1 ──┘  组 2   组 3  组 4
                             组内并行      独占    独占   独占
执行:                        组间串行 ──────────────────────→
```

| 规则 | 说明 |
| --- | --- |
| 组内并行、组间串行 | 同一组内同时执行，上一组全部结束才开下一组 |
| 组内上限 | `kMaxToolConcurrency = 10`（与 npm 的默认 `maxConcurrency` 一致） |
| 独占组 | `canRunInParallel() == false` 的工具单独成组，与前后调用形成串行屏障 |
| 终止语义 | 某调用要求终止本轮时，**等同组兄弟全部结束**再中断后续组 |
| 失败不截断 | 一个工具失败只影响它自己的结果，后续组照常执行 |

**并发策略是三态**（`ToolMetadata::Concurrency`）：`Safe` / `Serial` / `Unspecified`。这一点很关键——npm 里 `concurrentSafe` 是 `boolean | undefined`，用 C++ 的 `bool` 会把"显式声明必须串行"和"未声明"混为一谈，于是显式声明串行的**只读**工具会被"只读且无副作用 ⇒ 可并发"的豁免分支错误放行。这个缺陷是在写并行测试时被真实暴露出来的。

内置工具的分类：`Read` / `Glob` / `Grep` / `TodoRead` 可并发；`Bash` / `Write` / `Edit` / `TodoWrite` 串行（`Bash` 破坏性，`TodoWrite` 虽然 `readOnly` 但会写会话状态）。

与 npm 的差异：npm 的调度器还做依赖图的拓扑排序，因为工具可以声明 `dependencies`。本实现的工具不声明依赖，顺序约束只来自模型给出的调用次序，所以"顺序扫描 + 独占组"就是完整语义，不需要拓扑排序。

**结果顺序与完成顺序无关**：回灌给模型的工具结果始终按模型给出的调用次序排列，所以并行不会让对话历史变得不确定。

### 用量明细

状态栏右侧显示四个缓存相关指标（会话累计）：

```
就绪            上下文 44 / 1.0M ▬▬▬   缓存 71% · 未缓存 300 · 缓存读 750 · 输出 50
```

| 指标 | 含义 |
| --- | --- |
| **缓存命中率** | `缓存读取 /（缓存读取 + 未缓存输入）` |
| **未缓存** | 真正要模型从头处理的输入 token |
| **缓存读** | 命中缓存、直接从缓存读取的输入 token |
| **输出** | 生成的 token |

悬浮提示里给出四个字段的精确数字、会话累计与**最近一轮**的对比，以及命中率的分母口径。

两点值得说明：

- **分母不含"缓存写入"**。那是首次写入缓存的部分，本来就没有机会命中；算进分母会系统性低估命中率。
- **两种协议的口径已经归一**。Anthropic 的 `input_tokens` **不含**缓存部分（读/写单列），OpenAI 的 `prompt_tokens` **含**缓存部分（`prompt_tokens_details.cached_tokens`）。如果照原样落进同一个字段，同一个 `inputTokens` 就有了两种含义、命中率必然算错。现在统一为「`inputTokens` = 未命中缓存的输入」，OpenAI 路径会减掉缓存部分（并对 `cached > prompt` 的脏数据做 clamp）。

### 输入区布局

模型、思考等级、模式三者都在**输入框下方那一行**，而不是顶部工具条——它们影响的是"这一次发送"，贴着输入框才符合"先设参数再发送"的顺序：

```
┌──────────────────────────────────────────────────────┐
│  描述你想完成的任务…（Enter 发送，Shift+Enter 换行）      │
├──────────────────────────────────────────────────────┤
│  [build 默认 ▾]        [smoke-model ▾][思考 高 ▾][停止][发送] │
└──────────────────────────────────────────────────────┘
   状态栏： 完成。            上下文 44 / 1.0M ▬▬▬    累计 180 tokens
```

上下文用量（分母 + 进度条）在**状态栏**右侧。原先它在顶部工具条，三个选择器搬走后顶部只剩这一项，与其留一条只有一项的横栏，不如并入本来就在展示用量的状态栏。

### 思考等级

输入框下方、发送按钮左侧的**思考等级**下拉控制模型的推理强度。档位是**稳定 id**（`off` / `low` / `medium` / `high` / `max`），因为它要持久化到配置文件里，一旦发布就不能改名：

| 档位 | Anthropic（`thinking.budget_tokens`） | OpenAI 兼容（`reasoning_effort`） |
| --- | ---: | --- |
| `off` | 不传 thinking 字段 | 不传 `reasoning_effort` |
| `low` | 2048 | `low` |
| `medium` | 8192 | `medium` |
| `high` | 24576 | `high` |
| `max` | 49152 | `high` |

要点：

- 同一件事在两种协议里表达不同（Anthropic 用 token 预算，OpenAI 用枚举），Agent 循环按档位**同时填好两个字段**，provider 各取所需。`off` 的实现是**完全不传字段**，而不是传空值——部分兼容网关见到未知字段会直接 400。
- Anthropic 要求 `budget_tokens >= 1024` 且 `< max_tokens`，`clampReasoningBudget()` 会做一次收敛，保证发出的请求一定合法（`maxOutputTokens` 太小时会自动关闭思考而不是发出一个会被拒的请求）。
- **不支持思考的模型不显示这个下拉**，用户不会看到一个永远禁用的空控件。
- 每个模型上次用的档位会被记住（按 `providerId/modelId` 存），切换模型时自动恢复，不会静默重置。

### 模型上下文大小

Provider 自报的上下文窗口经常不准（第三方兼容服务尤其如此），而**窗口值错了会直接导致压缩阈值判断失误**。所以设置页里可以对每个模型覆盖：

- **上下文窗口**（`0` = 沿用内置默认，通常 128000，Claude 系列 200000）
- **最大输出**（`0` = 沿用默认）
- **思考档位**（该模型可用的档位列表，留空 = 用标准集合）与**默认档位**

覆盖的生效路径是**单点**的：`ProviderRegistry::setModelOverrides()` 会把覆盖应用到 `resolve()` 与 `allModels()` 返回的每一份 `ModelInfo` 上，因此状态栏的上下文用量分母、模型选择器、以及 Agent 循环里的压缩判断全都自动拿到有效值，不需要各处分别查询设置。

上下文用量在**会话一建立就显示**（此时已用为 0，但分母已知），改完设置立刻能看到反馈。

权限弹窗里的「始终允许」会固化一条规则（按工具 + 路径/命令前缀匹配），后续同类调用不再打扰。规则只存在内存里，切换会话即失效。

---

## 代码结构

```
src/
├── core/       领域模型（Types）、JSON 辅助、日志、ID 生成
├── model/      Provider 抽象、SSE 增量解析、Anthropic / OpenAI 兼容流式客户端
├── tools/      工具契约（metadata 驱动）、注册表、8 个内置工具、统一 diff
├── agent/      Agent 主循环、turn 相位机、权限门、系统提示词组装
├── storage/    SQLite 会话持久化
└── ui/         主题令牌、Markdown 渲染、对话流、工具卡片、对话框、主窗口
tests/          单元测试（领域模型、权限链、Markdown 渲染）
```

`src/ui/MainWindow.*` 同时是**组装点**：它持有存储、注册表与运行时并把它们连起来。当前只有一个窗口与一个运行时，额外的 Application 层只会让所有权变模糊。

### 关键不变量

这些是阅读或修改代码前必须知道的约定：

1. **工具策略只有一个来源**：`ToolMetadata`。不要在调度器或权限服务里按工具名加分支。
2. **权限只有一个判定点**：`PermissionGate`。UI 不直接改权限规则，只提交裁决。
3. **工具结果用独立的 user 消息承载**（全部 part 都是 `Tool` 类型）。两种 provider 协议的线格式都要求这样。
4. **未终态的工具必须在会话结束时关闭**（`Message::cancelPendingTools`），否则 UI 永远显示"运行中"。
5. **配置只有一个写盘入口**：`AppConfig::save`。
6. **枚举解析遇到未知值降级并告警，绝不丢弃整条记录**。时间线 part 会保留原始字面量。

---

## 功能状态

### 已完成

- 领域模型与 JSON 往返（含未知枚举降级）
- SSE 增量解析器（按字节缓冲，正确处理帧被切开与 UTF-8 边界）
- Anthropic Messages 流式客户端（含 thinking 块、prompt cache 字段）
- OpenAI Chat Completions 兼容流式客户端（含 `stream_options.include_usage`、按 index 组装工具调用）
- 8 个内置工具：`Bash`、`Read`、`Write`、`Edit`、`Glob`、`Grep`、`TodoRead`、`TodoWrite`
- 权限门：5 步判定链 + 规则匹配（前缀 / `*` / `prefix:*`）+ 异步裁决 + 取消收尾
- Agent 主循环：模型步 ↔ 工具队列循环、turn 相位、中断、安全上限
- **工具并行调度**：按 `canRunInParallel` 分组，组内并行（上限 10）、组间串行；不可并行的工具独占一组形成串行屏障
- **思考等级**：输入区选择器 + 两种协议的字段映射（Anthropic `thinking.budget_tokens` / OpenAI `reasoning_effort`），按模型记住上次档位
- **模型能力覆盖**：每个模型的上下文窗口 / 最大输出 / 思考档位都可在设置页覆盖，经 `ProviderRegistry` 单点应用到所有读取路径
- 用量明细：缓存命中率 / 未缓存 / 缓存读 / 输出（两种协议的 token 口径已归一）
- 会话持久化（SQLite，含 `sequence` 幂等语义）
- 系统提示词组装（含 `AGENTS.md` 逐级向上查找、项目上下文探测）
- 主题令牌（原版 `zai-light`/`zai-dark` 真实取色）+ 完整 QSS
- 界面：工作区/会话列表、流式对话流、Markdown（标题/代码块/表格/引用/任务列表）、工具调用卡片、权限确认弹窗、设置页、菜单与快捷键
- 单元测试

### 已知未实现 / 部分实现

| 项 | 状态 | 说明 |
| --- | --- | --- |
| `Bash` 后台任务 | **未实现** | `run_in_background` 会明确返回"不支持"，不假装成功 |
| 子 Agent | **未实现** | `Task` / `Agent` 工具及其子会话、上下文隔离、回传 |
| MCP | **未实现** | 无 MCP server 接入 |
| 上下文压缩 | **未实现** | 已估算用量并在 UI 显示压力，但还没有 microcompact / full compact |
| 代码语法高亮 | **未实现** | 代码块用等宽字体渲染，无着色 |
| Diff 视图 | **未实现** | `Write`/`Edit` 已生成 unified diff 并写入 metadata，但还没有专门的渲染控件 |
| 集成终端 | **未实现** | |
| 国际化的实际翻译 | **未实现** | 语言选项只做持久化；界面文案目前是中文 |
| 图片附件 | **未实现** | 数据模型与 provider 层已支持（`FilePart` 带 `image/*` mime），输入框还没有附加图片的入口 |
| 远程工作区 | **未实现** | `Workspace` 已保留 `identity` 与 `remoteSessionId` 字段 |
| 会话标题的模型生成 | **未实现** | 当前取首条用户输入的前 40 字符 |
| 动态工作流 | **未实现** | |

这一节刻意写得具体：把"没做"和"做了一半"分开，避免读代码的人以为某项已经可用。

---

## 许可

与原项目一致，见 [LICENSE](../ZCode-npm/LICENSE)。

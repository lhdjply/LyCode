# LyCode 架构

这份文档记录**为什么这样设计**，而不是重复代码里已有的信息。凡是"看起来可以更简单"的地方，这里都给出当时的取舍依据。

---

## 1. 分层与状态所有权

```
┌─────────────────────────────────────────────────────────────────┐
│ ui/  MainWindow（组装点）、ConversationView、SidebarPanel、       │
│      ToolCallWidget、PermissionDialog、SettingsDialog、Theme、    │
│      Markdown、AppConfig                                        │
└───────────────────────────┬─────────────────────────────────────┘
                            │ Qt 信号/槽（同线程直连）
┌───────────────────────────▼─────────────────────────────────────┐
│ agent/  AgentRuntime ── PermissionGate                          │
│                      └─ SystemPromptBuilder                     │
└───────┬───────────────────────────────┬─────────────────────────┘
        │                               │
┌───────▼──────────┐          ┌─────────▼──────────┐
│ model/           │          │ tools/             │
│ ProviderRegistry │          │ ToolRegistry       │
│ ModelProvider    │          │ ToolMetadata       │
│ SseParser        │          │ TodoStore / Diff   │
└──────────────────┘          └────────────────────┘
        │                               │
┌───────▼───────────────────────────────▼─────────────────────────┐
│ core/  Types（领域模型）、Json、Logging、Ids                     │
│ storage/ SessionStore（SQLite）                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 唯一所有者

| 状态 | 所有者 | 说明 |
| --- | --- | --- |
| 会话与消息 | `AgentRuntime` | 只有它能改 `messages_` 与 `session_` |
| turn 相位 | `AgentRuntime::phase_` | `RunState` 由相位**推导**，不独立维护 |
| 权限判定 | `PermissionGate` | UI 不直接改规则，只提交裁决 |
| 运行中工具的状态 | `ToolPart::state` | 由 `AgentRuntime` 写入，工具本身只返回 `ToolResult` |
| 会话 todo | `TodoStore` | 由 `AgentRuntime` 注入 `ToolContext`，工具不自己持有 |
| 应用设置 | `MainWindow::settings_` | 唯一写盘入口是 `AppConfig::save` |
| Provider 实例 | `ProviderRegistry` | 由设置整体替换（`replaceAll`） |

**为什么 `RunState` 不独立维护**：如果相位和 UI 状态各存一份，就必然出现"相位说完成了、UI 还在转圈"这类不一致。让 `RunState` 成为 `TurnPhase` 的纯函数，不一致在结构上就不可能发生。

---

## 2. 与常见做法的结构差异：为什么不用 row/delta

本实现的 V4 协议把对话建模成**扁平行**（`turnHeader` / `userInput` / `assistantText` / `reasoning` / `toolCall` / ...），配合 `logEpoch` + `seq` + `snapshot`/`delta`，目的是支持**跨进程 wire 的断线重放**：客户端断线后带 `base: {logEpoch, seq}` 重新订阅，服务端从该水位续传增量，不必重传整个会话。

这套机制的存在理由是进程边界与不稳定的网络。本实现把 Agent 运行时与 UI 放在**同一个进程、同一个线程**：

- 没有 wire，就没有"断线"，也没有需要续传的水位。
- 状态变化通过 Qt 信号同步投递，顺序天然确定。
- 快照/增量两套代码路径与其一致性校验（`revision`、`coalesce`、`apply`）全部变成纯粹的负担。

因此 本实现用更简单的 `Message` + `Part` 树。**但词表照抄**：会话模式、工具状态、权限决策、风险等级、失败原因码全部按既定语义，所以术语、文案和行为语义没有分叉。

### 工具结果为什么是独立的 user 消息

`ModelRequest::messages` 的约定是：**一条 role 为 User 且全部 part 都是 Tool 类型的消息**表示"上一轮工具调用的结果"。

这不是随意选择，而是两种 provider 协议的线格式要求：

- Anthropic：`tool_result` 块必须出现在 **user** 消息里
- OpenAI：每个工具结果是一条独立的 `{"role":"tool","tool_call_id":...}` 消息

把结果挂成独立消息（而不是塞回 assistant 消息）还有一个好处：assistant 消息可以整体作为历史保留，重放与 prompt cache 前缀都更稳定。

---

## 3. Agent 主循环

### 相位

取值语义：

```
idle → processing_input → awaiting_model_response → streaming
     → scheduling_tools → executing_tools → aggregating_results
     → (awaiting_model_response | completing | error)
awaiting_permission → executing_tools | error
completing → idle
```

### 循环形状

```
submitText()
  └─ beginTurn()                    创建 user 消息、启动取消令牌
       └─ runModelStep()            ←──────────────┐
            ├─ 建 assistant 占位消息                 │
            ├─ provider->stream(request)            │
            ├─ handleStreamEvent(...)  逐事件累积     │
            └─ finishModelStep()
                 ├─ 无工具调用 → completeTurn()      │
                 └─ 有工具调用 → runToolQueue()      │
                      └─ executeToolAt(i)           │
                           ├─ 校验入参               │
                           ├─ plan 模式拦截           │
                           ├─ PermissionGate.request │
                           └─ tool->execute(...)     │
                      └─ afterToolQueue()           │
                           ├─ 工具结果 → 合成 user 消息
                           └─ runModelStep() ───────┘
```

### 终止条件

本实现**没有** `maxSteps` / `maxTurns` 硬停止（`apps/lycode-cli/AGENTS.md` 明确说明"不用工具调用次数做硬停止"），因为那会让复杂任务被无故截断。本实现保留这个判断，真实边界是：

1. 模型返回无工具调用的最终文本 → `Success`
2. 工具结果带 `stopTurnAfterResult` → `Success`
3. 用户中断 → `Interrupted`
4. 模型错误不可恢复 → `Failed`
5. `kMaxModelStepsPerTurn`（200）→ `Failed`。这是**新增的防御**，只用于兜住失控循环，不作为常规边界。

### 递归深度

工具可能同步回调（纯内存工具如 `TodoWrite`）。如果 `finishOne → runToolQueue → executeToolAt → finishOne` 直接递归，N 个工具调用会产生 N 层栈帧。因此所有"推进到下一步"的调用都走 `QTimer::singleShot(0, ...)` 延后一跳：栈深度恒定，语义不变。

---

## 4. 工具系统：metadata 驱动

这是从 本实现照搬的**最值得复用的设计**。一份 `ToolMetadata` 同时决定三件事：

| 决策 | 依赖的字段 |
| --- | --- |
| 并发分组 | `destructive` / `concurrentSafe` / `readOnly` / `sideEffectScope` |
| 权限判定 | `readOnly` / `riskLevel` / `needsApproval` / `alwaysAsk` / `sideEffectScope` |
| turn 终止 | `stopTurnOnSuccess` |

执行器与权限服务**从不按工具名猜测行为**。新增工具只需要声明 metadata，不需要在调度器或权限服务里加 `switch`。

并发判定（`Tool::canRunInParallel`）：

```
destructive             → false
concurrentSafe == true  → true
concurrentSafe == false → false
readOnly                → true
否则                     → sideEffectScope == None
```

`SideEffectScope` 把"改写工作区"（`Workspace` / `Git` / `System`）与"只影响会话"（`Session` / `UserInteraction`）分开。这个区分是权限判定的基础：`session` 作用域的工具（如 `TodoWrite`）声明 `readOnly: true` 但仍会写会话状态——两者不矛盾。

> **当前实现状态**：调度器已具备完整的并发判定，但 `AgentRuntime` 目前**按队列串行执行**工具。语义正确，性能上不是最优。这是已知的实现缺口，不是设计意图。

---

## 5. 权限判定链

顺序是固定的（先命中先返回），因为权限是安全边界，判定顺序必须可预测：

```
1. 命中已授予规则（allow 或 deny）      → 直接结论，不打扰用户
2. mode == BypassPermissions           → 放行
3. mode == Plan 且非只读                → 拒绝（不是询问）
4. kind == Read                        → 放行
5. 模式策略允许（AcceptEdits 对 Write）  → 放行
6. 其余                                 → 请求用户裁决
```

### 三个刻意的决定

**① allow 规则优先于 deny 规则。** 用户最自然的操作顺序是"先拒绝全部，再放行一条"。如果按列表顺序匹配，先出现的 deny 会永久遮蔽后面的 allow，让用户的操作看起来没生效。

**② plan 模式拒绝而不是询问。** plan 的语义是"只读"，让用户"批准"一次写入就破坏了模式的承诺。这正是 本实现 `checkPlanMode` 直接 `deny` 而不是 `ask` 的原因。

**③ `AllowAlways` 只在提交裁决时生效。** 规则变更是裁决的一部分（`PermissionResponse::permissionUpdates`），不在 `request()` 里就改状态，否则取消弹窗也会留下规则。

### 生命周期与竞态

- `resolve()` 对未知 `requestId` 返回 `false` 而不报错——UI 可能因重绘重复提交同一个裁决，这必须是幂等的。
- 同一个 `requestId` 第二次 `request()` 立即被拒绝（`duplicate_request`），避免弹两个窗、产生两条回调。
- `cancelAll()` 分两阶段：**先发完所有 `resolved` 信号，再回调**。回调可能析构 `PermissionGate` 自身（会话被关闭），所以调用回调之后绝不能再访问成员。
- 规则只存内存，切换会话即失效（与 本实现 `PermissionService` 每实例一会话的语义一致）。

---

## 6. 系统提示词

分段顺序固定，且**前缀尽量稳定**（长而稳定的前缀才能命中 provider 的 prompt cache）：

```
① 身份与总体准则
② 环境信息（工作区、平台、日期、项目上下文）
③ 工具使用通用规范
④ 会话模式约束
⑤ 项目说明文件（AGENTS.md）
```

### 工具 schema 不进提示词

工具说明**只**经 provider 的 `tools` 字段下发，不重复写进系统提示词——两处各写一份必然会漂移，而且工具名与参数会白白占掉上下文。因此 `toolingNormsSection()` 只写跨工具的行为准则（先读后改、独立调用并行发出、不臆测路径），**不列举任何工具名与参数**。

复制一份 schema 进提示词会造成两处定义漂移，并白白占用上下文预算。

### AGENTS.md 的查找规则

- 用户级：`<数据根>/AGENTS.md`，最多一份
- 工作区级：自 `cwd` 向上直到**项目根**（第一个含 `.git` 的目录），取找到的**第一个** `AGENTS.md`

工作区级刻意只取第一个：多层 `AGENTS.md` 叠加会让模型收到互相矛盾的指令，而"最靠近 `cwd` 的那份"是最具体、最该生效的。单文件上限 100 KiB（按既定语义）。

---

## 7. 流式渲染

### SSE 解析必须按字节

`QNetworkReply::readyRead` 给出的分块边界是任意的：可能把一个事件劈成两半，也可能一次给出多个事件，还可能把一个 UTF-8 多字节字符切开。因此 `SseParser` 用 `QByteArray` 缓冲，只在切出**完整事件**后才做 UTF-8 解码。帧分隔符是空行（`\n\n` / `\r\n\r\n`），不是单换行。

末尾事件可能没有收尾空行（部分服务端直接关连接），所以 `finish()` 会把缓冲区里剩下的事件也交出来。

### 两阶段渲染

逐 token 调用 `setMarkdown()` 会让整个文档重新解析与排版，长回答会明显卡顿。因此：

- **流式中**：`insertPlainText()` 直接追加纯文本，代价 O(增量)
- **完成时**：用累积的 Markdown 源做一次 `setMarkdown()`，得到正确的标题/列表/代码块/表格排版

代价是流式过程中标记符号（`**`、```）会短暂可见。这是同类产品的常规取舍，换来的是长回答不掉帧。

### 自动滚动

只在用户**本来就贴着底部**时自动跟随（阈值 60px）。用户向上翻阅历史时抢走滚动位置是非常糟糕的体验。

---

## 8. 持久化

会话在 SQLite，配置在 JSON 文件。分开的理由：

- 配置是"少写多读、整份替换"的小文档，JSON 便于用户手工检查与迁移
- 会话是"高频增量写、按条件查询"的结构化数据，SQLite 更合适

### `sequence` 幂等语义（关键约束）

`insert ... on conflict(id) do update` 时，**同一 scope 再保存必须原样保留现有 `sequence`（含 NULL）**，只有跨 scope 改绑时才分配新队尾。这是防止时间线漂移的硬约束。

实现上压进单条 SQL 的 `CASE`，避免"读旧值 → 算新值 → 写回"的中间窗口：

```sql
sequence = CASE
  WHEN message.session_id = excluded.session_id THEN message.sequence
  ELSE excluded.sequence
END
```

### 流式期间的写入策略

流式中每个 token 都整体落盘代价太高。因此：

- 流式中只更新轻量列（`status` / `usage_json` / `error_message`）与内存里的 `Part`
- 终态由 `saveMessage` 整体落盘

`message.data` 里的 `status` 因此会阶段性陈旧——**读路径以列为权威**，这是有意的取舍，不是 bug。

### 为什么部分读失败要返回空列表

`loadMessages` 在 part 批量读失败时返回**空列表**，而不是"只带头部的消息"。因为 `saveMessage` 是整体替换语义（会删除不在 `messages.parts` 里的 part 行），半个消息被调用方重新保存会**真删掉 part 数据**。

---

## 9. 主题与设计令牌

Palette 的 51 个字段全部取自两套生效配色 `zai-light` / `zai-dark` 的 CSS 变量，没有一个值是凭空臆造的。默认浅色 / 默认深色两套只作 fallback，不是实际生效的主题。

字号走 `text-ui-*` 阶梯，基准 `--ui-font-size` 默认 14px：

| 令牌 | 偏移 | 默认 |
| --- | --- | --- |
| `UiXl` | +4 | 18px |
| `UiLg` | +2 | 16px |
| `UiBase` | 0 | 14px |
| `UiCaption` | −1 | 13px |
| `UiSm` | −2 | 12px |
| `UiXs` | −4 | 10px |

`Mono` / `MonoSm` 用**独立**的代码字号（默认 14 / 12），不随界面字号缩放——这是设计规范里明确要求的：代码、Diff、终端各有自己的字号设置。

界面用 `Fusion` 样式作基底：Linux 的原生样式（gtk/dde）会忽略部分 QSS 规则，导致主题令牌在个别控件上失效。Fusion 对 QSS 的支持最完整，跨平台表现也最一致。

---

## 10. 已知缺口

按"对可用性的影响"排序：

1. **工具串行执行** —— 语义正确但慢。并发判定已就绪，只差调度器接线。
2. **无上下文压缩** —— 长会话会撞上下文窗口。用量已估算并在 UI 显示压力，但没有 microcompact / full compact。
3. **子 Agent 未实现** —— `Task` / `Agent` 工具缺失，复杂任务无法拆分。
4. **`Bash` 后台任务未实现** —— `run_in_background` 明确返回不支持（不假装成功）。
5. **MCP 未接入** —— 无法使用外部工具服务。
6. **无代码语法高亮 / Diff 视图** —— `Write`/`Edit` 已生成 unified diff 并写入 `ToolPart::metadata`，只差渲染控件。
7. **无图片附件入口** —— 数据模型与 provider 层已支持（`FilePart` + `image/*` mime），缺输入框的附加入口。
8. **国际化只做了持久化** —— 界面文案目前是中文硬编码。
9. **无集成终端** —— 未实现。
10. **远程工作区未实现** —— `Workspace` 已保留 `identity` 与 `remoteSessionId` 字段作为接口预留。

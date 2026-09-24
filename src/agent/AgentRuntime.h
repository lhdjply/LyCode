// LyCode — Agent 运行时
//
// 这是把 本实现 turn 循环用 C++ 重写的核心。与 本实现的结构对应关系：
//
//   本实现                                        Qt
//   ─────────────────────────────────────────  ──────────────────────────────
//   TurnMachine（相位机）                       内部 TurnPhase + advanceTo()
//   runRegularTurnLoop                        runModelStep() ↔ runToolQueue()
//   runModelBackedTurnStep                    单个 model step
//   executeToolCallsForModelStep               工具批次调度（组内并行、组间串行）
//   PermissionService + broker                 PermissionGate
//   ContextBuilder                             SystemPromptBuilder
//   SessionStore (SQLite)                      SessionStore
//   MessageHistory                              messages_ (QList<Message>)
//
// ── 并发模型 ──────────────────────────────────────────────────────────────
// 单线程。所有会话状态只属于本对象，只在 GUI 线程访问，因此不需要锁。
// 网络流（ModelStream）与子进程（QProcess）都是异步的，用信号回到本线程。
//
// ── 终止条件（按既定语义）──────────────────────────────────────────────
// 主循环**没有 maxSteps / maxTurns 硬停止**（这是 本实现的既定设计：
// 用工具调用次数做硬停止会让复杂任务被无故截断）。真实边界是：
//   1. 模型返回无工具调用的最终文本  → 正常完成
//   2. 工具结果要求终止本轮（stopTurnAfterResult）
//   3. 用户取消
//   4. 模型错误不可恢复
//   5. 上下文超窗且压缩失败           → 失败
//   6. 工具调用轮数达到安全上限       → 失败（本实现新增的保护，默认很大）
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>

#include "agent/ContextCompactor.h"
#include "agent/PermissionGate.h"
#include "core/Types.h"
#include "storage/SessionStore.h"
#include "skills/SkillLibrary.h"
#include "tools/SubagentHost.h"
#include "tools/Tool.h"

namespace lycode
{

class ProviderRegistry;
class ModelStream;

/// turn 相位。取值turn-state.ts 对齐。
enum class TurnPhase {
  Idle,
  ProcessingInput,
  AwaitingModelResponse,
  Streaming,
  SchedulingTools,
  ExecutingTools,
  AggregatingResults,
  AwaitingPermission,
  Completing,
  Error,
};

QString toToken(TurnPhase phase);

/// 运行时状态（面向 UI）。由 TurnPhase 推导，不独立维护，
/// 避免"两个状态各说各话"。
enum class RunState {
  Idle,
  Streaming,          ///< 模型正在产出
  ExecutingTools,     ///< 工具正在执行
  WaitingPermission,  ///< 等待用户裁决
  Cancelling,         ///< 收到取消，正在收尾
  Failed,
};

QString toToken(RunState state);

/// 一次运行的结果。
enum class TurnResult {
  None,
  Success,
  Interrupted,
  Failed,
  Skipped,  ///< 因容量或模式限制未执行
};

class AgentRuntime : public QObject, public SubagentHost
{
    Q_OBJECT

  public:
    explicit AgentRuntime(QObject * parent = nullptr);
    ~AgentRuntime() override;

    // ── 依赖注入 ────────────────────────────────────────────────────────────
    // 全部为借用指针，生命周期由调用方（Application）保证。
    void setProviderRegistry(ProviderRegistry * registry);
    void setToolRegistry(ToolRegistry * registry);
    void setSessionStore(SessionStore * store);
    void setTodoStore(TodoStore * store);

    // ── 会话生命周期 ────────────────────────────────────────────────────────
    /// 新建会话。失败时通过 errorOut 返回原因。
    bool startSession(const Workspace & workspace, SessionMode mode,
                      const ModelSelection & model, QString * errorOut = nullptr);
    /// 从存储载入已有会话及其消息。
    bool loadSession(const Id & sessionId, QString * errorOut = nullptr);
    /// 关闭当前会话（取消运行、清空内存状态）。
    void closeSession();

    const Session & session() const
    {
      return session_;
    }
    QList<Message> messages() const
    {
      return messages_;
    }
    bool hasSession() const
    {
      return !session_.id.isEmpty();
    }

    // ── 用户输入 ────────────────────────────────────────────────────────────
    /// 提交一条用户消息并启动 turn。
    /// 运行中提交会被拒绝（返回 false）——输入排队属于 UI 层职责，
    /// 运行时只保证"一次只有一个 turn"。
    bool submitText(const QString & text, QString * errorOut = nullptr);
    /// 带附件的提交。`attachments` 里的每个 FilePart 会作为独立的 Part
    /// 追加到用户消息上（图片走 base64 内联，见各 provider 的序列化）。
    bool submitMessage(const QString & text, const QList<FilePart> & attachments,
                       QString * errorOut = nullptr);
    /// 提交权限裁决。
    bool resolvePermission(const Id & requestId, const PermissionResponse & response);
    /// 请求中断当前 turn。幂等。
    void abort();

    // ── 会话配置 ────────────────────────────────────────────────────────────
    void setMode(SessionMode mode);
    void setModel(const ModelSelection & model);

    /// 工具白名单。非空时只声明与执行这些工具（子代理的只读 profile 用它收窄工具面）。
    void setToolAllowlist(const QStringList & names);
    QStringList toolAllowlist() const
    {
      return toolAllowlist_;
    }

    /// 单个 turn 允许的模型步数上限。
    void setMaxModelStepsPerTurn(int steps);
    int maxModelStepsPerTurn() const
    {
      return maxModelStepsPerTurn_;
    }

    /// 当前运行时是否是子代理（子代理不能再派生子代理）。
    bool isSubagent() const
    {
      return subagentDepth_ > 0;
    }

    // ── 上下文压缩 ──────────────────────────────────────────────────────────
    /// 压缩策略（阈值、保护窗口等）。改完需要 remeasureContext() 刷新界面分母。
    CompactionPolicy compactionPolicy() const
    {
      return compactionPolicy_;
    }
    void setCompactionPolicy(const CompactionPolicy & policy);
    /// 手工覆盖上下文窗口（token）。0 = 用模型元信息里的值。
    void setContextWindowOverride(int tokens);
    int contextWindowOverride() const
    {
      return contextWindowOverride_;
    }
    /// 当前生效的上下文窗口（含用户覆盖），未知时 128k。
    int effectiveContextWindow() const;
    /// 最近一次全压缩得到的摘要；空表示这段历史没被压缩过。
    QString compactionSummary() const
    {
      return compactionSummary_;
    }

    /// 手动触发上下文压缩（`/compact`）。
    ///
    /// 与自动触发共用同一条路径：先做一次模型摘要，成功后用合成消息替换掉早期
    /// 历史，并把原始消息归档。**不必等到阈值**——用户显式要求就该照做。
    /// 返回 false 表示当场就没法压缩（没有会话 / 消息太少 / 已有请求在飞）。
    bool compactContextNow(QString * errorOut = nullptr);

    /// 是否正在生成压缩摘要（在飞）。
    bool compactionInFlight() const
    {
      return compactionInFlight_;
    }

    /// 把本会话登记为某个父会话的子会话，并落盘。
    void adoptAsChild(const Id & parentSessionId, SessionKind kind, const QString & title);

    /// 设置子代理身份，用于生成子代理专属的系统提示词。
    void setSubagentIdentity(const QString & subagentType, const QString & description);

    // ── SubagentHost ────────────────────────────────────────────────────────
    void launchSubagent(const LaunchRequest & request, Completion done) override;
    bool subagentsEnabled() const override;

    /// 后台任务注册表。Bash 的 run_in_background 与 TaskOutput/TaskStop 共用它。
    BackgroundTaskRegistry * backgroundTasks() const
    {
      return backgroundTasks_;
    }

    /// 注入 skills 库（不接管所有权）。传 nullptr 表示没有技能目录。
    void setSkillLibrary(const skills::Library * library);
    const skills::Library * skillLibrary() const
    {
      return skills_;
    }

    /// 用模型为当前会话生成标题。
    ///
    /// 走一条**独立于主回合**的轻量请求：不给工具、限制输出长度、
    /// 结果不进消息历史。这样标题生成既不阻塞对话，也不会污染上下文。
    /// 幂等：已经有模型生成的标题、或正在生成时直接返回。
    void requestTitleFromModel();

    /// 重新测量上下文用量并通知 UI。
    ///
    /// 外部改了会影响上下文窗口的东西之后必须调用它：用户在设置页调整
    /// "上下文窗口/最大输出"、或换了模型，都会改变分母。不调用的话界面会一直
    /// 显示旧值，看起来像"设置没生效"（只能等下一个 turn 结束才刷新）。
    void remeasureContext();
    /// 权限门。父会话拥有它；子代理**共享同一个实例**，
    /// 这样"谁有权限"只有一个判定点，UI 也只需向一个入口提交裁决
    /// （否则子代理弹出的确认框会把裁决提交到父会话的权限门上）。
    /// const 版本返回非 const 指针：权限门本身不是本对象状态的一部分，
    /// 在 const 方法里也需要读它的模式（buildModelRequest 就是 const）。
    PermissionGate * permissionGate() const
    {
      return permissionGate_;
    }
    /// 注入共享的权限门（子代理用）。传 nullptr 无效。
    void setPermissionGate(PermissionGate * gate);

    // ── 状态查询 ────────────────────────────────────────────────────────────
    RunState runState() const;
    TurnPhase phase() const
    {
      return phase_;
    }
    bool isRunning() const;
    /// 本轮已执行的模型步数。
    int modelStepCount() const
    {
      return modelStepCount_;
    }
    /// 本轮已执行的工具调用数。
    int toolCallCount() const
    {
      return toolCallCount_;
    }
    /// 估算的上下文 token 用量。
    /// 口径 = 消息 + 工具声明 + 系统提示词，与压缩阈值判断用的是同一个值。
    int estimatedInputTokens() const
    {
      return contextTokens();
    }

    /// 最近一次模型调用的用量（找最后一条带用量的 assistant 消息）。
    /// 没有则返回全 0。用于状态栏展示"本轮"的缓存命中情况。
    Usage lastTurnUsage() const;

    /// 安全上限：单个 turn 内允许的模型步数。
    /// 设为很大是为了不成为常规任务的瓶颈，只用于防御失控循环。
    static constexpr int kMaxModelStepsPerTurn = 200;

    /// 同一组内并行执行的上限。ToolScheduler 默认 maxConcurrency 一致。
    static constexpr int kMaxToolConcurrency = 10;

    /// 子代理的模型步上限。
    ///
    /// 本实现的子代理 `maxTurns` 默认 4；它按"turn"计数（一次模型响应 + 其工具批），
    /// 本实现按"模型步"计数（一步 = 一次模型响应）。一次工具往返需要两步
    /// （请求工具 + 消化结果），所以 4 轮工具往返约等于 12 步，取 12 作为等价上限。
    /// 目的是给子代理一个明确的边界，避免它无限自我消耗。
    static constexpr int kDefaultSubagentMaxModelSteps = 12;

  signals:
    void sessionChanged(const lycode::Session & session);
    /// 子代理的会话状态变化。与 sessionChanged 刻意分开：
    /// 接收方对 sessionChanged 的反应是"切换到该会话"，
    /// 而子会话只应出现在列表里，绝不能抢走当前会话。
    void subagentSessionChanged(const lycode::Session & session);
    /// 子代理结束（用于 UI 更新列表里的状态）。
    void subagentFinished(const lycode::Id & childSessionId, bool ok);
    /// 后台任务集合发生变化（新增/结束/停止）。UI 据此刷新计数。
    void backgroundTasksChanged(int runningCount);
    /// 模型生成的标题已应用（成功时才发）。
    void titleGenerated(const lycode::Id & sessionId, const QString & title);
    void messageAdded(const lycode::Message & message);
    void partAppended(const lycode::Id & messageId, const lycode::Part & part);
    void partUpdated(const lycode::Id & messageId, const lycode::Part & part);
    /// 流式文本增量。`reasoning` 为 true 表示这是思考内容。
    void deltaAppended(const lycode::Id & messageId, const lycode::Id & partId,
                       const QString & delta, bool reasoning);
    void messageFinished(const lycode::Message & message);
    void permissionRequested(const lycode::PermissionRequest & request);
    void permissionResolved(const lycode::Id & requestId);
    void runStateChanged(lycode::RunState state);
    void turnFinished(lycode::TurnResult result);
    /// 面向用户的失败提示（已本地化）。
    void failed(const QString & message);
    /// 上下文压缩的进展/结果提示（已本地化）。
    /// 与 failed 分开：压缩失败不是 turn 失败，界面上不该显示成一次错误。
    void compactionNotice(const QString & message);
    /// 消息列表被**整体替换**（目前只有上下文压缩会这样做）。
    /// UI 收到后必须自己重建对话流——增量信号描述不了"删掉一段、插入一段"。
    void conversationReplaced();

  private:
    // ── turn 流程 ───────────────────────────────────────────────────────────
    void beginTurn(const QString & userText, const QList<FilePart> & attachments);
    void runModelStep();
    void handleStreamEvent(const StreamEvent & event);
    void finishModelStep(const QString & finishReason);

    // ── 工具调度 ────────────────────────────────────────────────────────────
    /// 把本轮的调用按并行安全性分组：组内可并行，组与组之间串行。
    void scheduleToolBatches();
    /// 推进到下一组；所有组跑完则收口到 afterToolQueue()。
    void runToolQueue();
    /// 派发当前组的**全部**调用（权限请求与执行都是异步的，不在这里等待）。
    void startToolBatch(int batchIndex);
    /// 派发单个调用。完成时统一走 finishToolCall()。
    void executeToolAt(int index);
    /// 单个调用终结（幂等）。本组全部终结后才推进下一组。
    /// 用 callId 而不是下标标识调用：异步回调可能在下一轮 turn 才到达，
    /// 下标那时可能已经指向新 turn 的另一个调用。
    void finishToolCall(const QString & callId, const ToolResult & result);
    /// 清洗模型返回的标题；不可用时返回空串。
    static QString sanitizeTitle(const QString & raw);
    /// 应用生成的标题（落盘并通知 UI）。
    void applyGeneratedTitle(const QString & raw);

    /// 把待投递的后台任务完成通知注入上下文。
    /// 在每次模型步开始前调用：这样任务结束时如果正在跑 turn，模型能在下一步
    /// 就看到结果；如果当时空闲，则在下一次输入的首次模型步看到（语义一致）。
    void drainBackgroundNotifications();

    /// 把尚未终结的调用标记为取消（中断或提前终止时）。
    void cancelPendingToolCalls(const QString & reason);
    /// 订阅权限门的信号。切换共享门之后必须重新订阅。
    void wirePermissionGate();
    void afterToolQueue();
    void completeTurn(TurnResult result, const QString & errorMessage = {});

    // ── 状态迁移 ────────────────────────────────────────────────────────────
    void setPhase(TurnPhase phase);

    // ── 辅助 ────────────────────────────────────────────────────────────────
    /// 当前正在接收增量的 assistant 消息（可能为空）。
    Message * currentAssistantMessage();
    /// 在指定消息里追加一个 part，发信号并返回其 id。
    Id appendPart(Message & message, const Part & part);
    /// 更新指定消息里的 part，发信号。找不到返回 false。
    bool updatePart(Message & message, const Part & part);
    /// 本次 turn 的单个工具调用及其执行状态（按模型给出的顺序排列）。
    struct QueuedToolCall {
      Id messageId;
      Id partId;
      QString callId;
      QString name;
      QJsonObject input;
      /// 是否已派发（权限请求已发出）。未派发的在中断时直接记为取消。
      bool started = false;
      /// 是否已终结。用于保证 finishToolCall() 的幂等——
      /// 参数校验失败、权限拒绝、工具回调都可能走到同一个收口点。
      bool finished = false;
    };

    /// 一组可以并行执行的调用。`indices` 指向 toolQueue_。
    struct ToolBatch {
      QList<int> indices;
      /// false 表示独占组（只有一个元素）：不并行安全的工具要与前后形成串行屏障。
      bool parallel = true;
    };

    /// 构造模型请求。
    ModelRequest buildModelRequest() const;
    /// 把消息序列投影成 provider 可接受的形状（system 走独立字段）。
    void persistMessage(const Message & message);
    void persistSession();
    void refreshContextUsage();

    // ── 上下文压缩 ──────────────────────────────────────────────────────────
    /// 本会话当前生效的工具声明（含白名单收窄）。估算与请求都走它，
    /// 保证"估算的输入"与"真正发出去的输入"是同一份。
    QList<ToolSpec> effectiveToolSpecs() const;
    /// 本会话当前生效的系统提示词。
    QString effectiveSystemPrompt() const;
    /// 当前下发给模型的输入估算（token）。
    int contextTokens() const;
    /// 每个模型步开始前的自动压缩检查。
    void autoCompactIfNeeded();
    /// 发起一次摘要请求（异步）。`force` 表示用户显式要求（不看阈值）。
    bool startCompactionSummary(bool force, QString * errorOut);
    /// 摘要返回后落地：归档早期消息、用合成消息替换、通知界面。
    void applyCompactionSummary(const QString & summary);
    /// 放弃本次压缩（摘要失败/无内容），恢复状态并告知原因。
    void abortCompactionSummary(const QString & reason);
    /// 中断收尾：取消工具与权限请求，把未终态的 part 标记为已取消。
    void teardownAfterAbort();

    // ── 依赖 ────────────────────────────────────────────────────────────────
    ProviderRegistry * providers_ = nullptr;
    ToolRegistry * tools_ = nullptr;
    SessionStore * store_ = nullptr;
    TodoStore * todos_ = nullptr;

    // ── 会话状态 ────────────────────────────────────────────────────────────
    Session session_;
    QList<Message> messages_;
    ModelSelection model_;
    /// 默认权限门（本会话自有时使用）。
    std::unique_ptr<PermissionGate> ownedPermissionGate_;
    /// 生效的权限门；指向 ownedPermissionGate_ 或外部注入的实例。
    PermissionGate * permissionGate_ = nullptr;

    // ── turn 状态 ───────────────────────────────────────────────────────────
    TurnPhase phase_ = TurnPhase::Idle;
    Id currentAssistantMessageId_;
    Id currentTextPartId_;
    Id currentReasoningPartId_;
    QList<QueuedToolCall> toolQueue_;
    QList<ToolBatch> batches_;
    int batchCursor_ = 0;
    /// 当前组内尚未终结的调用数。归零才推进下一组——这保证
    /// stopTurnAfterResult 不会打断同组的兄弟调用（本实现的 batch-runner 同语义）。
    int batchPending_ = 0;
    int modelStepCount_ = 0;
    int toolCallCount_ = 0;
    bool abortRequested_ = false;
    bool toolStopRequested_ = false;
    StreamEvent pendingUsage_;
    Usage turnUsage_;
    ModelStream * activeStream_ = nullptr;
    TurnResult lastResult_ = TurnResult::None;
    /// 当前 turn 的标识，用于把工具执行与消息关联起来。
    Id currentTurnId_;
    /// 工具白名单；空表示不限制。
    QStringList toolAllowlist_;
    /// 本运行时的模型步上限（子代理取更小的值）。
    int maxModelStepsPerTurn_ = kMaxModelStepsPerTurn;
    /// 子代理深度。0 = 主代理；> 0 时禁止再派生。
    int subagentDepth_ = 0;
    /// **本运行时自己**发出的、尚未裁决的权限请求。
    /// 权限门可能与子代理共享，所以不能用 gate->hasPending() 判断
    /// "我是不是在等用户"——那会把别人的等待算到自己头上。
    QSet<Id> ownPendingPermissions_;
    /// 标题生成请求是否在飞。避免首条消息后连开多个请求。
    bool titleGenerationInFlight_ = false;

    // ── 上下文压缩状态 ──────────────────────────────────────────────────────
    CompactionPolicy compactionPolicy_;
    /// 用户手工覆盖的上下文窗口（0 = 用模型元信息）。
    int contextWindowOverride_ = 0;
    /// 最近一次全压缩的摘要。恢复自合成消息，换会话时随之清空。
    QString compactionSummary_;
    /// 摘要请求是否在飞。避免同一步发起两次压缩（那会白花一次模型调用）。
    bool compactionInFlight_ = false;
    /// 摘要请求流；需要显式 abort（换会话/中断时）。
    ModelStream * compactionStream_ = nullptr;
    /// 已算好、等待摘要返回后落地的压缩方案。
    FullCompaction pendingCompaction_;
    /// 摘要已生成、但**还不能立刻应用**时暂存在这里。
    ///
    /// 为什么需要暂存：压缩会把消息列表换成"摘要 + 尾部"，而流式中的 assistant
    /// 占位消息很可能落在被换掉的那一段里（调用工具时流会因工具批次而短暂停止，
    /// 此时该轮首条 assistant 消息可能还在历史里）。直接替换会让正在流式的
    /// 消息凭空消失，增量无处可落，turn 永远收不了尾。
    /// 因此摘要到达时如果本轮的流还活着，就只记录，等下一个模型步开始前
    /// （那时占位消息还没建）再统一应用。
    QString deferredCompactionSummary_;
    /// 是否已经就"接近上限"提醒过用户一次（避免每个模型步都刷同一条提示）。
    bool compactionWarned_ = false;

    /// 子代理身份（非空表示这是子代理运行时）。
    QString subagentType_;
    QString subagentDescription_;
    /// 子代理自己的 todo 存储（上下文隔离的一部分：父子不共享 todo）。
    std::unique_ptr<TodoStore> ownedTodoStore_;

    /// 后台任务注册表。每个运行时一套：子代理的后台任务是它自己的，
    /// 父代理不该看到、也不该被它的通知打扰。
    std::unique_ptr<BackgroundTaskRegistry> ownedBackgroundTasks_;
    BackgroundTaskRegistry * backgroundTasks_ = nullptr;

    /// Skills 库。由 MainWindow 持有，这里只借用。
    const skills::Library * skills_ = nullptr;

    /// 传给工具与权限层的取消令牌。一个 turn 一个，abort() 时置位。
    /// 用 shared_ptr 是因为工具可能在异步回调里持有它，生命周期必须独立于 turn。
    std::shared_ptr<std::atomic_bool> cancelFlag_;
};

}  // namespace lycode

Q_DECLARE_METATYPE(lycode::TurnPhase)
Q_DECLARE_METATYPE(lycode::RunState)
Q_DECLARE_METATYPE(lycode::TurnResult)

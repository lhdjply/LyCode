// ZCode Qt — Agent 运行时
//
// 这是把 npm 版 turn 循环用 C++ 重写的核心。与 npm 版的结构对应关系：
//
//   npm                                        Qt
//   ─────────────────────────────────────────  ──────────────────────────────
//   TurnMachine（相位机）                       内部 TurnPhase + advanceTo()
//   runRegularTurnLoop                        runModelStep() ↔ runToolQueue()
//   runModelBackedTurnStep                    单个 model step
//   executeToolCallsForModelStep               工具队列（串行执行）
//   PermissionService + broker                 PermissionGate
//   ContextBuilder                             SystemPromptBuilder
//   SessionStore (SQLite)                      SessionStore
//   MessageHistory                              messages_ (QList<Message>)
//
// ── 并发模型 ──────────────────────────────────────────────────────────────
// 单线程。所有会话状态只属于本对象，只在 GUI 线程访问，因此不需要锁。
// 网络流（ModelStream）与子进程（QProcess）都是异步的，用信号回到本线程。
//
// ── 终止条件（与 npm 版一致）──────────────────────────────────────────────
// 主循环**没有 maxSteps / maxTurns 硬停止**（这是 npm 版的既定设计：
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
#include <QString>

#include "agent/PermissionGate.h"
#include "core/Types.h"
#include "storage/SessionStore.h"
#include "tools/Tool.h"

namespace zcode {

class ProviderRegistry;
class ModelStream;

/// turn 相位。取值与 npm 的 turn-state.ts 对齐。
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

class AgentRuntime : public QObject {
    Q_OBJECT

public:
    explicit AgentRuntime(QObject *parent = nullptr);
    ~AgentRuntime() override;

    // ── 依赖注入 ────────────────────────────────────────────────────────────
    // 全部为借用指针，生命周期由调用方（Application）保证。
    void setProviderRegistry(ProviderRegistry *registry);
    void setToolRegistry(ToolRegistry *registry);
    void setSessionStore(SessionStore *store);
    void setTodoStore(TodoStore *store);

    // ── 会话生命周期 ────────────────────────────────────────────────────────
    /// 新建会话。失败时通过 errorOut 返回原因。
    bool startSession(const Workspace &workspace, SessionMode mode,
                      const ModelSelection &model, QString *errorOut = nullptr);
    /// 从存储载入已有会话及其消息。
    bool loadSession(const Id &sessionId, QString *errorOut = nullptr);
    /// 关闭当前会话（取消运行、清空内存状态）。
    void closeSession();

    const Session &session() const { return session_; }
    QList<Message> messages() const { return messages_; }
    bool hasSession() const { return !session_.id.isEmpty(); }

    // ── 用户输入 ────────────────────────────────────────────────────────────
    /// 提交一条用户消息并启动 turn。
    /// 运行中提交会被拒绝（返回 false）——输入排队属于 UI 层职责，
    /// 运行时只保证"一次只有一个 turn"。
    bool submitText(const QString &text, QString *errorOut = nullptr);
    /// 提交权限裁决。
    bool resolvePermission(const Id &requestId, const PermissionResponse &response);
    /// 请求中断当前 turn。幂等。
    void abort();

    // ── 会话配置 ────────────────────────────────────────────────────────────
    void setMode(SessionMode mode);
    void setModel(const ModelSelection &model);

    /// 重新测量上下文用量并通知 UI。
    ///
    /// 外部改了会影响上下文窗口的东西之后必须调用它：用户在设置页调整
    /// "上下文窗口/最大输出"、或换了模型，都会改变分母。不调用的话界面会一直
    /// 显示旧值，看起来像"设置没生效"（只能等下一个 turn 结束才刷新）。
    void remeasureContext();
    PermissionGate *permissionGate() { return &permissionGate_; }

    // ── 状态查询 ────────────────────────────────────────────────────────────
    RunState runState() const;
    TurnPhase phase() const { return phase_; }
    bool isRunning() const;
    /// 本轮已执行的模型步数。
    int modelStepCount() const { return modelStepCount_; }
    /// 本轮已执行的工具调用数。
    int toolCallCount() const { return toolCallCount_; }
    /// 估算的上下文 token 用量。
    int estimatedInputTokens() const;

    /// 安全上限：单个 turn 内允许的模型步数。
    /// 设为很大是为了不成为常规任务的瓶颈，只用于防御失控循环。
    static constexpr int kMaxModelStepsPerTurn = 200;

signals:
    void sessionChanged(const zcode::Session &session);
    void messageAdded(const zcode::Message &message);
    void partAppended(const zcode::Id &messageId, const zcode::Part &part);
    void partUpdated(const zcode::Id &messageId, const zcode::Part &part);
    /// 流式文本增量。`reasoning` 为 true 表示这是思考内容。
    void deltaAppended(const zcode::Id &messageId, const zcode::Id &partId,
                       const QString &delta, bool reasoning);
    void messageFinished(const zcode::Message &message);
    void permissionRequested(const zcode::PermissionRequest &request);
    void permissionResolved(const zcode::Id &requestId);
    void runStateChanged(zcode::RunState state);
    void turnFinished(zcode::TurnResult result);
    /// 面向用户的失败提示（已本地化）。
    void failed(const QString &message);

private:
    // ── turn 流程 ───────────────────────────────────────────────────────────
    void beginTurn(const QString &userText);
    void runModelStep();
    void handleStreamEvent(const StreamEvent &event);
    void finishModelStep(const QString &finishReason);
    void runToolQueue();
    void executeToolAt(int index);
    void afterToolQueue();
    void completeTurn(TurnResult result, const QString &errorMessage = {});

    // ── 状态迁移 ────────────────────────────────────────────────────────────
    void setPhase(TurnPhase phase);

    // ── 辅助 ────────────────────────────────────────────────────────────────
    /// 当前正在接收增量的 assistant 消息（可能为空）。
    Message *currentAssistantMessage();
    /// 在指定消息里追加一个 part，发信号并返回其 id。
    Id appendPart(Message &message, const Part &part);
    /// 更新指定消息里的 part，发信号。找不到返回 false。
    bool updatePart(Message &message, const Part &part);
    /// 本次 turn 的全部工具调用记录（按产生顺序）。
    struct PendingToolCall {
        Id messageId;
        Id partId;
        QString callId;
        QString name;
        QJsonObject input;
    };

    /// 构造模型请求。
    ModelRequest buildModelRequest() const;
    /// 把消息序列投影成 provider 可接受的形状（system 走独立字段）。
    void persistMessage(const Message &message);
    void persistSession();
    void refreshContextUsage();
    /// 中断收尾：取消工具与权限请求，把未终态的 part 标记为已取消。
    void teardownAfterAbort();

    // ── 依赖 ────────────────────────────────────────────────────────────────
    ProviderRegistry *providers_ = nullptr;
    ToolRegistry *tools_ = nullptr;
    SessionStore *store_ = nullptr;
    TodoStore *todos_ = nullptr;

    // ── 会话状态 ────────────────────────────────────────────────────────────
    Session session_;
    QList<Message> messages_;
    ModelSelection model_;
    PermissionGate permissionGate_;

    // ── turn 状态 ───────────────────────────────────────────────────────────
    TurnPhase phase_ = TurnPhase::Idle;
    Id currentAssistantMessageId_;
    Id currentTextPartId_;
    Id currentReasoningPartId_;
    QList<PendingToolCall> toolQueue_;
    int toolQueueCursor_ = 0;
    int modelStepCount_ = 0;
    int toolCallCount_ = 0;
    bool abortRequested_ = false;
    bool toolStopRequested_ = false;
    StreamEvent pendingUsage_;
    Usage turnUsage_;
    ModelStream *activeStream_ = nullptr;
    TurnResult lastResult_ = TurnResult::None;
    /// 当前 turn 的标识，用于把工具执行与消息关联起来。
    Id currentTurnId_;
    /// 传给工具与权限层的取消令牌。一个 turn 一个，abort() 时置位。
    /// 用 shared_ptr 是因为工具可能在异步回调里持有它，生命周期必须独立于 turn。
    std::shared_ptr<std::atomic_bool> cancelFlag_;
};

}  // namespace zcode

Q_DECLARE_METATYPE(zcode::TurnPhase)
Q_DECLARE_METATYPE(zcode::RunState)
Q_DECLARE_METATYPE(zcode::TurnResult)

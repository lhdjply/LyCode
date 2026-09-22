#include "agent/AgentRuntime.h"

#include "agent/SystemPrompt.h"
#include "core/Ids.h"
#include "core/Json.h"
#include "core/Logging.h"
#include "model/ProviderRegistry.h"

#include <QLoggingCategory>
#include <QTimer>

#include <utility>

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.agent")

/// 展示预算：工具输出回灌给模型的字符上限。
/// 超出部分留在 ToolPart::output 里供 UI 展示，但下发时截断，
/// 避免一次 `cat` 撑爆上下文。
constexpr int kToolOutputModelBudgetChars = 30000;
/// 估算 token 用的字符/token 比。英文约 4，中英混排更接近 2.5；
/// 取 3 作为折中，宁可高估（高估只会更早压缩，不会超窗）。
constexpr double kCharsPerToken = 3.0;

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 枚举转换
// ─────────────────────────────────────────────────────────────────────────────

QString toToken(TurnPhase phase) {
    switch (phase) {
        case TurnPhase::Idle:
            return QStringLiteral("idle");
        case TurnPhase::ProcessingInput:
            return QStringLiteral("processing_input");
        case TurnPhase::AwaitingModelResponse:
            return QStringLiteral("awaiting_model_response");
        case TurnPhase::Streaming:
            return QStringLiteral("streaming");
        case TurnPhase::SchedulingTools:
            return QStringLiteral("scheduling_tools");
        case TurnPhase::ExecutingTools:
            return QStringLiteral("executing_tools");
        case TurnPhase::AggregatingResults:
            return QStringLiteral("aggregating_results");
        case TurnPhase::AwaitingPermission:
            return QStringLiteral("awaiting_permission");
        case TurnPhase::Completing:
            return QStringLiteral("completing");
        case TurnPhase::Error:
            return QStringLiteral("error");
    }
    return QStringLiteral("idle");
}

QString toToken(RunState state) {
    switch (state) {
        case RunState::Idle:
            return QStringLiteral("idle");
        case RunState::Streaming:
            return QStringLiteral("streaming");
        case RunState::ExecutingTools:
            return QStringLiteral("executing_tools");
        case RunState::WaitingPermission:
            return QStringLiteral("waiting_permission");
        case RunState::Cancelling:
            return QStringLiteral("cancelling");
        case RunState::Failed:
            return QStringLiteral("failed");
    }
    return QStringLiteral("idle");
}

// ─────────────────────────────────────────────────────────────────────────────
// 构造与依赖
// ─────────────────────────────────────────────────────────────────────────────

AgentRuntime::AgentRuntime(QObject *parent) : QObject(parent) {
    connect(&permissionGate_, &PermissionGate::requested, this,
            &AgentRuntime::permissionRequested);
    connect(&permissionGate_, &PermissionGate::requested, this,
            [this](const PermissionRequest &) { setPhase(TurnPhase::AwaitingPermission); });
    connect(&permissionGate_, &PermissionGate::resolved, this,
            [this](const Id &requestId, const PermissionOutcome &) {
                emit permissionResolved(requestId);
                // 全部裁决完毕后回到执行态；否则 UI 会一直显示"等待确认"。
                if (!permissionGate_.hasPending() && phase_ == TurnPhase::AwaitingPermission) {
                    setPhase(TurnPhase::ExecutingTools);
                }
            });
}

AgentRuntime::~AgentRuntime() {
    // 析构时不能留下悬挂的流：先中断再断开。
    if (activeStream_ != nullptr) {
        activeStream_->abort();
        activeStream_ = nullptr;
    }
    if (cancelFlag_) {
        cancelFlag_->store(true);
    }
}

void AgentRuntime::setProviderRegistry(ProviderRegistry *registry) {
    providers_ = registry;
}

void AgentRuntime::setToolRegistry(ToolRegistry *registry) {
    tools_ = registry;
}

void AgentRuntime::setSessionStore(SessionStore *store) {
    store_ = store;
}

void AgentRuntime::setTodoStore(TodoStore *store) {
    todos_ = store;
}

// ─────────────────────────────────────────────────────────────────────────────
// 会话生命周期
// ─────────────────────────────────────────────────────────────────────────────

bool AgentRuntime::startSession(const Workspace &workspace, SessionMode mode,
                                const ModelSelection &model, QString *errorOut) {
    if (!workspace.isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("工作区路径为空。");
        }
        return false;
    }
    if (!model.isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("未选择模型，请先在设置中配置 Provider 与模型。");
        }
        return false;
    }

    closeSession();

    session_ = Session{};
    session_.id = newSessionId();
    session_.workspace = workspace;
    session_.mode = mode;
    session_.kind = SessionKind::Interactive;
    session_.status = SessionStatus::Draft;
    session_.modelId = model.modelId;
    session_.providerId = model.providerId;
    session_.createdAtMs = nowMs();
    session_.updatedAtMs = session_.createdAtMs;

    model_ = model;
    permissionGate_.setMode(defaultPermissionModeFor(mode));

    qCInfo(log) << "新建会话; id=" << session_.id << "workspace=" << workspace.path
                << "mode=" << toToken(mode) << "model=" << model.displayValue();

    persistSession();
    emit sessionChanged(session_);
    return true;
}

bool AgentRuntime::loadSession(const Id &sessionId, QString *errorOut) {
    if (store_ == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("会话存储未初始化。");
        }
        return false;
    }

    Session loaded;
    if (!store_->loadSession(sessionId, &loaded)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("找不到会话：") + sessionId;
        }
        return false;
    }

    closeSession();

    session_ = loaded;
    messages_ = store_->loadMessages(sessionId);
    model_.providerId = session_.providerId;
    model_.modelId = session_.modelId;
    permissionGate_.setMode(defaultPermissionModeFor(session_.mode));

    // 冷恢复：上次运行留下的"运行中"状态在本次进程里没有对应实体，
    // 必须归零，否则 UI 会永远显示转圈。
    if (session_.status == SessionStatus::Running || session_.status == SessionStatus::Prewarming) {
        session_.status = SessionStatus::CompletedInterrupted;
    }
    for (Message &message : messages_) {
        if (!message.isTerminal()) {
            message.status = MessageStatus::Interrupted;
        }
        message.cancelPendingTools();
    }

    qCInfo(log) << "载入会话; id=" << session_.id << "消息数=" << messages_.size();

    refreshContextUsage();
    emit sessionChanged(session_);
    for (const Message &message : std::as_const(messages_)) {
        emit messageAdded(message);
    }
    return true;
}

void AgentRuntime::closeSession() {
    if (isRunning()) {
        abort();
    }
    permissionGate_.cancelAll(QStringLiteral("会话已关闭"));
    permissionGate_.reset();

    session_ = Session{};
    messages_.clear();
    model_ = ModelSelection{};
    toolQueue_.clear();
    toolQueueCursor_ = 0;
    currentAssistantMessageId_.clear();
    currentTextPartId_.clear();
    currentReasoningPartId_.clear();
    phase_ = TurnPhase::Idle;
    modelStepCount_ = 0;
    toolCallCount_ = 0;
    turnUsage_ = Usage{};
    lastResult_ = TurnResult::None;
}

// ─────────────────────────────────────────────────────────────────────────────
// 用户输入
// ─────────────────────────────────────────────────────────────────────────────

bool AgentRuntime::submitText(const QString &text, QString *errorOut) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("输入为空。");
        }
        return false;
    }
    if (!hasSession()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("没有活动会话。");
        }
        return false;
    }
    if (isRunning()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("正在运行中，请先等待当前回合结束或中断。");
        }
        return false;
    }
    if (providers_ == nullptr || !model_.isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("未配置可用的模型。");
        }
        return false;
    }

    beginTurn(trimmed);
    return true;
}

void AgentRuntime::beginTurn(const QString &userText) {
    qCInfo(log) << "开始 turn; session=" << session_.id << "输入长度=" << userText.size();

    // 每个 turn 一个取消令牌：上一轮的工具可能还在异步收尾，
    // 复用同一个令牌会让上一轮的收尾被误认为本轮已取消。
    cancelFlag_ = std::make_shared<std::atomic_bool>(false);
    abortRequested_ = false;
    toolStopRequested_ = false;
    toolQueue_.clear();
    toolQueueCursor_ = 0;
    modelStepCount_ = 0;
    toolCallCount_ = 0;
    turnUsage_ = Usage{};
    currentAssistantMessageId_.clear();
    currentTextPartId_.clear();
    currentReasoningPartId_.clear();
    lastResult_ = TurnResult::None;
    currentTurnId_ = newId(QStringLiteral("turn"));

    Message userMessage;
    userMessage.id = newMessageId();
    userMessage.sessionId = session_.id;
    userMessage.role = MessageRole::User;
    userMessage.status = MessageStatus::Complete;
    userMessage.createdAtMs = nowMs();
    userMessage.updatedAtMs = userMessage.createdAtMs;
    userMessage.parts.append(Part::makeText(userText));

    messages_.append(userMessage);
    emit messageAdded(userMessage);
    persistMessage(userMessage);

    // 首条用户消息用作会话标题，避免列表里全是"新会话"。
    if (session_.title.trimmed().isEmpty()) {
        const QString singleLine = userText.simplified();
        session_.title = singleLine.size() > 40 ? singleLine.left(40) + QStringLiteral("…")
                                               : singleLine;
    }

    session_.status = SessionStatus::Running;
    session_.updatedAtMs = nowMs();
    persistSession();
    emit sessionChanged(session_);

    setPhase(TurnPhase::ProcessingInput);
    runModelStep();
}

bool AgentRuntime::resolvePermission(const Id &requestId, const PermissionResponse &response) {
    return permissionGate_.resolve(requestId, response);
}

void AgentRuntime::abort() {
    if (!isRunning()) {
        return;
    }
    if (abortRequested_) {
        return;  // 幂等
    }
    abortRequested_ = true;
    if (cancelFlag_) {
        cancelFlag_->store(true);
    }

    qCInfo(log) << "收到中断请求; session=" << session_.id;
    setPhase(TurnPhase::Completing);

    // 等待中的权限请求必须先收口，否则回调悬挂会让状态机停住。
    permissionGate_.cancelAll(QStringLiteral("用户中断了本次运行"));

    if (activeStream_ != nullptr) {
        ModelStream *stream = activeStream_;
        activeStream_ = nullptr;
        stream->abort();
        // abort() 会触发终止事件，收尾逻辑在 handleStreamEvent 里统一处理；
        // 但若 provider 没有发出终止事件，这里必须兜底，否则永远卡在 Cancelling。
        QTimer::singleShot(0, this, [this, stream]() {
            if (phase_ == TurnPhase::Completing && session_.status == SessionStatus::Running) {
                stream->deleteLater();
                completeTurn(TurnResult::Interrupted, QStringLiteral("用户中断了本次运行"));
            }
        });
        return;
    }

    // 没有活跃的流：可能停在工具执行或权限等待上，直接收尾。
    teardownAfterAbort();
    completeTurn(TurnResult::Interrupted, QStringLiteral("用户中断了本次运行"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 模型步
// ─────────────────────────────────────────────────────────────────────────────

void AgentRuntime::runModelStep() {
    if (abortRequested_) {
        teardownAfterAbort();
        completeTurn(TurnResult::Interrupted, QStringLiteral("用户中断了本次运行"));
        return;
    }

    if (modelStepCount_ >= kMaxModelStepsPerTurn) {
        completeTurn(TurnResult::Failed,
                     QStringLiteral("单次回合的模型步数超过安全上限（%1），已中止。")
                         .arg(kMaxModelStepsPerTurn));
        return;
    }

    if (providers_ == nullptr) {
        completeTurn(TurnResult::Failed, QStringLiteral("Provider 注册表未初始化。"));
        return;
    }

    ModelInfo info;
    ModelProvider *provider = providers_->resolve(model_, &info);
    if (provider == nullptr) {
        completeTurn(TurnResult::Failed,
                     QStringLiteral("找不到可用的模型：") + model_.displayValue());
        return;
    }

    setPhase(TurnPhase::AwaitingModelResponse);

    // 先建 assistant 占位消息，再发起请求：
    // 这样流式增量始终有明确的落点，UI 也能立刻看到"正在生成"。
    Message assistant;
    assistant.id = newMessageId();
    assistant.sessionId = session_.id;
    assistant.role = MessageRole::Assistant;
    assistant.status = MessageStatus::Streaming;
    assistant.modelId = model_.modelId;
    assistant.createdAtMs = nowMs();
    assistant.updatedAtMs = assistant.createdAtMs;
    if (!messages_.isEmpty()) {
        // 关联触发它的用户消息（工具结果轮则关联那条合成的用户消息）。
        assistant.parentMessageId = messages_.last().id;
    }

    messages_.append(assistant);
    emit messageAdded(assistant);
    currentAssistantMessageId_ = assistant.id;
    currentTextPartId_.clear();
    currentReasoningPartId_.clear();

    const ModelRequest request = buildModelRequest();
    qCInfo(log) << "发起模型请求; step=" << (modelStepCount_ + 1) << "model=" << request.modelId
                << "消息数=" << request.messages.size() << "工具数=" << request.tools.size();

    ModelStream *stream = provider->stream(request);
    if (stream == nullptr) {
        completeTurn(TurnResult::Failed, QStringLiteral("模型请求创建失败。"));
        return;
    }
    activeStream_ = stream;

    connect(stream, &ModelStream::event, this, &AgentRuntime::handleStreamEvent,
            Qt::UniqueConnection);
    connect(stream, &ModelStream::finished, this, [this, stream]() {
        if (activeStream_ == stream) {
            activeStream_ = nullptr;
        }
        stream->deleteLater();
    });
}

ModelRequest AgentRuntime::buildModelRequest() const {
    ModelRequest request;
    request.modelId = model_.modelId;
    request.maxOutputTokens = 8192;
    // 温度保持 provider 默认，不主动覆盖——覆盖会改变模型既有行为。

    SystemPromptInput promptInput;
    promptInput.workspace = session_.workspace;
    promptInput.cwd = session_.workspace.path;
    promptInput.mode = session_.mode;
    promptInput.permissionMode = permissionGate_.mode();
    promptInput.appVersion = QStringLiteral(ZCODE_QT_VERSION);
    if (tools_ != nullptr) {
        promptInput.tools = tools_->specs();
        request.tools = promptInput.tools;
    }
    request.systemPrompt = SystemPromptBuilder::build(promptInput);

    // 只投影 user / assistant：内部 System 角色（压缩摘要等）不进请求。
    // 注意保留 assistant 消息里的 Tool part——provider 需要它们来重建
    // tool_use / tool_calls 块；工具**结果**由单独的 user 消息承载。
    for (const Message &message : messages_) {
        if (message.role == MessageRole::System) {
            continue;
        }
        Message projected;
        projected.id = message.id;
        projected.sessionId = message.sessionId;
        projected.role = message.role;
        projected.status = message.status;
        projected.createdAtMs = message.createdAtMs;
        projected.modelId = message.modelId;

        for (const Part &part : message.parts) {
            switch (part.kind) {
                case PartKind::Text:
                case PartKind::Tool:
                case PartKind::Reasoning:
                case PartKind::File:
                    projected.parts.append(part);
                    break;
                default:
                    // Artifact / Subagent / Timeline / Step 是本地展示信息，
                    // 对模型没有意义，不下发。
                    break;
            }
        }
        if (!projected.parts.isEmpty()) {
            request.messages.append(projected);
        }
    }

    return request;
}

void AgentRuntime::handleStreamEvent(const StreamEvent &event) {
    if (abortRequested_ && !event.isTerminal()) {
        // 已经决定中断，忽略后续增量，但仍然处理终止事件以便收尾。
        return;
    }

    Message *assistant = currentAssistantMessage();
    if (assistant == nullptr) {
        qCWarning(log) << "流事件到达但没有目标消息，已忽略; kind=" << toToken(event.kind);
        return;
    }

    switch (event.kind) {
        case StreamEventKind::Started:
            setPhase(TurnPhase::Streaming);
            break;

        case StreamEventKind::TextDelta: {
            if (currentTextPartId_.isEmpty()) {
                Part part = Part::makeText(QString());
                currentTextPartId_ = part.id;
                const Id partId = appendPart(*assistant, part);
                Q_UNUSED(partId);
            }
            Part *part = assistant->findPart(currentTextPartId_);
            if (part != nullptr) {
                part->text.text += event.text;
                assistant->updatedAtMs = nowMs();
                emit deltaAppended(assistant->id, part->id, event.text, false);
            }
            break;
        }

        case StreamEventKind::ReasoningDelta: {
            if (currentReasoningPartId_.isEmpty()) {
                Part part = Part::makeReasoning(QString());
                currentReasoningPartId_ = part.id;
                appendPart(*assistant, part);
            }
            Part *part = assistant->findPart(currentReasoningPartId_);
            if (part != nullptr) {
                part->reasoning.text += event.text;
                if (!event.signature.isEmpty()) {
                    part->reasoning.signature = event.signature;
                }
                assistant->updatedAtMs = nowMs();
                emit deltaAppended(assistant->id, part->id, event.text, true);
            }
            break;
        }

        case StreamEventKind::ToolCallStart: {
            Part part = Part::makeTool(event.toolName, event.toolCallId);
            part.tool.state = ToolState::InputStreaming;
            part.tool.startedAtMs = nowMs();
            appendPart(*assistant, part);
            // 新的工具调用意味着正文块结束：后续 text delta 应开新的正文块，
            // 这样"文本 → 工具 → 文本"的顺序才能被保留。
            currentTextPartId_.clear();
            currentReasoningPartId_.clear();
            break;
        }

        case StreamEventKind::ToolCallDelta: {
            Part *part = nullptr;
            for (Part &candidate : assistant->parts) {
                if (candidate.kind == PartKind::Tool &&
                    candidate.tool.callId == event.toolCallId) {
                    part = &candidate;
                    break;
                }
            }
            if (part != nullptr) {
                part->tool.inputText += event.argumentsDelta;
                emit partUpdated(assistant->id, *part);
            }
            break;
        }

        case StreamEventKind::ToolCallEnd: {
            Part *part = nullptr;
            for (Part &candidate : assistant->parts) {
                if (candidate.kind == PartKind::Tool &&
                    candidate.tool.callId == event.toolCallId) {
                    part = &candidate;
                    break;
                }
            }
            if (part == nullptr) {
                qCWarning(log) << "工具结束事件找不到对应块; callId=" << event.toolCallId;
                break;
            }
            part->tool.input = event.toolInput;
            // 入参收齐后进入待批准：真正的"是否执行"由权限链决定。
            part->tool.state = ToolState::PendingApproval;
            part->tool.title = part->tool.name;
            if (tools_ != nullptr) {
                if (Tool *tool = tools_->find(part->tool.name)) {
                    part->tool.title = tool->title(part->tool.input);
                }
            }

            PendingToolCall pending;
            pending.messageId = assistant->id;
            pending.partId = part->id;
            pending.callId = part->tool.callId;
            pending.name = part->tool.name;
            pending.input = part->tool.input;
            toolQueue_.append(pending);

            emit partUpdated(assistant->id, *part);
            break;
        }

        case StreamEventKind::Usage:
            turnUsage_ += event.usage;
            assistant->usage = turnUsage_;
            break;

        case StreamEventKind::Completed:
            if (!event.finishReason.isEmpty()) {
                qCDebug(log) << "模型结束; finishReason=" << event.finishReason;
            }
            finishModelStep(event.finishReason);
            break;

        case StreamEventKind::Failed:
            // 用户主动中断时，provider 会以"请求已取消"之类的错误收尾。
            // 这是**中断**而不是失败：如果按 Failed 处理，UI 会报错、
            // 会话进入 Error 状态，用户会以为自己触发了故障。
            if (abortRequested_) {
                assistant->status = MessageStatus::Interrupted;
                assistant->updatedAtMs = nowMs();
                assistant->cancelPendingTools();
                persistMessage(*assistant);
                emit messageFinished(*assistant);
                teardownAfterAbort();
                completeTurn(TurnResult::Interrupted, QStringLiteral("用户中断了本次运行"));
                return;
            }

            assistant->status = MessageStatus::Failed;
            assistant->errorMessage = event.errorMessage;
            assistant->updatedAtMs = nowMs();
            persistMessage(*assistant);
            emit messageFinished(*assistant);
            completeTurn(TurnResult::Failed,
                         event.errorMessage.isEmpty() ? QStringLiteral("模型请求失败。")
                                                      : event.errorMessage);
            break;
    }
}

void AgentRuntime::finishModelStep(const QString &finishReason) {
    Q_UNUSED(finishReason)

    Message *assistant = currentAssistantMessage();
    if (assistant == nullptr) {
        completeTurn(TurnResult::Failed, QStringLiteral("模型响应没有对应消息。"));
        return;
    }

    modelStepCount_ += 1;
    assistant->updatedAtMs = nowMs();

    if (abortRequested_) {
        assistant->status = MessageStatus::Interrupted;
        assistant->cancelPendingTools();
        persistMessage(*assistant);
        emit messageFinished(*assistant);
        teardownAfterAbort();
        completeTurn(TurnResult::Interrupted, QStringLiteral("用户中断了本次运行"));
        return;
    }

    if (toolQueue_.isEmpty()) {
        // 终止条件 1：没有工具调用，本轮结束。
        assistant->status = MessageStatus::Complete;
        persistMessage(*assistant);
        emit messageFinished(*assistant);
        completeTurn(TurnResult::Success);
        return;
    }

    assistant->status = MessageStatus::Complete;
    persistMessage(*assistant);
    emit messageFinished(*assistant);

    setPhase(TurnPhase::SchedulingTools);
    QTimer::singleShot(0, this, [this]() { runToolQueue(); });
}

// ─────────────────────────────────────────────────────────────────────────────
// 工具队列
// ─────────────────────────────────────────────────────────────────────────────

void AgentRuntime::runToolQueue() {
    if (abortRequested_) {
        teardownAfterAbort();
        completeTurn(TurnResult::Interrupted, QStringLiteral("用户中断了本次运行"));
        return;
    }

    if (toolQueueCursor_ >= toolQueue_.size()) {
        afterToolQueue();
        return;
    }
    executeToolAt(toolQueueCursor_);
}

void AgentRuntime::executeToolAt(int index) {
    if (index < 0 || index >= toolQueue_.size()) {
        afterToolQueue();
        return;
    }

    const PendingToolCall pending = toolQueue_.at(index);
    Message *assistant = currentAssistantMessage();
    if (assistant == nullptr) {
        afterToolQueue();
        return;
    }

    Part *part = assistant->findPart(pending.partId);
    if (part == nullptr) {
        qCWarning(log) << "工具块已不存在，跳过; callId=" << pending.callId;
        toolQueueCursor_ = index + 1;
        QTimer::singleShot(0, this, [this]() { runToolQueue(); });
        return;
    }

    // 统一的"结束本次工具"路径，保证状态落盘与游标推进只发生一次。
    const auto finishOne = [this, index](ToolResult result) {
        Message *message = currentAssistantMessage();
        if (message != nullptr) {
            const PendingToolCall &call = toolQueue_.at(index);
            if (Part *target = message->findPart(call.partId)) {
                target->tool.state = result.ok ? ToolState::Success : ToolState::Error;
                target->tool.output = result.output;
                target->tool.error = result.error;
                target->tool.errorCode = result.errorCode;
                // QJsonObject 没有 merge：逐键写入，工具的 metadata 覆盖同名键。
                for (auto it = result.metadata.constBegin(); it != result.metadata.constEnd();
                     ++it) {
                    target->tool.metadata.insert(it.key(), it.value());
                }
                target->tool.endedAtMs = nowMs();
                target->tool.progress = QJsonObject{};
                emit partUpdated(message->id, *target);
            }
            message->updatedAtMs = nowMs();
            persistMessage(*message);
        }

        toolCallCount_ += 1;
        if (result.stopTurnAfterResult) {
            toolStopRequested_ = true;
        }

        toolQueueCursor_ = index + 1;
        if (toolStopRequested_ || abortRequested_) {
            afterToolQueue();
            return;
        }
        // 用 singleShot 而不是直接递归：纯内存工具（TodoWrite 等）会同步回调，
        // 直接递归会让 N 个工具调用产生 N 层栈帧，工具多时会栈溢出。
        QTimer::singleShot(0, this, [this]() { runToolQueue(); });
    };

    // 工具名未知：直接记为失败，不询问权限（执行不了的东西不该打扰用户）。
    Tool *tool = tools_ != nullptr ? tools_->find(pending.name) : nullptr;
    if (tool == nullptr) {
        qCWarning(log) << "未知工具:" << pending.name;
        finishOne(ToolResult::failure(QStringLiteral("未知工具：") + pending.name,
                                      QStringLiteral("tool_not_found")));
        return;
    }

    const ToolMetadata metadata = tool->metadata();

    // 入参校验放在权限之前：不合法的调用不该让用户看到确认框。
    const QString validationError = tool->validateInput(pending.input);
    if (!validationError.isEmpty()) {
        finishOne(ToolResult::failure(validationError, QStringLiteral("invalid_input")));
        return;
    }

    // plan 模式下带副作用的工具一律拒绝，且不弹窗（模式语义，不是用户决定）。
    if (session_.mode == SessionMode::Plan &&
        sideEffectScopeWritesWorkspace(metadata.sideEffectScope)) {
        const QString reason =
            QStringLiteral("当前处于 plan（只读）模式，不能执行会修改工作区的工具：") +
            pending.name;
        finishOne(ToolResult::failure(reason, QStringLiteral("plan_mode_denied")));
        return;
    }

    setPhase(TurnPhase::ExecutingTools);

    const PermissionKind kind = permissionKindFor(metadata);
    const QString capability = tool->permissionCapability();
    const QString subject = tool->ruleSubject(pending.input);

    PermissionRequest request;
    request.id = newPermissionId();
    request.sessionId = session_.id;
    request.callId = pending.callId;
    request.toolName = pending.name;
    request.input = pending.input;
    request.kind = kind;
    request.riskLevel = metadata.riskLevel;
    request.title = tool->title(pending.input);
    request.description = tool->permissionDescription(pending.input);
    request.createdAtMs = nowMs();

    // 选项由工具声明的规则生成；UI 不自行发明选项。
    const QList<PermissionRule> suggestedRules = tool->permissionRules(pending.input);

    PermissionOption allowOnce;
    allowOnce.optionId = QStringLiteral("allowOnce");
    allowOnce.label = QStringLiteral("允许一次");
    allowOnce.kind = PermissionOptionKind::AllowOnce;
    allowOnce.response.decision = PermissionDecision::Allow;
    request.options.append(allowOnce);

    for (const PermissionRule &rule : suggestedRules) {
        if (rule.behavior != PermissionRuleBehavior::Allow) {
            continue;
        }
        PermissionOption allowAlways;
        allowAlways.optionId = QStringLiteral("allowAlways");
        allowAlways.label = QStringLiteral("始终允许：") + rule.toolName +
                            (rule.ruleContent.isEmpty() ? QString()
                                                        : QStringLiteral(" (") + rule.ruleContent +
                                                              QStringLiteral(")"));
        allowAlways.kind = PermissionOptionKind::AllowAlways;
        allowAlways.response.decision = PermissionDecision::Allow;
        allowAlways.response.permissionUpdates.append(rule);
        request.options.append(allowAlways);
        break;  // 只提供一条最相关的持久化规则，避免选项爆炸
    }

    PermissionOption deny;
    deny.optionId = QStringLiteral("deny");
    deny.label = QStringLiteral("拒绝");
    deny.kind = PermissionOptionKind::Deny;
    deny.response.decision = PermissionDecision::Deny;
    request.options.append(deny);

    // 保存 partId 供异步回调使用：回调触发时 currentAssistantMessageId_ 可能已经变化。
    const Id assistantMessageId = assistant->id;

    // 按值捕获 tool / pending / finishOne：权限裁决可能是异步的，
    // 而 executeToolAt 的栈帧那时早已返回，引用捕获会悬空。
    permissionGate_.request(
        request, capability, subject,
        [this, index, assistantMessageId, request, tool, pending,
         finishOne](PermissionOutcome outcome) {
            Message *message = nullptr;
            for (Message &candidate : messages_) {
                if (candidate.id == assistantMessageId) {
                    message = &candidate;
                    break;
                }
            }
            if (message == nullptr) {
                toolQueueCursor_ = index + 1;
                QTimer::singleShot(0, this, [this]() { runToolQueue(); });
                return;
            }

            const PendingToolCall &call = toolQueue_.at(index);
            Part *target = message->findPart(call.partId);
            if (target == nullptr) {
                toolQueueCursor_ = index + 1;
                QTimer::singleShot(0, this, [this]() { runToolQueue(); });
                return;
            }

            if (!outcome.allowed()) {
                const QString reason = outcome.response.reason.isEmpty()
                                           ? QStringLiteral("用户拒绝了该工具调用。")
                                           : outcome.response.reason;
                target->tool.state = ToolState::Error;
                target->tool.error = reason;
                target->tool.errorCode = QStringLiteral("permission_denied");
                target->tool.endedAtMs = nowMs();
                emit partUpdated(message->id, *target);

                ToolResult denied = ToolResult::failure(reason, QStringLiteral("permission_denied"));
                denied.metadata.insert(QStringLiteral("permissionDenied"), true);

                toolCallCount_ += 1;
                toolQueueCursor_ = index + 1;
                if (abortRequested_) {
                    afterToolQueue();
                } else {
                    QTimer::singleShot(0, this, [this]() { runToolQueue(); });
                }
                return;
            }

            // 放行：进入运行态并真正执行。
            target->tool.state = ToolState::Running;
            target->tool.startedAtMs = nowMs();
            target->tool.approvalInteractionId = request.id;
            emit partUpdated(message->id, *target);

            if (abortRequested_) {
                target->tool.state = ToolState::Cancelled;
                target->tool.endedAtMs = nowMs();
                emit partUpdated(message->id, *target);
                afterToolQueue();
                return;
            }

            ToolContext context;
            context.sessionId = session_.id;
            context.turnId = currentTurnId_;
            context.workspace = session_.workspace;
            context.workingDirectory = session_.workspace.path;
            context.permissionMode = permissionGate_.mode();
            context.readOnly = session_.mode == SessionMode::Plan;
            context.grantedRules = permissionGate_.grantedRules();
            context.todoStore = todos_;
            context.cancelled = cancelFlag_;

            const Id targetMessageId = message->id;
            const Id targetPartId = call.partId;
            context.progress = [this, targetMessageId, targetPartId](const QString &chunk) {
                for (Message &candidate : messages_) {
                    if (candidate.id != targetMessageId) {
                        continue;
                    }
                    if (Part *toolPart = candidate.findPart(targetPartId)) {
                        // 进度只进 metadata，不覆盖最终输出，
                        // 这样 UI 可以区分"正在输出"和"最终结果"。
                        toolPart->tool.progress.insert(QStringLiteral("preview"), chunk);
                        toolPart->tool.progress.insert(QStringLiteral("updatedAtMs"),
                                                       static_cast<double>(nowMs()));
                        emit partUpdated(targetMessageId, *toolPart);
                    }
                    break;
                }
            };

            Tool *executingTool = tool;
            const QJsonObject toolInput = pending.input;
            executingTool->execute(toolInput, context, [finishOne](ToolResult result) {
                finishOne(std::move(result));
            });
        });
}

void AgentRuntime::afterToolQueue() {
    if (abortRequested_) {
        teardownAfterAbort();
        completeTurn(TurnResult::Interrupted, QStringLiteral("用户中断了本次运行"));
        return;
    }

    Message *assistant = currentAssistantMessage();
    if (assistant != nullptr) {
        assistant->updatedAtMs = nowMs();
        persistMessage(*assistant);
    }

    // 终止条件 2：某个工具要求结束本轮。
    if (toolStopRequested_) {
        completeTurn(TurnResult::Success);
        return;
    }

    setPhase(TurnPhase::AggregatingResults);

    // 把工具结果投影成一条 user 消息（见 ModelProvider.h 的约定），
    // 让下一轮模型请求能看到执行结果。
    Message toolResults;
    toolResults.id = newMessageId();
    toolResults.sessionId = session_.id;
    toolResults.role = MessageRole::User;
    toolResults.status = MessageStatus::Complete;
    // 这是给模型看的上下文，不是用户说过的话。
    toolResults.modelOnly = true;
    toolResults.createdAtMs = nowMs();
    toolResults.updatedAtMs = toolResults.createdAtMs;

    for (const PendingToolCall &call : std::as_const(toolQueue_)) {
        if (assistant == nullptr) {
            break;
        }
        const Part *source = assistant->findPart(call.partId);
        if (source == nullptr || source->kind != PartKind::Tool) {
            continue;
        }

        Part result = *source;
        result.id = newPartId();  // 新消息里的 part 需要自己的 id
        switch (source->tool.state) {
            case ToolState::Success:
                result.tool.output =
                    json::truncate(source->tool.output, kToolOutputModelBudgetChars);
                break;
            case ToolState::Error:
            case ToolState::Cancelled:
                result.tool.output = QStringLiteral("Error: ") + source->tool.error;
                break;
            case ToolState::InputStreaming:
            case ToolState::PendingApproval:
            case ToolState::Running:
                // 理论上不该出现：队列跑完时所有工具都应有终态。
                result.tool.state = ToolState::Cancelled;
                result.tool.output = QStringLiteral("Error: 工具未产生结果即被中断。");
                break;
        }
        toolResults.parts.append(result);
    }

    if (!toolResults.parts.isEmpty()) {
        messages_.append(toolResults);
        emit messageAdded(toolResults);
        persistMessage(toolResults);
    }

    toolQueue_.clear();
    toolQueueCursor_ = 0;
    toolStopRequested_ = false;

    // 回到模型步：这是"工具→再思考"的循环边。
    // 同样延后一跳，避免同步完成的工具把整条 turn 压进调用栈。
    QTimer::singleShot(0, this, [this]() { runModelStep(); });
}

void AgentRuntime::teardownAfterAbort() {
    if (cancelFlag_) {
        cancelFlag_->store(true);
    }

    // 把当前 assistant 消息里所有未终态的工具关闭，否则 UI 永远显示"运行中"。
    if (Message *assistant = currentAssistantMessage()) {
        assistant->cancelPendingTools();
        if (!assistant->isTerminal()) {
            assistant->status = MessageStatus::Interrupted;
        }
        assistant->updatedAtMs = nowMs();
        persistMessage(*assistant);
        emit messageFinished(*assistant);
    }

    // 未被执行的队列项也要在 UI 上有交代。
    for (int index = toolQueueCursor_; index < toolQueue_.size(); ++index) {
        const PendingToolCall &call = toolQueue_.at(index);
        if (currentAssistantMessage() == nullptr) {
            break;
        }
        if (Part *part = currentAssistantMessage()->findPart(call.partId)) {
            if (!toolStateIsTerminal(part->tool.state)) {
                part->tool.state = ToolState::Cancelled;
                part->tool.error = QStringLiteral("因中断未执行。");
                part->tool.errorCode = QStringLiteral("cancelled");
                part->tool.endedAtMs = nowMs();
                emit partUpdated(currentAssistantMessage()->id, *part);
            }
        }
    }
    toolQueue_.clear();
    toolQueueCursor_ = 0;
}

void AgentRuntime::completeTurn(TurnResult result, const QString &errorMessage) {
    lastResult_ = result;

    switch (result) {
        case TurnResult::Success:
            session_.status = SessionStatus::CompletedSuccess;
            break;
        case TurnResult::Interrupted:
            session_.status = SessionStatus::CompletedInterrupted;
            break;
        case TurnResult::Failed:
            session_.status = SessionStatus::Error;
            break;
        case TurnResult::Skipped:
        case TurnResult::None:
            session_.status = SessionStatus::CompletedInterrupted;
            break;
    }

    session_.updatedAtMs = nowMs();
    session_.modelId = model_.modelId;
    session_.providerId = model_.providerId;
    session_.cumulativeUsage += turnUsage_;

    refreshContextUsage();
    persistSession();
    emit sessionChanged(session_);
    setPhase(result == TurnResult::Failed ? TurnPhase::Error : TurnPhase::Completing);

    qCInfo(log) << "turn 结束; result=" << static_cast<int>(result)
                << "模型步=" << modelStepCount_ << "工具调用=" << toolCallCount_
                << "inputTokens=" << turnUsage_.inputTokens
                << "outputTokens=" << turnUsage_.outputTokens;

    if (!errorMessage.isEmpty() && result == TurnResult::Failed) {
        emit failed(errorMessage);
    }

    // 清理运行期状态，回到 idle。
    activeStream_ = nullptr;
    cancelFlag_.reset();
    phase_ = TurnPhase::Idle;
    currentAssistantMessageId_.clear();
    currentTextPartId_.clear();
    currentReasoningPartId_.clear();
    emit runStateChanged(RunState::Idle);
    emit turnFinished(result);
}

// ─────────────────────────────────────────────────────────────────────────────
// 辅助
// ─────────────────────────────────────────────────────────────────────────────

void AgentRuntime::setPhase(TurnPhase phase) {
    if (phase_ == phase) {
        return;
    }
    phase_ = phase;
    qCDebug(log) << "相位 ->" << toToken(phase);

    // RunState 由 TurnPhase 推导，不独立维护。
    RunState state = RunState::Idle;
    switch (phase) {
        case TurnPhase::Idle:
            state = RunState::Idle;
            break;
        case TurnPhase::Streaming:
        case TurnPhase::AwaitingModelResponse:
        case TurnPhase::ProcessingInput:
            state = RunState::Streaming;
            break;
        case TurnPhase::SchedulingTools:
        case TurnPhase::ExecutingTools:
        case TurnPhase::AggregatingResults:
            state = RunState::ExecutingTools;
            break;
        case TurnPhase::AwaitingPermission:
            state = RunState::WaitingPermission;
            break;
        case TurnPhase::Completing:
            state = RunState::Cancelling;
            break;
        case TurnPhase::Error:
            state = RunState::Failed;
            break;
    }
    emit runStateChanged(state);
}

Message *AgentRuntime::currentAssistantMessage() {
    if (currentAssistantMessageId_.isEmpty()) {
        return nullptr;
    }
    for (Message &message : messages_) {
        if (message.id == currentAssistantMessageId_) {
            return &message;
        }
    }
    return nullptr;
}

Id AgentRuntime::appendPart(Message &message, const Part &part) {
    message.parts.append(part);
    message.updatedAtMs = nowMs();
    emit partAppended(message.id, part);
    return part.id;
}

bool AgentRuntime::updatePart(Message &message, const Part &part) {
    if (!message.replacePart(part)) {
        return false;
    }
    message.updatedAtMs = nowMs();
    emit partUpdated(message.id, part);
    return true;
}

void AgentRuntime::persistMessage(const Message &message) {
    if (store_ == nullptr || !store_->isOpen()) {
        return;
    }
    if (!store_->saveMessage(message)) {
        // 落盘失败不阻断运行，但必须显式告警：用户可能因此丢失历史。
        qCWarning(log) << "消息落盘失败; id=" << message.id << store_->lastError();
    }
}

void AgentRuntime::persistSession() {
    if (store_ == nullptr || !store_->isOpen()) {
        return;
    }
    if (!store_->saveSession(session_)) {
        qCWarning(log) << "会话落盘失败; id=" << session_.id << store_->lastError();
    }
}

void AgentRuntime::refreshContextUsage() {
    const int used = estimatedInputTokens();
    // 上下文窗口取自模型元信息；未知时保守取 128k。
    int window = 128000;
    if (providers_ != nullptr && model_.isValid()) {
        ModelInfo info;
        if (providers_->resolve(model_, &info) != nullptr) {
            window = info.contextWindow;
        }
    }

    QJsonObject usage;
    usage.insert(QStringLiteral("usedTokens"), used);
    usage.insert(QStringLiteral("maxTokens"), window);
    usage.insert(QStringLiteral("percent"),
                 window > 0 ? (static_cast<double>(used) * 100.0 / window) : 0.0);
    session_.contextUsage = usage;
}

int AgentRuntime::estimatedInputTokens() const {
    int chars = 0;
    for (const Message &message : messages_) {
        for (const Part &part : message.parts) {
            switch (part.kind) {
                case PartKind::Text:
                    chars += part.text.text.size();
                    break;
                case PartKind::Reasoning:
                    chars += part.reasoning.text.size();
                    break;
                case PartKind::Tool:
                    chars += part.tool.output.size();
                    break;
                default:
                    break;
            }
        }
    }
    // 工具 schema 与系统提示词也占预算，但变化不大，这里只统计对话正文，
    // 并留出固定余量。精确 token 计数需要 provider 的 tokenizer，
    // 而本地估算只用于压缩阈值判断，不用于计费。
    return static_cast<int>(static_cast<double>(chars) / kCharsPerToken);
}

RunState AgentRuntime::runState() const {
    switch (phase_) {
        case TurnPhase::Idle:
            return RunState::Idle;
        case TurnPhase::Streaming:
        case TurnPhase::AwaitingModelResponse:
        case TurnPhase::ProcessingInput:
            return RunState::Streaming;
        case TurnPhase::SchedulingTools:
        case TurnPhase::ExecutingTools:
        case TurnPhase::AggregatingResults:
            return RunState::ExecutingTools;
        case TurnPhase::AwaitingPermission:
            return RunState::WaitingPermission;
        case TurnPhase::Completing:
            return RunState::Cancelling;
        case TurnPhase::Error:
            return RunState::Failed;
    }
    return RunState::Idle;
}

bool AgentRuntime::isRunning() const {
    return phase_ != TurnPhase::Idle;
}

void AgentRuntime::setMode(SessionMode mode) {
    if (session_.mode == mode) {
        return;
    }
    qCInfo(log) << "会话模式变更:" << toToken(session_.mode) << "->" << toToken(mode);
    session_.mode = mode;
    // 模式决定默认权限策略；用户已授予的规则保留（显式授权不该被模式切换抹掉）。
    permissionGate_.setMode(defaultPermissionModeFor(mode));
    session_.updatedAtMs = nowMs();
    persistSession();
    emit sessionChanged(session_);
}

void AgentRuntime::setModel(const ModelSelection &model) {
    if (model.providerId == model_.providerId && model.modelId == model_.modelId &&
        model.reasoningLevel == model_.reasoningLevel) {
        return;
    }
    qCInfo(log) << "模型变更:" << model_.displayValue() << "->" << model.displayValue();
    model_ = model;
    session_.modelId = model.modelId;
    session_.providerId = model.providerId;
    session_.updatedAtMs = nowMs();
    persistSession();
    emit sessionChanged(session_);
}

}  // namespace zcode

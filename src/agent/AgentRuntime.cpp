#include "agent/AgentRuntime.h"

#include "agent/SystemPrompt.h"
#include "core/Ids.h"
#include "core/Json.h"
#include "core/Logging.h"
#include "model/ProviderRegistry.h"
#include "model/ReasoningLevels.h"
#include "tools/BackgroundTaskRegistry.h"
#include "tools/TodoStore.h"

#include <QElapsedTimer>
#include <memory>
#include <QLoggingCategory>
#include <QTimer>

#include <utility>
#include <QCoreApplication>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.agent")

/// 展示预算：工具输出回灌给模型的字符上限。
/// 超出部分留在 ToolPart::output 里供 UI 展示，但下发时截断，
/// 避免一次 `cat` 撑爆上下文。
constexpr int kToolOutputModelBudgetChars = 30000;

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 枚举转换
// ─────────────────────────────────────────────────────────────────────────────

QString toToken(TurnPhase phase)
{
  switch(phase) {
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

QString toToken(RunState state)
{
  switch(state) {
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

AgentRuntime::AgentRuntime(QObject * parent) : QObject(parent)
{
  ownedPermissionGate_ = std::make_unique<PermissionGate>();
  permissionGate_ = ownedPermissionGate_.get();
  wirePermissionGate();

  ownedBackgroundTasks_ = std::make_unique<BackgroundTaskRegistry>();
  backgroundTasks_ = ownedBackgroundTasks_.get();
  // 任务状态变化要通知 UI；完成通知由 drainBackgroundNotifications() 注入上下文。
  connect(backgroundTasks_, &BackgroundTaskRegistry::taskAdded, this,
  [this](const Id &) {
    emit backgroundTasksChanged(backgroundTasks_->runningCount());
  });
  connect(backgroundTasks_, &BackgroundTaskRegistry::taskFinished, this,
  [this](const Id & taskId, bool ok, int exitCode) {
    qCInfo(log) << "后台任务结束通知就绪; task=" << taskId << "ok=" << ok
                << "exitCode=" << exitCode;
    emit backgroundTasksChanged(backgroundTasks_->runningCount());
  });
  // 注意：注册表在**构造时**就完成了账本对账，那时还没有任何订阅者，
  // 所以初始计数不能在这里播（播了也没人收到）。由界面接好线之后主动拉取，
  // 见 MainWindow 里的 onBackgroundTasksChanged(runningCount())。
}

void AgentRuntime::wirePermissionGate()
{
  PermissionGate * gate = permissionGate();
  if(gate == nullptr) {
    qCCritical(log) << "权限门为空，权限裁决将无法进行";
    return;
  }

  // 权限门可能与子代理共享，所以每个信号处理都要先确认"这是不是本会话的请求"。
  // 不判定的后果是：子代理要权限时父会话也进入"等待确认"，父子状态互相污染。
  connect(gate, &PermissionGate::requested, this, [this](const PermissionRequest & request) {
    if(request.sessionId != session_.id) {
      return;  // 属于别的运行时（子代理/父代理）
    }
    ownPendingPermissions_.insert(request.id);
    setPhase(TurnPhase::AwaitingPermission);
    emit permissionRequested(request);
  });

  connect(gate, &PermissionGate::resolved, this,
  [this](const Id & requestId, const PermissionOutcome & outcome) {
    if(!ownPendingPermissions_.remove(requestId)) {
      return;  // 不是本会话发出的请求
    }
    emit permissionResolved(requestId);
    // 本会话的请求全部裁决完毕 → 回到执行态，
    // 否则 UI 会一直显示"等待确认"。
    if(ownPendingPermissions_.isEmpty() &&
       phase_ == TurnPhase::AwaitingPermission) {
      setPhase(TurnPhase::ExecutingTools);
    }
    Q_UNUSED(outcome)
  });
}

void AgentRuntime::setPermissionGate(PermissionGate * gate)
{
  if(gate == nullptr) {
    qCCritical(log) << "拒绝注入空权限门";
    return;
  }
  if(gate == permissionGate_) {
    return;
  }
  // 断开与旧门的连接：不断开会让本运行时继续响应旧门的请求。
  if(permissionGate_ != nullptr) {
    permissionGate_->disconnect(this);
  }
  permissionGate_ = gate;
  ownPendingPermissions_.clear();
  wirePermissionGate();
  qCInfo(log) << "已切换到共享权限门; session=" << session_.id;
}

void AgentRuntime::launchSubagent(const LaunchRequest & request, Completion done)
{
  if(done == nullptr) {
    qCCritical(log) << "派生请求缺少回调，将被丢弃";
    return;
  }

  const auto fail = [&done](const QString & message, const QString & code) {
    LaunchResult result;
    result.error = message;
    result.errorCode = code;
    done(result);
  };

  if(!subagentsEnabled()) {
    fail(QCoreApplication::translate("agent::AgentRuntime", "A subagent cannot spawn another subagent."),
         QStringLiteral("subagent_disabled"));
    return;
  }
  if(request.prompt.trimmed().isEmpty()) {
    fail(QCoreApplication::translate("agent::AgentRuntime", "The subagent prompt cannot be empty."),
         QStringLiteral("invalid_input"));
    return;
  }

  // ── 构造子运行时 ────────────────────────────────────────────────────────
  // 上下文隔离的全部含义都在这几行：子运行时是独立的 AgentRuntime，
  // 有自己的 Session、自己的 Message 历史、自己的 todo。
  auto * child = new AgentRuntime(this);
  child->subagentDepth_ = subagentDepth_ + 1;
  child->setProviderRegistry(providers_);
  // 工具实现共享（工具本身无状态、可重入）；工具**面**由白名单收窄。
  child->setToolRegistry(tools_);
  child->setSessionStore(store_);
  // 权限门共享：这样"谁有权限"只有一个判定点，UI 也只需向一个入口提交裁决。
  child->setPermissionGate(permissionGate());
  child->setToolAllowlist(request.toolAllowlist);
  child->setMaxModelStepsPerTurn(kDefaultSubagentMaxModelSteps);
  child->setSubagentIdentity(request.subagentType, request.description);
  // 子代理有自己的 todo 列表，父子互不覆盖。
  child->ownedTodoStore_ = std::make_unique<TodoStore>();
  child->setTodoStore(child->ownedTodoStore_.get());

  // 只读 profile 用 Plan 模式兜底：白名单已经收窄了可见工具，
  // 这一层保证即使模型凭历史记忆调用写操作也会被运行时拒绝。
  const SessionMode childMode = request.readOnlyProfile ? SessionMode::Plan : session_.mode;
  const ModelSelection childModel = request.model.isValid() ? request.model : model_;
  const Workspace childWorkspace =
    request.workspace.isValid() ? request.workspace : session_.workspace;

  QString error;
  if(!child->startSession(childWorkspace, childMode, childModel, &error)) {
    qCWarning(log) << "子代理会话创建失败:" << error;
    child->deleteLater();
    fail(error, QStringLiteral("subagent_launch_failed"));
    return;
  }

  // 登记父子关系。放在 startSession 之后：startSession 会重置整个 Session。
  child->adoptAsChild(session_.id, SessionKind::SubagentChild,
                      request.description.isEmpty() ? QCoreApplication::translate("agent::AgentRuntime", "Subagent")
                      : request.description);

  // ── 让 UI 看到子会话，但不让它抢走当前会话 ──────────────────────────────
  connect(child, &AgentRuntime::sessionChanged, this,
  [this](const Session & updated) {
    emit subagentSessionChanged(updated);
  });
  // 子代理的权限请求转发给父代理的信号：UI 只监听父运行时的信号，
  // 转发后弹出确认框，用户裁决经父运行时提交到共享权限门，正好命中子代理的等待项。
  connect(child, &AgentRuntime::permissionRequested, this,
          &AgentRuntime::permissionRequested);
  connect(child, &AgentRuntime::permissionResolved, this,
          &AgentRuntime::permissionResolved);
  // 子代理的失败提示也转给用户，否则它会静默失败、只留下一句"子代理执行失败"。
  connect(child, &AgentRuntime::failed, this, [this, child](const QString & message) {
    qCWarning(log) << "子代理内部失败:" << message << "child=" << child->session().id;
  });

  emit subagentSessionChanged(child->session());

  QElapsedTimer elapsed;
  elapsed.start();

  connect(child, &AgentRuntime::turnFinished, this,
  [this, child, done, elapsed](TurnResult result) {
    LaunchResult launch;
    launch.childSessionId = child->session().id;
    launch.toolUseCount = child->toolCallCount();
    launch.durationMs = elapsed.elapsed();
    // 用会话累计用量：单条 assistant 消息的 usage 是"本轮累计"，
    // 多步 turn 里逐条相加会重复计数。
    launch.totalTokens = child->session().cumulativeUsage.effectiveTotal();

    // 回传子代理**最后一条**有正文的 assistant 消息。
    const QList<Message> messages = child->messages();
    for(auto iterator = messages.crbegin(); iterator != messages.crend();
        ++iterator) {
      if(iterator->role != MessageRole::Assistant) {
        continue;
      }
      const QString text = iterator->plainText().trimmed();
      if(!text.isEmpty()) {
        launch.output = text;
        break;
      }
    }

    switch(result) {
      case TurnResult::Success:
        launch.ok = true;
        break;
      case TurnResult::Interrupted:
        // 用户中断：子代理可能已产出部分结论，交给父代理判断，
        // 但要让模型知道这次结果不完整。
        launch.ok = true;
        launch.truncated = true;
        break;
      case TurnResult::Failed: {
          launch.ok = false;
          launch.errorCode = QStringLiteral("subagent_failed");
          // 触及步数上限是最常见也最需要区分的一种失败。
          if(child->modelStepCount() >= child->maxModelStepsPerTurn()) {
            launch.truncated = true;
            launch.error = QCoreApplication::translate("agent::AgentRuntime",
                                                       "The subagent hit its model-step limit (%1) and was stopped.")
                           .arg(child->maxModelStepsPerTurn());
          }
          else {
            launch.error = child->session().contextUsage.isEmpty()
                           ? QCoreApplication::translate("agent::AgentRuntime", "The subagent failed.")
                           : QCoreApplication::translate("agent::AgentRuntime", "The subagent failed.");
          }
          break;
        }
      case TurnResult::Skipped:
      case TurnResult::None:
        launch.ok = false;
        launch.errorCode = QStringLiteral("subagent_not_run");
        launch.error = QCoreApplication::translate("agent::AgentRuntime", "The subagent did not run.");
        break;
    }

    qCInfo(log) << "子代理结束; child=" << launch.childSessionId
                << "ok=" << launch.ok << "steps=" << child->modelStepCount()
                << "tools=" << launch.toolUseCount
                << "tokens=" << launch.totalTokens
                << "durationMs=" << launch.durationMs;

    emit subagentFinished(launch.childSessionId, launch.ok);
    child->deleteLater();
    done(launch);
  });

  qCInfo(log) << "子代理已启动; type=" << request.subagentType
              << "readOnly=" << request.readOnlyProfile << "child=" << child->session().id;

  QString submitError;
  if(!child->submitText(request.prompt, &submitError)) {
    // submitText 失败不会触发 turnFinished，必须在这里收口，否则回调悬挂。
    qCWarning(log) << "子代理首次提交失败:" << submitError;
    child->deleteLater();
    fail(submitError, QStringLiteral("subagent_submit_failed"));
    return;
  }
}

void AgentRuntime::setSkillLibrary(const skills::Library * library)
{
  skills_ = library;
}

void AgentRuntime::setToolAllowlist(const QStringList & names)
{
  toolAllowlist_ = names;
}

void AgentRuntime::setMaxModelStepsPerTurn(int steps)
{
  maxModelStepsPerTurn_ = steps > 0 ? steps : kMaxModelStepsPerTurn;
}

void AgentRuntime::setSubagentIdentity(const QString & subagentType,
                                       const QString & description)
{
  subagentType_ = subagentType;
  subagentDescription_ = description;
}

void AgentRuntime::adoptAsChild(const Id & parentSessionId, SessionKind kind,
                                const QString & title)
{
  session_.parentSessionId = parentSessionId;
  session_.kind = kind;
  if(!title.trimmed().isEmpty()) {
    session_.title = title.trimmed();
  }
  session_.updatedAtMs = nowMs();
  persistSession();
  emit sessionChanged(session_);
}

bool AgentRuntime::subagentsEnabled() const
{
  // 子代理不能再派生子代理：否则一个任务可以无限自我复制。
  return !isSubagent();
}

AgentRuntime::~AgentRuntime()
{
  // 析构时不能留下悬挂的流：先中断再断开。
  if(activeStream_ != nullptr) {
    activeStream_->abort();
    activeStream_ = nullptr;
  }
  if(cancelFlag_) {
    cancelFlag_->store(true);
  }
}

void AgentRuntime::setProviderRegistry(ProviderRegistry * registry)
{
  providers_ = registry;
}

void AgentRuntime::setToolRegistry(ToolRegistry * registry)
{
  tools_ = registry;
}

void AgentRuntime::setSessionStore(SessionStore * store)
{
  store_ = store;
}

void AgentRuntime::setTodoStore(TodoStore * store)
{
  todos_ = store;
}

// ─────────────────────────────────────────────────────────────────────────────
// 会话生命周期
// ─────────────────────────────────────────────────────────────────────────────

bool AgentRuntime::startSession(const Workspace & workspace, SessionMode mode,
                                const ModelSelection & model, QString * errorOut)
{
  if(!workspace.isValid()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "The workspace path is empty.");
    }
    return false;
  }
  if(!model.isValid()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime",
                                              "No model selected. Configure a provider and a model in Settings first.");
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
  permissionGate()->setMode(defaultPermissionModeFor(mode));

  qCInfo(log) << "新建会话; id=" << session_.id << "workspace=" << workspace.path
              << "mode=" << toToken(mode) << "model=" << model.displayValue();

  // 立刻算一次上下文用量：此时 used 还是 0，但 max 已经确定。
  // 不这样做的话状态栏在第一个 turn 结束前是空白的，用户改完"上下文窗口"
  // 看不到任何反馈，会以为设置没生效。
  refreshContextUsage();

  persistSession();
  emit sessionChanged(session_);
  return true;
}

bool AgentRuntime::loadSession(const Id & sessionId, QString * errorOut)
{
  if(store_ == nullptr) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "Session storage is not initialized.");
    }
    return false;
  }

  Session loaded;
  if(!store_->loadSession(sessionId, &loaded)) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "Session not found: ") + sessionId;
    }
    return false;
  }

  closeSession();

  session_ = loaded;
  messages_ = store_->loadMessages(sessionId);
  model_.providerId = session_.providerId;
  model_.modelId = session_.modelId;
  permissionGate()->setMode(defaultPermissionModeFor(session_.mode));

  // 冷恢复：上次运行留下的"运行中"状态在本次进程里没有对应实体，
  // 必须归零，否则 UI 会永远显示转圈。
  if(session_.status == SessionStatus::Running || session_.status == SessionStatus::Prewarming) {
    session_.status = SessionStatus::CompletedInterrupted;
  }
  for(Message & message : messages_) {
    if(!message.isTerminal()) {
      message.status = MessageStatus::Interrupted;
    }
    message.cancelPendingTools();
  }

  // 历史上做过压缩的话，把摘要认回来：合成消息本身就在消息列表里（会随请求
  // 下发），这里恢复的是"当前摘要是什么"，供界面展示与日志使用。
  compactionSummary_ = restoreContextSummary(messages_);
  if(!compactionSummary_.isEmpty()) {
    qCInfo(log) << "已恢复压缩摘要; 长度=" << compactionSummary_.size();
  }

  qCInfo(log) << "载入会话; id=" << session_.id << "消息数=" << messages_.size();

  refreshContextUsage();
  emit sessionChanged(session_);
  for(const Message & message : std::as_const(messages_)) {
    emit messageAdded(message);
  }
  return true;
}

void AgentRuntime::closeSession()
{
  if(isRunning()) {
    abort();
  }
  // 这里**不终止**后台任务。它们已经脱离工具调用的生命周期，而且设计目标
  // 就是活过会话切换与宿主退出（下次启动由账本认领）。要停下它们请用 TaskStop。
  permissionGate()->cancelAll(QCoreApplication::translate("agent::AgentRuntime", "Session closed"));
  permissionGate()->reset();

  // 压缩摘要可能还在飞：不显式收掉的话，回调会落到**新会话**的消息列表上
  // （那时 session_ 已经换人），把上一个会话的摘要塞进新会话。
  if(compactionStream_ != nullptr) {
    compactionStream_->abort();
    compactionStream_->deleteLater();
    compactionStream_ = nullptr;
  }
  compactionInFlight_ = false;
  deferredCompactionSummary_.clear();
  compactionSummary_.clear();
  compactionWarned_ = false;

  session_ = Session{};
  messages_.clear();
  model_ = ModelSelection{};
  toolQueue_.clear();
  batches_.clear();
  batchCursor_ = 0;
  batchPending_ = 0;
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

bool AgentRuntime::submitText(const QString & text, QString * errorOut)
{
  return submitMessage(text, {}, errorOut);
}

bool AgentRuntime::submitMessage(const QString & text, const QList<FilePart> & attachments,
                                 QString * errorOut)
{
  const QString trimmed = text.trimmed();
  // 只发图片、不写文字是合法用法（"这张图里是什么？"）。
  if(trimmed.isEmpty() && attachments.isEmpty()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "Input is empty.");
    }
    return false;
  }
  if(!hasSession()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "No active session.");
    }
    return false;
  }
  if(isRunning()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime",
                                              "A turn is already running. Wait for it to finish, or interrupt it.");
    }
    return false;
  }
  if(providers_ == nullptr || !model_.isValid()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "No usable model is configured.");
    }
    return false;
  }

  beginTurn(trimmed, attachments);
  return true;
}

void AgentRuntime::beginTurn(const QString & userText, const QList<FilePart> & attachments)
{
  qCInfo(log) << "开始 turn; session=" << session_.id << "输入长度=" << userText.size()
              << "附件数=" << attachments.size();

  // 每个 turn 一个取消令牌：上一轮的工具可能还在异步收尾，
  // 复用同一个令牌会让上一轮的收尾被误认为本轮已取消。
  cancelFlag_ = std::make_shared<std::atomic_bool>(false);
  abortRequested_ = false;
  toolStopRequested_ = false;
  toolQueue_.clear();
  batches_.clear();
  batchCursor_ = 0;
  batchPending_ = 0;
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
  if(!userText.isEmpty()) {
    userMessage.parts.append(Part::makeText(userText));
  }
  for(const FilePart & attachment : attachments) {
    userMessage.parts.append(Part::makeFile(attachment));
  }

  messages_.append(userMessage);
  emit messageAdded(userMessage);
  persistMessage(userMessage);

  // 首条用户消息用作会话标题，避免列表里全是"新会话"。
  if(session_.title.trimmed().isEmpty()) {
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

bool AgentRuntime::resolvePermission(const Id & requestId, const PermissionResponse & response)
{
  return permissionGate()->resolve(requestId, response);
}

void AgentRuntime::abort()
{
  if(!isRunning()) {
    return;
  }
  if(abortRequested_) {
    return;  // 幂等
  }
  abortRequested_ = true;
  if(cancelFlag_) {
    cancelFlag_->store(true);
  }

  qCInfo(log) << "收到中断请求; session=" << session_.id;
  setPhase(TurnPhase::Completing);

  // 等待中的权限请求必须先收口，否则回调悬挂会让状态机停住。
  permissionGate()->cancelAll(QCoreApplication::translate("agent::AgentRuntime", "Interrupted by the user"));

  if(activeStream_ != nullptr) {
    ModelStream * stream = activeStream_;
    activeStream_ = nullptr;
    stream->abort();
    // abort() 会触发终止事件，收尾逻辑在 handleStreamEvent 里统一处理；
    // 但若 provider 没有发出终止事件，这里必须兜底，否则永远卡在 Cancelling。
    QTimer::singleShot(0, this, [this, stream]() {
      if(phase_ == TurnPhase::Completing && session_.status == SessionStatus::Running) {
        stream->deleteLater();
        completeTurn(TurnResult::Interrupted, QCoreApplication::translate("agent::AgentRuntime", "Interrupted by the user"));
      }
    });
    return;
  }

  // 没有活跃的流：可能停在工具执行或权限等待上，直接收尾。
  teardownAfterAbort();
  completeTurn(TurnResult::Interrupted, QCoreApplication::translate("agent::AgentRuntime", "Interrupted by the user"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 模型步
// ─────────────────────────────────────────────────────────────────────────────

void AgentRuntime::runModelStep()
{
  if(abortRequested_) {
    teardownAfterAbort();
    completeTurn(TurnResult::Interrupted, QCoreApplication::translate("agent::AgentRuntime", "Interrupted by the user"));
    return;
  }

  if(modelStepCount_ >= maxModelStepsPerTurn_) {
    qCWarning(log) << "触及模型步上限; steps=" << modelStepCount_
                   << "limit=" << maxModelStepsPerTurn_ << "subagent=" << isSubagent();
    completeTurn(TurnResult::Failed,
                 isSubagent()
                 ? QCoreApplication::translate("agent::AgentRuntime", "The subagent exceeded its model-step limit (%1) and was stopped.")
                 .arg(maxModelStepsPerTurn_)
                 : QCoreApplication::translate("agent::AgentRuntime",
                                               "This turn exceeded the safety limit of %1 model steps and was stopped.")
                 .arg(maxModelStepsPerTurn_));
    return;
  }

  if(providers_ == nullptr) {
    completeTurn(TurnResult::Failed, QCoreApplication::translate("agent::AgentRuntime",
                                                                 "The provider registry is not initialized."));
    return;
  }

  ModelInfo info;
  ModelProvider * provider = providers_->resolve(model_, &info);
  if(provider == nullptr) {
    // 这里刻意**不回显 model_.displayValue()**：它通常指向一个已经不存在的
    // provider（用户删掉/重建过 Provider，而持久化的选择还留在配置里），把那个
    // 内部 id 摆给用户看只会把人引向一个不存在的东西。给出可行动的下一步。
    completeTurn(TurnResult::Failed,
                 QCoreApplication::translate("agent::AgentRuntime",
                                             "The model this session uses is no longer available (the provider was deleted or is not fully configured). Pick another model in the picker at the bottom, then send again."));
    return;
  }

  // 先把待投递的后台任务完成通知放进上下文：任务是在 turn 之外异步结束的，
  // 模型不会自己知道，必须由运行时注入。
  drainBackgroundNotifications();

  // 上一个模型步里生成的摘要在这里落地：此刻还没有本步的 assistant 占位消息，
  // 替换消息列表是安全的（放在流式期间替换会把占位消息一起换掉，见 header 说明）。
  if(!deferredCompactionSummary_.isEmpty()) {
    const QString summary = deferredCompactionSummary_;
    deferredCompactionSummary_.clear();
    applyCompactionSummary(summary);
  }

  // 再检查上下文压力（通知也算进去了）：每个模型步开始前收敛一次，
  // 这样长 turn 内部也不会等到超窗才发现。检查是**非阻塞**的——
  // 需要模型摘要时它只发起请求，本步照常用当前上下文发出去。
  autoCompactIfNeeded();

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
  if(!messages_.isEmpty()) {
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

  ModelStream * stream = provider->stream(request);
  if(stream == nullptr) {
    completeTurn(TurnResult::Failed, QCoreApplication::translate("agent::AgentRuntime",
                                                                 "Failed to create the model request."));
    return;
  }
  activeStream_ = stream;

  connect(stream, &ModelStream::event, this, &AgentRuntime::handleStreamEvent,
          Qt::UniqueConnection);
  connect(stream, &ModelStream::finished, this, [this, stream]() {
    if(activeStream_ == stream) {
      activeStream_ = nullptr;
    }
    stream->deleteLater();
    // 流收尾时补应用暂存的摘要：completeTurn() 往往在终态事件处理里就同步跑完了，
    // 那时 activeStream_ 还不是空，补应用的条件不成立——只靠 completeTurn 里的
    // 那次检查会漏掉这最后一轮。这里再兜一次，且不会重复（暂存已清）。
    if(deferredCompactionSummary_.isEmpty()) {
      return;
    }
    if(activeStream_ != nullptr || compactionInFlight_) {
      return;   // 还有别的流在跑（比如同一轮的下一步），留给下一步开始前应用
    }
    const QString deferred = deferredCompactionSummary_;
    deferredCompactionSummary_.clear();
    applyCompactionSummary(deferred);
  });
}

ModelRequest AgentRuntime::buildModelRequest() const
{
  ModelRequest request;
  request.modelId = model_.modelId;

  // 最大输出与上下文窗口都取自"有效模型元信息"（已应用用户在设置里的覆盖）。
  // 不写死 8192：那会让用户设置的 maxOutputTokens 失效，也会让思考预算算错。
  ModelInfo info;
  if(providers_ != nullptr && providers_->resolve(model_, &info) != nullptr) {
    request.maxOutputTokens = info.maxOutputTokens > 0 ? info.maxOutputTokens : 8192;
  }
  else {
    request.maxOutputTokens = 8192;
  }

  // 思考等级 → 两种协议各自的字段。档位不被模型支持时 normalize 会回退，
  // 返回空则完全不带思考参数（例如模型根本不支持思考）。
  const QString levelId =
    normalizeReasoningLevel(model_.reasoningLevel, info.reasoningLevels);
  if(const ReasoningLevel * level = findReasoningLevel(levelId)) {
    const int budget = clampReasoningBudget(level->budgetTokens, request.maxOutputTokens);
    request.enableReasoning = budget > 0;
    request.reasoningBudgetTokens = budget;
    request.reasoningEffort = level->openAiEffort;
  }

  // 温度保持 provider 默认，不主动覆盖——覆盖会改变模型既有行为；
  // 且 Anthropic 在开启 thinking 时不允许同时指定 temperature。

  // 系统提示词与工具声明都走 effective* 系列：估算与请求必须用同一份，
  // 否则"估算说还装得下"和"实际发出去超窗"会各说各话。
  request.tools = effectiveToolSpecs();
  request.systemPrompt = effectiveSystemPrompt();

  // 微压缩：把**旧**工具输出的体积压下来。真正的输出仍留在 messages_ 里
  // （工具卡片要显示完整内容），这里改的只是下发副本。放在全压缩之后：
  // 全压缩已经把早期历史换成了摘要时，这里只会处理剩下的尾部。
  const int beforeTrim = estimateContextTokens(messages_, request.tools, request.systemPrompt);
  const QList<Message> outgoing = microcompactedMessages(messages_, compactionPolicy_);
  const int afterTrim = estimateContextTokens(outgoing, request.tools, request.systemPrompt);
  if(afterTrim < beforeTrim) {
    qCInfo(log) << "微压缩已裁剪旧工具输出; 估算" << beforeTrim << "->" << afterTrim << "tokens";
  }

  // 只投影 user / assistant：内部 System 角色（压缩分隔行等）不进请求。
  // 注意保留 assistant 消息里的 Tool part——provider 需要它们来重建
  // tool_use / tool_calls 块；工具**结果**由单独的 user 消息承载。
  for(const Message & message : outgoing) {
    if(message.role == MessageRole::System) {
      continue;
    }
    Message projected;
    projected.id = message.id;
    projected.sessionId = message.sessionId;
    projected.role = message.role;
    projected.status = message.status;
    projected.createdAtMs = message.createdAtMs;
    projected.modelId = message.modelId;

    for(const Part & part : message.parts) {
      switch(part.kind) {
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
    if(!projected.parts.isEmpty()) {
      request.messages.append(projected);
    }
  }

  return request;
}

void AgentRuntime::handleStreamEvent(const StreamEvent & event)
{
  if(abortRequested_ && !event.isTerminal()) {
    // 已经决定中断，忽略后续增量，但仍然处理终止事件以便收尾。
    return;
  }

  Message * assistant = currentAssistantMessage();
  if(assistant == nullptr) {
    qCWarning(log) << "流事件到达但没有目标消息，已忽略; kind=" << toToken(event.kind);
    return;
  }

  switch(event.kind) {
    case StreamEventKind::Started:
      setPhase(TurnPhase::Streaming);
      break;

    case StreamEventKind::TextDelta: {
        if(currentTextPartId_.isEmpty()) {
          Part part = Part::makeText(QString());
          currentTextPartId_ = part.id;
          const Id partId = appendPart(*assistant, part);
          Q_UNUSED(partId);
        }
        Part * part = assistant->findPart(currentTextPartId_);
        if(part != nullptr) {
          part->text.text += event.text;
          assistant->updatedAtMs = nowMs();
          emit deltaAppended(assistant->id, part->id, event.text, false);
        }
        break;
      }

    case StreamEventKind::ReasoningDelta: {
        if(currentReasoningPartId_.isEmpty()) {
          Part part = Part::makeReasoning(QString());
          currentReasoningPartId_ = part.id;
          appendPart(*assistant, part);
        }
        Part * part = assistant->findPart(currentReasoningPartId_);
        if(part != nullptr) {
          part->reasoning.text += event.text;
          if(!event.signature.isEmpty()) {
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
        Part * part = nullptr;
        for(Part & candidate : assistant->parts) {
          if(candidate.kind == PartKind::Tool &&
             candidate.tool.callId == event.toolCallId) {
            part = &candidate;
            break;
          }
        }
        if(part != nullptr) {
          part->tool.inputText += event.argumentsDelta;
          emit partUpdated(assistant->id, *part);
        }
        break;
      }

    case StreamEventKind::ToolCallEnd: {
        Part * part = nullptr;
        for(Part & candidate : assistant->parts) {
          if(candidate.kind == PartKind::Tool &&
             candidate.tool.callId == event.toolCallId) {
            part = &candidate;
            break;
          }
        }
        if(part == nullptr) {
          qCWarning(log) << "工具结束事件找不到对应块; callId=" << event.toolCallId;
          break;
        }
        part->tool.input = event.toolInput;
        // 入参收齐后进入待批准：真正的"是否执行"由权限链决定。
        part->tool.state = ToolState::PendingApproval;
        part->tool.title = part->tool.name;
        if(tools_ != nullptr) {
          if(Tool * tool = tools_->find(part->tool.name)) {
            part->tool.title = tool->title(part->tool.input);
          }
        }

        QueuedToolCall pending;
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
      if(!event.finishReason.isEmpty()) {
        qCDebug(log) << "模型结束; finishReason=" << event.finishReason;
      }
      finishModelStep(event.finishReason);
      break;

    case StreamEventKind::Failed:
      // 用户主动中断时，provider 会以"请求已取消"之类的错误收尾。
      // 这是**中断**而不是失败：如果按 Failed 处理，UI 会报错、
      // 会话进入 Error 状态，用户会以为自己触发了故障。
      if(abortRequested_) {
        assistant->status = MessageStatus::Interrupted;
        assistant->updatedAtMs = nowMs();
        assistant->cancelPendingTools();
        persistMessage(*assistant);
        emit messageFinished(*assistant);
        teardownAfterAbort();
        completeTurn(TurnResult::Interrupted, QCoreApplication::translate("agent::AgentRuntime", "Interrupted by the user"));
        return;
      }

      assistant->status = MessageStatus::Failed;
      assistant->errorMessage = event.errorMessage;
      assistant->updatedAtMs = nowMs();
      persistMessage(*assistant);
      emit messageFinished(*assistant);
      completeTurn(TurnResult::Failed,
                   event.errorMessage.isEmpty() ? QCoreApplication::translate("agent::AgentRuntime", "The model request failed.")
                   : event.errorMessage);
      break;
  }
}

void AgentRuntime::finishModelStep(const QString & finishReason)
{
  Q_UNUSED(finishReason)

  Message * assistant = currentAssistantMessage();
  if(assistant == nullptr) {
    completeTurn(TurnResult::Failed, QCoreApplication::translate("agent::AgentRuntime",
                                                                 "The model response has no matching message."));
    return;
  }

  modelStepCount_ += 1;
  assistant->updatedAtMs = nowMs();

  if(abortRequested_) {
    assistant->status = MessageStatus::Interrupted;
    assistant->cancelPendingTools();
    persistMessage(*assistant);
    emit messageFinished(*assistant);
    teardownAfterAbort();
    completeTurn(TurnResult::Interrupted, QCoreApplication::translate("agent::AgentRuntime", "Interrupted by the user"));
    return;
  }

  if(toolQueue_.isEmpty()) {
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
  // 先算好批次，再开始执行：批次表是这一轮工具执行的唯一调度依据。
  scheduleToolBatches();
  QTimer::singleShot(0, this, [this]() {
    runToolQueue();
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// 工具调度与执行
//
// 语义本实现的 ToolScheduler + batch-runner：
//   * 组内并行、组间串行
//   * 不并行安全的工具独占一组，与前后调用形成串行屏障
//   * 组内上限 kMaxToolConcurrency
//   * 某个调用要求终止本轮时，**等同组兄弟全部结束**再中断后续组
//   * 失败不截断后续组（只影响该调用自己的结果）
//
// 与 本实现的差异：本实现的调度器还做依赖图的拓扑排序，因为工具可以声明 dependencies。
// 本实现的工具不声明依赖，顺序约束只来自模型给出的调用次序，
// 所以"顺序扫描 + 独占组"就是完整语义，不需要拓扑排序。
// ─────────────────────────────────────────────────────────────────────────────

void AgentRuntime::scheduleToolBatches()
{
  batches_.clear();
  batchCursor_ = 0;
  batchPending_ = 0;

  ToolBatch current;
  const auto flushCurrent = [this, &current]() {
    if(!current.indices.isEmpty()) {
      batches_.append(current);
      current = ToolBatch{};
    }
  };

  for(int index = 0; index < toolQueue_.size(); ++index) {
    const QueuedToolCall & call = toolQueue_.at(index);
    Tool * tool = tools_ != nullptr ? tools_->find(call.name) : nullptr;
    const bool parallelSafe = tool != nullptr && tool->canRunInParallel();

    if(!parallelSafe) {
      // 独占一组：先收掉正在攒的可并行组，再把该调用单独成组。
      flushCurrent();
      ToolBatch exclusive;
      exclusive.indices.append(index);
      exclusive.parallel = false;
      batches_.append(exclusive);
      continue;
    }

    current.indices.append(index);
    if(current.indices.size() >= kMaxToolConcurrency) {
      flushCurrent();
    }
  }
  flushCurrent();

  qCInfo(log) << "工具调度完成; 调用数=" << toolQueue_.size() << "组数=" << batches_.size();
}

void AgentRuntime::runToolQueue()
{
  if(abortRequested_) {
    teardownAfterAbort();
    completeTurn(TurnResult::Interrupted, QCoreApplication::translate("agent::AgentRuntime", "Interrupted by the user"));
    return;
  }

  if(batchCursor_ >= batches_.size()) {
    afterToolQueue();
    return;
  }
  startToolBatch(batchCursor_);
}

void AgentRuntime::startToolBatch(int batchIndex)
{
  const ToolBatch batch = batches_.at(batchIndex);

  // 先把整组标记为"已派发"再逐个启动：派发过程中可能有同步终结的调用
  // （未知工具、参数校验失败、plan 模式拦截），如果边派发边计数，
  // 计数会在循环中途归零、提前推进到下一组。
  for(int index : batch.indices) {
    toolQueue_[index].started = true;
  }
  batchPending_ = static_cast<int>(batch.indices.size());

  setPhase(TurnPhase::ExecutingTools);
  qCDebug(log) << "开始工具组; 序号=" << batchIndex << "调用数=" << batch.indices.size()
               << "并行=" << batch.parallel;

  // 组内全部派发出去、不等待：权限请求与执行都是异步的，
  // 这一组的完成由 finishToolCall() 的计数收口。
  for(int index : batch.indices) {
    executeToolAt(index);
  }
}

void AgentRuntime::finishToolCall(const QString & callId, const ToolResult & result)
{
  // 按 callId 定位：异步回调可能在下一轮 turn 才到达，
  // 那时按下标查找会命中完全不同的调用。
  int index = -1;
  for(int candidate = 0; candidate < toolQueue_.size(); ++candidate) {
    if(toolQueue_.at(candidate).callId == callId) {
      index = candidate;
      break;
    }
  }
  if(index < 0) {
    qCDebug(log) << "工具回调找不到对应调用（可能已被清理）; callId=" << callId;
    return;
  }

  QueuedToolCall & call = toolQueue_[index];
  if(call.finished) {
    // 幂等：参数校验失败、权限拒绝、工具回调都可能到达同一个收口点，
    // 重复计数会让组计数提前归零、打乱后续组的串行屏障。
    qCDebug(log) << "工具已终结，忽略重复回调; callId=" << callId;
    return;
  }
  call.finished = true;
  call.started = false;

  if(Message * message = currentAssistantMessage()) {
    if(Part * target = message->findPart(call.partId)) {
      target->tool.state = result.ok ? ToolState::Success : ToolState::Error;
      target->tool.output = result.output;
      target->tool.error = result.error;
      target->tool.errorCode = result.errorCode;
      // 图片随 part 一起持久化：重启后工具卡片还能显示出当时读到的图。
      target->tool.images = result.images;
      // QJsonObject 没有 merge：逐键写入，工具的 metadata 覆盖同名键。
      for(auto it = result.metadata.constBegin(); it != result.metadata.constEnd(); ++it) {
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
  if(result.stopTurnAfterResult) {
    toolStopRequested_ = true;
  }

  batchPending_ -= 1;
  if(batchPending_ > 0) {
    // 同组还有未完成的调用。必须等它们全部结束——
    // 尤其当上面置了 stopTurnRequested_ 时：同组兄弟的结果同样要提交，
    // 不能因为一个调用要求终止就打断它们。
    return;
  }

  if(toolStopRequested_ || abortRequested_) {
    afterToolQueue();
    return;
  }

  batchCursor_ += 1;
  // 用 singleShot 而不是直接递归：纯内存工具（TodoWrite 等）会同步回调，
  // 直接递归会让 N 组调用产生 N 层栈帧。
  QTimer::singleShot(0, this, [this]() {
    runToolQueue();
  });
}

QString AgentRuntime::sanitizeTitle(const QString & raw)
{
  QString text = raw.trimmed();
  if(text.isEmpty()) {
    return {};
  }

  // 只取第一行：模型经常先来一句"标题：xxx"再补充说明。
  const qsizetype newline = text.indexOf(QLatin1Char('\n'));
  if(newline > 0) {
    text = text.left(newline).trimmed();
  }

  // 去掉常见的前缀、markdown 符号与包裹符号。
  // ⚠ 必须**循环到稳定**：这些符号是嵌套的（`**"标题"**`），
  // 只过一遍的话剥掉外层 `**` 后露出的引号就没有机会再被剥掉。
  static const QStringList prefixes = {
    QStringLiteral("标题："), QStringLiteral("标题:"), QStringLiteral("Title:"),
    QStringLiteral("title:"), QStringLiteral("会话标题："), QStringLiteral("会话标题:"),
  };
  const auto stripOneLayer = [](QString & value) {
    const QString before = value;
    for(const QString & prefix : prefixes) {
      if(value.startsWith(prefix, Qt::CaseInsensitive)) {
        value = value.mid(prefix.size()).trimmed();
      }
    }
    // markdown 标题 / 列表符号
    while(value.startsWith(QLatin1Char('#')) || value.startsWith(QLatin1Char('-')) ||
          value.startsWith(QLatin1Char('*'))) {
      value = value.mid(1).trimmed();
    }
    while(value.endsWith(QLatin1Char('*')) || value.endsWith(QLatin1Char('#')) ||
          value.endsWith(QLatin1Char('-'))) {
      value.chop(1);
      value = value.trimmed();
    }
    // 成对包裹符号。⚠ 中文符号必须用 QChar(码点) 而不是 QLatin1Char：
    // 「」【】在 UTF-8 里是多字节，塞进 QLatin1Char 会被截断成乱码，
    // 比较永远不成立（编译期只有一条 -Wmultichar 警告，很容易漏掉）。
    static const QList<QPair<QChar, QChar>> wrappers = {
      {QChar('"'), QChar('"')},
      {QChar('\''), QChar('\'')},
      {QChar('`'), QChar('`')},
      {QChar(0x300C), QChar(0x300D)},  // 「」
      {QChar(0x3010), QChar(0x3011)},  // 【】
    };
    for(const auto & pair : wrappers) {
      if(value.size() >= 2 && value.startsWith(pair.first) &&
         value.endsWith(pair.second)) {
        value = value.mid(1, value.size() - 2).trimmed();
        break;
      }
    }
    return value != before;
  };
  while(stripOneLayer(text)) {
    // 反复剥到不动为止：嵌套符号一层层露出来。
  }
  // 与回退标题同一个上限，避免侧边栏被一行长文本撑开。
  constexpr int kMaxTitleChars = 40;
  if(text.size() > kMaxTitleChars) {
    text = text.left(kMaxTitleChars).trimmed();
    text += QStringLiteral("…");
  }
  // 全是标点的"标题"没有信息量，宁可保留回退标题。
  if(text.size() < 2) {
    return {};
  }
  return text;
}

void AgentRuntime::applyGeneratedTitle(const QString & raw)
{
  const QString title = sanitizeTitle(raw);
  if(title.isEmpty()) {
    qCWarning(log) << "模型返回的标题不可用，保留原标题; raw="
                   << json::truncate(raw.trimmed(), 120);
    return;
  }
  if(title == session_.title) {
    return;  // 没有变化就不打扰 UI
  }

  session_.title = title;
  session_.titleGenerated = true;
  session_.updatedAtMs = nowMs();
  persistSession();
  qCInfo(log) << "会话标题已由模型生成; session=" << session_.id << "title=" << title;
  emit sessionChanged(session_);
  emit titleGenerated(session_.id, title);
}

void AgentRuntime::requestTitleFromModel()
{
  if(providers_ == nullptr || !model_.isValid() || !hasSession()) {
    return;
  }
  if(titleGenerationInFlight_ || session_.titleGenerated) {
    return;  // 幂等：不重复花这次调用
  }

  // 素材：首条**用户可见**的消息正文。modelOnly 的合成轮（工具结果、
  // 后台任务通知）不是用户说的话，拿它当素材会生成出莫名其妙的标题。
  QString excerpt;
  for(const Message & message : std::as_const(messages_)) {
    if(message.role != MessageRole::User || message.modelOnly) {
      continue;
    }
    excerpt = message.plainText().trimmed();
    if(!excerpt.isEmpty()) {
      break;
    }
  }
  if(excerpt.isEmpty()) {
    return;
  }

  ModelProvider * provider = providers_->resolve(model_);
  if(provider == nullptr) {
    return;
  }

  ModelRequest request;
  request.modelId = model_.modelId;
  request.systemPrompt = QStringLiteral(
                           "You name chat sessions. Reply with a title only — no quotes, no prefix, no "
                           "punctuation at the end, no explanation. Use the same language as the user's "
                           "message. Keep it under 40 characters and make it specific to the task "
                           "(mention the file, feature, or error involved).");
  request.maxOutputTokens = 64;   // 标题很短，给多了只会让它开始解释
  request.temperature = 0.3;      // 要稳定，不要创意
  // 刻意不给工具：这是纯文本改写任务，给工具只会让它试图去读文件。
  request.tools.clear();

  Message user;
  user.id = newMessageId();
  user.sessionId = session_.id;
  user.role = MessageRole::User;
  user.status = MessageStatus::Complete;
  user.parts.append(Part::makeText(excerpt.left(2000)));
  request.messages.append(user);

  ModelStream * stream = provider->stream(request);
  if(stream == nullptr) {
    return;
  }

  titleGenerationInFlight_ = true;
  auto collected = std::make_shared<QString>();
  connect(stream, &ModelStream::event, this,
  [collected](const StreamEvent & event) {
    if(event.kind == StreamEventKind::TextDelta) {
      *collected += event.text;
    }
  });
  connect(stream, &ModelStream::finished, this, [this, stream, collected]() {
    titleGenerationInFlight_ = false;
    stream->deleteLater();
    applyGeneratedTitle(*collected);
  });

  qCDebug(log) << "已请求模型生成会话标题; session=" << session_.id;
}

// ─────────────────────────────────────────────────────────────────────────────
// 上下文压缩
// ─────────────────────────────────────────────────────────────────────────────

void AgentRuntime::setCompactionPolicy(const CompactionPolicy & policy)
{
  compactionPolicy_ = policy;
  // 阈值变了，状态栏里的"何时自动压缩"与进度条压力都要立刻反映。
  remeasureContext();
}

void AgentRuntime::setContextWindowOverride(int tokens)
{
  contextWindowOverride_ = tokens > 0 ? tokens : 0;
  remeasureContext();
}

bool AgentRuntime::compactContextNow(QString * errorOut)
{
  if(!hasSession()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "No active session.");
    }
    return false;
  }
  if(providers_ == nullptr || !model_.isValid()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime",
                                              "No usable model is configured, so no summary can be generated.");
    }
    return false;
  }
  if(compactionInFlight_) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "Compaction is already in progress.");
    }
    return false;
  }
  // 只压缩"已经完成"的历史：正在流式的 assistant 占位消息既不完整，
  // 也不该被摘要覆盖。
  if(isRunning()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime",
                                              "A turn is running. Compact after it finishes or after you interrupt it.");
    }
    return false;
  }
  return startCompactionSummary(true, errorOut);
}

void AgentRuntime::autoCompactIfNeeded()
{
  if(compactionInFlight_ || providers_ == nullptr || !model_.isValid()) {
    return;
  }
  if(messages_.size() < static_cast<qsizetype>(compactionPolicy_.minCompactionMessages)) {
    return;
  }

  const int used = contextTokens();
  const int window = effectiveContextWindow();
  const int microThreshold = compactionPolicy_.microThresholdTokens(window);
  const int fullThreshold = compactionPolicy_.fullThresholdTokens(window);

  // 一级：微压缩。它在 buildModelRequest() 里每次请求都会做，这里只是把
  // "已经进入压缩区"告诉用户一次，免得他们以为界面卡住了。
  if(used >= microThreshold && !compactionWarned_) {
    compactionWarned_ = true;
    emit compactionNotice(QCoreApplication::translate("agent::AgentRuntime",
                                                      "Context is %1% full; old tool output will be trimmed on the way out.")
                          .arg(window > 0 ? used * 100 / window : 0));
  }

  if(used < fullThreshold) {
    return;
  }

  qCInfo(log) << "上下文触及全压缩阈值; used=" << used << "threshold=" << fullThreshold;
  if(!startCompactionSummary(false, nullptr)) {
    // 起不来（消息太少等）不是错误：这一步本来就用当前上下文发出去，
    // 由 provider 决定是否超窗。
    qCDebug(log) << "自动压缩未发起; used=" << used;
  }
}

bool AgentRuntime::startCompactionSummary(bool force, QString * errorOut)
{
  const FullCompaction plan = buildFullCompaction(messages_, compactionPolicy_);
  if(!plan.applied) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime",
                                              "There is no history to compact (too few messages, or all of them are still inside the protected window).");
    }
    return false;
  }

  ModelProvider * provider = providers_->resolve(model_);
  if(provider == nullptr) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "No usable model found.");
    }
    return false;
  }

  ModelRequest request;
  request.modelId = model_.modelId;
  request.systemPrompt = buildSummaryPrompt();
  // 摘要是一次"读多写少"的任务：给足输出预算，但不需要工具。
  request.maxOutputTokens = 4096;
  request.temperature = 0.2;   // 要忠实的记录，不要发挥
  request.tools.clear();

  Message user;
  user.id = newMessageId();
  user.sessionId = session_.id;
  user.role = MessageRole::User;
  user.status = MessageStatus::Complete;
  user.modelOnly = true;
  user.createdAtMs = nowMs();
  user.updatedAtMs = user.createdAtMs;
  user.parts.append(Part::makeText(
                      QStringLiteral("以下是需要压缩的对话历史：\n\n") + plan.transcript));
  request.messages.append(user);

  ModelStream * stream = provider->stream(request);
  if(stream == nullptr) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("agent::AgentRuntime", "Failed to create the summary request.");
    }
    return false;
  }

  pendingCompaction_ = plan;
  compactionInFlight_ = true;
  compactionStream_ = stream;

  auto collected = std::make_shared<QString>();
  connect(stream, &ModelStream::event, this,
  [collected](const StreamEvent & event) {
    if(event.kind == StreamEventKind::TextDelta) {
      *collected += event.text;
    }
  });
  connect(stream, &ModelStream::finished, this, [this, stream, collected]() {
    if(compactionStream_ == stream) {
      compactionStream_ = nullptr;
    }
    stream->deleteLater();
    if(!compactionInFlight_) {
      return;   // closeSession/换会话已经收口过，别再动当前会话
    }
    compactionInFlight_ = false;
    const QString summary = collected->trimmed();
    if(summary.isEmpty()) {
      abortCompactionSummary(QCoreApplication::translate("agent::AgentRuntime",
                                                         "The summary model returned nothing, so this compaction was skipped."));
      return;
    }
    // 流还活着（调用工具时流会短暂停止，随后同一轮还会再起一次）就先只记录：
    // 现在替换消息列表会把这一轮正在用的消息换掉。
    if(activeStream_ != nullptr) {
      deferredCompactionSummary_ = summary;
      qCInfo(log) << "摘要已就绪，延后到下一个模型步应用; session=" << session_.id;
      return;
    }
    applyCompactionSummary(summary);
  });

  if(force) {
    emit compactionNotice(QCoreApplication::translate("agent::AgentRuntime",
                                                      "Compacting the context (generating a summary)…"));
  }
  qCInfo(log) << "已请求上下文压缩摘要; session=" << session_.id
              << "待归档消息=" << plan.replacedMessageCount
              << "尾部保留=" << plan.tailMessages.size() << "强制=" << force;
  return true;
}

void AgentRuntime::applyCompactionSummary(const QString & summary)
{
  const FullCompaction plan = pendingCompaction_;
  pendingCompaction_ = FullCompaction{};
  if(!plan.applied) {
    return;   // 状态已被清理过（换会话），丢弃这次结果
  }

  // 先归档、后替换内存：归档失败时保持内存不变，宁可这次压缩白跑，
  // 也不能把用户的历史从活动列表里抹掉却没有任何副本。
  if(store_ != nullptr && store_->isOpen()) {
    if(!store_->archiveMessages(plan.headMessages)) {
      qCWarning(log) << "压缩归档失败，放弃本次压缩:" << store_->lastError();
      abortCompactionSummary(QCoreApplication::translate("agent::AgentRuntime",
                                                         "Compaction could not complete: archiving the history failed."));
      return;
    }
  }

  const Message summaryMessage =
    makeCompactionSummaryMessage(session_.id, summary, model_.modelId);
  const Message markerMessage =
    makeCompactionMarkerMessage(session_.id, summary, plan.replacedMessageCount);

  QList<Message> rebuilt;
  rebuilt.reserve(plan.tailMessages.size() + 2);
  rebuilt.append(summaryMessage);
  rebuilt.append(plan.tailMessages);
  // 分隔行放在摘要之后、尾部之前：用户翻到压缩点时会先看到"这里压缩过"，
  // 然后是摘要（对模型可见、对用户隐藏），再往后是继续的对话。
  rebuilt.insert(1, markerMessage);

  messages_ = rebuilt;
  compactionSummary_ = summary;
  compactionWarned_ = false;

  // 新消息也要落盘，否则重开会话时摘要与分隔行就没了
  // （而活动历史已经被删掉，等于永久丢失）。
  persistMessage(summaryMessage);
  persistMessage(markerMessage);
  refreshContextUsage();
  persistSession();

  qCInfo(log) << "上下文压缩完成; 归档=" << plan.replacedMessageCount
              << "尾部=" << plan.tailMessages.size()
              << "摘要长度=" << summary.size()
              << "压缩后估算=" << contextTokens();

  // 整体替换没法用增量信号表达，让界面重建对话流。
  emit conversationReplaced();
  emit sessionChanged(session_);
  emit compactionNotice(QCoreApplication::translate("agent::AgentRuntime",
                                                    "Context compacted: %1 earlier messages were archived into a summary; now about %2 tokens.")
                        .arg(plan.replacedMessageCount)
                        .arg(contextTokens()));
}

void AgentRuntime::abortCompactionSummary(const QString & reason)
{
  compactionInFlight_ = false;
  pendingCompaction_ = FullCompaction{};
  if(!reason.isEmpty()) {
    qCWarning(log) << "压缩未完成:" << reason;
    emit compactionNotice(reason);
  }
}

void AgentRuntime::drainBackgroundNotifications()
{
  if(backgroundTasks_ == nullptr || !backgroundTasks_->hasPendingNotification()) {
    return;
  }

  QStringList notifications;
  const QList<BackgroundTask> tasks = backgroundTasks_->tasks();
  for(const BackgroundTask & task : tasks) {
    const QString notification = backgroundTasks_->takeNotification(task.id);
    if(!notification.isEmpty()) {
      notifications.append(notification);
    }
  }
  if(notifications.isEmpty()) {
    return;
  }

  Message message;
  message.id = newMessageId();
  message.sessionId = session_.id;
  message.role = MessageRole::User;
  message.status = MessageStatus::Complete;
  // model-only：这是运行时生成的上下文，不是用户说过的话。
  // （与工具结果轮同样的处理，UI 不会把它显示成一条"你"的消息。）
  message.modelOnly = true;
  message.createdAtMs = nowMs();
  message.updatedAtMs = message.createdAtMs;
  message.parts.append(Part::makeText(notifications.join(QStringLiteral("\n\n"))));

  messages_.append(message);
  emit messageAdded(message);
  persistMessage(message);

  qCInfo(log) << "已注入后台任务完成通知; 条数=" << notifications.size();
}

void AgentRuntime::cancelPendingToolCalls(const QString & reason)
{
  Message * assistant = currentAssistantMessage();
  for(int index = 0; index < toolQueue_.size(); ++index) {
    QueuedToolCall & call = toolQueue_[index];
    if(call.finished) {
      continue;
    }
    call.finished = true;
    call.started = false;

    if(assistant == nullptr) {
      continue;
    }
    Part * part = assistant->findPart(call.partId);
    if(part == nullptr || toolStateIsTerminal(part->tool.state)) {
      continue;
    }
    part->tool.state = ToolState::Cancelled;
    part->tool.error = reason;
    part->tool.errorCode = QStringLiteral("cancelled");
    part->tool.endedAtMs = nowMs();
    emit partUpdated(assistant->id, *part);
  }
  batchPending_ = 0;
}

void AgentRuntime::executeToolAt(int index)
{
  if(index < 0 || index >= toolQueue_.size()) {
    return;
  }

  const QueuedToolCall call = toolQueue_.at(index);
  const QString callId = call.callId;
  Message * assistant = currentAssistantMessage();
  if(assistant == nullptr) {
    finishToolCall(callId,
                   ToolResult::failure(QStringLiteral("工具无处写回：消息已不存在"),
                                       QStringLiteral("message_missing")));
    return;
  }

  if(assistant->findPart(call.partId) == nullptr) {
    qCWarning(log) << "工具块已不存在，跳过; callId=" << callId;
    finishToolCall(callId, ToolResult::failure(QStringLiteral("工具块已不存在"),
                                               QStringLiteral("part_missing")));
    return;
  }

  // 工具名未知：直接记为失败，不询问权限（执行不了的东西不该打扰用户）。
  Tool * tool = tools_ != nullptr ? tools_->find(call.name) : nullptr;
  if(tool == nullptr) {
    qCWarning(log) << "未知工具:" << call.name;
    finishToolCall(callId, ToolResult::failure(QStringLiteral("未知工具：") + call.name,
                                               QStringLiteral("tool_not_found")));
    return;
  }

  // 白名单的第二道闸门：声明层已经不给模型看这些工具了，
  // 但历史消息里可能残留旧声明，或模型凭记忆直接调用，所以执行侧必须再拦一次。
  if(!toolAllowlist_.isEmpty() && !toolAllowlist_.contains(call.name)) {
    qCWarning(log) << "工具不在当前白名单内，拒绝执行:" << call.name;
    finishToolCall(callId,
                   ToolResult::failure(
                     QStringLiteral("工具 %1 不在当前子代理的可用范围内。").arg(call.name),
                     QStringLiteral("tool_not_allowed")));
    return;
  }

  const ToolMetadata metadata = tool->metadata();

  // 入参校验放在权限之前：不合法的调用不该让用户看到确认框。
  const QString validationError = tool->validateInput(call.input);
  if(!validationError.isEmpty()) {
    finishToolCall(callId, ToolResult::failure(validationError,
                                               QStringLiteral("invalid_input")));
    return;
  }

  // plan 模式下带副作用的工具一律拒绝，且不弹窗（模式语义，不是用户决定）。
  if(session_.mode == SessionMode::Plan &&
     sideEffectScopeWritesWorkspace(metadata.sideEffectScope)) {
    const QString reason =
      QStringLiteral("当前处于 plan（只读）模式，不能执行会修改工作区的工具：") + call.name;
    finishToolCall(callId, ToolResult::failure(reason, QStringLiteral("plan_mode_denied")));
    return;
  }

  const PermissionKind kind = permissionKindFor(metadata);
  const QString capability = tool->permissionCapability();
  const QString subject = tool->ruleSubject(call.input);

  PermissionRequest request;
  request.id = newPermissionId();
  request.sessionId = session_.id;
  request.callId = call.callId;
  request.toolName = call.name;
  request.input = call.input;
  request.kind = kind;
  request.riskLevel = metadata.riskLevel;
  request.needsApproval = metadata.needsApproval;
  request.title = tool->title(call.input);
  request.description = tool->permissionDescription(call.input);
  request.createdAtMs = nowMs();

  // 选项由工具声明的规则生成；UI 不自行发明选项。
  const QList<PermissionRule> suggestedRules = tool->permissionRules(call.input);

  PermissionOption allowOnce;
  allowOnce.optionId = QStringLiteral("allowOnce");
  allowOnce.label = QCoreApplication::translate("agent::AgentRuntime", "Allow once");
  allowOnce.kind = PermissionOptionKind::AllowOnce;
  allowOnce.response.decision = PermissionDecision::Allow;
  request.options.append(allowOnce);

  for(const PermissionRule & rule : suggestedRules) {
    if(rule.behavior != PermissionRuleBehavior::Allow) {
      continue;
    }
    PermissionOption allowAlways;
    allowAlways.optionId = QStringLiteral("allowAlways");
    allowAlways.label = QCoreApplication::translate("agent::AgentRuntime", "Always allow: ") + rule.toolName +
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
  deny.label = QCoreApplication::translate("agent::AgentRuntime", "Deny");
  deny.kind = PermissionOptionKind::Deny;
  deny.response.decision = PermissionDecision::Deny;
  request.options.append(deny);

  const Id partId = call.partId;
  const QJsonObject toolInput = call.input;

  // 按值捕获：权限裁决与工具执行都可能是异步的，
  // 这个栈帧那时早已返回，引用捕获会悬空。
  permissionGate()->request(
    request, capability, subject,
  [this, callId, partId, request, tool, toolInput](PermissionOutcome outcome) {
    Message * message = currentAssistantMessage();
    Part * target = message != nullptr ? message->findPart(partId) : nullptr;
    if(message == nullptr || target == nullptr) {
      finishToolCall(callId,
                     ToolResult::failure(QStringLiteral("工具块已不存在"),
                                         QStringLiteral("part_missing")));
      return;
    }

    if(!outcome.allowed()) {
      const QString reason = outcome.response.reason.isEmpty()
                             ? QStringLiteral("用户拒绝了该工具调用。")
                             : outcome.response.reason;
      ToolResult denied =
        ToolResult::failure(reason, QStringLiteral("permission_denied"));
      denied.metadata.insert(QStringLiteral("permissionDenied"), true);
      finishToolCall(callId, denied);
      return;
    }

    // 放行：进入运行态并真正执行。
    target->tool.state = ToolState::Running;
    target->tool.startedAtMs = nowMs();
    target->tool.approvalInteractionId = request.id;
    emit partUpdated(message->id, *target);

    if(abortRequested_) {
      finishToolCall(callId,
                     ToolResult::failure(QStringLiteral("因中断未执行。"),
                                         QStringLiteral("cancelled")));
      return;
    }

    ToolContext context;
    context.sessionId = session_.id;
    context.turnId = currentTurnId_;
    context.callId = callId;
    context.workspace = session_.workspace;
    context.workingDirectory = session_.workspace.path;
    context.permissionMode = permissionGate()->mode();
    context.readOnly = session_.mode == SessionMode::Plan;
    context.grantedRules = permissionGate()->grantedRules();
    context.todoStore = todos_;
    // 子代理宿主就是本运行时。子代理里 subagentsEnabled() 为 false，
    // Agent 工具据此拒绝递归派生。
    context.subagentHost = this;
    context.backgroundTasks = backgroundTasks_;
    context.skills = skills_;
    // 并发执行时所有工具共享同一个取消令牌；这是刻意的：
    // 用户按一次"停止"应当让整轮的所有工具都停下。
    context.cancelled = cancelFlag_;

    const Id messageId = message->id;
    context.progress = [this, messageId, partId](const QString & chunk) {
      for(Message & candidate : messages_) {
        if(candidate.id != messageId) {
          continue;
        }
        if(Part * toolPart = candidate.findPart(partId)) {
          // 进度只进 metadata，不覆盖最终输出，
          // 这样 UI 可以区分"正在输出"和"最终结果"。
          toolPart->tool.progress.insert(QStringLiteral("preview"), chunk);
          toolPart->tool.progress.insert(QStringLiteral("updatedAtMs"),
                                         static_cast<double>(nowMs()));
          emit partUpdated(messageId, *toolPart);
        }
        break;
      }
    };

    Tool * executingTool = tool;
    executingTool->execute(toolInput, context, [this, callId](ToolResult result) {
      finishToolCall(callId, result);
    });
  });
}

void AgentRuntime::afterToolQueue()
{
  if(abortRequested_) {
    teardownAfterAbort();
    completeTurn(TurnResult::Interrupted, QCoreApplication::translate("agent::AgentRuntime", "Interrupted by the user"));
    return;
  }

  // 未派发的调用必须在这里显式取消。
  // 正常跑完时所有调用都已终结，这里是空操作；而某组要求提前终止时
  // （stopTurnAfterResult 会让后续组直接跳过），剩下的调用否则会永远停在
  // "接收参数中"，UI 一直显示未完成，回灌给模型的结果里也是半成品。
  // 对应 batch-runner：命中 stopTurnAfterResult 时把后续组标为 ToolCancelled。
  cancelPendingToolCalls(QStringLiteral("本轮已结束，未执行。"));

  Message * assistant = currentAssistantMessage();
  if(assistant != nullptr) {
    assistant->updatedAtMs = nowMs();
    persistMessage(*assistant);
  }

  // 终止条件 2：某个工具要求结束本轮。
  if(toolStopRequested_) {
    toolQueue_.clear();
    batches_.clear();
    batchCursor_ = 0;
    batchPending_ = 0;
    toolStopRequested_ = false;
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

  for(const QueuedToolCall & call : std::as_const(toolQueue_)) {
    if(assistant == nullptr) {
      break;
    }
    const Part * source = assistant->findPart(call.partId);
    if(source == nullptr || source->kind != PartKind::Tool) {
      continue;
    }

    Part result = *source;
    result.id = newPartId();  // 新消息里的 part 需要自己的 id
    switch(source->tool.state) {
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

    // 结果里的图片要以 **File part** 的形式进这条消息，而不是只留在
    // Tool part 的字段里：provider 的图片序列化走的是 File part，
    // 只挂在 Tool 上模型依然看不到像素。
    for(const FilePart & image : result.tool.images) {
      toolResults.parts.append(Part::makeFile(image));
    }
  }

  if(!toolResults.parts.isEmpty()) {
    messages_.append(toolResults);
    emit messageAdded(toolResults);
    persistMessage(toolResults);
  }

  toolQueue_.clear();
  batches_.clear();
  batchCursor_ = 0;
  batchPending_ = 0;
  toolStopRequested_ = false;

  // 回到模型步：这是"工具→再思考"的循环边。
  // 同样延后一跳，避免同步完成的工具把整条 turn 压进调用栈。
  QTimer::singleShot(0, this, [this]() {
    runModelStep();
  });
}

void AgentRuntime::teardownAfterAbort()
{
  if(cancelFlag_) {
    cancelFlag_->store(true);
  }

  // 未终结的调用（含未派发的与正在运行的）都要有交代，否则 UI 会永远显示
  // "等待确认"或"运行中"。运行中的工具由各自的取消令牌负责尽快返回，
  // 这里先把状态与界面收口。
  cancelPendingToolCalls(QStringLiteral("因中断未执行。"));

  // 把当前 assistant 消息里所有未终态的工具关闭。
  if(Message * assistant = currentAssistantMessage()) {
    assistant->cancelPendingTools();
    if(!assistant->isTerminal()) {
      assistant->status = MessageStatus::Interrupted;
    }
    assistant->updatedAtMs = nowMs();
    persistMessage(*assistant);
    emit messageFinished(*assistant);
  }

  toolQueue_.clear();
  batches_.clear();
  batchCursor_ = 0;
  batchPending_ = 0;
}

void AgentRuntime::completeTurn(TurnResult result, const QString & errorMessage)
{
  lastResult_ = result;

  switch(result) {
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

  // 本轮里生成、但为了不打断流式而暂存的摘要在这里补上：turn 已经结束，
  // 不会再建新的占位消息，替换是安全的。不补的话，一次在 turn 中途生成好的
  // 压缩就白跑了，上下文要一直涨到下一轮才会收敛。
  if(!deferredCompactionSummary_.isEmpty() && activeStream_ == nullptr) {
    const QString summary = deferredCompactionSummary_;
    deferredCompactionSummary_.clear();
    applyCompactionSummary(summary);
  }

  refreshContextUsage();
  persistSession();
  emit sessionChanged(session_);
  setPhase(result == TurnResult::Failed ? TurnPhase::Error : TurnPhase::Completing);

  qCInfo(log) << "turn 结束; result=" << static_cast<int>(result)
              << "模型步=" << modelStepCount_ << "工具调用=" << toolCallCount_
              << "inputTokens=" << turnUsage_.inputTokens
              << "outputTokens=" << turnUsage_.outputTokens;

  if(!errorMessage.isEmpty() && result == TurnResult::Failed) {
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

void AgentRuntime::setPhase(TurnPhase phase)
{
  if(phase_ == phase) {
    return;
  }
  phase_ = phase;
  qCDebug(log) << "相位 ->" << toToken(phase);

  // RunState 由 TurnPhase 推导，不独立维护。
  RunState state = RunState::Idle;
  switch(phase) {
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

Message * AgentRuntime::currentAssistantMessage()
{
  if(currentAssistantMessageId_.isEmpty()) {
    return nullptr;
  }
  for(Message & message : messages_) {
    if(message.id == currentAssistantMessageId_) {
      return &message;
    }
  }
  return nullptr;
}

Id AgentRuntime::appendPart(Message & message, const Part & part)
{
  message.parts.append(part);
  message.updatedAtMs = nowMs();
  emit partAppended(message.id, part);
  return part.id;
}

bool AgentRuntime::updatePart(Message & message, const Part & part)
{
  if(!message.replacePart(part)) {
    return false;
  }
  message.updatedAtMs = nowMs();
  emit partUpdated(message.id, part);
  return true;
}

void AgentRuntime::persistMessage(const Message & message)
{
  if(store_ == nullptr || !store_->isOpen()) {
    return;
  }
  if(!store_->saveMessage(message)) {
    // 落盘失败不阻断运行，但必须显式告警：用户可能因此丢失历史。
    qCWarning(log) << "消息落盘失败; id=" << message.id << store_->lastError();
  }
}

void AgentRuntime::persistSession()
{
  if(store_ == nullptr || !store_->isOpen()) {
    return;
  }
  if(!store_->saveSession(session_)) {
    qCWarning(log) << "会话落盘失败; id=" << session_.id << store_->lastError();
  }
}

void AgentRuntime::refreshContextUsage()
{
  const int used = contextTokens();
  const int window = effectiveContextWindow();
  const int microThreshold = compactionPolicy_.microThresholdTokens(window);
  const int fullThreshold = compactionPolicy_.fullThresholdTokens(window);

  QJsonObject usage;
  usage.insert(QStringLiteral("usedTokens"), used);
  usage.insert(QStringLiteral("maxTokens"), window);
  usage.insert(QStringLiteral("percent"),
               window > 0 ? (static_cast<double>(used) * 100.0 / window) : 0.0);
  // 两个阈值一并下发：状态栏要画"什么时候会自动压缩"，不能只让引擎自己知道。
  usage.insert(QStringLiteral("autoCompactThresholdTokens"), microThreshold);
  usage.insert(QStringLiteral("fullCompactThresholdTokens"), fullThreshold);
  if(!compactionSummary_.isEmpty()) {
    usage.insert(QStringLiteral("compacted"), true);
  }
  session_.contextUsage = usage;
}

int AgentRuntime::contextTokens() const
{
  // 估算走压缩模块的**同一条**路径：消息 + 工具声明 + 系统提示词。
  // 早先只统计对话正文，工具 schema 与提示词（几千 token 的固定成本）
  // 完全不计，窗口快满时会低估到压缩根本不触发。
  return estimateContextTokens(messages_, effectiveToolSpecs(), effectiveSystemPrompt());
}

QList<ToolSpec> AgentRuntime::effectiveToolSpecs() const
{
  if(tools_ == nullptr) {
    return {};
  }
  // 白名单非空时只声明这些工具：子代理的只读 profile 靠它收窄工具面。
  return toolAllowlist_.isEmpty() ? tools_->specs() : tools_->specsFor(toolAllowlist_);
}

QString AgentRuntime::effectiveSystemPrompt() const
{
  SystemPromptInput promptInput;
  promptInput.workspace = session_.workspace;
  promptInput.cwd = session_.workspace.path;
  promptInput.mode = session_.mode;
  promptInput.permissionMode = permissionGate()->mode();
  promptInput.appVersion = QStringLiteral(LYCODE_QT_VERSION);
  promptInput.tools = effectiveToolSpecs();
  promptInput.subagentType = subagentType_;
  // 技能清单每次请求重新生成：用户中途加了 skill 不该要重启应用。
  if(skills_ != nullptr) {
    promptInput.skillsSection = skills_->promptSection();
  }
  promptInput.subagentDescription = subagentDescription_;
  return SystemPromptBuilder::build(promptInput);
}

int AgentRuntime::effectiveContextWindow() const
{
  // 优先级：用户手工覆盖 > 模型元信息（已含设置页的按模型覆盖）> 保守默认。
  if(contextWindowOverride_ > 0) {
    return contextWindowOverride_;
  }
  if(providers_ != nullptr && model_.isValid()) {
    ModelInfo info;
    if(providers_->resolve(model_, &info) != nullptr && info.contextWindow > 0) {
      return info.contextWindow;
    }
  }
  return 128000;
}

Usage AgentRuntime::lastTurnUsage() const
{
  // 从后往前找第一条带用量的 assistant 消息（工具结果轮的用量是 0，会跳过）。
  for(auto iterator = messages_.crbegin(); iterator != messages_.crend(); ++iterator) {
    if(iterator->role != MessageRole::Assistant) {
      continue;
    }
    if(iterator->usage.effectiveTotal() > 0 || iterator->usage.cacheReadTokens > 0) {
      return iterator->usage;
    }
  }
  return {};
}

RunState AgentRuntime::runState() const
{
  switch(phase_) {
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

bool AgentRuntime::isRunning() const
{
  return phase_ != TurnPhase::Idle;
}

void AgentRuntime::setMode(SessionMode mode)
{
  if(session_.mode == mode) {
    return;
  }
  qCInfo(log) << "会话模式变更:" << toToken(session_.mode) << "->" << toToken(mode);
  session_.mode = mode;
  // 模式决定默认权限策略；用户已授予的规则保留（显式授权不该被模式切换抹掉）。
  permissionGate()->setMode(defaultPermissionModeFor(mode));
  session_.updatedAtMs = nowMs();
  persistSession();
  emit sessionChanged(session_);
}

void AgentRuntime::setModel(const ModelSelection & model)
{
  if(model.providerId == model_.providerId && model.modelId == model_.modelId &&
     model.reasoningLevel == model_.reasoningLevel) {
    return;
  }
  qCInfo(log) << "模型变更:" << model_.displayValue() << "->" << model.displayValue();
  model_ = model;
  session_.modelId = model.modelId;
  session_.providerId = model.providerId;
  session_.updatedAtMs = nowMs();
  // 换模型就换了上下文窗口，分母必须立刻跟着变。
  refreshContextUsage();
  persistSession();
  emit sessionChanged(session_);
}

void AgentRuntime::remeasureContext()
{
  if(!hasSession()) {
    return;
  }
  refreshContextUsage();
  session_.updatedAtMs = nowMs();
  persistSession();
  emit sessionChanged(session_);
}

}  // namespace lycode

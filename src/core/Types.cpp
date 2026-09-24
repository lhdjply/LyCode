#include "core/Types.h"

#include "core/Ids.h"
#include "core/Json.h"

#include <QDateTime>
#include <QDir>
#include <QJsonValue>
#include <QLoggingCategory>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.types")

/// 统一的枚举 ↔ 字符串表驱动转换。
/// 用表而不是 if 链，保证 to/from 两个方向不会各自漂移。
template <typename Enum>
struct EnumName {
  Enum value;
  const char * name;
};

template <typename Enum, size_t N>
QString nameOf(const EnumName<Enum> (&table)[N], Enum value, const QString & fallback)
{
  for(const auto & entry : table) {
    if(entry.value == value) {
      return QString::fromLatin1(entry.name);
    }
  }
  qCWarning(log) << "未知枚举值，使用回退名:" << fallback;
  return fallback;
}

/// 解析失败时降级到 fallback，并记录原始字面量。
/// 明确不抛异常、不丢弃记录：本实现多次因为闭集枚举加值而整帧被丢。
template <typename Enum, size_t N>
Enum valueOf(const EnumName<Enum> (&table)[N], const QString & text, Enum fallback)
{
  for(const auto & entry : table) {
    if(text == QLatin1String(entry.name)) {
      return entry.value;
    }
  }
  if(!text.isEmpty()) {
    qCWarning(log) << "未知枚举字面量，使用回退值:" << text;
  }
  return fallback;
}

constexpr EnumName<MessageRole> kMessageRoles[] = {
  {MessageRole::User, "user"},
  {MessageRole::Assistant, "assistant"},
  {MessageRole::System, "system"},
};

constexpr EnumName<MessageStatus> kMessageStatuses[] = {
  {MessageStatus::Pending, "pending"},
  {MessageStatus::Streaming, "streaming"},
  {MessageStatus::Complete, "complete"},
  {MessageStatus::Interrupted, "interrupted"},
  {MessageStatus::Failed, "failed"},
};

constexpr EnumName<PartKind> kPartKinds[] = {
  {PartKind::Text, "text"},
  {PartKind::Reasoning, "reasoning"},
  {PartKind::Tool, "tool"},
  {PartKind::File, "file"},
  {PartKind::Artifact, "artifact"},
  {PartKind::Subagent, "subagent"},
  {PartKind::Timeline, "timeline"},
  {PartKind::Step, "step"},
};

constexpr EnumName<ToolState> kToolStates[] = {
  {ToolState::InputStreaming, "inputStreaming"},
  {ToolState::PendingApproval, "pendingApproval"},
  {ToolState::Running, "running"},
  {ToolState::Success, "success"},
  {ToolState::Error, "error"},
  {ToolState::Cancelled, "cancelled"},
};

constexpr EnumName<TimelineKind> kTimelineKinds[] = {
  {TimelineKind::ContextCompaction, "context_compaction"},
  {TimelineKind::GoalVerification, "goal_verification"},
  {TimelineKind::SessionFork, "session_fork"},
  {TimelineKind::ModelChange, "model_change"},
  {TimelineKind::Retry, "retryNotice"},
  {TimelineKind::CheckpointRestored, "checkpointRestored"},
  {TimelineKind::Unknown, "unknown"},
};

constexpr EnumName<SessionStatus> kSessionStatuses[] = {
  {SessionStatus::Draft, "draft"},
  {SessionStatus::Prewarming, "prewarming"},
  {SessionStatus::Running, "running"},
  {SessionStatus::CompletedSuccess, "completedSuccess"},
  {SessionStatus::CompletedInterrupted, "completedInterrupted"},
  {SessionStatus::Error, "error"},
};

constexpr EnumName<SessionMode> kSessionModes[] = {
  {SessionMode::Plan, "plan"},
  {SessionMode::Build, "build"},
  {SessionMode::Edit, "edit"},
  {SessionMode::Yolo, "yolo"},
  {SessionMode::Auto, "auto"},
};

constexpr EnumName<SessionKind> kSessionKinds[] = {
  {SessionKind::Interactive, "interactive"},
  {SessionKind::Fork, "fork"},
  {SessionKind::SelectionSideChat, "selection_side_chat"},
  {SessionKind::WorkflowParent, "workflow_parent"},
  {SessionKind::WorkflowChild, "workflow_child"},
  {SessionKind::SubagentChild, "subagent_child"},
  {SessionKind::NestedWorkflowChild, "nested_workflow_child"},
};

constexpr EnumName<PermissionKind> kPermissionKinds[] = {
  {PermissionKind::Read, "read"},
  {PermissionKind::Write, "write"},
  {PermissionKind::Execute, "execute"},
  {PermissionKind::Network, "network"},
  {PermissionKind::Other, "other"},
};

constexpr EnumName<RiskLevel> kRiskLevels[] = {
  {RiskLevel::Low, "low"},
  {RiskLevel::Medium, "medium"},
  {RiskLevel::High, "high"},
  {RiskLevel::Critical, "critical"},
};

constexpr EnumName<PermissionMode> kPermissionModes[] = {
  {PermissionMode::Default, "default"},
  {PermissionMode::AcceptEdits, "accept_edits"},
  {PermissionMode::Plan, "plan"},
  {PermissionMode::BypassPermissions, "bypass_permissions"},
};

constexpr EnumName<PermissionRuleBehavior> kPermissionRuleBehaviors[] = {
  {PermissionRuleBehavior::Allow, "allow"},
  {PermissionRuleBehavior::Deny, "deny"},
  {PermissionRuleBehavior::Ask, "ask"},
};

constexpr EnumName<PermissionDecision> kPermissionDecisions[] = {
  {PermissionDecision::Allow, "allow"},
  {PermissionDecision::Deny, "deny"},
  {PermissionDecision::Escalate, "escalate"},
  {PermissionDecision::Modify, "modify"},
};

constexpr EnumName<PermissionOptionKind> kPermissionOptionKinds[] = {
  {PermissionOptionKind::AllowOnce, "allowOnce"},
  {PermissionOptionKind::AllowAlways, "allowAlways"},
  {PermissionOptionKind::Deny, "deny"},
  {PermissionOptionKind::Custom, "custom"},
};

constexpr EnumName<ProviderKind> kProviderKinds[] = {
  {ProviderKind::Anthropic, "anthropic"},
  {ProviderKind::OpenAICompatible, "openai_compatible"},
};

constexpr EnumName<AccountAvailability> kAccountAvailabilities[] = {
  {AccountAvailability::Available, "available"},
  {AccountAvailability::Pending, "pending"},
  {AccountAvailability::Unavailable, "unavailable"},
  {AccountAvailability::Unknown, "unknown"},
};

constexpr EnumName<AccountUnavailableReason> kAccountUnavailableReasons[] = {
  {AccountUnavailableReason::None, "none"},
  {AccountUnavailableReason::NotAuthenticated, "not-authenticated"},
  {AccountUnavailableReason::NotConnected, "not-connected"},
  {AccountUnavailableReason::CredentialFailed, "credential-failed"},
  {AccountUnavailableReason::NotEntitled, "not-entitled"},
};

/// 本实现的 LYCODE_MODEL_REASONING_SEPARATOR。
constexpr char kReasoningSeparator = '$';

}  // namespace

TimestampMs nowMs()
{
  return QDateTime::currentMSecsSinceEpoch();
}

// ─────────────────────────────────────────────────────────────────────────────
// 工作区
// ─────────────────────────────────────────────────────────────────────────────

QString Workspace::key() const
{
  const QString trimmed = identity.trimmed();
  // 身份优先，缺失时回退到路径——与 本实现 workspaceIdentity 口径一致。
  return trimmed.isEmpty() ? path : trimmed;
}

QString Workspace::displayName() const
{
  if(path.isEmpty()) {
    return {};
  }
  const QString name = QDir(path).dirName();
  // 根目录的 dirName() 为空，退回完整路径以免显示空白。
  return name.isEmpty() ? path : name;
}

QJsonObject Workspace::toJson() const
{
  QJsonObject result;
  result.insert(QStringLiteral("workspacePath"), path);
  result.insert(QStringLiteral("workspaceIdentity"), identity);
  result.insert(QStringLiteral("remoteSessionId"), remoteSessionId);
  // workspaceKey 是派生字段，写入以便外部消费方无需重算。
  result.insert(QStringLiteral("workspaceKey"), key());
  return result;
}

Workspace Workspace::fromJson(const QJsonObject & value)
{
  Workspace workspace;
  workspace.path = json::str(value, QStringLiteral("workspacePath"));
  workspace.identity = json::str(value, QStringLiteral("workspaceIdentity"));
  workspace.remoteSessionId = json::str(value, QStringLiteral("remoteSessionId"));
  return workspace;
}

// ─────────────────────────────────────────────────────────────────────────────
// 枚举转换
// ─────────────────────────────────────────────────────────────────────────────

QString toToken(MessageRole role)
{
  return nameOf(kMessageRoles, role, QStringLiteral("user"));
}
QString toToken(MessageStatus status)
{
  return nameOf(kMessageStatuses, status, QStringLiteral("complete"));
}
QString toToken(PartKind kind)
{
  return nameOf(kPartKinds, kind, QStringLiteral("text"));
}
QString toToken(ToolState state)
{
  return nameOf(kToolStates, state, QStringLiteral("inputStreaming"));
}
QString toToken(TimelineKind kind)
{
  return nameOf(kTimelineKinds, kind, QStringLiteral("unknown"));
}
QString toToken(SessionStatus status)
{
  return nameOf(kSessionStatuses, status, QStringLiteral("draft"));
}
QString toToken(SessionMode mode)
{
  return nameOf(kSessionModes, mode, QStringLiteral("build"));
}
QString toToken(SessionKind kind)
{
  return nameOf(kSessionKinds, kind, QStringLiteral("interactive"));
}
QString toToken(PermissionKind kind)
{
  return nameOf(kPermissionKinds, kind, QStringLiteral("other"));
}
QString toToken(RiskLevel level)
{
  return nameOf(kRiskLevels, level, QStringLiteral("medium"));
}
QString toToken(PermissionMode mode)
{
  return nameOf(kPermissionModes, mode, QStringLiteral("default"));
}
QString toToken(PermissionRuleBehavior behavior)
{
  return nameOf(kPermissionRuleBehaviors, behavior, QStringLiteral("ask"));
}
QString toToken(PermissionDecision decision)
{
  return nameOf(kPermissionDecisions, decision, QStringLiteral("deny"));
}
QString toToken(PermissionOptionKind kind)
{
  return nameOf(kPermissionOptionKinds, kind, QStringLiteral("deny"));
}
QString toToken(ProviderKind kind)
{
  return nameOf(kProviderKinds, kind, QStringLiteral("openai_compatible"));
}
QString toToken(AccountAvailability availability)
{
  return nameOf(kAccountAvailabilities, availability, QStringLiteral("unknown"));
}
QString toToken(AccountUnavailableReason reason)
{
  return nameOf(kAccountUnavailableReasons, reason, QStringLiteral("none"));
}

MessageRole messageRoleFromToken(const QString & value)
{
  return valueOf(kMessageRoles, value, MessageRole::User);
}
MessageStatus messageStatusFromToken(const QString & value)
{
  return valueOf(kMessageStatuses, value, MessageStatus::Complete);
}
PartKind partKindFromToken(const QString & value)
{
  return valueOf(kPartKinds, value, PartKind::Text);
}
ToolState toolStateFromToken(const QString & value)
{
  return valueOf(kToolStates, value, ToolState::InputStreaming);
}
TimelineKind timelineKindFromToken(const QString & value)
{
  // 未知时间线类型保留为 Unknown，同时由调用方保留 rawType。
  return valueOf(kTimelineKinds, value, TimelineKind::Unknown);
}
SessionStatus sessionStatusFromToken(const QString & value)
{
  return valueOf(kSessionStatuses, value, SessionStatus::Draft);
}
SessionMode sessionModeFromToken(const QString & value)
{
  return valueOf(kSessionModes, value, SessionMode::Build);
}
SessionKind sessionKindFromToken(const QString & value)
{
  return valueOf(kSessionKinds, value, SessionKind::Interactive);
}
PermissionKind permissionKindFromToken(const QString & value)
{
  return valueOf(kPermissionKinds, value, PermissionKind::Other);
}
RiskLevel riskLevelFromToken(const QString & value)
{
  return valueOf(kRiskLevels, value, RiskLevel::Medium);
}
PermissionMode permissionModeFromToken(const QString & value)
{
  return valueOf(kPermissionModes, value, PermissionMode::Default);
}
PermissionRuleBehavior permissionRuleBehaviorFromToken(const QString & value)
{
  return valueOf(kPermissionRuleBehaviors, value, PermissionRuleBehavior::Ask);
}
PermissionDecision permissionDecisionFromToken(const QString & value)
{
  return valueOf(kPermissionDecisions, value, PermissionDecision::Deny);
}
PermissionOptionKind permissionOptionKindFromToken(const QString & value)
{
  return valueOf(kPermissionOptionKinds, value, PermissionOptionKind::Deny);
}
ProviderKind providerKindFromToken(const QString & value)
{
  return valueOf(kProviderKinds, value, ProviderKind::OpenAICompatible);
}
AccountAvailability accountAvailabilityFromToken(const QString & value)
{
  return valueOf(kAccountAvailabilities, value, AccountAvailability::Unknown);
}
AccountUnavailableReason accountUnavailableReasonFromToken(const QString & value)
{
  return valueOf(kAccountUnavailableReasons, value, AccountUnavailableReason::None);
}

bool toolStateIsTerminal(ToolState state)
{
  switch(state) {
    case ToolState::Success:
    case ToolState::Error:
    case ToolState::Cancelled:
      return true;
    case ToolState::InputStreaming:
    case ToolState::PendingApproval:
    case ToolState::Running:
      return false;
  }
  return false;
}

bool toolStateIsActive(ToolState state)
{
  // conversationSharePublicProjection 活跃集判定一致。
  return state == ToolState::InputStreaming || state == ToolState::PendingApproval ||
         state == ToolState::Running;
}

bool sessionModeAllowsWrites(SessionMode mode)
{
  // Plan 是唯一的只读模式。
  return mode != SessionMode::Plan;
}

PermissionMode defaultPermissionModeFor(SessionMode mode)
{
  switch(mode) {
    case SessionMode::Plan:
      return PermissionMode::Plan;
    case SessionMode::Edit:
      return PermissionMode::AcceptEdits;
    case SessionMode::Yolo:
      return PermissionMode::BypassPermissions;
    case SessionMode::Auto:
    case SessionMode::Build:
      return PermissionMode::Default;
  }
  return PermissionMode::Default;
}

bool permissionModeRequiresPrompt(PermissionMode mode, PermissionKind kind)
{
  switch(mode) {
    case PermissionMode::BypassPermissions:
      return false;
    case PermissionMode::Plan:
      // Plan 模式下副作用工具应当被 Agent 循环直接拒绝；
      // 这里对只读放行、其余仍返回 true，避免造成"静默执行"的错觉。
      return kind != PermissionKind::Read;
    case PermissionMode::AcceptEdits:
      return kind != PermissionKind::Read && kind != PermissionKind::Write;
    case PermissionMode::Default:
      return kind != PermissionKind::Read;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 权限
// ─────────────────────────────────────────────────────────────────────────────

QString PermissionRule::key() const
{
  return toolName + QLatin1Char('\u0001') + ruleContent + QLatin1Char('\u0001') +
         toToken(behavior);
}

const PermissionOption * PermissionRequest::defaultOption() const
{
  for(const PermissionOption & option : options) {
    if(option.response.allowed()) {
      return &option;
    }
  }
  return options.isEmpty() ? nullptr : &options.first();
}

// ─────────────────────────────────────────────────────────────────────────────
// 内容块
// ─────────────────────────────────────────────────────────────────────────────

qint64 ToolPart::durationMs() const
{
  if(startedAtMs == 0 || endedAtMs == 0) {
    return 0;
  }
  return endedAtMs - startedAtMs;
}

Part Part::makeText(const QString & value)
{
  Part part;
  part.id = newPartId();
  part.kind = PartKind::Text;
  part.text.text = value;
  return part;
}

Part Part::makeFile(const FilePart & value)
{
  Part part;
  part.id = newId(QStringLiteral("part"));
  part.kind = PartKind::File;
  part.file = value;
  return part;
}

Part Part::makeReasoning(const QString & value)
{
  Part part;
  part.id = newPartId();
  part.kind = PartKind::Reasoning;
  part.reasoning.text = value;
  return part;
}

Part Part::makeTool(const QString & name, const QString & callId)
{
  Part part;
  part.id = newPartId();
  part.kind = PartKind::Tool;
  part.tool.name = name;
  part.tool.callId = callId.isEmpty() ? newToolCallId() : callId;
  part.tool.state = ToolState::InputStreaming;
  return part;
}

Part Part::makeStep(int index, const QString & title)
{
  Part part;
  part.id = newPartId();
  part.kind = PartKind::Step;
  part.step.index = index;
  part.step.title = title;
  return part;
}

Part Part::makeTimeline(TimelineKind kind, const QString & summary)
{
  Part part;
  part.id = newPartId();
  part.kind = PartKind::Timeline;
  part.timeline.kind = kind;
  part.timeline.rawType = toToken(kind);
  part.timeline.display = QStringLiteral("separator");
  part.timeline.summary = summary;
  return part;
}

bool Part::isEmpty() const
{
  switch(kind) {
    case PartKind::Text:
      return text.text.isEmpty();
    case PartKind::Reasoning:
      return reasoning.text.isEmpty();
    case PartKind::Tool:
      // 工具块即使还没有输出也必须在场：它是权限请求的载体。
      return tool.name.isEmpty() && tool.callId.isEmpty();
    case PartKind::File:
      // base64 也是载荷：粘贴进来的图片没有磁盘路径，
      // 只看 path/ref 会把一个有内容的图片附件判成"空"，
      // 于是它被界面整个跳过——图发出去了，用户却什么都看不到。
      return file.path.isEmpty() && file.ref.isEmpty() && file.base64.isEmpty() &&
             file.fileName.isEmpty();
    case PartKind::Artifact:
      return artifact.artifactId.isEmpty() && artifact.path.isEmpty();
    case PartKind::Subagent:
      return subagent.childSessionId.isEmpty() && subagent.title.isEmpty();
    case PartKind::Timeline:
      return false;  // 时间线标记本身就是内容，即使 summary 为空
    case PartKind::Step:
      return step.title.isEmpty();
  }
  return true;
}

QJsonObject Part::toJson() const
{
  QJsonObject result;
  result.insert(QStringLiteral("id"), id);
  result.insert(QStringLiteral("kind"), toToken(kind));

  switch(kind) {
    case PartKind::Text:
      result.insert(QStringLiteral("text"), text.text);
      break;

    case PartKind::Reasoning: {
        QJsonObject payload;
        payload.insert(QStringLiteral("text"), reasoning.text);
        payload.insert(QStringLiteral("signature"), reasoning.signature);
        payload.insert(QStringLiteral("durationMs"), static_cast<double>(reasoning.durationMs));
        result.insert(QStringLiteral("reasoning"), payload);
        break;
      }

    case PartKind::Tool: {
        QJsonObject payload;
        payload.insert(QStringLiteral("callId"), tool.callId);
        payload.insert(QStringLiteral("name"), tool.name);
        payload.insert(QStringLiteral("input"), tool.input);
        payload.insert(QStringLiteral("inputText"), tool.inputText);
        payload.insert(QStringLiteral("state"), toToken(tool.state));
        payload.insert(QStringLiteral("output"), tool.output);
        payload.insert(QStringLiteral("error"), tool.error);
        payload.insert(QStringLiteral("errorCode"), tool.errorCode);
        payload.insert(QStringLiteral("title"), tool.title);
        payload.insert(QStringLiteral("metadata"), tool.metadata);
        payload.insert(QStringLiteral("progress"), tool.progress);
        payload.insert(QStringLiteral("startedAtMs"), static_cast<double>(tool.startedAtMs));
        payload.insert(QStringLiteral("endedAtMs"), static_cast<double>(tool.endedAtMs));
        payload.insert(QStringLiteral("approvalInteractionId"), tool.approvalInteractionId);
        if(!tool.images.isEmpty()) {
          QJsonArray images;
          for(const FilePart & image : tool.images) {
            QJsonObject item;
            item.insert(QStringLiteral("mimeType"), image.mimeType);
            item.insert(QStringLiteral("fileName"), image.fileName);
            item.insert(QStringLiteral("sizeBytes"), static_cast<double>(image.sizeBytes));
            item.insert(QStringLiteral("path"), image.path);
            item.insert(QStringLiteral("base64"), image.base64);
            images.append(item);
          }
          payload.insert(QStringLiteral("images"), images);
        }
        result.insert(QStringLiteral("tool"), payload);
        break;
      }

    case PartKind::File: {
        QJsonObject payload;
        payload.insert(QStringLiteral("path"), file.path);
        payload.insert(QStringLiteral("mimeType"), file.mimeType);
        payload.insert(QStringLiteral("fileName"), file.fileName);
        payload.insert(QStringLiteral("sizeBytes"), static_cast<double>(file.sizeBytes));
        payload.insert(QStringLiteral("base64"), file.base64);
        payload.insert(QStringLiteral("ref"), file.ref);
        result.insert(QStringLiteral("file"), payload);
        break;
      }

    case PartKind::Artifact: {
        QJsonObject payload;
        payload.insert(QStringLiteral("artifactId"), artifact.artifactId);
        payload.insert(QStringLiteral("displayName"), artifact.displayName);
        payload.insert(QStringLiteral("artifactType"), artifact.artifactType);
        payload.insert(QStringLiteral("mimeType"), artifact.mimeType);
        payload.insert(QStringLiteral("path"), artifact.path);
        payload.insert(QStringLiteral("sha256"), artifact.sha256);
        payload.insert(QStringLiteral("sizeBytes"), static_cast<double>(artifact.sizeBytes));
        result.insert(QStringLiteral("artifact"), payload);
        break;
      }

    case PartKind::Subagent: {
        QJsonObject payload;
        payload.insert(QStringLiteral("childSessionId"), subagent.childSessionId);
        payload.insert(QStringLiteral("subagentType"), subagent.subagentType);
        payload.insert(QStringLiteral("title"), subagent.title);
        payload.insert(QStringLiteral("summary"), subagent.summary);
        payload.insert(QStringLiteral("status"), subagent.status);
        payload.insert(QStringLiteral("parentToolCallId"), subagent.parentToolCallId);
        payload.insert(QStringLiteral("backgrounded"), subagent.backgrounded);
        payload.insert(QStringLiteral("startedAtMs"), static_cast<double>(subagent.startedAtMs));
        payload.insert(QStringLiteral("endedAtMs"), static_cast<double>(subagent.endedAtMs));
        result.insert(QStringLiteral("subagent"), payload);
        break;
      }

    case PartKind::Timeline: {
        QJsonObject payload;
        payload.insert(QStringLiteral("kind"), toToken(timeline.kind));
        // rawType 优先：未知类型时它是唯一的信息来源。
        payload.insert(QStringLiteral("timelineType"),
                       timeline.rawType.isEmpty() ? toToken(timeline.kind) : timeline.rawType);
        payload.insert(QStringLiteral("display"), timeline.display);
        payload.insert(QStringLiteral("status"), timeline.status);
        payload.insert(QStringLiteral("reason"), timeline.reason);
        payload.insert(QStringLiteral("summary"), timeline.summary);
        payload.insert(QStringLiteral("metadata"), timeline.metadata);
        result.insert(QStringLiteral("timeline"), payload);
        break;
      }

    case PartKind::Step: {
        QJsonObject payload;
        payload.insert(QStringLiteral("index"), step.index);
        payload.insert(QStringLiteral("title"), step.title);
        result.insert(QStringLiteral("step"), payload);
        break;
      }
  }

  // 只在非空时写出：老数据的形状与体积保持不变（见 Types.h 的字段说明）。
  if(!metadata.isEmpty()) {
    result.insert(QStringLiteral("metadata"), metadata);
  }
  return result;
}

Part Part::fromJson(const QJsonObject & value)
{
  Part part;
  part.id = json::str(value, QStringLiteral("id"));
  if(part.id.isEmpty()) {
    part.id = newPartId();
  }
  part.kind = partKindFromToken(json::str(value, QStringLiteral("kind"), QStringLiteral("text")));

  switch(part.kind) {
    case PartKind::Text:
      part.text.text = json::str(value, QStringLiteral("text"));
      break;

    case PartKind::Reasoning: {
        const QJsonObject payload = json::object(value, QStringLiteral("reasoning"));
        part.reasoning.text = json::str(payload, QStringLiteral("text"));
        part.reasoning.signature = json::str(payload, QStringLiteral("signature"));
        part.reasoning.durationMs = json::integer64(payload, QStringLiteral("durationMs"));
        break;
      }

    case PartKind::Tool: {
        const QJsonObject payload = json::object(value, QStringLiteral("tool"));
        part.tool.callId = json::str(payload, QStringLiteral("callId"));
        part.tool.name = json::str(payload, QStringLiteral("name"));
        part.tool.input = json::object(payload, QStringLiteral("input"));
        part.tool.inputText = json::str(payload, QStringLiteral("inputText"));
        part.tool.state = toolStateFromToken(
                            json::str(payload, QStringLiteral("state"), QStringLiteral("inputStreaming")));
        part.tool.output = json::str(payload, QStringLiteral("output"));
        part.tool.error = json::str(payload, QStringLiteral("error"));
        part.tool.errorCode = json::str(payload, QStringLiteral("errorCode"));
        part.tool.title = json::str(payload, QStringLiteral("title"));
        part.tool.metadata = json::object(payload, QStringLiteral("metadata"));
        part.tool.progress = json::object(payload, QStringLiteral("progress"));
        part.tool.startedAtMs = json::integer64(payload, QStringLiteral("startedAtMs"));
        part.tool.endedAtMs = json::integer64(payload, QStringLiteral("endedAtMs"));
        part.tool.approvalInteractionId =
          json::str(payload, QStringLiteral("approvalInteractionId"));
        for(const QJsonValue & imageValue : json::array(payload, QStringLiteral("images"))) {
          const QJsonObject item = imageValue.toObject();
          FilePart image;
          image.mimeType = json::str(item, QStringLiteral("mimeType"));
          image.fileName = json::str(item, QStringLiteral("fileName"));
          image.sizeBytes = json::integer64(item, QStringLiteral("sizeBytes"));
          image.path = json::str(item, QStringLiteral("path"));
          image.base64 = json::str(item, QStringLiteral("base64"));
          part.tool.images.append(image);
        }
        break;
      }

    case PartKind::File: {
        const QJsonObject payload = json::object(value, QStringLiteral("file"));
        part.file.path = json::str(payload, QStringLiteral("path"));
        part.file.mimeType = json::str(payload, QStringLiteral("mimeType"));
        part.file.fileName = json::str(payload, QStringLiteral("fileName"));
        part.file.sizeBytes = json::integer64(payload, QStringLiteral("sizeBytes"));
        part.file.base64 = json::str(payload, QStringLiteral("base64"));
        part.file.ref = json::str(payload, QStringLiteral("ref"));
        break;
      }

    case PartKind::Artifact: {
        const QJsonObject payload = json::object(value, QStringLiteral("artifact"));
        part.artifact.artifactId = json::str(payload, QStringLiteral("artifactId"));
        part.artifact.displayName = json::str(payload, QStringLiteral("displayName"));
        part.artifact.artifactType = json::str(payload, QStringLiteral("artifactType"));
        part.artifact.mimeType = json::str(payload, QStringLiteral("mimeType"));
        part.artifact.path = json::str(payload, QStringLiteral("path"));
        part.artifact.sha256 = json::str(payload, QStringLiteral("sha256"));
        part.artifact.sizeBytes = json::integer64(payload, QStringLiteral("sizeBytes"));
        break;
      }

    case PartKind::Subagent: {
        const QJsonObject payload = json::object(value, QStringLiteral("subagent"));
        part.subagent.childSessionId = json::str(payload, QStringLiteral("childSessionId"));
        part.subagent.subagentType = json::str(payload, QStringLiteral("subagentType"));
        part.subagent.title = json::str(payload, QStringLiteral("title"));
        part.subagent.summary = json::str(payload, QStringLiteral("summary"));
        part.subagent.status = json::str(payload, QStringLiteral("status"));
        part.subagent.parentToolCallId = json::str(payload, QStringLiteral("parentToolCallId"));
        part.subagent.backgrounded = json::boolean(payload, QStringLiteral("backgrounded"));
        part.subagent.startedAtMs = json::integer64(payload, QStringLiteral("startedAtMs"));
        part.subagent.endedAtMs = json::integer64(payload, QStringLiteral("endedAtMs"));
        break;
      }

    case PartKind::Timeline: {
        const QJsonObject payload = json::object(value, QStringLiteral("timeline"));
        part.timeline.rawType = json::str(payload, QStringLiteral("timelineType"));
        // 先按原始字面量解析；未知时 timelineKindFromToken 回退到 Unknown，
        // rawType 已经保留，信息不丢。
        part.timeline.kind = timelineKindFromToken(part.timeline.rawType);
        part.timeline.display = json::str(payload, QStringLiteral("display"));
        part.timeline.status = json::str(payload, QStringLiteral("status"));
        part.timeline.reason = json::str(payload, QStringLiteral("reason"));
        part.timeline.summary = json::str(payload, QStringLiteral("summary"));
        part.timeline.metadata = json::object(payload, QStringLiteral("metadata"));
        break;
      }

    case PartKind::Step: {
        const QJsonObject payload = json::object(value, QStringLiteral("step"));
        part.step.index = json::integer(payload, QStringLiteral("index"));
        part.step.title = json::str(payload, QStringLiteral("title"));
        break;
      }
  }
  part.metadata = json::object(value, QStringLiteral("metadata"));
  return part;
}

// ─────────────────────────────────────────────────────────────────────────────
// 用量
// ─────────────────────────────────────────────────────────────────────────────

int Usage::effectiveTotal() const
{
  // provider 可能只给总量，也可能只给明细；两者都有时以明细为准。
  const int fromParts = inputTokens + outputTokens;
  return fromParts > 0 ? fromParts : totalTokens;
}

int Usage::promptTokens() const
{
  return inputTokens + cacheReadTokens + cacheWriteTokens;
}

double Usage::cacheHitRate() const
{
  const int cacheable = inputTokens + cacheReadTokens;
  if(cacheable <= 0) {
    return 0.0;
  }
  return static_cast<double>(cacheReadTokens) / static_cast<double>(cacheable);
}

QJsonObject Usage::toJson() const
{
  QJsonObject cache;
  cache.insert(QStringLiteral("read"), cacheReadTokens);
  cache.insert(QStringLiteral("write"), cacheWriteTokens);

  QJsonObject result;
  result.insert(QStringLiteral("total"), totalTokens);
  result.insert(QStringLiteral("input"), inputTokens);
  result.insert(QStringLiteral("output"), outputTokens);
  result.insert(QStringLiteral("reasoning"), reasoningTokens);
  result.insert(QStringLiteral("cache"), cache);
  return result;
}

Usage Usage::fromJson(const QJsonObject & value)
{
  Usage usage;
  usage.totalTokens = json::integer(value, QStringLiteral("total"));
  usage.inputTokens = json::integer(value, QStringLiteral("input"));
  usage.outputTokens = json::integer(value, QStringLiteral("output"));
  usage.reasoningTokens = json::integer(value, QStringLiteral("reasoning"));

  const QJsonObject cache = json::object(value, QStringLiteral("cache"));
  usage.cacheReadTokens = json::integer(cache, QStringLiteral("read"));
  usage.cacheWriteTokens = json::integer(cache, QStringLiteral("write"));
  return usage;
}

Usage & Usage::operator+=(const Usage & other)
{
  inputTokens += other.inputTokens;
  outputTokens += other.outputTokens;
  reasoningTokens += other.reasoningTokens;
  totalTokens += other.totalTokens;
  cacheReadTokens += other.cacheReadTokens;
  cacheWriteTokens += other.cacheWriteTokens;
  return *this;
}

// ─────────────────────────────────────────────────────────────────────────────
// 消息
// ─────────────────────────────────────────────────────────────────────────────

QString Message::plainText() const
{
  QStringList chunks;
  for(const Part & part : parts) {
    if(part.kind == PartKind::Text && !part.text.text.isEmpty()) {
      chunks.append(part.text.text);
    }
  }
  return chunks.join(QLatin1Char('\n'));
}

QList<ToolPart> Message::toolParts() const
{
  QList<ToolPart> result;
  for(const Part & part : parts) {
    if(part.kind == PartKind::Tool) {
      result.append(part.tool);
    }
  }
  return result;
}

Part * Message::findPart(const Id & partId)
{
  for(Part & part : parts) {
    if(part.id == partId) {
      return &part;
    }
  }
  return nullptr;
}

const Part * Message::findPart(const Id & partId) const
{
  for(const Part & part : parts) {
    if(part.id == partId) {
      return &part;
    }
  }
  return nullptr;
}

bool Message::replacePart(const Part & part)
{
  Part * target = findPart(part.id);
  if(target == nullptr) {
    return false;
  }
  *target = part;
  return true;
}

bool Message::isTerminal() const
{
  return status == MessageStatus::Complete || status == MessageStatus::Interrupted ||
         status == MessageStatus::Failed;
}

void Message::cancelPendingTools()
{
  for(Part & part : parts) {
    if(part.kind == PartKind::Tool && !toolStateIsTerminal(part.tool.state)) {
      part.tool.state = ToolState::Cancelled;
      if(part.tool.endedAtMs == 0) {
        part.tool.endedAtMs = nowMs();
      }
    }
  }
}

QJsonObject Message::toJson() const
{
  QJsonObject result;
  result.insert(QStringLiteral("id"), id);
  result.insert(QStringLiteral("sessionId"), sessionId);
  result.insert(QStringLiteral("role"), toToken(role));
  result.insert(QStringLiteral("status"), toToken(status));

  QJsonArray partArray;
  for(const Part & part : parts) {
    partArray.append(part.toJson());
  }
  result.insert(QStringLiteral("parts"), partArray);

  result.insert(QStringLiteral("createdAtMs"), static_cast<double>(createdAtMs));
  result.insert(QStringLiteral("updatedAtMs"), static_cast<double>(updatedAtMs));
  result.insert(QStringLiteral("modelId"), modelId);
  result.insert(QStringLiteral("usage"), usage.toJson());
  result.insert(QStringLiteral("errorMessage"), errorMessage);
  result.insert(QStringLiteral("parentMessageId"), parentMessageId);
  if(modelOnly) {
    // visibility 字段对齐：只在 true 时写出，保持旧数据体积不变。
    result.insert(QStringLiteral("visibility"), QStringLiteral("model-only"));
  }
  return result;
}

Message Message::fromJson(const QJsonObject & value)
{
  Message message;
  message.id = json::str(value, QStringLiteral("id"));
  if(message.id.isEmpty()) {
    message.id = newMessageId();
  }
  message.sessionId = json::str(value, QStringLiteral("sessionId"));
  message.role =
    messageRoleFromToken(json::str(value, QStringLiteral("role"), QStringLiteral("user")));
  message.status = messageStatusFromToken(
                     json::str(value, QStringLiteral("status"), QStringLiteral("complete")));

  const QJsonArray partArray = json::array(value, QStringLiteral("parts"));
  message.parts.reserve(partArray.size());
  for(const QJsonValue & partValue : partArray) {
    if(partValue.isObject()) {
      message.parts.append(Part::fromJson(partValue.toObject()));
    }
  }

  message.createdAtMs = json::integer64(value, QStringLiteral("createdAtMs"));
  message.updatedAtMs = json::integer64(value, QStringLiteral("updatedAtMs"));
  message.modelId = json::str(value, QStringLiteral("modelId"));
  message.usage = Usage::fromJson(json::object(value, QStringLiteral("usage")));
  message.errorMessage = json::str(value, QStringLiteral("errorMessage"));
  message.parentMessageId = json::str(value, QStringLiteral("parentMessageId"));
  message.modelOnly =
    json::str(value, QStringLiteral("visibility")) == QStringLiteral("model-only");
  return message;
}

// ─────────────────────────────────────────────────────────────────────────────
// 会话
// ─────────────────────────────────────────────────────────────────────────────

Session Session::fromJson(const QJsonObject & value)
{
  Session session;
  session.id = json::str(value, QStringLiteral("id"));
  if(session.id.isEmpty()) {
    session.id = newSessionId();
  }
  session.title = json::str(value, QStringLiteral("title"));
  session.titleGenerated =
    json::boolean(value, QStringLiteral("titleGenerated"), false);
  session.workspace = Workspace::fromJson(value);
  session.parentSessionId = json::str(value, QStringLiteral("parentSessionId"));
  session.kind = sessionKindFromToken(
                   json::str(value, QStringLiteral("kind"), QStringLiteral("interactive")));
  session.status = sessionStatusFromToken(
                     json::str(value, QStringLiteral("status"), QStringLiteral("draft")));
  session.mode =
    sessionModeFromToken(json::str(value, QStringLiteral("mode"), QStringLiteral("build")));
  session.createdAtMs = json::integer64(value, QStringLiteral("createdAtMs"));
  session.updatedAtMs = json::integer64(value, QStringLiteral("updatedAtMs"));
  session.archivedAtMs = json::integer64(value, QStringLiteral("archivedAtMs"));
  session.modelId = json::str(value, QStringLiteral("modelId"));
  session.providerId = json::str(value, QStringLiteral("providerId"));
  session.messageCount = json::integer(value, QStringLiteral("messageCount"));
  session.contextUsage = json::object(value, QStringLiteral("contextUsage"));
  session.cumulativeUsage =
    Usage::fromJson(json::object(value, QStringLiteral("cumulativeUsage")));
  session.pendingPermissionCount = json::integer(value, QStringLiteral("pendingPermissionCount"));
  session.pendingInputCount = json::integer(value, QStringLiteral("pendingInputCount"));
  return session;
}

QJsonObject Session::toJson() const
{
  QJsonObject result;
  result.insert(QStringLiteral("id"), id);
  result.insert(QStringLiteral("title"), title);
  result.insert(QStringLiteral("titleGenerated"), titleGenerated);
  // 工作区字段摊平到同一层，WorkspaceRef 形状一致。
  const QJsonObject workspaceJson = workspace.toJson();
  for(auto it = workspaceJson.constBegin(); it != workspaceJson.constEnd(); ++it) {
    result.insert(it.key(), it.value());
  }
  result.insert(QStringLiteral("parentSessionId"), parentSessionId);
  result.insert(QStringLiteral("kind"), toToken(kind));
  result.insert(QStringLiteral("status"), toToken(status));
  result.insert(QStringLiteral("mode"), toToken(mode));
  result.insert(QStringLiteral("createdAtMs"), static_cast<double>(createdAtMs));
  result.insert(QStringLiteral("updatedAtMs"), static_cast<double>(updatedAtMs));
  result.insert(QStringLiteral("archivedAtMs"), static_cast<double>(archivedAtMs));
  result.insert(QStringLiteral("modelId"), modelId);
  result.insert(QStringLiteral("providerId"), providerId);
  result.insert(QStringLiteral("messageCount"), messageCount);
  result.insert(QStringLiteral("contextUsage"), contextUsage);
  result.insert(QStringLiteral("cumulativeUsage"), cumulativeUsage.toJson());
  result.insert(QStringLiteral("pendingPermissionCount"), pendingPermissionCount);
  result.insert(QStringLiteral("pendingInputCount"), pendingInputCount);
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Provider 与模型选择
// ─────────────────────────────────────────────────────────────────────────────

bool ProviderConfig::isUsable() const
{
  if(!enabled || baseUrl.trimmed().isEmpty()) {
    return false;
  }
  if(availability == AccountAvailability::Unavailable) {
    return false;
  }
  // 自建网关可能不需要 key（内网直连），因此只对 Anthropic 强制要求 apiKey。
  if(kind == ProviderKind::Anthropic && apiKey.trimmed().isEmpty()) {
    return false;
  }
  return true;
}

QJsonObject ProviderConfig::toJson() const
{
  QJsonObject result;
  result.insert(QStringLiteral("id"), id);
  result.insert(QStringLiteral("name"), name);
  result.insert(QStringLiteral("kind"), toToken(kind));
  result.insert(QStringLiteral("baseUrl"), baseUrl);
  result.insert(QStringLiteral("apiKey"), apiKey);
  result.insert(QStringLiteral("models"), QJsonArray::fromStringList(models));
  result.insert(QStringLiteral("enabled"), enabled);
  result.insert(QStringLiteral("extraHeaders"), extraHeaders);
  result.insert(QStringLiteral("availability"), toToken(availability));
  result.insert(QStringLiteral("unavailableReason"), toToken(unavailableReason));
  return result;
}

ProviderConfig ProviderConfig::fromJson(const QJsonObject & value)
{
  ProviderConfig config;
  config.id = json::str(value, QStringLiteral("id"));
  config.name = json::str(value, QStringLiteral("name"));
  config.kind = providerKindFromToken(
                  json::str(value, QStringLiteral("kind"), QStringLiteral("openai_compatible")));
  config.baseUrl = json::str(value, QStringLiteral("baseUrl"));
  config.apiKey = json::str(value, QStringLiteral("apiKey"));
  config.models = json::stringList(value, QStringLiteral("models"));
  config.enabled = json::boolean(value, QStringLiteral("enabled"), true);
  config.extraHeaders = json::object(value, QStringLiteral("extraHeaders"));
  config.availability = accountAvailabilityFromToken(
                          json::str(value, QStringLiteral("availability"), QStringLiteral("unknown")));
  config.unavailableReason = accountUnavailableReasonFromToken(
                               json::str(value, QStringLiteral("unavailableReason"), QStringLiteral("none")));
  return config;
}

// ─────────────────────────────────────────────────────────────────────────────
// 模型能力覆盖
// ─────────────────────────────────────────────────────────────────────────────

QString modelOptionKey(const QString & providerId, const QString & modelId)
{
  return providerId + QLatin1Char('/') + modelId;
}

bool ModelOptionOverride::isEmpty() const
{
  return contextWindow <= 0 && maxOutputTokens <= 0 && reasoningLevels.isEmpty() &&
         defaultReasoningLevel.isEmpty();
}

QJsonObject ModelOptionOverride::toJson() const
{
  QJsonObject result;
  // 只写非零字段：让配置文件保持精简，也让"未设置"与"设为 0"不会混淆。
  if(contextWindow > 0) {
    result.insert(QStringLiteral("contextWindow"), contextWindow);
  }
  if(maxOutputTokens > 0) {
    result.insert(QStringLiteral("maxOutputTokens"), maxOutputTokens);
  }
  if(!reasoningLevels.isEmpty()) {
    result.insert(QStringLiteral("reasoningLevels"), QJsonArray::fromStringList(reasoningLevels));
  }
  if(!defaultReasoningLevel.isEmpty()) {
    result.insert(QStringLiteral("defaultReasoningLevel"), defaultReasoningLevel);
  }
  return result;
}

ModelOptionOverride ModelOptionOverride::fromJson(const QJsonObject & value)
{
  ModelOptionOverride override;
  override.contextWindow = json::integer(value, QStringLiteral("contextWindow"));
  override.maxOutputTokens = json::integer(value, QStringLiteral("maxOutputTokens"));
  override.reasoningLevels = json::stringList(value, QStringLiteral("reasoningLevels"));
  override.defaultReasoningLevel = json::str(value, QStringLiteral("defaultReasoningLevel"));
  return override;
}

void ModelOptionOverride::applyTo(ModelInfo * info) const
{
  if(info == nullptr) {
    return;
  }
  if(contextWindow > 0) {
    info->contextWindow = contextWindow;
  }
  if(maxOutputTokens > 0) {
    info->maxOutputTokens = maxOutputTokens;
  }
  if(!reasoningLevels.isEmpty()) {
    info->reasoningLevels = reasoningLevels;
    // 覆盖了档位列表就要同步"是否支持思考"，否则 UI 会拿着旧标志做判断。
    // 列表非空即视为支持；要表达"不支持思考"就把列表留空（即不做覆盖，
    // 由 provider 的默认值决定）——这样语义只有一个方向，不会自相矛盾。
    info->supportsReasoning = true;
  }
}

QString ModelSelection::displayValue() const
{
  // formatModelPickerValue 一致：providerId/modelId[$level]
  QString result = providerId + QLatin1Char('/') + modelId;
  if(!reasoningLevel.isEmpty()) {
    result += QLatin1Char(kReasoningSeparator) + reasoningLevel;
  }
  return result;
}

ModelSelection ModelSelection::parseDisplayValue(const QString & value)
{
  ModelSelection selection;
  const QString trimmed = value.trimmed();
  if(trimmed.isEmpty()) {
    return selection;
  }

  QString head = trimmed;
  const qsizetype separatorIndex = trimmed.indexOf(QLatin1Char(kReasoningSeparator));
  if(separatorIndex >= 0) {
    head = trimmed.left(separatorIndex);
    selection.reasoningLevel = trimmed.mid(separatorIndex + 1).trimmed();
  }

  const qsizetype slashIndex = head.indexOf(QLatin1Char('/'));
  if(slashIndex <= 0 || slashIndex == head.size() - 1) {
    // 形状不符：整体当作 providerId 无效处理，返回空选择而不是猜测。
    return {};
  }
  selection.providerId = head.left(slashIndex).trimmed();
  selection.modelId = head.mid(slashIndex + 1).trimmed();
  return selection;
}

}  // namespace lycode

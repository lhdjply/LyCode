// LyCode — 领域模型
//
// 本文件是 C++ 重写的类型契约中心，按既定语义 LyCode 的领域概念与**词表**
// （session mode / tool state / permission decision / risk level 等取值与
// LyCode 保持一致），但按 C++ 与 Qt 的惯例重新建模。
//
// ── 与 本实现的关键差异（有意为之）────────────────────────────────────────
// 本实现有两条并存链路：legacy message/part，与 V4 row（扁平行 + 快照 + 增量）。
// V4 引入 row/delta/logEpoch/seq 的目的是**跨进程 wire 的断线重放**。
// 本实现把 Agent 运行时与 UI 放在同一进程内，不存在 wire，因此重放机制
// 没有收益；这里采用更简单的 message/part 树，只用信号把变化推给 UI。
//
// 词表仍然照抄 LyCode，保证术语、状态语义、权限选项按既定语义，
// 这样行为与文案不会分叉。
//
// ── 建模取舍 ──────────────────────────────────────────────────────────────
//   * Part 使用「判别字段 + 各类型独立子结构」的胖结构，而不是 std::variant。
//     Part 需要在 JSON、UI、持久化之间频繁往返，胖结构让序列化与 switch 分派
//     都保持简单，代价是额外内存（此处无影响）。
//   * 工具输入/输出保留为 QJsonObject 透传：内置工具签名稳定，但 MCP 与
//     插件工具的形状由外部决定，强类型化会带来无收益的耦合。
//   * 未知枚举值一律降级为明确的安全默认 + 告警，绝不丢弃整条记录。
//     本实现多次记录过「闭集枚举加值 → 旧客户端整帧被丢」的偏斜成本。
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace lycode
{

using Id = QString;

/// 毫秒时间戳（Unix epoch，一律本地运行时时钟）。0 表示未设置。
using TimestampMs = qint64;

TimestampMs nowMs();

// ─────────────────────────────────────────────────────────────────────────────
// 前向声明
// ─────────────────────────────────────────────────────────────────────────────

enum class PermissionMode;

// ─────────────────────────────────────────────────────────────────────────────
// 工作区
// ─────────────────────────────────────────────────────────────────────────────

/// 工作区引用。
///
/// `identity` 用于身份隔离（去重、绑定、缓存、队列、持久化的键），
/// `path` 用于文件操作、命令 cwd、Git 与路径展示。二者不可互相替代：
/// 同一物理路径可能对应不同身份，远程工作区尤其如此。
struct Workspace {
  QString path;
  QString identity;
  /// 远程链路标识（如 `remote:ssh:host:22:user:/path`）。本地为空。
  QString remoteSessionId;

  /// 身份 key 的统一口径，按既定语义：
  /// `workspaceIdentity?.trim() || workspacePath`
  QString key() const;
  QString displayName() const;
  bool isValid() const
  {
    return !path.isEmpty();
  }
  bool isRemote() const
  {
    return !remoteSessionId.isEmpty();
  }

  QJsonObject toJson() const;
  static Workspace fromJson(const QJsonObject & json);
};

// ─────────────────────────────────────────────────────────────────────────────
// 消息与内容块
// ─────────────────────────────────────────────────────────────────────────────

/// 消息角色。
///
/// 注意：LyCode 的 wire 协议只有 `user` 与 `assistant` 两种角色（system 提示词
/// 走独立字段）。`System` 是本实现内部保留的角色，用于存放压缩摘要等本地记录，
/// 不参与 provider 消息序列的角色映射。
enum class MessageRole {
  User,
  Assistant,
  System,
};

/// 消息状态。取值来自 V4 assistantText 行的 state 词表。
enum class MessageStatus {
  Pending,      ///< 已入库，尚未开始生成（本地创建）
  Streaming,    ///< 正在流式接收增量
  Complete,     ///< 正常结束
  Interrupted,  ///< 被用户中断或运行时取消
  Failed,       ///< 以错误结束
};

/// 内容块类型判别字段。
enum class PartKind {
  Text,       ///< 正文
  Reasoning,  ///< 思考过程
  Tool,       ///< 工具调用（含输入、状态、输出）
  File,       ///< 附件引用（图片也走这里：mime 以 image/ 开头）
  Artifact,   ///< 产物（生成的文档、图片等）
  Subagent,   ///< 子 Agent 派生记录
  Timeline,   ///< 时间线标记（压缩、fork、模型切换、目标校验）
  Step,       ///< 步骤分隔
};

/// 工具调用状态。取值来自 V4 toolCallRow.status 的六态词表，
/// 转换方向为：InputStreaming → PendingApproval → Running → (Success|Error|Cancelled)。
enum class ToolState {
  InputStreaming,   ///< 入参仍在流式接收
  PendingApproval,  ///< 等待权限裁决
  Running,          ///< 已放行，正在执行
  Success,          ///< 执行成功
  Error,            ///< 执行失败
  Cancelled,        ///< 被取消（中断、或流结束时仍未闭合）
};

QString toToken(MessageRole role);
QString toToken(MessageStatus status);
QString toToken(PartKind kind);
QString toToken(ToolState state);

MessageRole messageRoleFromToken(const QString & value);
MessageStatus messageStatusFromToken(const QString & value);
PartKind partKindFromToken(const QString & value);
ToolState toolStateFromToken(const QString & value);

/// 是否为终态。未终态的工具在会话结束时必须被显式关闭为 Cancelled，
/// 否则 UI 会永远显示"运行中"。
bool toolStateIsTerminal(ToolState state);
/// 是否为"活跃"态（conversationSharePublicProjection 判定一致）。
bool toolStateIsActive(ToolState state);

struct TextPart {
  QString text;
};

/// 模型的思考/推理内容。与正文分开存储，UI 可折叠展示。
struct ReasoningPart {
  QString text;
  /// 部分 provider 返回加密的推理签名，回传时必须原样带回。
  QString signature;
  /// 推理耗时（毫秒），未知为 0。
  qint64 durationMs = 0;
};

/// 附件/文件引用。图片不单独建类型，用 mimeType 区分
/// （按既定语义：没有独立的 image part）。
struct FilePart {
  QString path;
  QString mimeType;
  QString fileName;
  qint64 sizeBytes = 0;
  /// 图片预览数据（base64，无 data URL 前缀）。仅图片类型可能非空。
  QString base64;
  /// 附件引用 id，用于与会话附件表对应。
  QString ref;

  bool isImage() const
  {
    return mimeType.startsWith(QStringLiteral("image/"), Qt::CaseInsensitive);
  }
};

/// 产物（生成的文件）。与 FilePart 的区别：File 是输入附件，Artifact 是输出产物。
struct ArtifactPart {
  QString artifactId;
  QString displayName;
  QString artifactType;  ///< pdf | pptx | docx | xlsx | image | html | md | text
  QString mimeType;
  QString path;
  QString sha256;
  qint64 sizeBytes = 0;
};

/// 子 Agent 派生记录。
struct SubagentPart {
  QString childSessionId;
  QString subagentType;
  QString title;
  QString summary;
  /// running | success | failed | cancelled
  QString status;
  QString parentToolCallId;
  bool backgrounded = false;
  TimestampMs startedAtMs = 0;
  TimestampMs endedAtMs = 0;
};

/// 时间线标记类型。取值来自 V4 TimelineMarkerPayload 的 type 词表。
enum class TimelineKind {
  ContextCompaction,
  GoalVerification,
  SessionFork,
  ModelChange,
  Retry,
  CheckpointRestored,
  Unknown,
};

QString toToken(TimelineKind kind);
TimelineKind timelineKindFromToken(const QString & value);

/// 上下文压缩在数据模型里用的稳定标记。
///
/// 集中放在 Types.h 而不是压缩模块里：SessionStore（归档原因）、
/// ContextCompactor（合成消息的 metadata）与 UI（识别压缩行）三处都要用同一个
/// 字面量，各写一份迟早会漂移。
namespace compaction
{
/// Part::metadata 里的标记：这条 TextPart 是压缩摘要。
inline constexpr const char * kKind = "context_compaction";
/// SessionStore 的归档原因。
inline constexpr const char * kReason = "context_compaction";
}  // namespace compaction

/// 时间线标记。用于在对话流中插入分隔行（压缩、fork、换模型等）。
struct TimelinePart {
  TimelineKind kind = TimelineKind::Unknown;
  /// 原始 timelineType 字面量，保留未知值以免信息丢失。
  QString rawType;
  /// separator | worklog
  QString display;
  QString status;
  QString reason;
  QString summary;
  QJsonObject metadata;
};

/// 一个步骤标记，用于在同一轮内分隔"思考 → 工具 → 再思考"。
struct StepPart {
  int index = 0;
  QString title;
};

/// 工具调用的完整记录。
struct ToolPart {
  QString callId;
  QString name;
  QJsonObject input;
  /// 入参的原始流式文本（用于"正在接收参数"的展示）。
  QString inputText;

  ToolState state = ToolState::InputStreaming;

  /// 人类可读的输出文本（已按展示预算截断）。
  QString output;
  /// 失败原因；仅当 state == Error 时有意义。
  QString error;
  /// 错误码（V4 toolCallRow.error.code）。
  QString errorCode;

  /// 供 UI 直接展示的一行摘要，例如 `Bash: make build`。
  QString title;
  /// 结构化补充信息：cwd、退出码、耗时、文件路径、diff 等。
  QJsonObject metadata;
  /// 随结果返回的图片，供界面渲染缩略图，并随 part 一起持久化。
  QList<FilePart> images;
  /// 执行进度（字节数、预览行、更新时间），终态清空。
  QJsonObject progress;

  TimestampMs startedAtMs = 0;
  TimestampMs endedAtMs = 0;

  /// 关联的权限交互 id；仅当 state == PendingApproval 时有意义。
  QString approvalInteractionId;

  qint64 durationMs() const;
};

/// 内容块。`kind` 决定哪个子结构有效，其余子结构应被忽略。
struct Part {
  Id id;
  PartKind kind = PartKind::Text;

  TextPart text;
  ReasoningPart reasoning;
  ToolPart tool;
  FilePart file;
  ArtifactPart artifact;
  SubagentPart subagent;
  TimelinePart timeline;
  StepPart step;

  /// 随 part 一起持久化的附加标记。
  ///
  /// 只放"重载时必须还原"的少量结构化信息（例如压缩摘要的 kind/summary），
  /// 不放展示数据——展示数据各有专门的字段。没有标记时序列化不写出该键，
  /// 老数据的体积不变。
  QJsonObject metadata;

  static Part makeText(const QString & value);
  /// 附件（图片走这里，见 FilePart::isImage）。
  static Part makeFile(const FilePart & value);
  static Part makeReasoning(const QString & value);
  static Part makeTool(const QString & name, const QString & callId);
  static Part makeStep(int index, const QString & title);
  static Part makeTimeline(TimelineKind kind, const QString & summary);

  /// 该块是否包含可展示内容（用于过滤空白块）。
  bool isEmpty() const;

  QJsonObject toJson() const;
  static Part fromJson(const QJsonObject & json);
};

/// token 用量。字段名TokenUsage 结构 对齐。
///
/// ── 字段口径（**必须**按这个含义填，两处 provider 已归一）────────────────
/// 两种协议对"输入 token"的统计口径本来是不同的：
///   * Anthropic：`input_tokens` **不含**缓存部分（读/写分别单列）
///   * OpenAI：`prompt_tokens` **含**缓存部分（`prompt_tokens_details.cached_tokens`）
/// 如果照原样落进同一个字段，同一个 `inputTokens` 就有了两种含义，
/// 缓存命中率必然算错。因此这里统一定义为：
///
///   inputTokens      = **未命中缓存**的输入 token（真正要模型从头处理的）
///   cacheReadTokens  = 命中缓存、直接从缓存读取的输入 token
///   cacheWriteTokens = 本次写入缓存（供后续复用）的输入 token
///   outputTokens     = 生成的 token
///
/// 因此一次请求的完整提示长度 = inputTokens + cacheReadTokens + cacheWriteTokens。
struct Usage {
  int inputTokens = 0;
  int outputTokens = 0;
  int reasoningTokens = 0;
  /// 未提供明细时 provider 给出的总量；0 表示未提供。
  int totalTokens = 0;
  int cacheReadTokens = 0;
  int cacheWriteTokens = 0;

  int effectiveTotal() const;
  /// 一次请求的完整提示 token 数（未缓存 + 缓存读 + 缓存写）。
  int promptTokens() const;
  /// 缓存命中率，取值 0.0-1.0。
  ///
  /// 分母刻意**不含 cacheWriteTokens**：那是首次写入缓存的部分，
  /// 本来就没有机会命中；把它算进分母会系统性低估命中率。
  /// 分母为 0 时返回 0。
  double cacheHitRate() const;
  QJsonObject toJson() const;
  static Usage fromJson(const QJsonObject & json);
  Usage & operator+=(const Usage & other);
};

/// 一条对话消息。消息由有序的 Part 组成，而不是单一文本，
/// 这样思考、工具调用、正文可以按真实发生顺序交错渲染。
struct Message {
  Id id;
  Id sessionId;
  MessageRole role = MessageRole::User;
  MessageStatus status = MessageStatus::Complete;
  QList<Part> parts;

  TimestampMs createdAtMs = 0;
  TimestampMs updatedAtMs = 0;

  QString modelId;
  Usage usage;
  QString errorMessage;

  /// 触发本轮的父消息（assistant 消息指向对应的 user 消息）。
  Id parentMessageId;

  /// 仅发给模型、不进入用户可见对话流。对应 visibility: "model-only"。
  /// 合成的工具结果轮就属于这一类：工具输出已经渲染在工具卡片里，
  /// 再作为一条"用户消息"出现会让用户以为自己说过那句话。
  bool modelOnly = false;

  /// 便捷方法：拼接所有 TextPart。
  QString plainText() const;
  /// 便捷方法：收集所有 ToolPart。
  QList<ToolPart> toolParts() const;
  /// 替换指定 part；未找到返回 false。
  bool replacePart(const Part & part);
  Part * findPart(const Id & partId);
  const Part * findPart(const Id & partId) const;
  bool isTerminal() const;
  /// 把全部未终态的工具块关闭为 Cancelled（会话中断时使用）。
  void cancelPendingTools();

  QJsonObject toJson() const;
  static Message fromJson(const QJsonObject & json);
};

// ─────────────────────────────────────────────────────────────────────────────
// 会话
// ─────────────────────────────────────────────────────────────────────────────

/// 会话阶段。取值来自 V4 sessionPhaseSchema（6 值）。
/// 注意：这是"会话本身的阶段"，不表达"是否在等权限"——
/// 后者由 pendingPermissionCount 派生，避免两处状态各说各话。
enum class SessionStatus {
  Draft,                 ///< 纯内存草稿，尚未落盘
  Prewarming,            ///< 正在预热运行时
  Running,               ///< 有活跃工作
  CompletedSuccess,      ///< 正常结束
  CompletedInterrupted,  ///< 被中断结束
  Error,                 ///< 以错误结束
};

/// 会话模式。取值来自 SessionMode 取值。
enum class SessionMode {
  Plan,   ///< 只读规划
  Build,  ///< 默认：可读写、可执行
  Edit,   ///< 以编辑为主
  Yolo,   ///< 免确认执行
  Auto,   ///< 运行时自动裁决（内部态）
};

QString toToken(SessionStatus status);
QString toToken(SessionMode mode);
SessionStatus sessionStatusFromToken(const QString & value);
SessionMode sessionModeFromToken(const QString & value);

/// 该模式是否允许产生副作用的工具。
bool sessionModeAllowsWrites(SessionMode mode);
/// 该模式对应的默认权限模式。
PermissionMode defaultPermissionModeFor(SessionMode mode);

/// 会话种类。取值来自 SessionKind 取值。
enum class SessionKind {
  Interactive,
  Fork,
  SelectionSideChat,
  WorkflowParent,
  WorkflowChild,
  SubagentChild,
  NestedWorkflowChild,
};

QString toToken(SessionKind kind);
SessionKind sessionKindFromToken(const QString & value);

struct Session {
  Id id;
  QString title;
  /// 标题是否由模型生成（而不是取首条输入的前若干字符）。
  /// 用它做幂等：只有 false 时才值得再花一次模型调用去生成。
  bool titleGenerated = false;
  Workspace workspace;

  /// 父会话 id；非空表示这是派生会话（fork / 子 Agent）。
  Id parentSessionId;
  SessionKind kind = SessionKind::Interactive;

  SessionStatus status = SessionStatus::Draft;
  SessionMode mode = SessionMode::Build;

  TimestampMs createdAtMs = 0;
  TimestampMs updatedAtMs = 0;
  TimestampMs archivedAtMs = 0;

  QString modelId;
  QString providerId;
  int messageCount = 0;

  /// 上下文用量：usedTokens / maxTokens / autoCompactThresholdTokens。
  QJsonObject contextUsage;
  /// 累计用量。
  Usage cumulativeUsage;

  /// 等待处理的交互数量（权限 / 用户提问）。
  int pendingPermissionCount = 0;
  int pendingInputCount = 0;

  bool isSubagent() const
  {
    return kind == SessionKind::SubagentChild;
  }
  bool hasParent() const
  {
    return !parentSessionId.isEmpty();
  }
  /// 是否需要用户立即处理。
  bool isWaitingOnUser() const
  {
    return pendingPermissionCount > 0 || pendingInputCount > 0;
  }

  QJsonObject toJson() const;
  static Session fromJson(const QJsonObject & json);
};

// ─────────────────────────────────────────────────────────────────────────────
// 权限
// ─────────────────────────────────────────────────────────────────────────────

/// 权限类别：决定默认策略与 UI 呈现的风险等级。
enum class PermissionKind {
  Read,     ///< 读取文件、列目录、搜索
  Write,    ///< 写入或修改文件
  Execute,  ///< 执行命令
  Network,  ///< 网络请求
  Other,
};

QString toToken(PermissionKind kind);
PermissionKind permissionKindFromToken(const QString & value);

/// 风险等级。取值来自权限请求的风险评估。
enum class RiskLevel {
  Low,
  Medium,
  High,
  Critical,
};

QString toToken(RiskLevel level);
RiskLevel riskLevelFromToken(const QString & value);

/// 会话级权限模式。
enum class PermissionMode {
  Default,             ///< 每个有副作用的工具都需要确认
  AcceptEdits,         ///< 自动放行文件编辑，其余仍需确认
  Plan,                ///< 全部只读，拒绝一切副作用
  BypassPermissions,   ///< 放行全部（危险，需显式开启）
};

QString toToken(PermissionMode mode);
PermissionMode permissionModeFromToken(const QString & value);
bool permissionModeRequiresPrompt(PermissionMode mode, PermissionKind kind);

/// 权限规则行为。决定命中规则后是放行、拒绝还是转人工。
enum class PermissionRuleBehavior {
  Allow,
  Deny,
  Ask,
};

QString toToken(PermissionRuleBehavior behavior);
PermissionRuleBehavior permissionRuleBehaviorFromToken(const QString & value);

/// 一条权限规则。
struct PermissionRule {
  QString toolName;
  /// 规则内容（通常是路径前缀或命令前缀）。空表示匹配整个工具。
  QString ruleContent;
  PermissionRuleBehavior behavior = PermissionRuleBehavior::Allow;

  /// 去重与匹配用的稳定键。
  QString key() const;
};

/// 用户裁决。由用户或规则给出的最终结论。
enum class PermissionDecision {
  Allow,
  Deny,
  Escalate,  ///< 请求升级权限（如从只读升级到写入）
  Modify,    ///< 允许但修改入参
};

QString toToken(PermissionDecision decision);
PermissionDecision permissionDecisionFromToken(const QString & value);

/// 权限应答。承载最终结论与可选的自定义输入。
struct PermissionResponse {
  PermissionDecision decision = PermissionDecision::Deny;
  QString reason;
  /// 仅 decision == Modify 时有效。
  QJsonObject modifiedInput;
  /// 本次裁决要持久化的规则变更（addRules）。
  QList<PermissionRule> permissionUpdates;

  bool allowed() const
  {
    return decision == PermissionDecision::Allow || decision == PermissionDecision::Modify;
  }
};

/// 权限选项类型。取值来自 V4 permissionOptionSchema.kind。
enum class PermissionOptionKind {
  AllowOnce,
  AllowAlways,
  Deny,
  Custom,
};

QString toToken(PermissionOptionKind kind);
PermissionOptionKind permissionOptionKindFromToken(const QString & value);

/// 一个可选的裁决选项。UI 直接渲染这些选项，不自行发明。
struct PermissionOption {
  QString optionId;
  QString label;
  PermissionOptionKind kind = PermissionOptionKind::AllowOnce;
  /// 该选项对应的裁决载荷。
  PermissionResponse response;
};

/// 一次权限请求。
struct PermissionRequest {
  Id id;              ///< 交互 id
  Id sessionId;
  Id callId;          ///< 关联的 ToolPart::callId

  QString toolName;
  QJsonObject input;
  PermissionKind kind = PermissionKind::Other;
  RiskLevel riskLevel = RiskLevel::Medium;
  /// 工具显式声明"必须经用户批准"。为真时**跳过只读/模式直通**，
  /// 直接进入询问——否则一个声明了 needsApproval 的只读工具会被静默放行。
  bool needsApproval = false;

  /// 面向用户的标题与说明，由工具自身生成。
  QString title;
  QString description;

  /// 可选的裁决选项；至少一项。UI 不得自行发明选项。
  QList<PermissionOption> options;

  TimestampMs createdAtMs = 0;

  /// 取默认选项（第一个允许项，若无则第一个拒绝项）。
  const PermissionOption * defaultOption() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// 模型与 Provider
// ─────────────────────────────────────────────────────────────────────────────

enum class ProviderKind {
  Anthropic,         ///< Anthropic Messages API
  OpenAICompatible,  ///< OpenAI Chat Completions 兼容协议（含自建网关）
};

QString toToken(ProviderKind kind);
ProviderKind providerKindFromToken(const QString & value);

/// 账号可用性。取值来自 account-provider-state。
enum class AccountAvailability {
  Available,
  Pending,
  Unavailable,
  Unknown,
};

QString toToken(AccountAvailability availability);
AccountAvailability accountAvailabilityFromToken(const QString & value);

/// 账号不可用原因。
enum class AccountUnavailableReason {
  None,
  NotAuthenticated,
  NotConnected,
  CredentialFailed,
  NotEntitled,
};

QString toToken(AccountUnavailableReason reason);
AccountUnavailableReason accountUnavailableReasonFromToken(const QString & value);

struct ProviderConfig {
  QString id;
  QString name;
  ProviderKind kind = ProviderKind::OpenAICompatible;
  QString baseUrl;
  QString apiKey;
  /// 该 provider 暴露的模型 id 列表。
  QStringList models;
  bool enabled = true;
  /// 额外请求头，用于自建网关。
  QJsonObject extraHeaders;

  /// 运行时账号状态；由账号刷新流程写入。
  AccountAvailability availability = AccountAvailability::Unknown;
  AccountUnavailableReason unavailableReason = AccountUnavailableReason::None;

  /// 配置是否足以发起请求。
  bool isUsable() const;
  QJsonObject toJson() const;
  static ProviderConfig fromJson(const QJsonObject & json);
};

struct ModelInfo {
  QString providerId;
  QString modelId;
  QString displayName;
  int contextWindow = 128000;
  int maxOutputTokens = 8192;
  bool supportsTools = true;
  bool supportsReasoning = false;
  bool supportsImageInput = false;
  /// 可用思考档位（如 low/medium/high）；空表示不支持。
  QStringList reasoningLevels;
};

/// 单个模型的能力覆盖。
///
/// 用途：provider 自报的模型元信息经常不完整或不准（尤其是自建网关与第三方兼容
/// 服务），而上下文窗口错了会直接导致压缩阈值判断失误。这里让用户能手工修正。
/// 0 / 空表示"沿用内置默认"，因此全空的覆盖等价于没有覆盖。
struct ModelOptionOverride {
  /// 上下文窗口（token）。0 = 用内置默认。
  int contextWindow = 0;
  /// 单次最大输出（token）。0 = 用内置默认。
  int maxOutputTokens = 0;
  /// 该模型可用的思考档位 id 列表。空 = 用内置默认。
  QStringList reasoningLevels;
  /// 新建会话时的默认思考档位。空 = 取第一个可用档位。
  QString defaultReasoningLevel;

  bool isEmpty() const;
  QJsonObject toJson() const;
  static ModelOptionOverride fromJson(const QJsonObject & json);

  /// 把非空字段覆盖到模型元信息上。`info` 必须非空。
  void applyTo(ModelInfo * info) const;
};

/// 模型覆盖的配置键。与 `ModelSelection::displayValue()` 的前半段一致，
/// 但**不含**思考档位——档位是会话级选择，不是模型能力配置。
QString modelOptionKey(const QString & providerId, const QString & modelId);

/// 会话使用的模型选择。对应 ModelSelection。
struct ModelSelection {
  QString providerId;
  QString modelId;
  /// 思考档位（本实现的 options.reasoningLevel）。
  QString reasoningLevel;

  bool isValid() const
  {
    return !providerId.isEmpty() && !modelId.isEmpty();
  }

  /// 展示串，格式formatModelPickerValue 一致：
  /// `providerId/modelId[$reasoningLevel]`
  QString displayValue() const;
  static ModelSelection parseDisplayValue(const QString & value);
};

}  // namespace lycode

// 域类型需要参与 Qt 的信号槽与 QSignalSpy，因此声明为元类型。
// 放在文件末尾：Q_DECLARE_METATYPE 要求类型完整，且必须位于命名空间之外。
// 只做编译期声明，不调用 qRegisterMetaType——本应用所有连接都是同线程直连，
// 不需要运行时注册；QSignalSpy 也只需要编译期声明就能把实参存进 QVariant。
Q_DECLARE_METATYPE(lycode::Workspace)
Q_DECLARE_METATYPE(lycode::Usage)
Q_DECLARE_METATYPE(lycode::Part)
Q_DECLARE_METATYPE(lycode::Message)
Q_DECLARE_METATYPE(lycode::Session)
Q_DECLARE_METATYPE(lycode::PermissionRule)
Q_DECLARE_METATYPE(lycode::PermissionOption)
Q_DECLARE_METATYPE(lycode::PermissionRequest)
Q_DECLARE_METATYPE(lycode::PermissionResponse)
Q_DECLARE_METATYPE(lycode::ProviderConfig)
Q_DECLARE_METATYPE(lycode::ModelInfo)
Q_DECLARE_METATYPE(lycode::ModelSelection)

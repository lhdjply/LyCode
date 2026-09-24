// LyCode — 工具系统基础设施实现
//
// 本文件只放"与具体工具无关"的部分：枚举转换、工具基类默认行为、
// 执行上下文（路径安全）、注册表。具体工具在各自的 .cpp 里。
//
// 设计约束（来自 Tool.h 的契约，实现时不得偏离）：
//   * 策略完全由声明式 metadata 驱动，这里不出现任何工具名分支。
//   * 每个工具的 execute 必须恰好回调一次（各工具用统一的成功/失败出口保证）。
#include "tools/Tool.h"

#include <csignal>

#ifdef Q_OS_UNIX
  #include <unistd.h>  // setsid()
#endif

#include "core/Json.h"
#include "tools/TodoStore.h"
#include "tools/ToolUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringConverter>

#include <algorithm>

#include "tools/BashTool.h"
#include "tools/EditTool.h"
#include "tools/GlobTool.h"
#include "tools/GrepTool.h"
#include "tools/ReadTool.h"
#include "tools/AgentTool.h"
#include "tools/SkillTool.h"
#include "tools/TaskTools.h"
#include "tools/TodoTool.h"
#include "tools/WriteTool.h"
#include <QCoreApplication>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.tool.base")

// ── 枚举 ↔ 字面量 ────────────────────────────────────────────────────────────
// 与 Types.cpp 一样用表驱动，保证 to/from 两个方向不会各自漂移。
template <typename Enum>
struct EnumName {
  Enum value;
  const char * name;
};

constexpr EnumName<SideEffectScope> kScopes[] = {
  {SideEffectScope::None, "none"},
  {SideEffectScope::Workspace, "workspace"},
  {SideEffectScope::Git, "git"},
  {SideEffectScope::Network, "network"},
  {SideEffectScope::System, "system"},
  {SideEffectScope::Session, "session"},
  {SideEffectScope::UserInteraction, "userInteraction"},
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

/// 规则内容为空时代表"匹配整个工具"，因此需要一个稳定的空串比较口径。
bool sameRule(const PermissionRule & lhs, const PermissionRule & rhs)
{
  return lhs.toolName == rhs.toolName && lhs.ruleContent == rhs.ruleContent &&
         lhs.behavior == rhs.behavior;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// SideEffectScope
// ─────────────────────────────────────────────────────────────────────────────

QString toToken(SideEffectScope scope)
{
  return nameOf(kScopes, scope, QStringLiteral("none"));
}

SideEffectScope sideEffectScopeFromToken(const QString & value)
{
  return valueOf(kScopes, value, SideEffectScope::None);
}

bool sideEffectScopeWritesWorkspace(SideEffectScope scope)
{
  // 集合口径来自 Tool.h 注释：{Workspace, Git, System} 会改写工作区
  // （System 之所以算，是因为 shell 命令可以改任何东西）。
  switch(scope) {
    case SideEffectScope::Workspace:
    case SideEffectScope::Git:
    case SideEffectScope::System:
      return true;
    case SideEffectScope::None:
    case SideEffectScope::Network:
    case SideEffectScope::Session:
    case SideEffectScope::UserInteraction:
      return false;
  }
  return false;
}

bool sideEffectScopeTouchesOutsideWorld(SideEffectScope scope)
{
  // Session / UserInteraction 只动会话内部状态，不碰外部世界。
  return scope != SideEffectScope::None && scope != SideEffectScope::Session &&
         scope != SideEffectScope::UserInteraction;
}

// ─────────────────────────────────────────────────────────────────────────────
// 权限类别推导
// ─────────────────────────────────────────────────────────────────────────────

PermissionKind permissionKindFor(const ToolMetadata & metadata)
{
  // 顺序很重要：只读优先，因为"读"与"写"的 UI 呈现完全不同；
  // 之后按副作用范围推导，保证新增工具只要填对 scope 就能得到合理类别。
  if(metadata.readOnly) {
    return PermissionKind::Read;
  }
  switch(metadata.sideEffectScope) {
    case SideEffectScope::System:
      return PermissionKind::Execute;
    case SideEffectScope::Network:
      return PermissionKind::Network;
    case SideEffectScope::Workspace:
    case SideEffectScope::Git:
      return PermissionKind::Write;
    case SideEffectScope::None:
    case SideEffectScope::Session:
    case SideEffectScope::UserInteraction:
      // 这三类都不触碰工作区或外部世界，因此不该按"写入/执行"处理。
      // 归为 Read 后走只读直通；它们真正的副作用发生在**内部**工具上
      // （例如 Agent 派生的子代理会各自走权限链），不需要在这里弹窗。
      // 本实现的 checkBuildMode 对 scope=session + risk=low + 非破坏性
      // + 不需批准的工具同样是直接 allow。
      return PermissionKind::Read;
  }
  return PermissionKind::Read;
}

// ─────────────────────────────────────────────────────────────────────────────
// ToolResult
// ─────────────────────────────────────────────────────────────────────────────

ToolResult ToolResult::success(const QString & output, const QJsonObject & metadata)
{
  ToolResult result;
  result.ok = true;
  result.output = output;
  result.metadata = metadata;
  return result;
}

ToolResult ToolResult::failure(const QString & error, const QString & errorCode,
                               const QJsonObject & metadata)
{
  ToolResult result;
  result.ok = false;
  result.error = error;
  result.errorCode = errorCode;
  result.metadata = metadata;
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// ToolContext
// ─────────────────────────────────────────────────────────────────────────────

bool ToolContext::isCancelled() const
{
  return cancelled != nullptr && cancelled->load();
}

QString ToolContext::resolvePath(const QString & pathOrRelative) const
{
  if(pathOrRelative.isEmpty()) {
    return {};
  }

  const QString base = !workingDirectory.isEmpty()
                       ? workingDirectory
                       : (!workspace.path.isEmpty() ? workspace.path : QString());
  const QString workspacePath =
    !workspace.path.isEmpty() ? QDir::cleanPath(workspace.path) : QString();

  // 绝对路径直接用；相对路径挂到 base 下。
  QString candidate = QDir::isAbsolutePath(pathOrRelative)
                      ? QDir::cleanPath(pathOrRelative)
                      : QDir::cleanPath(base + QLatin1Char('/') + pathOrRelative);
  if(candidate.isEmpty()) {
    return {};
  }

  // 归一化的关键：canonicalFilePath 会解析符号链接与 `..`，
  // 但**只对已存在的路径有效**，写新文件时会返回空串。
  // 所以从目标向上找到第一个存在的祖先做 canonical 化，再把剩余部件拼回去。
  QString existing = candidate;
  QStringList tail;
  while(!existing.isEmpty() && !QFileInfo::exists(existing)) {
    const QString name = QFileInfo(existing).fileName();
    const QString parent = QFileInfo(existing).absolutePath();
    if(parent == existing) {
      break;  // 到达根仍不存在（理论不可达）
    }
    if(!name.isEmpty()) {
      tail.prepend(name);
    }
    existing = parent;
  }

  QString normalized;
  if(!existing.isEmpty() && QFileInfo::exists(existing)) {
    const QString real = QFileInfo(existing).canonicalFilePath();
    normalized = QDir::cleanPath(real.isEmpty() ? existing : real);
  }
  else {
    normalized = candidate;
  }
  for(const QString & part : tail) {
    normalized = QDir::cleanPath(normalized + QLatin1Char('/') + part);
  }

  // 工作区为空时只做归一不校验（无边界可校验）。
  if(workspacePath.isEmpty()) {
    return normalized;
  }

  // 逃逸判定用 workspace 的 canonical 形式做基准，这样工作区本身
  // 位于符号链接之下时不会误判。工作区若不存在（尚未创建），退回 cleanPath。
  const QString realWorkspaceRaw = QFileInfo(workspacePath).canonicalFilePath();
  const QString realWorkspace =
    realWorkspaceRaw.isEmpty() ? workspacePath : QDir::cleanPath(realWorkspaceRaw);

  // 同时接受"归一前"与"归一后"的工作区前缀：candidate 可能已经是
  // workingDirectory 下的绝对路径，而 workingDirectory 可能与 workspace 不同。
  const QString baseClean = !base.isEmpty() ? QDir::cleanPath(base) : QString();
  const QString realBaseRaw = !baseClean.isEmpty() ? QFileInfo(baseClean).canonicalFilePath() : QString();
  const QString realBase = realBaseRaw.isEmpty() ? baseClean : QDir::cleanPath(realBaseRaw);

  const bool insideWorkspace = toolutil::pathWithin(realWorkspace, normalized) ||
                               toolutil::pathWithin(workspacePath, normalized);
  const bool insideBase =
    (!realBase.isEmpty() && toolutil::pathWithin(realBase, normalized)) ||
    (!baseClean.isEmpty() && toolutil::pathWithin(baseClean, normalized));

  if(!insideWorkspace && !insideBase) {
    qCWarning(log) << "路径越界，已拒绝:" << toolutil::redactForLog(pathOrRelative)
                   << "workspace=" << workspacePath;
    return {};
  }
  return normalized;
}

QString ToolContext::displayPath(const QString & absolutePath) const
{
  if(absolutePath.isEmpty()) {
    return absolutePath;
  }
  const QString normalized = QDir::cleanPath(absolutePath);
  const QString root = !workspace.path.isEmpty() ? QDir::cleanPath(workspace.path) : QString();
  if(root.isEmpty()) {
    return normalized;
  }
  if(normalized == root) {
    return QStringLiteral(".");
  }
  if(toolutil::pathWithin(root, normalized)) {
    return normalized.mid(root.size() + 1);
  }
  return normalized;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tool 基类
// ─────────────────────────────────────────────────────────────────────────────

Tool::~Tool() = default;

QString Tool::permissionCapability() const
{
  return metadata().name;
}

QString Tool::ruleSubject(const QJsonObject & input) const
{
  // 顺序照抄 本实现的候选键优先级：命令 > URL > 文件路径 > 通用路径 > 模式。
  static const QStringList candidates = {
    QStringLiteral("command"), QStringLiteral("url"), QStringLiteral("file_path"),
    QStringLiteral("path"), QStringLiteral("pattern"),
  };
  for(const QString & key : candidates) {
    const QString value = json::str(input, key);
    if(!value.isEmpty()) {
      return value;
    }
  }
  return {};
}

QString Tool::title(const QJsonObject & input) const
{
  const QString subject = ruleSubject(input);
  if(subject.isEmpty()) {
    return metadata().name;
  }
  return metadata().name + QStringLiteral(": ") + toolutil::redactForLog(subject, 120);
}

QString Tool::permissionDescription(const QJsonObject & input) const
{
  const ToolMetadata meta = metadata();
  const QString subject = ruleSubject(input);
  if(subject.isEmpty()) {
    return QCoreApplication::translate("tools::Tool", "Tool %1 requires execute permission.").arg(meta.name);
  }
  return QCoreApplication::translate("tools::Tool", "Tool %1 will operate on: %2").arg(meta.name,
                                                                                       toolutil::redactForLog(subject, 200));
}

QList<PermissionRule> Tool::permissionRules(const QJsonObject & input) const
{
  // 两条粒度：本次调用的精确主体 + 一个更宽的"范围"（目录前缀 / 命令首词）。
  // UI 的"始终允许"通常给后者，但精确主体在文件、命令混用场景下更安全。
  const QString capability = permissionCapability();
  const QString subject = ruleSubject(input);
  if(capability.isEmpty() || subject.isEmpty()) {
    return {};
  }

  PermissionRule exact;
  exact.toolName = capability;
  exact.ruleContent = subject;
  exact.behavior = PermissionRuleBehavior::Allow;

  PermissionRule scope;
  scope.toolName = capability;
  scope.behavior = PermissionRuleBehavior::Allow;
  if(capability == QLatin1String("bash")) {
    scope.ruleContent = toolutil::commandFirstWord(subject);
  }
  else {
    scope.ruleContent = toolutil::directoryPrefix(subject);
  }

  QList<PermissionRule> rules;
  rules.append(exact);
  if(!scope.ruleContent.isEmpty() && !sameRule(exact, scope)) {
    rules.append(scope);
  }
  return rules;
}

QString Tool::validateInput(const QJsonObject & input) const
{
  const QJsonObject schema = inputSchema();
  const QJsonArray required = schema.value(QStringLiteral("required")).toArray();
  for(const QJsonValue & value : required) {
    const QString key = value.toString();
    if(key.isEmpty()) {
      continue;
    }
    if(!json::has(input, key)) {
      // 文案面向模型：明确指出缺哪个字段，模型下一轮才能自我修正。
      return QCoreApplication::translate("tools::Tool", "Missing required argument `%1`").arg(key);
    }
  }
  return {};
}

ToolSpec Tool::spec() const
{
  const ToolMetadata meta = metadata();
  ToolSpec toolSpec;
  toolSpec.name = meta.name;
  toolSpec.description = meta.description;
  if(!meta.modelInstructions.isEmpty()) {
    toolSpec.description += QStringLiteral("\n\n") + meta.modelInstructions;
  }
  toolSpec.inputSchema = inputSchema();
  return toolSpec;
}

bool Tool::canRunInParallel() const
{
  // 判定顺序照抄 本实现 canRunInParallel：
  //   1. destructive 一律串行（破坏性操作并发执行无法审计）
  //   2. 显式 Safe   → 可并发
  //   3. 显式 Serial → 串行
  //   4. 未声明      → 只读且无副作用才可并发
  //
  // 第 3 步必须独立存在：只读工具也可能有必须独占的理由（共享缓存、
  // 单例资源等）。少了它，显式声明串行的只读工具会被第 4 步的豁免放行。
  const ToolMetadata meta = metadata();
  if(meta.destructive) {
    return false;
  }
  if(meta.concurrency == ToolMetadata::Concurrency::Safe) {
    return true;
  }
  if(meta.concurrency == ToolMetadata::Concurrency::Serial) {
    return false;
  }
  return meta.readOnly && meta.sideEffectScope == SideEffectScope::None;
}

QString Tool::stringArg(const QJsonObject & input, const QString & key, const QString & fallback)
{
  return json::str(input, key, fallback);
}

bool Tool::boolArg(const QJsonObject & input, const QString & key, bool fallback)
{
  return json::boolean(input, key, fallback);
}

int Tool::intArg(const QJsonObject & input, const QString & key, int fallback)
{
  return json::integer(input, key, fallback);
}

QJsonArray Tool::arrayArg(const QJsonObject & input, const QString & key)
{
  return json::array(input, key);
}

// ─────────────────────────────────────────────────────────────────────────────
// ToolRegistry
// ─────────────────────────────────────────────────────────────────────────────

ToolRegistry::ToolRegistry() = default;
ToolRegistry::~ToolRegistry() = default;

ToolRegistry::ToolRegistry(ToolRegistry &&other) noexcept
  : entries_(std::move(other.entries_)),
    index_(std::move(other.index_)),
    owned_(std::move(other.owned_))
{
  // 源对象只需清掉索引：继续用它查找会命中已经不归它所有的指针。
  other.entries_.clear();
  other.index_.clear();
}

ToolRegistry & ToolRegistry::operator=(ToolRegistry &&other) noexcept
{
  if(this == &other) {
    return *this;
  }
  // 移动赋值必须先释放自己持有的工具（owned_ 的移动赋值会做这件事），
  // 再接管对方的三个容器。
  owned_ = std::move(other.owned_);
  entries_ = std::move(other.entries_);
  index_ = std::move(other.index_);
  other.entries_.clear();
  other.index_.clear();
  return *this;
}

bool ToolRegistry::add(Tool * tool, const QStringList & aliases)
{
  if(tool == nullptr) {
    qCCritical(log) << "拒绝注册空工具指针";
    return false;
  }
  const QString name = tool->metadata().name;
  if(name.isEmpty()) {
    qCCritical(log) << "拒绝注册无名工具（metadata().name 为空）";
    return false;
  }

  Tool * existing = index_.value(name, nullptr);
  if(existing != nullptr) {
    // 同名覆盖：先彻底移除旧实现（含它的别名索引），否则销毁后
    // index_ 里会留下悬垂指针。
    for(Entry & entry : entries_) {
      if(entry.name != name) {
        continue;
      }
      for(const QString & alias : entry.aliases) {
        if(index_.value(alias, nullptr) == existing) {
          index_.remove(alias);
        }
      }
      break;
    }
    index_.remove(name);
    for(auto iterator = owned_.begin(); iterator != owned_.end(); ++iterator) {
      if(iterator->get() == existing) {
        owned_.erase(iterator);  // 析构旧实现
        break;
      }
    }

    // 保留原位置，模型看到的声明顺序不变。
    owned_.emplace_back(tool);
    for(Entry & entry : entries_) {
      if(entry.name == name) {
        entry.tool = tool;
        entry.aliases = aliases;
        break;
      }
    }
    qCInfo(log) << "工具被覆盖注册:" << name;
  }
  else {
    owned_.emplace_back(tool);
    Entry entry;
    entry.name = name;
    entry.tool = tool;
    entry.aliases = aliases;
    entries_.append(entry);
  }
  index_.insert(name, tool);

  for(const QString & alias : aliases) {
    if(alias.isEmpty() || alias == name) {
      continue;
    }
    if(index_.contains(alias)) {
      // 别名不覆盖已有名字：否则一个别名可能悄悄遮蔽真实工具，
      // 让模型的调用落到意料之外的工具上。
      qCWarning(log) << "别名已被占用，忽略:" << alias << "→" << name;
      continue;
    }
    index_.insert(alias, tool);
  }
  return true;
}

Tool * ToolRegistry::find(const QString & name) const
{
  return index_.value(name, nullptr);
}

bool ToolRegistry::contains(const QString & name) const
{
  return index_.contains(name);
}

QStringList ToolRegistry::names() const
{
  QStringList result;
  result.reserve(entries_.size());
  for(const Entry & entry : entries_) {
    result.append(entry.name);
  }
  return result;
}

QList<ToolSpec> ToolRegistry::specs() const
{
  QList<ToolSpec> result;
  for(const Entry & entry : entries_) {
    if(entry.tool == nullptr) {
      continue;
    }
    if(!entry.tool->metadata().providerVisible) {
      continue;
    }
    result.append(entry.tool->spec());
  }
  return result;
}

QList<ToolSpec> ToolRegistry::specsFor(const QStringList & allowedNames) const
{
  QList<ToolSpec> result;
  for(const Entry & entry : entries_) {
    if(entry.tool == nullptr || !entry.tool->metadata().providerVisible) {
      continue;
    }
    if(allowedNames.contains(entry.name)) {
      result.append(entry.tool->spec());
    }
  }
  return result;
}

QList<ToolSpec> ToolRegistry::specsExcluding(const QStringList & disallowedPatterns) const
{
  // 支持 `Name` 与 `Name(pattern)` 两种写法。由于此处只有名字、没有入参，
  // `Name(pattern)` 的括号部分也按名字整体匹配（不做入参级过滤）。
  QStringList blocked;
  blocked.reserve(disallowedPatterns.size());
  for(const QString & raw : disallowedPatterns) {
    QString name = raw.trimmed();
    const qsizetype open = name.indexOf(QLatin1Char('('));
    if(open > 0) {
      name = name.left(open);
    }
    if(!name.isEmpty()) {
      blocked.append(name);
    }
  }

  QList<ToolSpec> result;
  for(const Entry & entry : entries_) {
    if(entry.tool == nullptr || !entry.tool->metadata().providerVisible) {
      continue;
    }
    if(blocked.contains(entry.name)) {
      continue;
    }
    result.append(entry.tool->spec());
  }
  return result;
}

QList<Tool *> ToolRegistry::tools() const
{
  QList<Tool *> result;
  result.reserve(entries_.size());
  for(const Entry & entry : entries_) {
    if(entry.tool != nullptr) {
      result.append(entry.tool);
    }
  }
  return result;
}

ToolRegistry ToolRegistry::createWithBuiltins()
{
  ToolRegistry registry;
  // 主名固定，别名留待后续阶段（Task / 子 Agent 本阶段不实现）。
  registry.add(new BashTool());
  registry.add(new ReadTool());
  registry.add(new WriteTool());
  registry.add(new EditTool());
  registry.add(new GlobTool());
  registry.add(new GrepTool());
  registry.add(new TodoReadTool());
  registry.add(new TodoWriteTool());
  // `Task` 是 本实现里 Agent 的 Claude Code 兼容别名，模型两种写法都能调到。
  registry.add(new AgentTool(), {QStringLiteral("Task")});
  // 别名按既定语义，兼容按 Claude Code 习惯发起的调用。
  registry.add(new TaskOutputTool(),
  {QStringLiteral("BashOutput"), QStringLiteral("AgentOutput")});
  registry.add(new SkillTool());
  registry.add(new TaskStopTool(),
  {QStringLiteral("KillBash"), QStringLiteral("KillShell")});
  qCInfo(log) << "已注册内置工具:" << registry.names().join(QStringLiteral(", "));
  return registry;
}

// ─────────────────────────────────────────────────────────────────────────────
// toolutil —— 内置工具共享辅助
//
// 故意实现在 Tool.cpp：工具层只需要一个必定被链接的基础翻译单元，
// 测试与验证程序链接 Tool.cpp 即可获得全部共享辅助（见 ToolUtils.h 顶部说明）。
// ─────────────────────────────────────────────────────────────────────────────

namespace toolutil
{
namespace
{

Q_LOGGING_CATEGORY(utilLog, "lycode.tool.utils")

/// 递归扫描时跳过的目录名。`build*` 用前缀匹配（build / build-tools / build-debug），
/// 其余按名字精确匹配。
const QStringList kSkipExact = {
  QStringLiteral(".git"),         QStringLiteral(".hg"),        QStringLiteral(".svn"),
  QStringLiteral("node_modules"), QStringLiteral(".cache"),    QStringLiteral("__pycache__"),
  QStringLiteral(".venv"),        QStringLiteral("venv"),      QStringLiteral("dist"),
  QStringLiteral("out"),          QStringLiteral("target"),    QStringLiteral(".next"),
  QStringLiteral(".gradle"),      QStringLiteral(".idea"),     QStringLiteral(".tox"),
};

const QStringList kSkipPrefix = {
  QStringLiteral("build"),
  QStringLiteral("cmake-build-"),
};

/// 转义正则特殊字符（glob 的元字符在调用处单独处理）。
QString escapeRegexChar(QChar ch)
{
  static const QString specials = QStringLiteral(R"(\.^$|()[]+?)");
  if(specials.contains(ch)) {
    return QStringLiteral("\\") + ch;
  }
  return QString(ch);
}

}  // namespace

bool shouldSkipDirectory(const QString & dirName)
{
  if(dirName.isEmpty()) {
    return false;
  }
  for(const QString & name : kSkipExact) {
    if(dirName.compare(name, Qt::CaseInsensitive) == 0) {
      return true;
    }
  }
  for(const QString & prefix : kSkipPrefix) {
    if(dirName.startsWith(prefix, Qt::CaseInsensitive)) {
      return true;
    }
  }
  return false;
}

bool looksBinary(const QByteArray & bytes)
{
  // 只扫描前 8KiB：文本文件里 NUL 出现在极靠后的位置几乎不可能，
  // 而全量扫描大文件会白白浪费一次内存遍历。
  const qsizetype index = bytes.indexOf('\0');
  const qsizetype probe = std::min<qsizetype>(bytes.size(), 8192);
  return index >= 0 && index < probe;
}

QString normalizeGlob(const QString & pattern)
{
  QString result = pattern;
  result.replace(QLatin1Char('\\'), QLatin1Char('/'));
  while(result.startsWith(QStringLiteral("./"))) {
    result.remove(0, 2);
  }
  return result;
}

QRegularExpression globToRegex(const QString & pattern, bool caseInsensitive)
{
  const QString source = normalizeGlob(pattern);
  QString out;
  out.reserve(source.size() * 2);

  int i = 0;
  const int n = source.size();
  bool inClass = false;
  while(i < n) {
    const QChar ch = source.at(i);
    if(inClass) {
      // 字符类内部只处理转义与闭合，不做 glob 展开。
      if(ch == QLatin1Char(']')) {
        inClass = false;
        out += QLatin1Char(']');
      }
      else if(ch == QLatin1Char('\\')) {
        out += QStringLiteral("\\\\");
      }
      else {
        out += ch;
      }
      ++i;
      continue;
    }
    if(ch == QLatin1Char('*')) {
      const bool doubleStar = (i + 1 < n) && source.at(i + 1) == QLatin1Char('*');
      if(doubleStar) {
        i += 2;
        // `**/` 匹配"零个或多个目录层级"，这样 `**/*.txt` 也能命中文档根下的
        // 文件（globstar 语义，也是使用者的直觉预期）。
        if(i < n && source.at(i) == QLatin1Char('/')) {
          out += QStringLiteral("(?:.*/)?");
          ++i;
        }
        else {
          out += QStringLiteral(".*");
        }
      }
      else {
        out += QStringLiteral("[^/]*");
        ++i;
      }
      continue;
    }
    if(ch == QLatin1Char('?')) {
      out += QStringLiteral("[^/]");
      ++i;
      continue;
    }
    if(ch == QLatin1Char('{')) {
      // `{a,b}` 简单展开；嵌套花括号不支持（glob 实践里几乎不出现）。
      const int close = source.indexOf(QLatin1Char('}'), i + 1);
      if(close < 0) {
        out += QStringLiteral("\\{");
        ++i;
        continue;
      }
      const QString body = source.mid(i + 1, close - i - 1);
      const QStringList alternatives = body.split(QLatin1Char(','), Qt::KeepEmptyParts);
      QStringList escaped;
      escaped.reserve(alternatives.size());
      for(const QString & alt : alternatives) {
        escaped.append(QRegularExpression::escape(alt));
      }
      out += QStringLiteral("(?:") + escaped.join(QLatin1Char('|')) + QLatin1Char(')');
      i = close + 1;
      continue;
    }
    if(ch == QLatin1Char('[')) {
      // 字符类原样透传，但补一个 `!` → `^` 的 glob 习惯写法。
      inClass = true;
      out += QLatin1Char('[');
      ++i;
      if(i < n && (source.at(i) == QLatin1Char('!') || source.at(i) == QLatin1Char('^'))) {
        out += QLatin1Char('^');
        ++i;
      }
      continue;
    }
    if(ch == QLatin1Char('/')) {
      out += QLatin1Char('/');
      ++i;
      continue;
    }
    out += escapeRegexChar(ch);
    ++i;
  }

  QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
  if(caseInsensitive) {
    options |= QRegularExpression::CaseInsensitiveOption;
  }
  QRegularExpression regex(QStringLiteral("\\A(?:") + out + QStringLiteral(")\\z"), options);
  if(!regex.isValid()) {
    qCWarning(utilLog) << "glob 转换出的正则非法，按永不匹配处理:" << pattern
                       << regex.errorString();
    return QRegularExpression(QStringLiteral("(?!x)x"));
  }
  return regex;
}

bool globMatches(const QRegularExpression & regex, const QString & relativePath)
{
  return regex.match(relativePath).hasMatch();
}

QString joinPath(const QString & base, const QString & relative)
{
  if(base.isEmpty()) {
    return relative;
  }
  if(relative.isEmpty()) {
    return base;
  }
  QString left = base;
  while(left.endsWith(QLatin1Char('/'))) {
    left.chop(1);
  }
  QString right = relative;
  while(right.startsWith(QLatin1Char('/'))) {
    right.remove(0, 1);
  }
  return left + QLatin1Char('/') + right;
}

bool pathWithin(const QString & root, const QString & absolutePath)
{
  if(root.isEmpty()) {
    return true;
  }
  QString normalizedRoot = root;
  while(normalizedRoot.endsWith(QLatin1Char('/'))) {
    normalizedRoot.chop(1);
  }
  if(absolutePath == normalizedRoot) {
    return true;
  }
  return absolutePath.startsWith(normalizedRoot + QLatin1Char('/'));
}

QString commandFirstWord(const QString & command)
{
  const QString trimmed = command.trimmed();
  if(trimmed.isEmpty()) {
    return {};
  }
  // 跳过 `FOO=1 make test` 这类前置赋值，取真正的可执行名。
  static const QRegularExpression whitespace(QStringLiteral("\\s+"));
  static const QRegularExpression identifier(QStringLiteral("\\A[A-Za-z_][A-Za-z0-9_]*\\z"));
  for(const QString & token : trimmed.split(whitespace, Qt::SkipEmptyParts)) {
    if(token.contains(QLatin1Char('=')) && !token.contains(QLatin1Char('/'))) {
      const QString name = token.section(QLatin1Char('='), 0, 0);
      if(identifier.match(name).hasMatch()) {
        continue;
      }
    }
    return token;
  }
  return {};
}

QString directoryPrefix(const QString & path)
{
  if(path.isEmpty()) {
    return {};
  }
  const QFileInfo info(path);
  if(info.isDir()) {
    QString dir = info.absoluteFilePath();
    while(dir.endsWith(QLatin1Char('/')) && dir.size() > 1) {
      dir.chop(1);
    }
    return dir;
  }
  const QString parent = info.absolutePath();
  if(parent.isEmpty()) {
    return path;
  }
  return parent;
}

QString redactForLog(const QString & value, int maxChars)
{
  if(maxChars <= 0 || value.size() <= maxChars) {
    return value;
  }
  return value.left(maxChars) + QStringLiteral("…[+%1 chars]").arg(value.size() - maxChars);
}

bool shouldRejectForReadOnly(const ToolMetadata & metadata, const ToolContext & context)
{
  // 只读模式（plan/ask）下，写入集（Workspace/Git/System）一律拒绝。
  // 注意判据是 metadata 的副作用范围，不是工具名——新增工具自动获得正确行为。
  return context.readOnly && sideEffectScopeWritesWorkspace(metadata.sideEffectScope);
}

ToolResult readOnlyModeFailure(const ToolMetadata & metadata)
{
  ToolResult result = ToolResult::failure(
                        QCoreApplication::translate("tools::Tool",
                                                    "This session is in read-only mode (plan/ask) and tool %1 has write side effects (sideEffectScope=%2), so it was denied. Ask the user to leave read-only mode first.")
                        .arg(metadata.name, toToken(metadata.sideEffectScope)),
                        QStringLiteral("read_only_mode"));
  result.metadata.insert(QStringLiteral("readOnly"), true);
  return result;
}

bool writeFileAtomic(const QString & absolutePath, const QByteArray & bytes, QString * errorOut)
{
  QSaveFile file(absolutePath);
  if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if(errorOut != nullptr) {
      *errorOut = file.errorString();
    }
    return false;
  }
  const qint64 written = file.write(bytes);
  if(written != bytes.size()) {
    if(errorOut != nullptr) {
      *errorOut = QCoreApplication::translate("tools::Tool", "Bytes written do not match (expected %1, wrote %2)")
                  .arg(bytes.size())
                  .arg(written);
    }
    file.cancelWriting();
    return false;
  }
  if(!file.commit()) {
    if(errorOut != nullptr) {
      *errorOut = file.errorString();
    }
    return false;
  }
  return true;
}

QString mimeTypeForPath(const QString & path)
{
  const QString suffix = QFileInfo(path).suffix().toLower();
  static const QHash<QString, QString> table = {
    {QStringLiteral("png"), QStringLiteral("image/png")},
    {QStringLiteral("jpg"), QStringLiteral("image/jpeg")},
    {QStringLiteral("jpeg"), QStringLiteral("image/jpeg")},
    {QStringLiteral("gif"), QStringLiteral("image/gif")},
    {QStringLiteral("webp"), QStringLiteral("image/webp")},
    {QStringLiteral("bmp"), QStringLiteral("image/bmp")},
    {QStringLiteral("svg"), QStringLiteral("image/svg+xml")},
    {QStringLiteral("pdf"), QStringLiteral("application/pdf")},
    {QStringLiteral("json"), QStringLiteral("application/json")},
    {QStringLiteral("txt"), QStringLiteral("text/plain")},
    {QStringLiteral("md"), QStringLiteral("text/markdown")},
    {QStringLiteral("cpp"), QStringLiteral("text/x-c++src")},
    {QStringLiteral("h"), QStringLiteral("text/x-c++hdr")},
    {QStringLiteral("py"), QStringLiteral("text/x-python")},
  };
  return table.value(suffix, QStringLiteral("text/plain"));
}

QByteArray trimIncompleteUtf8(QByteArray bytes)
{
  // UTF-8 序列最长 4 字节。从尾部回退最多 3 个续接字节，
  // 找到起始字节后判断它声明的长度是否已被完整包含。
  int back = 0;
  while(back < 3 && back < bytes.size()) {
    const unsigned char ch = static_cast<unsigned char>(bytes.at(bytes.size() - 1 - back));
    if((ch & 0xC0) == 0x80) {
      ++back;
      continue;
    }
    int expected = 1;
    if((ch & 0x80) == 0x00) {
      expected = 1;
    }
    else if((ch & 0xE0) == 0xC0) {
      expected = 2;
    }
    else if((ch & 0xF0) == 0xE0) {
      expected = 3;
    }
    else if((ch & 0xF8) == 0xF0) {
      expected = 4;
    }
    if(expected > back + 1) {
      bytes.chop(back + 1);
    }
    break;
  }
  return bytes;
}

QString chompTrailingNewlines(QString text)
{
  while(text.endsWith(QLatin1Char('\n'))) {
    text.chop(1);
  }
  return text;
}

OutputBudget::OutputBudget(qint64 maxBytes) : maxBytes_(maxBytes > 0 ? maxBytes : 0) {}

void OutputBudget::appendBytes(const QByteArray & bytes)
{
  totalBytes_ += bytes.size();
  if(maxBytes_ <= 0) {
    kept_ += bytes;
    return;
  }
  if(kept_.size() >= maxBytes_) {
    truncated_ = true;
    return;
  }
  const qsizetype room = static_cast<qsizetype>(maxBytes_ - kept_.size());
  if(bytes.size() <= room) {
    kept_ += bytes;
    return;
  }
  kept_ += bytes.left(room);
  truncated_ = true;
}

void OutputBudget::append(const QString & chunk)
{
  appendBytes(chunk.toUtf8());
}

void OutputBudget::appendLine(const QString & line)
{
  append(line);
  appendBytes(QByteArrayLiteral("\n"));
}

QString OutputBudget::text() const
{
  QString result = QString::fromUtf8(trimIncompleteUtf8(kept_));
  if(truncated_) {
    result += QCoreApplication::translate("tools::Tool", "\n…[output truncated: over the %1 byte budget]").arg(maxBytes_);
  }
  return result;
}

QString shellSingleQuote(const QString & value)
{
  QString escaped = value;
  // 单引号里唯一不能直接出现的就是单引号本身；关掉、插一个转义的单引号、再打开。
  escaped.replace(QLatin1String("'"), QLatin1String("'\\''"));
  return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

#ifdef Q_OS_WIN
namespace
{

/// 把候选程序名解析成绝对路径；解析不出返回空。
QString resolveOnWindows(const QString & name)
{
  return QStandardPaths::findExecutable(name);
}

/// PowerShell 的固定前缀：UTF-8 输出 + Unix 习惯的 sleep。
///
/// 编码部分：不做这一步，`pwsh -Command` 在中文 Windows 上按控制台代码页（936）
/// 输出，而调用方按 UTF-8 解码，中文会变成乱码。两个变量缺一不可：
/// `[Console]::OutputEncoding` 管原生命令（git/cmake）写进管道的那一路，
/// `$OutputEncoding` 管 PowerShell 自己写给自己管道的那一路。
/// 注意编码部分用**单引号**字符串，`$` 才不会被外层提前展开。
///
/// sleep 部分不是可有可无的糖：PowerShell 自带的 `Start-Sleep 2` 按**毫秒**解释
/// 位置参数，也就是 2ms 而不是 2 秒；而 `sleep 2` 恰恰是模型最常写的等待写法。
/// 没有这个垫片，命令会"成功"但等待时间差了三个数量级——比直接报错更危险。
/// 这里把它改成 Unix 语义（按秒，支持小数），并让 `-Seconds`/`-Milliseconds`
/// 这类显式写法继续走原生参数绑定，不破坏正确的 PowerShell 写法。
QString powershellPreamble()
{
  return QStringLiteral(
           "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;"
           "$OutputEncoding=[System.Text.Encoding]::UTF8;"
           "function global:sleep { param([Parameter(Position=0)][double]$Seconds,"
           "[double]$Milliseconds) "
           "$ms = if ($PSBoundParameters.ContainsKey('Milliseconds')) { $Milliseconds } "
           "else { $Seconds * 1000 }; "
           "Start-Sleep -Milliseconds ([int]$ms) };");
}

/// 去掉整体包裹命令的一对花括号。
///
/// 调用方习惯写 `{ cmd; }` 表示"把这几条语句当一个整体"（POSIX shell 里合法），
/// 但在 PowerShell 里 `{ ... }` 是 ScriptBlock **字面量**，只有 `&` / `.` 去调用
/// 它才会执行。留着它，命令会被静默当成一个值——不报错、不执行，只是没效果；
/// 而 `;` 后面直接跟 `{` 还会变成语法错误。两种情况都比报错更难排查。
///
/// 只剥离"整个字符串被一对括号包住"的情形。带参数声明的真脚本块
/// （`{ param($a) ... }`）不匹配，原样保留——那是调用方有意写的。
QString normalizePowerShellBlock(const QString & command)
{
  static const QRegularExpression outerBraces(
    QStringLiteral(R"(^\s*\{\s*(.*?)\s*\}\s*$)"),
    QRegularExpression::DotMatchesEverythingOption);
  const QRegularExpressionMatch match = outerBraces.match(command);
  return match.hasMatch() ? match.captured(1) : command;
}

/// 命令 → `-EncodedCommand` 需要的 Base64（UTF-16LE）。
///
/// 走编码命令而不是普通参数：命令里带引号、`$`、中文、换行是常态，用命令行参数
/// 传递要经过 QProcess → CreateProcess → PowerShell 解析三层引号转义，任何一层
/// 出错都是静默变形（命令被截断或参数错位）。编码后只剩 [A-Za-z0-9+/=]，
/// 与引号、空格、代码页全都无关。
QString encodePowershellCommand(const QString & command)
{
  QStringEncoder encoder(QStringConverter::Utf16LE);
  const QByteArray utf16 = encoder(command);
  return QString::fromLatin1(utf16.toBase64());
}

}  // namespace
#endif

ShellSpec shellSpec()
{
  // 探测只做一次：一个进程内的 shell 不会中途换人，而 metadata()（拼给模型的
  // 说明）和 execute()（真正启动进程）必须拿到**同一个**结果——如果两次探测
  // 结果不同，"告诉模型的 shell"和"实际执行的 shell"就会分叉，那正是本工具的
  // 原始故障模式。函数内静态变量顺带省掉 Windows 上重复的 PATH 遍历。
  static const ShellSpec cached = []() -> ShellSpec {
    ShellSpec spec;
#ifdef Q_OS_WIN
    // 先看显式覆盖：用户可以据此指定特定版本的 PowerShell（如 7 的 pwsh）。
    const QString override = qEnvironmentVariable("LYCODE_SHELL").trimmed();
    if(!override.isEmpty()) {
      const QString resolved = resolveOnWindows(override);
      if(!resolved.isEmpty()) {
        spec.program = resolved;
        spec.label = QFileInfo(resolved).completeBaseName();
        spec.posix = false;
        spec.arguments = spec.label.compare(QLatin1String("cmd"), Qt::CaseInsensitive) == 0
        ? QStringList{QStringLiteral("/d"), QStringLiteral("/s"), QStringLiteral("/c")}
:
        QStringList{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                    QStringLiteral("-EncodedCommand")};
        return spec;
      }
      qWarning("LYCODE_SHELL=%s 解析不到可执行文件，改用自动探测",
               qUtf8Printable(override));
    }

    // PowerShell 7（pwsh）优先，其次 Windows 自带的 Windows PowerShell 5.1。
    for(const QString & name : {
    QStringLiteral("pwsh.exe"), QStringLiteral("powershell.exe")
    })
    {
      const QString resolved = resolveOnWindows(name);
      if(resolved.isEmpty()) {
        continue;
      }
      spec.program = resolved;
      spec.label = name.startsWith(QLatin1String("pwsh"), Qt::CaseInsensitive)
                   ? QStringLiteral("PowerShell")
                   : QStringLiteral("Windows PowerShell");
      spec.posix = false;
      // -NoProfile：不加载用户 profile。profile 里的 Write-Host 横幅会混进工具输出，
      // 而工具输出是喂给模型的——每一行噪声都是白花的 token。
      // -NonInteractive：明确不要交互提示，否则命令卡在确认框上直到超时。
      spec.arguments = QStringList{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                                   QStringLiteral("-EncodedCommand")};
      return spec;
    }

    // 兜底：PowerShell 不可用（被裁剪的系统镜像）时退回 cmd。
    spec.program = qEnvironmentVariable("ComSpec", QStringLiteral("cmd.exe"));
    spec.label = QStringLiteral("cmd");
    spec.posix = false;
    // /d 跳过 AutoRun（某些机器上的注册表 AutoRun 会先跑一段无关命令），
    // /s 保证首尾引号按 cmd 的规则处理，不额外剥一层。
    spec.arguments = QStringList{QStringLiteral("/d"), QStringLiteral("/s"), QStringLiteral("/c")};
    return spec;
#else
    // `/bin/bash` 在极简容器里可能不存在；退回 /bin/sh 也比直接失败好。
    const bool hasBash = QFileInfo::exists(QStringLiteral("/bin/bash"));
    spec.program = hasBash ? QStringLiteral("/bin/bash") : QStringLiteral("/bin/sh");
    spec.label = hasBash ? QStringLiteral("bash") : QStringLiteral("sh");
    spec.posix = true;
    spec.arguments = QStringList{QStringLiteral("-lc")};
    return spec;
#endif
  }();
  return cached;
}

QStringList shellArgumentsFor(const ShellSpec & spec, const QString & command)
{
  QStringList arguments = spec.arguments;
#ifdef Q_OS_WIN
  if(!spec.posix && spec.label != QLatin1String("cmd")) {
    // PowerShell：编码命令，见 encodePowershellCommand 的说明。
    // -EncodedCommand 收的是**脚本原文**，所以这里传的就是命令本身，
    // 不能带引号或 `-Command` 那套转义（那是 `-Command` 的规矩）。
    arguments << encodePowershellCommand(powershellPreamble() + command);
    return arguments;
  }
#endif
  arguments << command;
  return arguments;
}

QString shellBackgroundWrapper(const ShellSpec & spec, const QString & command,
                               const QString & outputPath, const QString & exitPath)
{
  if(spec.posix) {
    // 刻意**不用 exec**：让外层 shell 留下来，跑完命令后把 $? 写进旁路文件。
    // 分离式任务没有 wait() 可取退出码，没有这一步就永远只能记 -1。
    return QStringLiteral("{ ") + command + QStringLiteral("; } >> ") +
           shellSingleQuote(outputPath) +
           QStringLiteral(" 2>&1; echo $? > ") +
           shellSingleQuote(exitPath);
  }

#ifdef Q_OS_WIN
  if(spec.label == QLatin1String("cmd")) {
    // 退出码取自 cmd 的 errorlevel，写完再重定向，保证 echo 本身不被记成结果。
    // 刻意**不用括号**把命令包起来：cmd 是整行预解析的，`( ... | findstr x )`
    // 这种带管道的命令会被拆坏，直接执行才是正确的语义。
    return QStringLiteral(">> \"") + outputPath + QStringLiteral("\" 2>&1 ") +
           command + QStringLiteral(" & echo %errorlevel% > \"") + exitPath +
           QLatin1Char('"');
  }

  // PowerShell：整段必须是**一条语句**，重定向才能挂在它末尾。
  //
  // 踩过两次的坑，都是"把 PowerShell 当 POSIX 写"：
  //   1. 用分号把 `*>>` 和前面的语句切开 → `*>>` 变成一条独立语句，PowerShell
  //      去把它当命令名找，报 "The term '*>>' is not recognized"。重定向是
  //      **语句级操作符**，必须紧跟被重定向的语句。
  //   2. 把命令写成语句序列、`*>>` 贴在最后一句 → 只有最后一句的输出被重定向，
  //      命令自己的输出直接漏到 stdout。
  //
  // 正解就是 PowerShell 自己的写法：`& { <语句序列> } *>> <file>`。
  // 外层大括号在这里是**调用一个脚本块**，不是块字面量——不会触发前两个坑。
  // `*>>` 覆盖所有流（stdout/stderr/verbose…），比 `>> ... 2>&1` 更贴合流模型。
  //
  // 退出码：原生命令取 $LASTEXITCODE，纯 cmdlet 用 $? 落到 0/1。
  //
  // 源码里刻意让每条内容字面量都短于 120 列：超过就会触发 astyle 的
  // `--max-code-length` 在 `+` 处折行，`*>>` 跟着换行，拼出来的脚本虽然相邻，
  // 但阅读和 diff 都更难核对（本文件已经被这样折过一次）。
  QString body = normalizePowerShellBlock(command);
  // 剥壳后可能留下结尾分号（`{ cmd; }` → `cmd;`），和后面拼的 `;` 撞成 `;;`。
  // PowerShell 容忍空语句，但那是拼装没收敛，顺手收干净。
  while(body.endsWith(QLatin1Char(';'))) {
    body.chop(1);
  }
  return QStringLiteral("& { $ErrorActionPreference='Continue'; ") + body +
         QStringLiteral("; $c = if ($LASTEXITCODE -ne $null) { $LASTEXITCODE } "
                        "elseif ($?) { 0 } else { 1 }; "
                        "$c | Out-File -FilePath '") + QString(exitPath) +
         QStringLiteral("' -Encoding ascii -NoNewline } *>> '") + outputPath +
         QStringLiteral("'");
#else
  Q_UNUSED(outputPath)
  Q_UNUSED(exitPath)
  return command;
#endif
}

void configureProcessGroup(QProcess * process)
{
  if(process == nullptr) {
    return;
  }
#ifdef Q_OS_UNIX
  process->setChildProcessModifier([]() {
    ::setsid();
  });
#else
  // Windows 没有会话组概念；终止时由 killProcessGroup 走 taskkill /T 兜底。
  Q_UNUSED(process)
#endif
}

void killProcessGroup(QProcess * process)
{
  if(process == nullptr) {
    return;
  }
  const qint64 pid = process->processId();
#ifdef Q_OS_UNIX
  if(pid > 0) {
    ::kill(-static_cast<pid_t>(pid), SIGKILL);
  }
#else
  // Windows 的等价物是 taskkill /T：连同子进程一起终止。
  // 只调 process->kill() 会漏掉命令自己 fork 出来的孙进程（`make -j`、`npm run`
  // 这类最常见），而 configureProcessGroup 的注释一直承诺这里走 /T——
  // 之前没实现，承诺是空的。
  if(pid > 0 && process->state() != QProcess::NotRunning) {
    QProcess taskkill;
    taskkill.start(QStringLiteral("taskkill"), {
      QStringLiteral("/PID"), QString::number(pid),
      QStringLiteral("/T"), QStringLiteral("/F")
    });
    // 有界等待：taskkill 正常毫秒级返回。取消/超时路径上不能无限等——
    // 这个函数被调用的场合本身就是"必须立刻释放资源"。
    taskkill.waitForFinished(5000);
  }
#endif
  // 兜底：进程组信号失败（或非 Unix）时至少杀掉直接子进程。
  if(process->state() != QProcess::NotRunning) {
    process->kill();
  }
}

}  // namespace toolutil

}  // namespace lycode

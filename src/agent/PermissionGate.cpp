#include "agent/PermissionGate.h"

#include "core/Ids.h"

#include <QLoggingCategory>

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.permission")

/// 规则主体的归一：去掉首尾空白。
/// 不做路径归一或大小写折叠——规则匹配必须是可预测的字面前缀比较，
/// 否则"看见的规则"和"实际匹配的规则"会不一致。
QString normalizeSubject(const QString &subject) {
    return subject.trimmed();
}

/// 剥掉规则内容里的"任意后缀"通配，得到真正用于前缀比较的部分。
///
/// 支持两种写法（与 npm 版的规则语法一致）：
///   `npm run test:*` —— `:*` 表示"后面可以跟任意参数"，前缀是 `npm run test`
///   `npm*`           —— 裸 `*` 同样作为通配后缀
/// 只剥一层：不能把 `test` 也吃掉，否则规则会匹配过宽。
QString stripTrailingWildcard(const QString &ruleContent) {
    if (ruleContent.endsWith(QStringLiteral(":*"))) {
        return ruleContent.left(ruleContent.size() - 2);
    }
    if (ruleContent.endsWith(QLatin1Char('*'))) {
        return ruleContent.left(ruleContent.size() - 1);
    }
    return ruleContent;
}

/// 单条规则是否匹配本次调用。
bool ruleMatches(const PermissionRule &rule, const QString &capability, const QString &subject) {
    if (rule.toolName != capability) {
        return false;
    }
    const QString content = rule.ruleContent.trimmed();
    // 空规则内容 = 该能力的全部调用。
    if (content.isEmpty() || content == QStringLiteral("*")) {
        return true;
    }
    const QString prefix = stripTrailingWildcard(content);
    return subject.startsWith(prefix);
}

/// 在规则列表中找第一条指定行为的匹配规则。
const PermissionRule *findRule(const QList<PermissionRule> &rules, const QString &capability,
                               const QString &subject, PermissionRuleBehavior behavior) {
    for (const PermissionRule &rule : rules) {
        if (rule.behavior == behavior && ruleMatches(rule, capability, subject)) {
            return &rule;
        }
    }
    return nullptr;
}

}  // namespace

PermissionGate::PermissionGate(QObject *parent) : QObject(parent) {}

PermissionGate::~PermissionGate() = default;

void PermissionGate::setMode(PermissionMode mode) {
    if (mode_ == mode) {
        return;
    }
    qCInfo(log) << "权限模式变更:" << toToken(mode_) << "->" << toToken(mode);
    mode_ = mode;
}

void PermissionGate::reset() {
    qCDebug(log) << "清空会话权限规则，数量:" << rules_.size();
    rules_.clear();
}

void PermissionGate::addRule(const PermissionRule &rule) {
    // 用 key() 去重（含行为），避免 allow 与 deny 互相覆盖。
    const QString key = rule.key();
    for (const PermissionRule &existing : rules_) {
        if (existing.key() == key) {
            return;
        }
    }
    qCInfo(log) << "新增权限规则:" << rule.toolName << rule.ruleContent << toToken(rule.behavior);
    rules_.append(rule);
}

const PermissionRule *PermissionGate::matchRule(const QList<PermissionRule> &rules,
                                               const QString &capability,
                                               const QString &subject) {
    const QString normalized = normalizeSubject(subject);
    // allow 优先：用户显式授予的规则比拒绝规则更晚出现时也应生效，
    // 否则"先拒绝全部、再放行一条"这种自然操作顺序会失效。
    if (const PermissionRule *allowed =
            findRule(rules, capability, normalized, PermissionRuleBehavior::Allow)) {
        return allowed;
    }
    return findRule(rules, capability, normalized, PermissionRuleBehavior::Deny);
}

bool PermissionGate::requiresPrompt(PermissionMode mode, PermissionKind kind,
                                    const QList<PermissionRule> &rules, const QString &capability,
                                    const QString &subject) {
    // 1. 显式规则优先于一切默认策略。
    if (matchRule(rules, capability, subject) != nullptr) {
        return false;
    }
    // 2. 其余交给模式策略。
    return permissionModeRequiresPrompt(mode, kind);
}

void PermissionGate::request(const PermissionRequest &request, const QString &capability,
                             const QString &subject, PermissionCallback callback) {
    if (!callback) {
        qCCritical(log) << "权限请求缺少回调，调用将被丢弃; tool=" << request.toolName;
        return;
    }

    const QString normalizedSubject = normalizeSubject(subject);

    // ── 自动裁决链 ─────────────────────────────────────────────────────────
    PermissionOutcome outcome;

    if (const PermissionRule *matched = matchRule(rules_, capability, normalizedSubject)) {
        outcome.automatic = true;
        outcome.reasonCode = matched->behavior == PermissionRuleBehavior::Deny ? QStringLiteral("denied_rule")
                                                                              : QStringLiteral("granted_rule");
        outcome.response.decision = matched->behavior == PermissionRuleBehavior::Deny
                                        ? PermissionDecision::Deny
                                        : PermissionDecision::Allow;
        qCInfo(log) << "权限由已授予规则裁决;" << capability << normalizedSubject
                    << toToken(matched->behavior);
        emit resolved(request.id, outcome);
        callback(outcome);
        return;
    }

    if (mode_ == PermissionMode::BypassPermissions) {
        outcome.automatic = true;
        outcome.reasonCode = QStringLiteral("bypass_mode");
        outcome.response.decision = PermissionDecision::Allow;
        qCDebug(log) << "bypass 模式直接放行;" << capability;
        emit resolved(request.id, outcome);
        callback(outcome);
        return;
    }

    // Plan 模式下带副作用的调用必须被拒绝，而不是询问用户。
    // 这是模式的语义，不是用户的临时决定：让用户"批准"一次写入会破坏 plan 的只读承诺。
    if (mode_ == PermissionMode::Plan && request.kind != PermissionKind::Read) {
        outcome.automatic = true;
        outcome.reasonCode = QStringLiteral("plan_mode");
        outcome.response.decision = PermissionDecision::Deny;
        outcome.response.reason =
            QStringLiteral("当前处于 plan（只读）模式，该工具会产生副作用，已被拒绝。");
        qCInfo(log) << "plan 模式拒绝副作用工具;" << capability;
        emit resolved(request.id, outcome);
        callback(outcome);
        return;
    }

    // 工具显式要求批准时跳过只读/模式直通。顺序放在 plan 之后：
    // plan 模式下带副作用的调用仍然直接拒绝（模式语义优先于工具的申请）。
    if (request.needsApproval) {
        qCDebug(log) << "工具声明需要批准，跳过直通;" << capability;
    } else if (request.kind == PermissionKind::Read) {
        outcome.automatic = true;
        outcome.reasonCode = QStringLiteral("read_only");
        outcome.response.decision = PermissionDecision::Allow;
        emit resolved(request.id, outcome);
        callback(outcome);
        return;
    } else if (!permissionModeRequiresPrompt(mode_, request.kind)) {
        outcome.automatic = true;
        outcome.reasonCode = QStringLiteral("mode_allow");
        outcome.response.decision = PermissionDecision::Allow;
        emit resolved(request.id, outcome);
        callback(outcome);
        return;
    }

    // ── 需要用户裁决 ───────────────────────────────────────────────────────
    if (pending_.contains(request.id)) {
        // 幂等边界：同一个 requestId 重复请求直接拒绝第二次，避免弹两个窗。
        qCWarning(log) << "重复的权限请求，忽略; requestId=" << request.id;
        PermissionOutcome duplicate;
        duplicate.response.decision = PermissionDecision::Deny;
        duplicate.response.reason = QStringLiteral("重复的权限请求");
        duplicate.reasonCode = QStringLiteral("duplicate_request");
        callback(duplicate);
        return;
    }

    Pending pending;
    pending.request = request;
    pending.callback = std::move(callback);
    pending_.insert(request.id, pending);

    qCInfo(log) << "等待用户裁决; requestId=" << request.id << "tool=" << request.toolName
                << "risk=" << toToken(request.riskLevel);

    // 先 emit 再返回：UI 在同一帧内就能建好弹窗，
    // 而调用方此时已经挂起，不存在"回调先于弹窗"的竞态。
    emit requested(request);
}

bool PermissionGate::resolve(const Id &requestId, const PermissionResponse &response) {
    const auto iterator = pending_.find(requestId);
    if (iterator == pending_.end()) {
        // UI 可能因为重绘重复提交，这里保持幂等：不报错、不回调第二次。
        qCDebug(log) << "裁决对应的请求不存在（可能已处理）; requestId=" << requestId;
        return false;
    }

    Pending pending = iterator.value();
    pending_.erase(iterator);

    // AllowAlways 裁决要把规则固化下来，供后续同能力调用直接命中。
    for (const PermissionRule &rule : response.permissionUpdates) {
        addRule(rule);
    }

    PermissionOutcome outcome;
    outcome.response = response;
    outcome.automatic = false;
    outcome.reasonCode = QStringLiteral("user_choice");

    qCInfo(log) << "用户裁决; requestId=" << requestId << toToken(response.decision)
                << "rules=" << response.permissionUpdates.size();

    emit resolved(requestId, outcome);
    pending.callback(outcome);
    return true;
}

void PermissionGate::cancelAll(const QString &reason) {
    if (pending_.isEmpty()) {
        return;
    }

    const QList<Pending> pending = pending_.values();
    pending_.clear();

    qCInfo(log) << "取消全部等待中的权限请求，数量:" << pending.size() << "原因:" << reason;

    // 两阶段收尾：先把全部 resolved 信号发完，再调用回调。
    // 回调可能析构本对象（会话被关闭），所以调用回调之后绝不能再访问成员。
    QList<PermissionOutcome> outcomes;
    outcomes.reserve(pending.size());
    for (const Pending &entry : pending) {
        PermissionOutcome outcome;
        outcome.automatic = true;
        outcome.reasonCode = QStringLiteral("cancelled");
        outcome.response.decision = PermissionDecision::Deny;
        outcome.response.reason = reason;
        outcomes.append(outcome);
        emit resolved(entry.request.id, outcome);
    }

    for (qsizetype index = 0; index < pending.size(); ++index) {
        pending.at(index).callback(outcomes.at(index));
    }
}

}  // namespace zcode

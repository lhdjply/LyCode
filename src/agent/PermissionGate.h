// ZCode Qt — 权限门
//
// 职责边界（重要）：
//   * PermissionGate 只负责「决策」：判断某次工具调用是否需要用户确认，
//     以及在需要时把请求送去 UI 并等待裁决。
//   * PermissionGate 不执行工具，也不修改消息状态 —— 那是 AgentRuntime 的职责。
//
// 这样可以保证"是否有权限"只有一个判断点，不会出现 UI 与服务层各判一次、
// 结果不一致的情况。
//
// 决策顺序（先命中先返回）：
//   1. 会话权限模式为 BypassPermissions        → 放行
//   2. 命中已授予的 allow 规则                  → 放行
//   3. 命中已授予的 deny 规则                   → 拒绝
//   4. 只读类别且模式允许                       → 放行
//   5. Plan 模式且工具有副作用                   → 拒绝（并告知模型原因）
//   6. 其余                                     → 请求用户确认
// 注意第 2/3 步在只读判断之前：显式授予/拒绝总是比默认策略优先。
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

#include "core/Types.h"

namespace zcode {

/// 一次裁决的结果。
struct PermissionOutcome {
    PermissionResponse response;
    /// 是否由策略直接决定（未打扰用户）。
    bool automatic = false;
    /// 策略给出该结果的稳定原因码，便于日志与测试断言。
    /// 取值：bypass_mode | granted_rule | denied_rule | read_only | plan_mode |
    ///       user_choice | cancelled
    QString reasonCode;

    bool allowed() const { return response.allowed(); }
};

using PermissionCallback = std::function<void(PermissionOutcome)>;

class PermissionGate : public QObject {
    Q_OBJECT

public:
    explicit PermissionGate(QObject *parent = nullptr);
    ~PermissionGate() override;

    /// 会话级权限模式。
    PermissionMode mode() const { return mode_; }
    void setMode(PermissionMode mode);

    /// 清空已累积的规则（切换会话时调用）。
    void reset();
    /// 当前已累积的规则。
    QList<PermissionRule> grantedRules() const { return rules_; }
    /// 追加一条规则（去重）。用于应用 AllowAlways 裁决或从会话配置恢复。
    void addRule(const PermissionRule &rule);

    /// 请求裁决。可能同步回调（自动放行/拒绝），也可能异步（等用户点击）。
    /// 回调保证恰好被调用一次，且总是在调用方所在的线程。
    ///
    /// `capability` 是权限能力名（如 read / edit / bash），`subject` 是本次调用的
    /// 规则主体（命令、路径、URL）。二者共同构成规则匹配的输入；
    /// 拆开来传而不是拼成一个字符串，是为了让前缀规则与通配规则能正确匹配。
    void request(const PermissionRequest &request, const QString &capability,
                 const QString &subject, PermissionCallback callback);

    /// 是否有请求正在等待用户。
    bool hasPending() const { return !pending_.isEmpty(); }
    /// 当前等待用户的请求数量。
    int pendingCount() const { return static_cast<int>(pending_.size()); }

    /// 处理一次用户裁决。requestId 不存在时返回 false（幂等边界：
    /// UI 可能因为重绘而重复提交同一个裁决）。
    bool resolve(const Id &requestId, const PermissionResponse &response);

    /// 取消所有等待中的请求，统一按拒绝回调。
    /// 用于中断运行时收尾，避免回调悬挂造成状态机卡死。
    void cancelAll(const QString &reason);

    /// 判定某次调用是否需要询问用户（纯函数，便于测试）。
    /// 命中 allow 或 deny 规则时返回 false——策略已能直接给出结论，不必打扰用户。
    static bool requiresPrompt(PermissionMode mode, PermissionKind kind,
                               const QList<PermissionRule> &rules, const QString &capability,
                               const QString &subject);

    /// 在已授予规则中查找匹配项；未命中返回 nullptr。
    /// 匹配规则：`toolName` 必须等于 capability；`ruleContent` 为空表示匹配该工具全部调用；
    /// 否则要求 subject 以 ruleContent 为前缀（支持 `prefix:*` 写法）。
    static const PermissionRule *matchRule(const QList<PermissionRule> &rules,
                                           const QString &capability, const QString &subject);

signals:
    /// 需要用户裁决时发出；UI 连接它来弹窗。
    void requested(const zcode::PermissionRequest &request);
    /// 裁决完成后发出，供 UI 收起弹窗（无论自动还是用户点击）。
    void resolved(const zcode::Id &requestId, const zcode::PermissionOutcome &outcome);

private:
    struct Pending {
        PermissionRequest request;
        PermissionCallback callback;
    };

    PermissionMode mode_ = PermissionMode::Default;
    QList<PermissionRule> rules_;
    QHash<Id, Pending> pending_;
};

}  // namespace zcode

Q_DECLARE_METATYPE(zcode::PermissionOutcome)

// ZCode Qt — 子 Agent 宿主接口
//
// 依赖方向：`agent/` 依赖 `tools/`，反过来不行。所以 Agent 工具不直接持有
// AgentRuntime，而是通过这个接口向宿主申请派生子代理。由 AgentRuntime 实现它。
//
// 这样 AgentRuntime 才知道怎么构造子运行时、怎么共享依赖、怎么把子会话的进展
// 回报给 UI；工具只负责参数校验与结果格式化。
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

#include "core/Types.h"

namespace zcode {

/// 子代理 profile：决定子代理的工具面与职责说明。
///
/// `identityNote` 是提示词文本，由 AgentRuntime 组装子代理系统提示词时消费；
/// 放在这里是为了让"profile → 工具面 + 提示词"是同一张表，不会两处分叉。
struct SubagentProfile {
    QString id;
    QString displayName;
    /// 附加到子代理系统提示词的职责说明。
    QString identityNote;
    /// 只读 profile：不得产生任何副作用。
    bool readOnly = false;
    /// 工具白名单。空表示继承父代理的完整工具面。
    QStringList toolAllowlist;
};

/// 全部 profile。顺序即 UI/文档的展示顺序。
const QList<SubagentProfile> &subagentProfiles();

/// 按 id 查找；未知 id 返回 nullptr（调用方应据此拒绝该次调用）。
const SubagentProfile *findSubagentProfile(const QString &id);

/// 默认 profile id（与 npm 一致）。
QString defaultSubagentProfileId();

/// 子代理宿主。由 AgentRuntime 实现并注入 ToolContext。
class SubagentHost {
public:
    virtual ~SubagentHost();

    /// 派生请求。
    struct LaunchRequest {
        QString description;
        QString prompt;
        QString subagentType;
        /// profile 要求只读时，子运行时会拒绝一切副作用工具。
        bool readOnlyProfile = false;
        /// profile 的工具白名单；空表示继承。
        QStringList toolAllowlist;
        /// 触发本次派生的工具调用 id（用于把子会话关联回父消息）。
        QString parentToolCallId;
        Workspace workspace;
        /// 子代理使用的模型。留空表示沿用父代理当前的模型快照。
        ModelSelection model;
    };

    /// 派生结果。
    struct LaunchResult {
        bool ok = false;
        QString error;
        QString errorCode;
        QString childSessionId;
        /// 子代理最后一条消息的正文，作为回传给父代理的内容。
        QString output;
        int toolUseCount = 0;
        int totalTokens = 0;
        qint64 durationMs = 0;
        /// 子代理是否因为触及步数上限而提前结束。
        bool truncated = false;
    };

    /// 完成回调。**必须**恰好被调用一次。
    using Completion = std::function<void(LaunchResult)>;

    /// 派生子代理。实现必须异步（子代理要跑完整的 turn 循环）。
    virtual void launchSubagent(const LaunchRequest &request, Completion done) = 0;

    /// 当前运行时是否允许再派生子代理。
    ///
    /// 本实现返回 false 表示"当前已是子代理"：npm 也把子代理的
    /// `subagents.enabled` 置为 false 来禁止递归派生，否则一个任务可能
    /// 无限自我复制下去。
    virtual bool subagentsEnabled() const = 0;
};

}  // namespace zcode

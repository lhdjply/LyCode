#include "tools/AgentTool.h"

#include <QJsonArray>
#include <QLoggingCategory>

#include "tools/SubagentHost.h"

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.tool.agent")

constexpr char kDefaultProfileId[] = "general-purpose";

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 子代理 profile
// ─────────────────────────────────────────────────────────────────────────────

const QList<SubagentProfile> &subagentProfiles() {
    // 与 npm 一致的两个内建 profile：
    //   general-purpose —— 继承父代理的完整工具面与权限模式，能改代码
    //   Explore         —— 只读，工具面收窄到"读/搜/查"，适合调研而不动手
    //
    // 只读是通过**工具白名单**实现的，而不是把子会话切到 plan 模式：
    // plan 模式会改写系统提示词（要求产出计划），而 Explore 需要的是
    // "能把结论讲清楚"，不是"给一份计划"。
    static const QList<SubagentProfile> profiles = {
        {QStringLiteral("general-purpose"), QStringLiteral("通用"),
         QStringLiteral("You are a general-purpose subagent. You can read and edit files and "
                        "run commands. Work autonomously on the task you were given; do not ask "
                        "the user questions. Finish by reporting what you found or changed."),
         false, {}},
        {QStringLiteral("Explore"), QStringLiteral("只读调研"),
         QStringLiteral("You are a read-only exploration subagent. You must not modify files or "
                        "run commands that change state — your tool set does not allow it. "
                        "Investigate the codebase and report findings with concrete file paths "
                        "and line references."),
         true,
         {QStringLiteral("Read"), QStringLiteral("Glob"), QStringLiteral("Grep"),
          QStringLiteral("TodoRead")}},
    };
    return profiles;
}

const SubagentProfile *findSubagentProfile(const QString &id) {
    const QString trimmed = id.trimmed();
    if (trimmed.isEmpty()) {
        return nullptr;
    }
    for (const SubagentProfile &profile : subagentProfiles()) {
        if (profile.id == trimmed) {
            return &profile;
        }
    }
    return nullptr;
}

QString defaultSubagentProfileId() {
    return QString::fromLatin1(kDefaultProfileId);
}

SubagentHost::~SubagentHost() = default;

// ─────────────────────────────────────────────────────────────────────────────
// AgentTool
// ─────────────────────────────────────────────────────────────────────────────

ToolMetadata AgentTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("Agent");
    meta.description =
        QStringLiteral("Launch a subagent to work on a self-contained task. The subagent has its "
                       "own context and returns only its final report, so use it to keep long "
                       "investigations or multi-file changes out of this conversation.");
    meta.modelInstructions =
        QStringLiteral("Give the subagent a complete, standalone prompt — it cannot see this "
                       "conversation. Set subagent_type to \"Explore\" for read-only "
                       "investigation or leave it as \"general-purpose\" when the subagent must "
                       "change code. Independent subagents can be launched in the same message "
                       "so they run in parallel.");

    // 子代理会改文件、跑命令，所以不是只读、也不是破坏性（它自己受权限约束）。
    meta.readOnly = false;
    meta.destructive = false;
    // 多个 Agent 调用并行发出是 npm 明确鼓励的用法，因此显式声明可并发。
    meta.concurrency = ToolMetadata::Concurrency::Safe;
    // 作用域是会话：它不直接碰工作区，真正碰工作区的是子代理内部的工具，
    // 那些工具会各自走权限链。
    meta.sideEffectScope = SideEffectScope::Session;
    meta.riskLevel = RiskLevel::Low;
    // 不额外询问：派生本身不需要批准，子代理内部的副作用工具才是需要批准的。
    meta.needsApproval = false;
    // 子代理可能要跑很久，不设短超时。
    meta.timeoutMs = 0;
    meta.maxOutputBytes = 256 * 1024;
    meta.stopTurnOnSuccess = false;
    return meta;
}

QJsonObject AgentTool::inputSchema() const {
    QJsonObject properties;

    properties.insert(QStringLiteral("description"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("A short (3-5 word) description of the task.")}});

    properties.insert(QStringLiteral("prompt"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("The complete, self-contained task for the "
                                                  "subagent. It cannot see this conversation.")}});

    QJsonArray typeNames;
    QStringList typeDescriptions;
    for (const SubagentProfile &profile : subagentProfiles()) {
        typeNames.append(profile.id);
        typeDescriptions.append(QStringLiteral("%1 = %2").arg(profile.id, profile.displayName));
    }
    properties.insert(
        QStringLiteral("subagent_type"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                    {QStringLiteral("enum"), typeNames},
                    {QStringLiteral("description"),
                     QStringLiteral("Which subagent profile to use. %1. Defaults to %2.")
                         .arg(typeDescriptions.join(QStringLiteral("; ")),
                              defaultSubagentProfileId())}});

    properties.insert(
        QStringLiteral("run_in_background"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                    {QStringLiteral("description"),
                     QStringLiteral("Run the subagent in the background. Not supported yet.")}});

    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), properties);
    schema.insert(QStringLiteral("required"),
                  QJsonArray{QStringLiteral("description"), QStringLiteral("prompt")});
    return schema;
}

QString AgentTool::title(const QJsonObject &input) const {
    const QString description = stringArg(input, QStringLiteral("description")).trimmed();
    const QString type = stringArg(input, QStringLiteral("subagent_type"),
                                  defaultSubagentProfileId());
    return description.isEmpty() ? QStringLiteral("Agent (%1)").arg(type)
                                 : QStringLiteral("Agent[%1]: %2").arg(type, description);
}

QString AgentTool::permissionDescription(const QJsonObject &input) const {
    Q_UNUSED(input)
    return QStringLiteral("派生子代理。子代理拥有独立的上下文，"
                          "其内部的工具调用会各自向你请求确认。");
}

QString AgentTool::formatResultForModel(const SubagentHost::LaunchResult &result) {
    QString output = result.output.trimmed();
    if (output.isEmpty()) {
        output = QStringLiteral("(子代理没有产出任何内容)");
    }

    QStringList lines;
    lines << output;
    lines << QString();
    // 用量行与 npm 的 <usage> 标签对齐，便于模型判断"这次派生值不值"。
    lines << QStringLiteral("<usage>subagent_tokens: %1 tool_uses: %2 duration_ms: %3</usage>")
                 .arg(result.totalTokens)
                 .arg(result.toolUseCount)
                 .arg(result.durationMs);
    if (result.truncated) {
        lines << QStringLiteral("<warning>子代理触及步数上限被提前结束，"
                                "其结论可能不完整。</warning>");
    }
    return lines.join(QLatin1Char('\n'));
}

void AgentTool::execute(const QJsonObject &input, const ToolContext &context, ToolCallback done) {
    if (done == nullptr) {
        qCCritical(log) << "Agent 工具缺少回调，调用将被丢弃";
        return;
    }

    // 禁止递归派生：否则一个任务可以无限自我复制。
    if (context.subagentHost == nullptr || !context.subagentHost->subagentsEnabled()) {
        qCWarning(log) << "拒绝派生：当前运行时不允许子代理";
        done(ToolResult::failure(QStringLiteral("子代理不能再派生子代理。请自己完成该任务。"),
                                 QStringLiteral("subagent_disabled")));
        return;
    }

    const QString description = stringArg(input, QStringLiteral("description")).trimmed();
    const QString prompt = stringArg(input, QStringLiteral("prompt")).trimmed();
    if (description.isEmpty()) {
        done(ToolResult::failure(QStringLiteral("description 不能为空。"),
                                 QStringLiteral("invalid_input")));
        return;
    }
    if (prompt.isEmpty()) {
        done(ToolResult::failure(QStringLiteral("prompt 不能为空。"),
                                 QStringLiteral("invalid_input")));
        return;
    }

    // 本阶段不实现后台子代理。明确失败，不假装成功——
    // 假装成功会让模型以为任务已经在跑，进而去读一个不存在的输出文件。
    if (boolArg(input, QStringLiteral("run_in_background"), false)) {
        done(ToolResult::failure(
            QStringLiteral("后台子代理尚未实现（run_in_background 暂不支持），"
                           "请改为前台运行。"),
            QStringLiteral("unsupported")));
        return;
    }

    const QString typeId = stringArg(input, QStringLiteral("subagent_type"),
                                     defaultSubagentProfileId());
    const SubagentProfile *profile = findSubagentProfile(typeId);
    if (profile == nullptr) {
        QStringList known;
        for (const SubagentProfile &candidate : subagentProfiles()) {
            known.append(candidate.id);
        }
        done(ToolResult::failure(
            QStringLiteral("未知的 subagent_type：%1（可用：%2）").arg(typeId, known.join(QStringLiteral(", "))),
            QStringLiteral("invalid_input")));
        return;
    }

    qCInfo(log) << "派生子代理; type=" << profile->id << "description=" << description
                << "readOnly=" << profile->readOnly;

    SubagentHost::LaunchRequest request;
    request.description = description;
    request.prompt = prompt;
    request.subagentType = profile->id;
    request.readOnlyProfile = profile->readOnly;
    request.toolAllowlist = profile->toolAllowlist;
    request.parentToolCallId = context.callId;
    request.workspace = context.workspace;
    // request.model 留空：由宿主沿用父代理当前的模型快照。

    const QString profileId = profile->id;
    context.subagentHost->launchSubagent(
        request, [done, profileId, description](SubagentHost::LaunchResult result) {
            if (!result.ok) {
                qCWarning(log) << "子代理失败:" << result.error;
                ToolResult failure = ToolResult::failure(
                    QStringLiteral("子代理执行失败：%1").arg(result.error), result.errorCode);
                failure.metadata.insert(QStringLiteral("subagentType"), profileId);
                done(failure);
                return;
            }

            ToolResult ok = ToolResult::success(formatResultForModel(result));
            // 结构化字段给 UI 与日志用；文本给模型用。
            ok.metadata.insert(QStringLiteral("subagentType"), profileId);
            ok.metadata.insert(QStringLiteral("childSessionId"), result.childSessionId);
            ok.metadata.insert(QStringLiteral("agentDescription"), description);
            ok.metadata.insert(QStringLiteral("toolUseCount"), result.toolUseCount);
            ok.metadata.insert(QStringLiteral("totalTokens"), result.totalTokens);
            ok.metadata.insert(QStringLiteral("durationMs"), result.durationMs);
            ok.metadata.insert(QStringLiteral("truncated"), result.truncated);
            done(ok);
        });
}

}  // namespace zcode

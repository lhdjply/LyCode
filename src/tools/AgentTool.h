// LyCode — 子 Agent 工具（`Agent`，别名 `Task`）
//
// 让主代理把一段子任务交给一个**独立的子会话**去做：子代理有自己的消息历史、
// 自己的系统提示词、自己的 todo 列表，只把最后一条消息回传给父代理。
//
// 上下文隔离是这里的全部价值：子代理跑十几轮工具调用产生的中间过程
// 不会污染父代理的上下文，父代理只看到一段结论 + 一行用量摘要。
//
// 与 本实现的对应关系见 tools/SubagentHost.h 与 AgentRuntime::launchSubagent()。
#pragma once

#include <QJsonObject>

#include "tools/SubagentHost.h"
#include "tools/Tool.h"

namespace lycode
{

class AgentTool : public Tool
{
  public:
    AgentTool() = default;
    ~AgentTool() override = default;

    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString permissionDescription(const QJsonObject & input) const override;
    QString title(const QJsonObject & input) const override;

    void execute(const QJsonObject & input, const ToolContext & context,
                 ToolCallback done) override;

    /// 把子代理结果渲染成回传给模型的内容。
    ///
    /// 格式formatAgentOutputForModel 对齐：先正文，再一行用量摘要。
    /// 本实现还会附一句"用 SendMessage 继续这个 agent"，但本实现没有 SendMessage
    /// 工具，所以不写那句——不提示一个不存在的动作。
    static QString formatResultForModel(const SubagentHost::LaunchResult & result);
};

}  // namespace lycode

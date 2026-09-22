// ZCode Qt — 后台任务查询与终止工具
//
// 与 Bash(run_in_background: true) 配套：
//   TaskOutput —— 读一个后台任务的当前状态与输出（可选阻塞等待）
//   TaskStop   —— 终止一个后台任务
//
// 别名与 npm 对齐：TaskOutput 也叫 BashOutput，TaskStop 也叫 KillBash，
// 这样模型按 Claude Code 的习惯调用也能命中。
#pragma once

#include <QJsonObject>

#include "tools/Tool.h"

namespace zcode {

/// 读取后台任务的输出。
///
/// 存在两个工具而不是让 Bash 自己返回，是为了让"启动"与"查看"解耦：
/// 模型可以先启动一个长任务、去做别的事，稍后再回来读结果。
class TaskOutputTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString title(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

/// 终止后台任务。
class TaskStopTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString title(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

}  // namespace zcode

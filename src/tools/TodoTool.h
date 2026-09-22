// ZCode Qt — Todo 工具（TodoRead / TodoWrite）
//
// 两个类放在同一对文件里：它们共享同一份字段校验与格式化逻辑，
// 分开反而要复制粘贴一遍（字段名必须逐字一致，是最容易漂移的地方）。
#pragma once

#include "tools/Tool.h"

namespace zcode {

/// 读取当前会话的 todo 列表。
class TodoReadTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString permissionCapability() const override;
    QList<PermissionRule> permissionRules(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

/// 整表替换当前会话的 todo 列表。
class TodoWriteTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString permissionCapability() const override;
    QList<PermissionRule> permissionRules(const QJsonObject &input) const override;
    QString validateInput(const QJsonObject &input) const override;
    QString title(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

}  // namespace zcode

// ZCode Qt — Edit 工具
//
// 精确字符串替换。比 Write 安全：模型不需要重述整个文件，
// 且"找不到/找到多次"能立刻暴露模型的错误假设。
#pragma once

#include "tools/Tool.h"

namespace zcode {

/// 在文件内容中精确替换 `old_string`。生成统一 diff。
class EditTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString permissionCapability() const override;
    QString title(const QJsonObject &input) const override;
    QString validateInput(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

}  // namespace zcode

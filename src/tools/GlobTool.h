// LyCode — Glob 工具
//
// 按 glob 模式查找文件。只读、可并发。
#pragma once

#include "tools/Tool.h"

namespace lycode {

/// 按 glob 模式递归查找文件，返回工作区相对路径列表。
class GlobTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString permissionCapability() const override;
    QString title(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

}  // namespace lycode

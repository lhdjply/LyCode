// LyCode — Write 工具
//
// 整文件写入。存在的文件会覆盖，并返回结构化 diff 供 UI 展示。
#pragma once

#include "tools/Tool.h"

namespace lycode {

/// 写入（覆盖）一个文件。必要时创建父目录。
class WriteTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString permissionCapability() const override;
    QString title(const QJsonObject &input) const override;
    QString validateInput(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

}  // namespace lycode

// ZCode Qt — Read 工具
//
// 语义与 npm 版对齐：把文件按行编号后交给模型，图片返回 base64 附件，
// 二进制文件明确拒绝（而不是把乱码塞进上下文浪费 token）。
#pragma once

#include "tools/Tool.h"

namespace zcode {

/// 读取文本/图片文件。只读工具，不产生任何副作用。
class ReadTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString permissionCapability() const override;
    QString title(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

}  // namespace zcode

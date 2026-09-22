// LyCode — Grep 工具
//
// 优先用系统 ripgrep（快、尊重 .gitignore）；找不到时退回纯 Qt 实现。
// 两个后端产出同一种结果行格式，分页与展示逻辑只有一份。
#pragma once

#include "tools/Tool.h"

namespace lycode {

/// 按正则搜索文件内容。只读、可并发。
class GrepTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString permissionCapability() const override;
    QString title(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

}  // namespace lycode

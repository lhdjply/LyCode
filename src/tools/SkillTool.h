// LyCode — Skill 工具
//
// 模型用名字取出某个 skill 的正文。清单（名字 + 描述）在系统提示词里，
// 正文按需加载——这样几十个 skill 也不会把上下文塞满。
//
// 只读、无副作用：它读的是本地的指令文件，改变的是模型接下来的行为，
// 而不是工作区。因此免确认。
#pragma once

#include <QJsonObject>

#include "tools/Tool.h"

namespace lycode
{

class SkillTool : public Tool
{
  public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString title(const QJsonObject & input) const override;

    void execute(const QJsonObject & input, const ToolContext & context,
                 ToolCallback done) override;
};

}  // namespace lycode

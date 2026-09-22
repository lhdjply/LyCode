#include "tools/SkillTool.h"

#include <QJsonArray>
#include <QLoggingCategory>

#include "skills/SkillLibrary.h"

namespace lycode {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.tool.skill")

}  // namespace

ToolMetadata SkillTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("Skill");
    meta.description =
        QStringLiteral("Load the full instructions of a skill listed in the system prompt.");
    meta.modelInstructions =
        QStringLiteral("Call this with the skill name before starting a task that matches its "
                       "description. Load one skill at a time and follow its instructions.");
    // 读本地指令文件，不碰工作区，也不产生外部副作用。
    meta.readOnly = true;
    meta.concurrency = ToolMetadata::Concurrency::Safe;
    meta.sideEffectScope = SideEffectScope::None;
    meta.riskLevel = RiskLevel::Low;
    meta.needsApproval = false;
    meta.maxOutputBytes = 128 * 1024;
    return meta;
}

QJsonObject SkillTool::inputSchema() const {
    QJsonObject properties;
    properties.insert(
        QStringLiteral("name"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                    {QStringLiteral("description"),
                     QStringLiteral("The skill name exactly as listed in the system prompt.")}});

    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), properties);
    schema.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("name")});
    return schema;
}

QString SkillTool::title(const QJsonObject &input) const {
    return QStringLiteral("Skill: ") + stringArg(input, QStringLiteral("name"));
}

void SkillTool::execute(const QJsonObject &input, const ToolContext &context, ToolCallback done) {
    const QString name = stringArg(input, QStringLiteral("name")).trimmed();
    if (name.isEmpty()) {
        done(ToolResult::failure(QStringLiteral("name 不能为空。"),
                                 QStringLiteral("invalid_input")));
        return;
    }
    if (context.skills == nullptr) {
        done(ToolResult::failure(QStringLiteral("当前环境没有配置技能目录。"),
                                 QStringLiteral("skills_unavailable")));
        return;
    }

    const skills::Skill *skill = context.skills->find(name);
    if (skill == nullptr) {
        // 明确列出可用的名字：模型偶尔会记错或猜名字，让它一次就能纠正，
        // 而不是反复试错浪费上下文。
        QStringList available;
        for (const skills::Skill &candidate : context.skills->skills()) {
            available.append(candidate.id);
        }
        done(ToolResult::failure(
            QStringLiteral("找不到技能「%1」。可用：%2")
                .arg(name, available.isEmpty() ? QStringLiteral("(无)")
                                               : available.join(QStringLiteral(", "))),
            QStringLiteral("skill_not_found")));
        return;
    }

    qCInfo(log) << "加载技能;" << skill->id << skill->path;
    ToolResult result = ToolResult::success(skill->body);
    result.metadata.insert(QStringLiteral("skillId"), skill->id);
    result.metadata.insert(QStringLiteral("skillPath"), skill->path);
    done(std::move(result));
}

}  // namespace lycode

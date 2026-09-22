#include "model/ReasoningLevels.h"

#include <QLoggingCategory>

#include <algorithm>

namespace lycode {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.model.reasoning")

/// 标准档位表。
///
/// budgetTokens 的取值参考：Anthropic 的下限是 1024；实际使用中 low 档要能
/// 覆盖"读几个文件再回答"的思考量，high 档要能覆盖跨文件重构的推理量。
/// 这些是经验值，用户可以在设置里改档位列表，但不能改这些 id。
const QList<ReasoningLevel> &levelsTable() {
    static const QList<ReasoningLevel> levels = {
        {QStringLiteral("off"), QStringLiteral("关闭"), 0, QString()},
        {QStringLiteral("low"), QStringLiteral("低"), 2048, QStringLiteral("low")},
        {QStringLiteral("medium"), QStringLiteral("中"), 8192, QStringLiteral("medium")},
        {QStringLiteral("high"), QStringLiteral("高"), 24576, QStringLiteral("high")},
        {QStringLiteral("max"), QStringLiteral("最高"), 49152, QStringLiteral("high")},
    };
    return levels;
}

}  // namespace

const QList<ReasoningLevel> &standardReasoningLevels() {
    return levelsTable();
}

const ReasoningLevel *findReasoningLevel(const QString &id) {
    if (id.isEmpty()) {
        return nullptr;
    }
    for (const ReasoningLevel &level : levelsTable()) {
        if (level.id == id) {
            return &level;
        }
    }
    qCWarning(log) << "未知的思考档位 id，已忽略:" << id;
    return nullptr;
}

QString reasoningLevelLabel(const QString &id) {
    if (id.isEmpty()) {
        return QStringLiteral("关闭");
    }
    if (const ReasoningLevel *level = findReasoningLevel(id)) {
        return level->label;
    }
    // 未知档位（例如配置来自更新版本）原样展示，而不是显示成"关闭"——
    // 后者会让用户以为已关闭思考，实际请求里仍带着那个档位。
    return id;
}

QString formatReasoningLevels(const QStringList &ids) {
    if (ids.isEmpty()) {
        return QStringLiteral("（未配置）");
    }
    QStringList labels;
    labels.reserve(ids.size());
    for (const QString &id : ids) {
        labels.append(reasoningLevelLabel(id));
    }
    return labels.join(QStringLiteral(" / "));
}

int clampReasoningBudget(int budgetTokens, int maxOutputTokens) {
    if (budgetTokens <= 0) {
        return 0;
    }
    constexpr int kAnthropicMinimumBudget = 1024;
    int budget = std::max(budgetTokens, kAnthropicMinimumBudget);

    // Anthropic 要求 budget_tokens < max_tokens。留出 1024 的余量给正文，
    // 否则思考会吃掉全部输出预算，回答直接空掉。
    if (maxOutputTokens > 0) {
        const int ceiling = std::max(kAnthropicMinimumBudget, maxOutputTokens - 1024);
        budget = std::min(budget, ceiling);
        if (budget < kAnthropicMinimumBudget) {
            qCWarning(log) << "maxOutputTokens 太小，无法容纳思考预算，已关闭思考; max="
                           << maxOutputTokens;
            return 0;
        }
    }
    return budget;
}

QString normalizeReasoningLevel(const QString &requested, const QStringList &supported) {
    if (supported.isEmpty()) {
        return {};
    }

    // 请求合法且被支持 → 直接用。
    if (!requested.isEmpty() && supported.contains(requested) &&
        findReasoningLevel(requested) != nullptr) {
        return requested;
    }

    // 否则回退到第一个"非 off"的档位；没有就退到第一个。
    for (const QString &id : supported) {
        if (id != QLatin1String("off")) {
            return id;
        }
    }
    return supported.first();
}

bool modelSupportsReasoning(const ModelInfo &info) {
    if (!info.reasoningLevels.isEmpty()) {
        // 列表里只有 off 等价于"能显式关闭，但没有可用的思考强度"。
        for (const QString &id : info.reasoningLevels) {
            if (id != QLatin1String("off")) {
                return true;
            }
        }
        return false;
    }
    return info.supportsReasoning;
}

QStringList defaultReasoningLevelIds() {
    return {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"),
            QStringLiteral("high")};
}

}  // namespace lycode

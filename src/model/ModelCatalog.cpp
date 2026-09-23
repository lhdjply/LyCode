#include "model/ModelCatalog.h"

#include "core/DataPaths.h"
#include "core/Logging.h"

#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>

namespace lycode::model {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.model.catalog")

QStringList splitList(const QString &value) {
    QStringList result;
    for (const QString &item : value.split(QLatin1Char(','))) {
        const QString trimmed = item.trimmed();
        if (!trimmed.isEmpty()) {
            result.append(trimmed);
        }
    }
    return result;
}

/// 块头：`1.DeepSeek` / `2. Qwen`。返回提供商名；不是块头时返回空。
QString providerHeaderName(const QString &line) {
    static const QRegularExpression pattern(QStringLiteral("^\\s*\\d+\\s*[.、]\\s*(.+?)\\s*$"));
    const QRegularExpressionMatch match = pattern.match(line);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

}  // namespace

QString ModelCatalog::bundledResourcePath() {
    return QStringLiteral(":/model_list.txt");
}

QString ModelCatalog::overridePath() {
    return dataDir() + QStringLiteral("/model_list.txt");
}

QList<CatalogProvider> ModelCatalog::load(QString *errorOut) {
    // 磁盘上的覆盖文件优先：模型清单和接口地址会变，让用户不必为了改一行而重新编译。
    const QString override = overridePath();
    if (QFile::exists(override)) {
        QFile file(override);
        if (file.open(QIODevice::ReadOnly)) {
            qCInfo(log) << "使用用户模型目录:" << override;
            return parse(QString::fromUtf8(file.readAll()), errorOut);
        }
        qCWarning(log) << "用户模型目录无法读取，回退到内置目录:" << override
                       << file.errorString();
    }

    QFile bundled(bundledResourcePath());
    if (!bundled.open(QIODevice::ReadOnly)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("内置模型目录缺失（%1）。").arg(bundledResourcePath());
        }
        return {};
    }
    return parse(QString::fromUtf8(bundled.readAll()), errorOut);
}

QList<CatalogProvider> ModelCatalog::parse(const QString &text, QString *errorOut) {
    QList<CatalogProvider> providers;
    CatalogProvider current;
    bool hasCurrent = false;
    /// 块内暂存：这两个字段可能出现在 model 行**之前**，所以在块结束时
    /// 统一套用，不依赖文件里的字段顺序。
    QStringList pendingWindows;
    QStringList pendingThinking;

    const auto flush = [&]() {
        if (!hasCurrent) {
            return;
        }
        for (int index = 0; index < current.models.size(); ++index) {
            CatalogModel &model = current.models[index];
            // thinking 是**提供商级**的：整块的模型共用同一组档位。
            model.reasoningLevels = pendingThinking;
            if (index < pendingWindows.size()) {
                bool ok = false;
                const int value = pendingWindows.at(index).toInt(&ok);
                // 解析不出来就当"未指定"，而不是当成 0 个 token。
                model.contextWindow = ok ? value : 0;
            }
        }
        if (!current.models.isEmpty()) {
            providers.append(current);
        } else {
            qCWarning(log) << "模型目录里的块没有模型，已跳过:" << current.name;
        }
        current = CatalogProvider{};
        pendingWindows.clear();
        pendingThinking.clear();
        hasCurrent = false;
    };

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }

        if (const QString name = providerHeaderName(line); !name.isEmpty()) {
            flush();  // 上一个块结束
            current = CatalogProvider{};
            current.name = name;
            hasCurrent = true;
            continue;
        }

        if (!hasCurrent) {
            continue;  // 块头之前的内容不属于任何提供商
        }

        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            qCWarning(log) << "模型目录里无法解析的行，已跳过:" << line;
            continue;
        }
        const QString key = line.left(colon).trimmed().toLower();
        const QString value = line.mid(colon + 1).trimmed();

        if (key == QLatin1String("model")) {
            for (const QString &id : splitList(value)) {
                current.models.append(CatalogModel{id, {}, 0});
            }
        } else if (key == QLatin1String("base_url_type")) {
            current.kind = value.compare(QLatin1String("Anthropic"), Qt::CaseInsensitive) == 0
                               ? ProviderKind::Anthropic
                               : ProviderKind::OpenAICompatible;
        } else if (key == QLatin1String("base_url")) {
            current.baseUrl = value;
        } else if (key == QLatin1String("thinking")) {
            pendingThinking = splitList(value);
        } else if (key == QLatin1String("maxcontextwindow")) {
            pendingWindows = splitList(value);
        } else {
            // 未知字段跳过：目录以后加字段时，旧版本仍能读。
            qCDebug(log) << "模型目录里的未知字段，已忽略:" << key;
        }
    }
    flush();

    if (providers.isEmpty() && errorOut != nullptr) {
        *errorOut = QStringLiteral("模型目录里没有解析出任何提供商。");
    }
    qCInfo(log) << "模型目录已载入; 提供商=" << providers.size();
    return providers;
}

ModelOptionOverride ModelCatalog::overrideFor(const CatalogProvider &provider,
                                              const QString &modelId,
                                              const QString &providerId) {
    ModelOptionOverride result;
    for (const CatalogModel &model : provider.models) {
        if (model.id != modelId) {
            continue;
        }
        result.contextWindow = model.contextWindow;
        result.reasoningLevels = model.reasoningLevels;
        if (!result.reasoningLevels.isEmpty()) {
            // 默认档位取第一个：目录里的顺序就是推荐顺序（off 在最前）。
            result.defaultReasoningLevel = result.reasoningLevels.first();
        }
        break;
    }
    Q_UNUSED(providerId)
    return result;
}

}  // namespace lycode::model

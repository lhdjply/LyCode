#include "skills/SkillLibrary.h"

#include "core/DataPaths.h"
#include "core/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>

namespace lycode::skills {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.skills")

/// 把 id 归一成可用作工具入参的形式（小写、空格换连字符）。
QString normalizeId(const QString &raw) {
    QString id = raw.trimmed().toLower();
    id.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("-"));
    return id;
}

}  // namespace

QStringList Library::defaultDirectories(const QString &workspacePath) {
    QStringList directories;
    // 用户级在前、项目级在后。rescan 让后扫到的覆盖先扫到的，
    // 于是项目级 skill 会覆盖同名的用户级 skill——项目级更具体，应当胜出。
    directories.append(dataDir() + QStringLiteral("/skills"));
    if (!workspacePath.isEmpty()) {
        directories.append(QDir(workspacePath).filePath(QStringLiteral(".lycode/skills")));
    }
    return directories;
}

Skill Library::parse(const QString &content, const QString &fallbackName,
                     const QString &path) {
    Skill skill;
    skill.path = path;
    skill.name = fallbackName;
    skill.id = normalizeId(fallbackName);

    QString body = content;

    // frontmatter 必须出现在**第一行**。出现在正文中间就当普通内容，
    // 否则一段以 --- 开头的正文会被误当成元数据，正文静默丢失。
    if (content.startsWith(QStringLiteral("---"))) {
        const qsizetype end = content.indexOf(QStringLiteral("\n---"), 3);
        if (end > 0) {
            const QString front = content.mid(3, end - 3);
            body = content.mid(end + 4);
            for (const QString &line : front.split(QLatin1Char('\n'))) {
                const QString trimmed = line.trimmed();
                if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
                    continue;
                }
                const qsizetype colon = trimmed.indexOf(QLatin1Char(':'));
                if (colon <= 0) {
                    continue;
                }
                const QString key = trimmed.left(colon).trimmed().toLower();
                QString value = trimmed.mid(colon + 1).trimmed();
                // 去掉成对的引号。
                if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) &&
                    value.endsWith(QLatin1Char('"'))) {
                    value = value.mid(1, value.size() - 2);
                }
                if (key == QLatin1String("name")) {
                    skill.name = value;
                    skill.id = normalizeId(value);
                } else if (key == QLatin1String("description")) {
                    skill.description = value;
                }
            }
        }
    }

    skill.body = body.trimmed();
    if (skill.description.isEmpty()) {
        // 没有描述就取正文第一行非空、非标题的内容：清单里总得有点说明，
        // 否则模型只能看到一个名字，无从判断该不该加载。
        for (const QString &line : skill.body.split(QLatin1Char('\n'))) {
            QString candidate = line.trimmed();
            if (candidate.isEmpty() || candidate.startsWith(QLatin1Char('#'))) {
                continue;
            }
            if (candidate.size() > 160) {
                candidate = candidate.left(160) + QStringLiteral("…");
            }
            skill.description = candidate;
            break;
        }
    }
    skill.body = body.trimmed();
    return skill;
}

void Library::rescan(const QStringList &directories) {
    skills_.clear();
    int skipped = 0;

    for (const QString &directoryPath : directories) {
        QDir directory(directoryPath);
        if (!directory.exists()) {
            qCDebug(log) << "技能目录不存在，已跳过:" << directoryPath;
            continue;
        }

        // 收集候选：子目录里的 SKILL.md，以及顶层的 .md 文件。
        QList<QPair<QString, QString>> candidates;  // 路径, 回退名
        const QFileInfoList entries =
            directory.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                    QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (entry.isDir()) {
                QDir sub(entry.absoluteFilePath());
                for (const QString &fileName :
                     {QStringLiteral("SKILL.md"), QStringLiteral("skill.md"),
                      QStringLiteral("SKILL.MD")}) {
                    const QString candidate = sub.filePath(fileName);
                    if (QFileInfo::exists(candidate)) {
                        candidates.append({candidate, entry.fileName()});
                        break;
                    }
                }
            } else if (entry.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0) {
                candidates.append({entry.absoluteFilePath(), entry.completeBaseName()});
            }
        }

        for (const auto &candidate : candidates) {
            QFile file(candidate.first);
            if (!file.open(QIODevice::ReadOnly)) {
                qCWarning(log) << "技能文件无法读取:" << candidate.first;
                ++skipped;
                continue;
            }
            const Skill skill =
                parse(QString::fromUtf8(file.readAll()), candidate.second, candidate.first);
            if (!skill.isValid()) {
                qCWarning(log) << "技能内容为空，已跳过:" << candidate.first;
                ++skipped;
                continue;
            }

            // 同名覆盖：后来者（项目级）胜出，且保持原有位置以稳定清单顺序。
            bool replaced = false;
            for (Skill &existing : skills_) {
                if (existing.id == skill.id) {
                    existing = skill;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                skills_.append(skill);
            }
        }
    }

    qCInfo(log) << "技能扫描完成; 目录数=" << directories.size() << "技能数=" << skills_.size()
                << "跳过=" << skipped;
}

const Skill *Library::find(const QString &id) const {
    const QString normalized = normalizeId(id);
    for (const Skill &skill : skills_) {
        if (skill.id == normalized) {
            return &skill;
        }
    }
    return nullptr;
}

QString Library::promptSection() const {
    if (skills_.isEmpty()) {
        return {};
    }
    QStringList lines;
    lines << QStringLiteral("你可以使用下列技能（skills）。它们是可复用的指令包："
                            "**只有名字和描述在这里**，正文要用 Skill 工具按名字取出来。"
                            "当任务与某个技能描述相符时，先取出它再动手。");
    lines << QString();
    for (const Skill &skill : skills_) {
        lines << QStringLiteral("- %1: %2").arg(skill.id, skill.description);
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace lycode::skills

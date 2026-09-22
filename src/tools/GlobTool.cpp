// ZCode Qt — Glob 工具实现
#include "tools/GlobTool.h"

#include "core/Json.h"
#include "tools/ToolUtils.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.tool.glob")

/// 结果上限。5000 个文件路径已经远超"给模型看"的信息量，
/// 再多只会挤掉真正的上下文；命中上限时明确标 truncated。
constexpr int kMaxFiles = 5000;

/// 扫描条目上限（含目录）。用于防止在超大仓库里无界递归，
/// 与结果上限不同：这是"看过多少"，不是"返回多少"。
constexpr qint64 kMaxScannedEntries = 500'000;

constexpr int kMaxOutputBytes = 1'000'000;

/// 递归收集匹配的文件。
/// 用"手工 DFS + 每层 QDirIterator"而不是 QDirIterator::Subdirectories：
/// 后者没有剪枝能力，无法跳过 node_modules 这类目录，在真实仓库里会慢到不可用。
/// 剪枝是性能关键，所以这里牺牲一点代码简洁性。
struct WalkResult {
    QStringList matches;
    QStringList skippedDirs;
    qint64 scannedEntries = 0;
    bool truncated = false;
    bool scanLimitReached = false;
    bool cancelled = false;
};

void walk(const QString &dir, const QString &baseDir, const QRegularExpression &regex,
          WalkResult *result, const ToolContext &context) {
    if (result->truncated || result->scanLimitReached || result->cancelled) {
        return;
    }
    // 取消检查放在每一层目录的入口：大仓库扫描可能持续数秒，
    // 用户点"停止"后必须尽快退出，而不是等整棵树走完。
    if (context.isCancelled()) {
        result->cancelled = true;
        return;
    }

    QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::NoIteratorFlags);
    QStringList subdirs;
    while (it.hasNext()) {
        const QFileInfo info = it.nextFileInfo();
        ++result->scannedEntries;
        if (result->scannedEntries > kMaxScannedEntries) {
            result->scanLimitReached = true;
            result->truncated = true;
            return;
        }
        if (info.isDir()) {
            // 不跟随符号链接目录：避免 `link -> ..` 造成的无限递归。
            // 真实仓库里符号链接目录很常见，这个保护比"多找到几个文件"重要。
            if (info.isSymLink()) {
                continue;
            }
            const QString name = info.fileName();
            if (toolutil::shouldSkipDirectory(name)) {
                if (!result->skippedDirs.contains(name)) {
                    result->skippedDirs.append(name);
                }
                continue;
            }
            subdirs.append(info.absoluteFilePath());
            continue;
        }
        if (!info.isFile()) {
            continue;
        }
        const QString absolute = info.absoluteFilePath();
        // 匹配用的相对路径始终用 '/' 分隔，保证 glob 语义跨平台一致。
        QString relative = QDir::fromNativeSeparators(absolute);
        const QString normalizedBase = QDir::fromNativeSeparators(baseDir);
        if (relative.startsWith(normalizedBase + QLatin1Char('/'))) {
            relative = relative.mid(normalizedBase.size() + 1);
        }
        if (!toolutil::globMatches(regex, relative)) {
            continue;
        }
        result->matches.append(absolute);
        if (result->matches.size() >= kMaxFiles) {
            result->truncated = true;
            return;
        }
    }

    subdirs.sort();
    for (const QString &sub : subdirs) {
        walk(sub, baseDir, regex, result, context);
        if (result->truncated || result->scanLimitReached || result->cancelled) {
            return;
        }
    }
}

}  // namespace

ToolMetadata GlobTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("Glob");
    meta.description = QStringLiteral(
        "Fast file pattern matching. Supports glob patterns like `**/*.js`; "
        "results are sorted by path.");
    meta.modelInstructions = QStringLiteral(
        "Returns workspace-relative paths, one per line. A pattern without `/` "
        "is matched against basenames at any depth. Heavy directories such as "
        ".git, node_modules and build* are always skipped.");
    meta.allowedInPlanMode = true;
    meta.readOnly = true;
    meta.destructive = false;
    meta.concurrency = ToolMetadata::Concurrency::Safe;
    meta.requiresUserInteraction = false;
    meta.timeoutMs = 30000;
    meta.maxOutputBytes = kMaxOutputBytes;
    meta.sideEffectScope = SideEffectScope::None;
    meta.riskLevel = RiskLevel::Low;
    meta.needsApproval = false;
    meta.alwaysAsk = false;
    meta.providerVisible = true;
    meta.stopTurnOnSuccess = false;
    meta.idempotent = true;
    return meta;
}

QJsonObject GlobTool::inputSchema() const {
    QJsonObject properties;
    properties.insert(QStringLiteral("pattern"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("glob 模式，如 `**/*.cpp`、`src/**/*.h`")}});
    properties.insert(QStringLiteral("path"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("搜索根目录（默认工作目录）")}});
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("pattern")}},
    };
}

QString GlobTool::title(const QJsonObject &input) const {
    const QString pattern = json::str(input, QStringLiteral("pattern"));
    return pattern.isEmpty() ? QStringLiteral("Glob") : QStringLiteral("Glob: ") + pattern;
}

QString GlobTool::permissionCapability() const {
    return QStringLiteral("read");
}

void GlobTool::execute(const QJsonObject &input, const ToolContext &context, ToolCallback done) {
    const ToolMetadata meta = metadata();
    auto finish = [&done](ToolResult result) { done(std::move(result)); };

    const QString rawPattern = json::str(input, QStringLiteral("pattern"));
    const QString rawPath = json::str(input, QStringLiteral("path"));
    qCDebug(log) << "Glob 参数; pattern=" << toolutil::redactForLog(rawPattern)
                 << "path=" << toolutil::redactForLog(rawPath);

    if (context.isCancelled()) {
        finish(ToolResult::failure(QStringLiteral("执行已取消"), QStringLiteral("cancelled")));
        return;
    }
    if (rawPattern.trimmed().isEmpty()) {
        finish(ToolResult::failure(QStringLiteral("pattern 不能为空"),
                                   QStringLiteral("invalid_input")));
        return;
    }

    // 基目录：显式 path > workingDirectory > workspace.path。
    QString base;
    if (!rawPath.isEmpty()) {
        base = context.resolvePath(rawPath);
        if (base.isEmpty()) {
            finish(ToolResult::failure(
                QStringLiteral("path 非法或超出工作区范围：%1").arg(toolutil::redactForLog(rawPath)),
                QStringLiteral("path_outside_workspace")));
            return;
        }
    } else if (!context.workingDirectory.isEmpty()) {
        base = QDir::cleanPath(context.workingDirectory);
    } else {
        base = context.workspace.path;
    }

    const QFileInfo baseInfo(base);
    if (base.isEmpty() || !baseInfo.exists() || !baseInfo.isDir()) {
        finish(ToolResult::failure(QStringLiteral("搜索根目录不存在或不是目录：%1").arg(base),
                                   QStringLiteral("invalid_base_directory")));
        return;
    }

    QString pattern = toolutil::normalizeGlob(rawPattern.trimmed());
    // 绝对 pattern：只有当它落在基目录内时才转成相对模式，否则明确拒绝，
    // 避免"看起来搜到了"其实是搜了工作区外面。
    if (QDir::isAbsolutePath(pattern)) {
        const QString normalizedPattern = QDir::fromNativeSeparators(pattern);
        const QString normalizedBase = QDir::fromNativeSeparators(QDir::cleanPath(base));
        if (!toolutil::pathWithin(normalizedBase, normalizedPattern)) {
            finish(ToolResult::failure(
                QStringLiteral("绝对 glob 模式必须位于搜索根目录内：%1").arg(pattern),
                QStringLiteral("invalid_input")));
            return;
        }
        pattern = normalizedPattern.mid(normalizedBase.size());
        while (pattern.startsWith(QLatin1Char('/'))) {
            pattern.remove(0, 1);
        }
    }
    // 无 '/' 的模式按"任意层级的文件名"处理（`*.txt` == `**/*.txt`）。
    // 这是使用者的直觉预期：只想用 Glob 找文件名时不该强制写 `**/`。
    if (!pattern.contains(QLatin1Char('/'))) {
        pattern = QStringLiteral("**/") + pattern;
    }

    const QRegularExpression regex = toolutil::globToRegex(pattern);
    const qint64 startedMs = nowMs();

    WalkResult walkResult;
    walk(base, base, regex, &walkResult, context);

    if (walkResult.cancelled) {
        finish(ToolResult::failure(QStringLiteral("执行已取消"), QStringLiteral("cancelled"),
                                   QJsonObject{{QStringLiteral("numFiles"),
                                                walkResult.matches.size()}}));
        return;
    }

    std::sort(walkResult.matches.begin(), walkResult.matches.end());

    toolutil::OutputBudget budget(meta.maxOutputBytes);
    QStringList displayPaths;
    displayPaths.reserve(walkResult.matches.size());
    for (const QString &absolute : walkResult.matches) {
        displayPaths.append(context.displayPath(absolute));
    }
    std::sort(displayPaths.begin(), displayPaths.end());
    for (const QString &display : displayPaths) {
        budget.appendLine(display);
    }

    const bool truncated = walkResult.truncated || budget.truncated();
    QString output = toolutil::chompTrailingNewlines(budget.text());
    if (output.isEmpty()) {
        output = QStringLiteral("(no files matched)");
    }

    if (walkResult.scanLimitReached) {
        qCWarning(log) << "Glob 扫描条目达到上限:" << kMaxScannedEntries << "base=" << base;
    }

    QJsonObject resultMeta;
    resultMeta.insert(QStringLiteral("pattern"), pattern);
    resultMeta.insert(QStringLiteral("basePath"), base);
    resultMeta.insert(QStringLiteral("numFiles"), displayPaths.size());
    resultMeta.insert(QStringLiteral("scannedEntries"), static_cast<double>(walkResult.scannedEntries));
    resultMeta.insert(QStringLiteral("truncated"), truncated);
    resultMeta.insert(QStringLiteral("scanLimitReached"), walkResult.scanLimitReached);
    resultMeta.insert(QStringLiteral("maxFiles"), kMaxFiles);
    QJsonArray skipped;
    for (const QString &name : walkResult.skippedDirs) {
        skipped.append(name);
    }
    resultMeta.insert(QStringLiteral("skippedDirectories"), skipped);
    resultMeta.insert(QStringLiteral("skippedNote"),
                      QStringLiteral("跳过了依赖/构建目录（.git、node_modules、build*、.cache 等）"));
    resultMeta.insert(QStringLiteral("durationMs"), static_cast<double>(nowMs() - startedMs));

    qCInfo(log) << "Glob 完成; pattern=" << pattern << "base=" << base
                << "files=" << displayPaths.size() << "scanned=" << walkResult.scannedEntries
                << "truncated=" << truncated << "durationMs=" << (nowMs() - startedMs);

    finish(ToolResult::success(output, resultMeta));
}

}  // namespace zcode

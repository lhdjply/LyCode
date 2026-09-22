// ZCode Qt — Write 工具实现
#include "tools/WriteTool.h"

#include "core/Json.h"
#include "tools/Diff.h"
#include "tools/ToolUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QLoggingCategory>

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.tool.write")

constexpr int kMaxOutputBytes = 1'000'000;

/// 读取已存在文件的内容；文件不存在返回 null 且 *existed = false。
/// 读取失败（权限等）返回 false 表示"无法安全覆盖"。
bool readExisting(const QString &path, QString *contentOut, bool *existedOut, QString *errorOut) {
    const QFileInfo info(path);
    *existedOut = info.exists();
    if (!*existedOut) {
        *contentOut = QString();
        return true;
    }
    if (info.isDir()) {
        *errorOut = QStringLiteral("%1 是目录，不能用 Write 覆盖。").arg(path);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // 为了生成正确的 diff，必须先读到原文；读不到就拒绝覆盖，
        // 否则会把用户的内容无声地替换掉。
        *errorOut = QStringLiteral("无法读取原文件（%1），拒绝覆盖。").arg(file.errorString());
        return false;
    }
    *contentOut = QString::fromUtf8(file.readAll());
    return true;
}

}  // namespace

ToolMetadata WriteTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("Write");
    meta.description = QStringLiteral(
        "Writes a file to the local filesystem, overwriting it if it exists.");
    meta.modelInstructions = QStringLiteral(
        "file_path must be an absolute path. Prefer Edit for small changes to "
        "existing files. Parent directories are created automatically.");
    meta.allowedInPlanMode = false;
    meta.readOnly = false;
    meta.destructive = false;
    meta.concurrentSafe = false;
    meta.requiresUserInteraction = false;
    meta.timeoutMs = 30000;
    meta.maxOutputBytes = kMaxOutputBytes;
    meta.sideEffectScope = SideEffectScope::Workspace;
    meta.riskLevel = RiskLevel::Medium;
    meta.needsApproval = true;
    meta.alwaysAsk = false;
    meta.providerVisible = true;
    meta.stopTurnOnSuccess = false;
    meta.idempotent = false;
    return meta;
}

QJsonObject WriteTool::inputSchema() const {
    QJsonObject properties;
    properties.insert(QStringLiteral("file_path"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("要写入的文件的绝对路径")}});
    properties.insert(QStringLiteral("content"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("写入文件的完整内容")}});
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("file_path"), QStringLiteral("content")}},
    };
}

QString WriteTool::permissionCapability() const {
    // Write 与 Edit 共享 edit 能力：用户授予"允许编辑"时两者都放行，
    // 这也解释了为什么不能按工具名做权限分支。
    return QStringLiteral("edit");
}

QString WriteTool::title(const QJsonObject &input) const {
    const QString path = json::str(input, QStringLiteral("file_path"));
    return path.isEmpty() ? QStringLiteral("Write") : QStringLiteral("Write: ") + path;
}

QString WriteTool::validateInput(const QJsonObject &input) const {
    const QString base = Tool::validateInput(input);
    if (!base.isEmpty()) {
        return base;
    }
    if (json::str(input, QStringLiteral("file_path")).trimmed().isEmpty()) {
        return QStringLiteral("file_path 不能为空");
    }
    // content 允许是空字符串（用于清空文件），但必须是字符串类型。
    const QJsonValue content = input.value(QStringLiteral("content"));
    if (!content.isString()) {
        return QStringLiteral("content 必须是字符串");
    }
    return {};
}

void WriteTool::execute(const QJsonObject &input, const ToolContext &context, ToolCallback done) {
    const ToolMetadata meta = metadata();
    auto finish = [&done](ToolResult result) { done(std::move(result)); };

    const QString rawPath = json::str(input, QStringLiteral("file_path"));
    const QString content = json::str(input, QStringLiteral("content"));
    qCDebug(log) << "Write 参数; file_path=" << toolutil::redactForLog(rawPath)
                 << "contentChars=" << content.size()
                 << "contentPreview=" << toolutil::redactForLog(content, 200);

    if (context.isCancelled()) {
        finish(ToolResult::failure(QStringLiteral("执行已取消"), QStringLiteral("cancelled")));
        return;
    }
    // 只读模式：metadata 的 sideEffectScope 属于写入集，直接拒绝执行。
    if (toolutil::shouldRejectForReadOnly(meta, context)) {
        qCWarning(log) << "只读模式拒绝 Write:" << toolutil::redactForLog(rawPath);
        finish(toolutil::readOnlyModeFailure(meta));
        return;
    }

    const QString path = context.resolvePath(rawPath);
    if (path.isEmpty()) {
        finish(ToolResult::failure(
            QStringLiteral("路径非法或超出工作区范围：%1").arg(toolutil::redactForLog(rawPath)),
            QStringLiteral("path_outside_workspace")));
        return;
    }

    const qint64 startedMs = nowMs();
    QString original;
    bool existed = false;
    QString readError;
    if (!readExisting(path, &original, &existed, &readError)) {
        finish(ToolResult::failure(readError, QStringLiteral("read_failed")));
        return;
    }

    const QByteArray bytes = content.toUtf8();
    // mkpath 是必要的：模型经常写 `<新目录>/file.cpp`，而它并不知道目录不存在。
    const QString parent = QFileInfo(path).absolutePath();
    if (!parent.isEmpty() && !QDir().mkpath(parent)) {
        finish(ToolResult::failure(QStringLiteral("无法创建父目录：%1").arg(parent),
                                   QStringLiteral("mkdir_failed")));
        return;
    }
    QString writeError;
    if (!toolutil::writeFileAtomic(path, bytes, &writeError)) {
        qCCritical(log) << "写入失败; path=" << path << "error=" << writeError;
        finish(ToolResult::failure(QStringLiteral("写入文件失败：%1").arg(writeError),
                                   QStringLiteral("write_failed")));
        return;
    }

    const QJsonArray hunks = Diff::unified(original, content, path);
    int additions = 0;
    int deletions = 0;
    Diff::summary(hunks, &additions, &deletions);

    QJsonObject resultMeta;
    resultMeta.insert(QStringLiteral("type"),
                      existed ? QStringLiteral("update") : QStringLiteral("create"));
    resultMeta.insert(QStringLiteral("filePath"), path);
    resultMeta.insert(QStringLiteral("displayPath"), context.displayPath(path));
    resultMeta.insert(QStringLiteral("structuredPatch"), hunks);
    // 新建文件时原内容为 null（而不是空串）：模型据此区分"文件原本不存在"。
    resultMeta.insert(QStringLiteral("originalFile"),
                      existed ? QJsonValue(original) : QJsonValue(QJsonValue::Null));
    resultMeta.insert(QStringLiteral("bytesWritten"), static_cast<double>(bytes.size()));
    resultMeta.insert(QStringLiteral("additions"), additions);
    resultMeta.insert(QStringLiteral("deletions"), deletions);
    resultMeta.insert(QStringLiteral("durationMs"), static_cast<double>(nowMs() - startedMs));

    const QString output =
        existed ? QStringLiteral("Updated %1 (+%2 -%3)").arg(path).arg(additions).arg(deletions)
                : QStringLiteral("Created %1 (%2 bytes)").arg(path).arg(bytes.size());

    qCInfo(log) << (existed ? "Write 更新完成" : "Write 新建完成") << "; path=" << path
                << "bytes=" << bytes.size() << "additions=" << additions
                << "deletions=" << deletions << "durationMs=" << (nowMs() - startedMs);

    finish(ToolResult::success(output, resultMeta));
}

}  // namespace zcode

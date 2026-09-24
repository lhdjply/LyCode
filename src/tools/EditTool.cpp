// LyCode — Edit 工具实现
#include "tools/EditTool.h"

#include "core/Json.h"
#include "tools/Diff.h"
#include "tools/ToolUtils.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QList>
#include <QCoreApplication>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.tool.edit")

constexpr int kMaxOutputBytes = 1'000'000;

/// 找出 needle 在 haystack 中所有出现位置（不允许重叠）。
/// 用 QString::indexOf 循环而不是正则：old_string 是字面量，
/// 里面可能包含正则元字符，走正则就得先转义，反而多一层出错机会。
QList<int> findAll(const QString & haystack, const QString & needle)
{
  QList<int> positions;
  if(needle.isEmpty()) {
    return positions;
  }
  int from = 0;
  while(from <= haystack.size() - needle.size()) {
    const int index = haystack.indexOf(needle, from);
    if(index < 0) {
      break;
    }
    positions.append(index);
    from = index + needle.size();
  }
  return positions;
}

}  // namespace

ToolMetadata EditTool::metadata() const
{
  ToolMetadata meta;
  meta.name = QStringLiteral("Edit");
  meta.description = QStringLiteral(
                       "Performs exact string replacements in a file. Fails if old_string is not "
                       "found or is ambiguous, unless replace_all is true.");
  meta.modelInstructions = QStringLiteral(
                             "file_path must be an absolute path. old_string must match the file "
                             "content exactly, including indentation. Use replace_all for renaming "
                             "every occurrence.");
  meta.allowedInPlanMode = false;
  meta.readOnly = false;
  meta.destructive = false;
  meta.concurrency = ToolMetadata::Concurrency::Serial;
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

QJsonObject EditTool::inputSchema() const
{
  QJsonObject properties;
  properties.insert(QStringLiteral("file_path"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
    {
      QStringLiteral("description"),
      QStringLiteral("要修改的文件的绝对路径")
    }});
  properties.insert(QStringLiteral("old_string"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
    {
      QStringLiteral("description"),
      QStringLiteral("要被替换的原文（必须与文件内容完全一致）")
    }});
  properties.insert(QStringLiteral("new_string"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
    {
      QStringLiteral("description"),
      QStringLiteral("替换后的新文本（可为空字符串表示删除）")
    }});
  properties.insert(QStringLiteral("replace_all"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
    {
      QStringLiteral("description"),
      QStringLiteral("是否替换全部出现（默认 false）")
    }});
  return QJsonObject{
    {QStringLiteral("type"), QStringLiteral("object")},
    {QStringLiteral("properties"), properties},
    {
      QStringLiteral("required"),
      QJsonArray{
        QStringLiteral("file_path"), QStringLiteral("old_string"),
        QStringLiteral("new_string")}
    },
  };
}

QString EditTool::permissionCapability() const
{
  return QStringLiteral("edit");
}

QString EditTool::title(const QJsonObject & input) const
{
  const QString path = json::str(input, QStringLiteral("file_path"));
  return path.isEmpty() ? QStringLiteral("Edit") : QStringLiteral("Edit: ") + path;
}

QString EditTool::validateInput(const QJsonObject & input) const
{
  const QString base = Tool::validateInput(input);
  if(!base.isEmpty()) {
    return base;
  }
  if(json::str(input, QStringLiteral("file_path")).trimmed().isEmpty()) {
    return QCoreApplication::translate("tools::EditTool", "file_path cannot be empty");
  }
  if(json::str(input, QStringLiteral("old_string")).isEmpty()) {
    // 空 old_string 会在每个位置匹配，必然造成意外的整文件改写。
    return QCoreApplication::translate("tools::EditTool", "old_string cannot be empty");
  }
  if(!input.value(QStringLiteral("new_string")).isString()) {
    return QCoreApplication::translate("tools::EditTool", "new_string must be a string");
  }
  return {};
}

void EditTool::execute(const QJsonObject & input, const ToolContext & context, ToolCallback done)
{
  const ToolMetadata meta = metadata();
  auto finish = [&done](ToolResult result) {
    done(std::move(result));
  };

  const QString rawPath = json::str(input, QStringLiteral("file_path"));
  const QString oldString = json::str(input, QStringLiteral("old_string"));
  const QString newString = json::str(input, QStringLiteral("new_string"));
  const bool replaceAll = json::boolean(input, QStringLiteral("replace_all"), false);

  qCDebug(log) << "Edit 参数; file_path=" << toolutil::redactForLog(rawPath)
               << "old=" << toolutil::redactForLog(oldString, 200)
               << "new=" << toolutil::redactForLog(newString, 200)
               << "replaceAll=" << replaceAll;

  if(context.isCancelled()) {
    finish(ToolResult::failure(QCoreApplication::translate("tools::EditTool", "Execution cancelled"),
                               QStringLiteral("cancelled")));
    return;
  }
  if(toolutil::shouldRejectForReadOnly(meta, context)) {
    qCWarning(log) << "只读模式拒绝 Edit:" << toolutil::redactForLog(rawPath);
    finish(toolutil::readOnlyModeFailure(meta));
    return;
  }
  const QString earlyValidation = validateInput(input);
  if(!earlyValidation.isEmpty()) {
    finish(ToolResult::failure(earlyValidation, QStringLiteral("invalid_input")));
    return;
  }

  const QString path = context.resolvePath(rawPath);
  if(path.isEmpty()) {
    finish(ToolResult::failure(
             QCoreApplication::translate("tools::EditTool",
                                         "Invalid path, or outside the workspace: %1").arg(toolutil::redactForLog(rawPath)),
             QStringLiteral("path_outside_workspace")));
    return;
  }

  const QFileInfo info(path);
  if(!info.exists()) {
    finish(ToolResult::failure(
             QCoreApplication::translate("tools::EditTool",
                                         "File does not exist: %1 (Edit cannot create files; use Write)").arg(path),
             QStringLiteral("file_not_found")));
    return;
  }
  if(!info.isFile()) {
    finish(ToolResult::failure(QCoreApplication::translate("tools::EditTool", "%1 is not a regular file.").arg(path),
                               QStringLiteral("not_a_regular_file")));
    return;
  }
  // 精确替换需要把整个文件读进内存：给出一个明确的规模上限，
  // 超限时指向 Write/Bash，而不是在 Edit 里做流式替换（收益远小于复杂度）。
  constexpr qint64 kMaxEditableBytes = 32 * 1024 * 1024;
  if(info.size() > kMaxEditableBytes) {
    finish(ToolResult::failure(
             QCoreApplication::translate("tools::EditTool",
                                         "The file is too large (%1 bytes; Edit is limited to %2 bytes). Use Bash or Write.")
             .arg(info.size())
             .arg(kMaxEditableBytes),
             QStringLiteral("file_too_large")));
    return;
  }

  const qint64 startedMs = nowMs();
  QFile file(path);
  if(!file.open(QIODevice::ReadOnly)) {
    finish(ToolResult::failure(QCoreApplication::translate("tools::EditTool",
                                                           "Could not read the file: %1").arg(file.errorString()),
                               QStringLiteral("read_failed")));
    return;
  }
  const QByteArray originalBytes = file.readAll();
  file.close();
  const QString original = QString::fromUtf8(originalBytes);

  const QList<int> positions = findAll(original, oldString);
  const int matchCount = static_cast<int>(positions.size());

  if(matchCount == 0) {
    // 最常见的失败：模型记错了缩进或行内容。错误里给出可操作的下一步。
    finish(ToolResult::failure(
             QCoreApplication::translate("tools::EditTool",
                                         "old_string was not found in %1. Use Read to check the exact text first (indentation and whitespace must match exactly).")
             .arg(path),
             QStringLiteral("string_not_found")));
    return;
  }
  if(matchCount > 1 && !replaceAll) {
    // 歧义必须失败：静默改第一个匹配是"看起来成功"的错误行为。
    finish(ToolResult::failure(
             QCoreApplication::translate("tools::EditTool",
                                         "old_string appears %2 times in %1, so it is ambiguous. Include more context to make the match unique, or pass replace_all=true to replace every occurrence.")
             .arg(path)
             .arg(matchCount),
             QStringLiteral("multiple_matches")));
    return;
  }

  QString updated;
  if(replaceAll) {
    updated = original;
    // 从后往前替换，避免前面替换后偏移量失效。
    for(int i = matchCount - 1; i >= 0; --i) {
      updated.replace(positions.at(i), oldString.size(), newString);
    }
  }
  else {
    updated = original;
    updated.replace(positions.first(), oldString.size(), newString);
  }

  QString writeError;
  if(!toolutil::writeFileAtomic(path, updated.toUtf8(), &writeError)) {
    qCCritical(log) << "Edit 写回失败; path=" << path << "error=" << writeError;
    finish(ToolResult::failure(QCoreApplication::translate("tools::EditTool",
                                                           "Failed to write the file back: %1").arg(writeError),
                               QStringLiteral("write_failed")));
    return;
  }

  const QJsonArray hunks = Diff::unified(original, updated, path);
  int additions = 0;
  int deletions = 0;
  Diff::summary(hunks, &additions, &deletions);

  QJsonObject resultMeta;
  resultMeta.insert(QStringLiteral("type"), QStringLiteral("update"));
  resultMeta.insert(QStringLiteral("filePath"), path);
  resultMeta.insert(QStringLiteral("displayPath"), context.displayPath(path));
  resultMeta.insert(QStringLiteral("structuredPatch"), hunks);
  resultMeta.insert(QStringLiteral("originalFile"), original);
  resultMeta.insert(QStringLiteral("bytesWritten"),
                    static_cast<double>(updated.toUtf8().size()));
  resultMeta.insert(QStringLiteral("replaceAll"), replaceAll);
  resultMeta.insert(QStringLiteral("matchCount"), matchCount);
  resultMeta.insert(QStringLiteral("additions"), additions);
  resultMeta.insert(QStringLiteral("deletions"), deletions);
  resultMeta.insert(QStringLiteral("durationMs"), static_cast<double>(nowMs() - startedMs));

  const QString output = QStringLiteral("Updated %1 (+%2 -%3)").arg(path).arg(additions).arg(deletions);

  qCInfo(log) << "Edit 完成; path=" << path << "matches=" << matchCount
              << "replaceAll=" << replaceAll << "additions=" << additions
              << "deletions=" << deletions << "durationMs=" << (nowMs() - startedMs);

  finish(ToolResult::success(output, resultMeta));
}

}  // namespace lycode

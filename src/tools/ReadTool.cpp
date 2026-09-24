// LyCode — Read 工具实现
#include "tools/ReadTool.h"

#include "core/Json.h"
#include "tools/ToolUtils.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QStringList>
#include <algorithm>
#include <QCoreApplication>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.tool.read")

/// 默认最多返回多少行。2000 行足以覆盖绝大多数源码文件，
/// 又不至于一次吃掉整个上下文预算。
constexpr int kDefaultLimit = 2000;
constexpr int kMaxLimit = 1'000'000;

/// 内容读取的硬上限（不受 maxOutputBytes 影响）。
/// offset/limit 要在"大文件里翻页"，如果只读 256KiB 就永远读不到后面，
/// 所以内容先按 4MiB 上限读入，再由 maxOutputBytes 约束真正返回的正文。
constexpr qint64 kHardContentLimit = 4 * 1024 * 1024;

/// 图片读取上限。base64 会膨胀 1/3，10MiB 原图已接近模型侧可接受的边界。
constexpr qint64 kMaxImageBytes = 10 * 1024 * 1024;

constexpr int kMaxOutputBytes = 256 * 1024;

bool isImagePath(const QString & path, QString * mimeOut)
{
  static const QStringList extensions = {
    QStringLiteral("png"), QStringLiteral("jpg"),  QStringLiteral("jpeg"),
    QStringLiteral("gif"), QStringLiteral("webp"),
  };
  const QString suffix = QFileInfo(path).suffix().toLower();
  if(!extensions.contains(suffix)) {
    return false;
  }
  if(mimeOut != nullptr) {
    *mimeOut = toolutil::mimeTypeForPath(path);
  }
  return true;
}

ToolResult readImage(const QString & path, const QFileInfo & info, const QString & mime)
{
  if(info.size() > kMaxImageBytes) {
    return ToolResult::failure(
             QCoreApplication::translate("tools::ReadTool",
                                         "The image is too large (%1 bytes; limit %2 bytes). Use Bash to handle it instead.")
             .arg(info.size())
             .arg(kMaxImageBytes),
             QStringLiteral("file_too_large"));
  }
  QFile file(path);
  if(!file.open(QIODevice::ReadOnly)) {
    return ToolResult::failure(
             QCoreApplication::translate("tools::ReadTool", "Could not open the image file: %1").arg(file.errorString()),
             QStringLiteral("read_failed"));
  }
  const QByteArray bytes = file.readAll();
  if(bytes.size() != info.size() && info.size() > 0) {
    // 大小在读取过程中变了：不致命，但值得记录。
    qCWarning(log) << "图片读取字节数与 stat 不一致:" << bytes.size() << info.size();
  }

  QJsonObject meta;
  meta.insert(QStringLiteral("filePath"), path);
  meta.insert(QStringLiteral("mimeType"), mime);
  // 图片走 ToolResult::images（真正的图片内容块），不再塞进 metadata：
  // 放在 metadata 里模型只会收到一行文字、看不到像素，于是会退回去用
  // Bash+Python 猜图片内容——这正是用户报的问题。
  FilePart image;
  image.path = path;
  image.mimeType = mime;
  image.fileName = QFileInfo(path).fileName();
  image.sizeBytes = bytes.size();
  image.base64 = QString::fromLatin1(bytes.toBase64());
  meta.insert(QStringLiteral("sizeBytes"), static_cast<double>(bytes.size()));
  meta.insert(QStringLiteral("totalLines"), 0);
  meta.insert(QStringLiteral("numLines"), 0);
  meta.insert(QStringLiteral("truncated"), false);

  // 正文里明确告诉模型"图已经附在结果里了"，避免它再去用外部命令确认。
  ToolResult result = ToolResult::success(
                        QCoreApplication::translate("tools::ReadTool",
                                                    "Read image %1 (%2, %3 bytes). The image is attached to this result; just look at it to answer, no external command is needed.")
                        .arg(path, mime)
                        .arg(bytes.size()),
                        meta);
  result.images.append(image);
  return result;
}

}  // namespace

ToolMetadata ReadTool::metadata() const
{
  ToolMetadata meta;
  meta.name = QStringLiteral("Read");
  meta.description = QStringLiteral(
                       "Reads a file from the local filesystem. Returns the file content with "
                       "line numbers, or an image attachment for image files.");
  meta.modelInstructions = QStringLiteral(
                             "file_path must be an absolute path. Use offset/limit to page through "
                             "large files; lines are returned as `<line number>\\t<content>`. "
                             "Binary files are rejected — use Bash instead.");
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

QJsonObject ReadTool::inputSchema() const
{
  QJsonObject properties;
  properties.insert(QStringLiteral("file_path"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
    {
      QStringLiteral("description"),
      QStringLiteral("要读取的文件的绝对路径")
    }});
  properties.insert(QStringLiteral("offset"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
    {
      QStringLiteral("description"),
      QStringLiteral("起始行号（1-based，默认 1）")
    }});
  properties.insert(QStringLiteral("limit"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
    {
      QStringLiteral("description"),
      QStringLiteral("最多返回的行数（默认 2000）")
    }});
  return QJsonObject{
    {QStringLiteral("type"), QStringLiteral("object")},
    {QStringLiteral("properties"), properties},
    {QStringLiteral("required"), QJsonArray{QStringLiteral("file_path")}},
  };
}

QString ReadTool::title(const QJsonObject & input) const
{
  const QString path = json::str(input, QStringLiteral("file_path"));
  if(path.isEmpty()) {
    return QStringLiteral("Read");
  }
  // 标题优先展示工作区相对路径，路径很长时也不至于把 UI 撑爆。
  return QStringLiteral("Read: ") + path;
}

QString ReadTool::permissionCapability() const
{
  // Read/Glob/Grep 共享 read 能力：用户"允许读取"时三者一起放行。
  return QStringLiteral("read");
}

void ReadTool::execute(const QJsonObject & input, const ToolContext & context, ToolCallback done)
{
  const ToolMetadata meta = metadata();

  // 每个返回点都走这个 lambda：保证 done 恰好被调用一次。
  auto finish = [&done](ToolResult result) {
    done(std::move(result));
  };

  const QString rawPath = json::str(input, QStringLiteral("file_path"));
  qCDebug(log) << "Read 参数; file_path=" << toolutil::redactForLog(rawPath)
               << "offset=" << json::integer(input, QStringLiteral("offset"), 1)
               << "limit=" << json::integer(input, QStringLiteral("limit"), kDefaultLimit);

  if(context.isCancelled()) {
    finish(ToolResult::failure(QCoreApplication::translate("tools::ReadTool", "Execution cancelled"),
                               QStringLiteral("cancelled")));
    return;
  }
  if(rawPath.isEmpty()) {
    finish(ToolResult::failure(QCoreApplication::translate("tools::ReadTool", "file_path cannot be empty"),
                               QStringLiteral("invalid_input")));
    return;
  }

  const QString path = context.resolvePath(rawPath);
  if(path.isEmpty()) {
    // resolvePath 返回空串 == 越界（含 `../` 逃逸）或路径非法。
    finish(ToolResult::failure(
             QCoreApplication::translate("tools::ReadTool",
                                         "Invalid path, or outside the workspace: %1").arg(toolutil::redactForLog(rawPath)),
             QStringLiteral("path_outside_workspace")));
    return;
  }

  const QFileInfo info(path);
  if(!info.exists()) {
    finish(ToolResult::failure(QCoreApplication::translate("tools::ReadTool", "File does not exist: %1").arg(path),
                               QStringLiteral("file_not_found")));
    return;
  }
  if(info.isDir()) {
    finish(ToolResult::failure(
             QCoreApplication::translate("tools::ReadTool",
                                         "%1 is a directory; Read only reads files. Use Glob/Grep or Bash ls.").arg(path),
             QStringLiteral("is_a_directory")));
    return;
  }
  if(!info.isFile()) {
    finish(ToolResult::failure(QCoreApplication::translate("tools::ReadTool",
                                                           "%1 is not a regular file (it may be a device or FIFO).").arg(path),
                               QStringLiteral("not_a_regular_file")));
    return;
  }

  const qint64 startedMs = nowMs();
  QString mime;
  if(isImagePath(path, &mime)) {
    ToolResult result = readImage(path, info, mime);
    result.metadata.insert(QStringLiteral("durationMs"), static_cast<double>(nowMs() - startedMs));
    qCInfo(log) << "Read 图片完成; path=" << path << "bytes=" << info.size();
    finish(std::move(result));
    return;
  }

  QFile file(path);
  if(!file.open(QIODevice::ReadOnly)) {
    finish(ToolResult::failure(QCoreApplication::translate("tools::ReadTool",
                                                           "Could not open the file: %1").arg(file.errorString()),
                               QStringLiteral("read_failed")));
    return;
  }
  // 大文件只读前 kHardContentLimit：避免一次读取把内存打满；
  // 真正返回的正文还会被 maxOutputBytes 二次约束。
  QByteArray bytes = file.read(kHardContentLimit);
  const bool contentTruncated = info.size() > bytes.size();
  if(contentTruncated) {
    qCWarning(log) << "文件超过硬读取上限，只读取前" << kHardContentLimit << "字节; path=" << path;
  }

  if(toolutil::looksBinary(bytes)) {
    finish(ToolResult::failure(
             QCoreApplication::translate("tools::ReadTool",
                                         "%1 looks like a binary file (a NUL byte appears in the first 8 KiB), which Read does not support. Use Bash instead (for example `file`, `xxd`, `strings`).")
             .arg(path),
             QStringLiteral("binary_file")));
    return;
  }

  QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
  // 末尾换行切出的空串只是行尾符产物；但**完全空文件**保留 0 行语义。
  if(!lines.isEmpty() && lines.last().isEmpty() && !bytes.isEmpty()) {
    lines.removeLast();
  }
  if(bytes.isEmpty()) {
    lines.clear();
  }
  const int totalLines = static_cast<int>(lines.size());

  int offset = json::integer(input, QStringLiteral("offset"), 1);
  if(offset < 1) {
    offset = 1;
  }
  int limit = json::integer(input, QStringLiteral("limit"), kDefaultLimit);
  if(limit <= 0) {
    limit = kDefaultLimit;
  }
  limit = std::min(limit, kMaxLimit);

  const int startIndex = std::min(offset - 1, totalLines);
  const int endIndex = std::min(startIndex + limit, totalLines);

  toolutil::OutputBudget budget(meta.maxOutputBytes);
  int emitted = 0;
  for(int i = startIndex; i < endIndex; ++i) {
    // 行号用 1-based，与编辑器/编译器一致；Tab 分隔便于模型按列定位。
    budget.appendLine(QStringLiteral("%1\t%2").arg(i + 1).arg(lines.at(i)));
    ++emitted;
    if(budget.truncated()) {
      // 字节预算已耗尽：停止累加，这样 numLines 才能如实反映"模型真正看到多少行"，
      // 而不是"逻辑上切了多少行"。
      break;
    }
  }

  const bool truncated = contentTruncated || (startIndex + emitted < totalLines) ||
                         budget.truncated();

  QString output = toolutil::chompTrailingNewlines(budget.text());
  if(output.isEmpty()) {
    output = totalLines == 0 ? QStringLiteral("(empty file)")
             : QStringLiteral("(no lines in the requested range)");
  }

  QJsonObject resultMeta;
  resultMeta.insert(QStringLiteral("filePath"), path);
  resultMeta.insert(QStringLiteral("mimeType"), QStringLiteral("text/plain"));
  resultMeta.insert(QStringLiteral("totalLines"), totalLines);
  resultMeta.insert(QStringLiteral("numLines"), emitted);
  resultMeta.insert(QStringLiteral("startLine"), totalLines == 0 ? 0 : startIndex + 1);
  resultMeta.insert(QStringLiteral("endLine"), emitted == 0 ? 0 : startIndex + emitted);
  resultMeta.insert(QStringLiteral("sizeBytes"), static_cast<double>(info.size()));
  resultMeta.insert(QStringLiteral("truncated"), truncated);
  // 区分两种截断：行范围截断（可再用 offset/limit 翻页）与字节预算截断
  // （单行过长，翻页也读不完，模型应当改用 Bash 处理）。
  resultMeta.insert(QStringLiteral("byteBudgetExceeded"), budget.truncated());
  resultMeta.insert(QStringLiteral("durationMs"), static_cast<double>(nowMs() - startedMs));

  qCInfo(log) << "Read 完成; path=" << path << "lines=" << emitted << "/" << totalLines
              << "bytes=" << info.size() << "truncated=" << truncated
              << "durationMs=" << (nowMs() - startedMs);

  finish(ToolResult::success(output, resultMeta));
}

}  // namespace lycode

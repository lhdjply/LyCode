// LyCode — 统一 diff 生成实现
#include "tools/Diff.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLoggingCategory>
#include <algorithm>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.tool.diff")

/// 单行编辑操作。type 取值与结构化补丁一致。
enum class EditType { Context, Add, Remove };

struct EditOp {
  EditType type = EditType::Context;
  QString text;
};

/// 一个变更块（连续的 add/remove，前后各带若干行上下文）。
struct ChangeBlock {
  int oldStart = 0;  ///< 0-based，指向 old 序列
  int oldCount = 0;
  int newStart = 0;  ///< 0-based，指向 new 序列
  int newCount = 0;
  int contextStart = 0;  ///< 0-based，展开上下文后的起点
  int contextOldCount = 0;
  int contextNewCount = 0;
  QList<EditOp> ops;
};

/// DP 规模上限：N*M 超过这个量级就退化为"整段替换"，
/// 宁可给一个粗糙但正确的补丁，也不要卡死或 OOM。
constexpr qint64 kMaxCells = 4'000'000;

/// 公共前后缀裁剪后的中间段 DP，产出编辑操作序列（含 context）。
QList<EditOp> diffMiddle(const QStringList & oldLines, const QStringList & newLines)
{
  const qsizetype n = oldLines.size();
  const qsizetype m = newLines.size();
  QList<EditOp> ops;

  if(n == 0 && m == 0) {
    return ops;
  }

  // LCS 长度表：lcs[i][j] = old[i..] 与 new[j..] 的最长公共子序列长度。
  QList<int> table((n + 1) * (m + 1), 0);
  auto at = [&table, m](qsizetype i, qsizetype j) -> int & {
    return table[i * (m + 1) + j];
  };
  for(qsizetype i = n; i-- > 0;) {
    for(qsizetype j = m; j-- > 0;) {
      if(oldLines.at(i) == newLines.at(j)) {
        at(i, j) = at(i + 1, j + 1) + 1;
      }
      else {
        at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
      }
    }
  }

  qsizetype i = 0;
  qsizetype j = 0;
  while(i < n && j < m) {
    if(oldLines.at(i) == newLines.at(j)) {
      ops.append({EditType::Context, oldLines.at(i)});
      ++i;
      ++j;
    }
    else if(at(i + 1, j) >= at(i, j + 1)) {
      ops.append({EditType::Remove, oldLines.at(i)});
      ++i;
    }
    else {
      ops.append({EditType::Add, newLines.at(j)});
      ++j;
    }
  }
  while(i < n) {
    ops.append({EditType::Remove, oldLines.at(i)});
    ++i;
  }
  while(j < m) {
    ops.append({EditType::Add, newLines.at(j)});
    ++j;
  }
  return ops;
}

/// 把编辑操作序列聚成 change block：变更行前后各保留 contextLines 行上下文。
QList<ChangeBlock> buildBlocks(const QList<EditOp> & ops, qsizetype contextLines)
{
  QList<ChangeBlock> blocks;

  qsizetype index = 0;
  const qsizetype total = ops.size();
  while(index < total) {
    if(ops.at(index).type == EditType::Context) {
      ++index;
      continue;
    }
    // 找到变更起点，向前展开上下文。
    const qsizetype firstChange = index;
    qsizetype lastChange = index;
    while(lastChange + 1 < total) {
      if(ops.at(lastChange + 1).type != EditType::Context) {
        ++lastChange;
        continue;
      }
      // 中间夹着的 context 若不超过 2*contextLines，则并入同一个 block
      // （否则会出现只有一个 context 行的碎片 hunk，可读性反而更差）。
      qsizetype run = lastChange + 1;
      while(run < total && ops.at(run).type == EditType::Context) {
        ++run;
      }
      if(run < total && run - (lastChange + 1) <= contextLines * 2) {
        lastChange = run;
        continue;
      }
      break;
    }

    const qsizetype blockStart = std::max<qsizetype>(0, firstChange - contextLines);
    const qsizetype blockEnd = std::min<qsizetype>(total - 1, lastChange + contextLines);

    ChangeBlock block;
    int oldCursor = 0;
    int newCursor = 0;
    for(qsizetype k = 0; k < blockStart; ++k) {
      if(ops.at(k).type != EditType::Add) {
        ++oldCursor;
      }
      if(ops.at(k).type != EditType::Remove) {
        ++newCursor;
      }
    }
    block.contextStart = static_cast<int>(blockStart);
    block.oldStart = oldCursor;
    block.newStart = newCursor;
    const int oldAtBlockStart = oldCursor;
    const int newAtBlockStart = newCursor;

    for(qsizetype k = blockStart; k <= blockEnd; ++k) {
      const EditOp & op = ops.at(k);
      block.ops.append(op);
      switch(op.type) {
        case EditType::Context:
          ++oldCursor;
          ++newCursor;
          break;
        case EditType::Remove:
          ++oldCursor;
          break;
        case EditType::Add:
          ++newCursor;
          break;
      }
    }
    block.oldCount = oldCursor - oldAtBlockStart;
    block.newCount = newCursor - newAtBlockStart;
    block.contextOldCount = block.oldCount;
    block.contextNewCount = block.newCount;
    blocks.append(block);

    index = blockEnd + 1;
  }
  return blocks;
}

}  // namespace

QStringList Diff::splitLines(const QString & text)
{
  if(text.isEmpty()) {
    return {};
  }
  QStringList lines = text.split(QLatin1Char('\n'));
  // "a\nb\n" 切出 ["a","b",""]，末尾空串只是换行符的产物，不是一行。
  if(!lines.isEmpty() && lines.last().isEmpty()) {
    lines.removeLast();
  }
  return lines;
}

QJsonArray Diff::unified(const QString & oldText, const QString & newText, const QString & path)
{
  QJsonArray hunks;
  if(oldText == newText) {
    return hunks;
  }

  const QStringList oldLines = splitLines(oldText);
  const QStringList newLines = splitLines(newText);

  // 公共前后缀裁剪：真实编辑通常只动文件的很小一段，裁剪后 DP 规模骤降。
  qsizetype prefix = 0;
  while(prefix < oldLines.size() && prefix < newLines.size() &&
        oldLines.at(prefix) == newLines.at(prefix)) {
    ++prefix;
  }
  qsizetype suffix = 0;
  while(suffix < (oldLines.size() - prefix) && suffix < (newLines.size() - prefix) &&
        oldLines.at(oldLines.size() - 1 - suffix) == newLines.at(newLines.size() - 1 - suffix)) {
    ++suffix;
  }

  const QStringList oldMiddle = oldLines.mid(prefix, oldLines.size() - prefix - suffix);
  const QStringList newMiddle = newLines.mid(prefix, newLines.size() - prefix - suffix);

  QList<EditOp> ops;
  ops.reserve(prefix + oldMiddle.size() + newMiddle.size() + suffix);

  // 前缀：都是 context。
  for(qsizetype k = 0; k < prefix; ++k) {
    ops.append({EditType::Context, oldLines.at(k)});
  }

  const qint64 cells = static_cast<qint64>(oldMiddle.size() + 1) *
                       static_cast<qint64>(newMiddle.size() + 1);
  if(cells > kMaxCells) {
    qCWarning(log) << "diff 规模超限，退化为整段替换; path=" << path
                   << "oldLines=" << oldLines.size() << "newLines=" << newLines.size();
    for(const QString & line : oldMiddle) {
      ops.append({EditType::Remove, line});
    }
    for(const QString & line : newMiddle) {
      ops.append({EditType::Add, line});
    }
  }
  else {
    ops.append(diffMiddle(oldMiddle, newMiddle));
  }

  // 后缀：都是 context。
  for(qsizetype k = 0; k < suffix; ++k) {
    ops.append({EditType::Context, oldLines.at(oldLines.size() - suffix + k)});
  }

  constexpr qsizetype kContextLines = 3;
  const QList<ChangeBlock> blocks = buildBlocks(ops, kContextLines);

  for(const ChangeBlock & block : blocks) {
    QJsonArray lines;
    for(const EditOp & op : block.ops) {
      QJsonObject line;
      switch(op.type) {
        case EditType::Context:
          line.insert(QStringLiteral("type"), QStringLiteral("context"));
          break;
        case EditType::Add:
          line.insert(QStringLiteral("type"), QStringLiteral("add"));
          break;
        case EditType::Remove:
          line.insert(QStringLiteral("type"), QStringLiteral("remove"));
          break;
      }
      line.insert(QStringLiteral("text"), op.text);
      lines.append(line);
    }

    QJsonObject hunk;
    // 统一 diff 的坐标是 1-based；空段落按惯例写成 0 起点的表达。
    hunk.insert(QStringLiteral("oldStart"), block.oldCount == 0 ? block.oldStart : block.oldStart + 1);
    hunk.insert(QStringLiteral("oldLines"), block.oldCount);
    hunk.insert(QStringLiteral("newStart"), block.newCount == 0 ? block.newStart : block.newStart + 1);
    hunk.insert(QStringLiteral("newLines"), block.newCount);
    hunk.insert(QStringLiteral("lines"), lines);
    hunks.append(hunk);
  }

  qCDebug(log) << "生成 diff; path=" << path << "hunks=" << hunks.size();
  return hunks;
}

void Diff::summary(const QJsonArray & hunks, int * additions, int * deletions)
{
  int add = 0;
  int remove = 0;
  for(const QJsonValue & hunkValue : hunks) {
    const QJsonArray lines = hunkValue.toObject().value(QStringLiteral("lines")).toArray();
    for(const QJsonValue & lineValue : lines) {
      const QString type = lineValue.toObject().value(QStringLiteral("type")).toString();
      if(type == QLatin1String("add")) {
        ++add;
      }
      else if(type == QLatin1String("remove")) {
        ++remove;
      }
    }
  }
  if(additions != nullptr) {
    *additions = add;
  }
  if(deletions != nullptr) {
    *deletions = remove;
  }
}

QString Diff::render(const QJsonArray & hunks, const QString & path)
{
  if(hunks.isEmpty()) {
    return {};
  }
  QString out;
  out += QStringLiteral("--- a/") + path + QLatin1Char('\n');
  out += QStringLiteral("+++ b/") + path + QLatin1Char('\n');
  for(const QJsonValue & hunkValue : hunks) {
    const QJsonObject hunk = hunkValue.toObject();
    const int oldStart = hunk.value(QStringLiteral("oldStart")).toInt();
    const int oldLines = hunk.value(QStringLiteral("oldLines")).toInt();
    const int newStart = hunk.value(QStringLiteral("newStart")).toInt();
    const int newLines = hunk.value(QStringLiteral("newLines")).toInt();
    out += QStringLiteral("@@ -%1,%2 +%3,%4 @@\n")
           .arg(oldStart)
           .arg(oldLines)
           .arg(newStart)
           .arg(newLines);
    const QJsonArray lines = hunk.value(QStringLiteral("lines")).toArray();
    for(const QJsonValue & lineValue : lines) {
      const QJsonObject line = lineValue.toObject();
      const QString type = line.value(QStringLiteral("type")).toString();
      const QString text = line.value(QStringLiteral("text")).toString();
      if(type == QLatin1String("add")) {
        out += QLatin1Char('+') + text + QLatin1Char('\n');
      }
      else if(type == QLatin1String("remove")) {
        out += QLatin1Char('-') + text + QLatin1Char('\n');
      }
      else {
        out += QLatin1Char(' ') + text + QLatin1Char('\n');
      }
    }
  }
  return out;
}

}  // namespace lycode

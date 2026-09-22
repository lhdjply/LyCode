// ZCode Qt — Grep 工具实现
//
// 后端选择：
//   * ripgrep（系统装了就用）：交给 QProcess 异步跑，避免阻塞 GUI 线程。
//   * native（纯 Qt）：QDirIterator + QRegularExpression，跑在 QThreadPool
//     的工作线程里，结果用 queued invoke 送回调用线程再回调。
//
// 两个后端都产出同一套 GrepEntry，分页/格式化/元数据只有一份实现，
// 不会出现"换了后端结果格式就变"的问题。
#include "tools/GrepTool.h"

#include "core/Json.h"
#include "tools/Diff.h"
#include "tools/ToolUtils.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QSet>

#include <algorithm>
#include <memory>

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.tool.grep")

constexpr int kMaxOutputBytes = 1'000'000;
constexpr int kDefaultHeadLimit = 250;
/// 单文件读取上限：更大的文件跳过（在 metadata 里计数），
/// 否则一次 grep 就能把内存打满。
constexpr qint64 kMaxFileBytes = 16 * 1024 * 1024;
/// 扫描条目上限，防止在超大仓库里无界递归。
constexpr qint64 kMaxScannedEntries = 300'000;
/// 结果行硬上限：超过之后即使 head_limit=0 也停止，
/// 因为保留几十万行对模型没有任何价值。
constexpr qsizetype kHardResultLines = 100'000;

struct GrepOptions {
    QString pattern;
    QString basePath;
    QString globPattern;   ///< 空的表示不限制
    QString typeFilter;    ///< 只有 rg 后端支持
    QString outputMode = QStringLiteral("files_with_matches");
    bool ignoreCase = false;
    bool showLineNumbers = true;
    bool onlyMatching = false;
    bool multiline = false;
    int before = 0;
    int after = 0;
    int headLimit = kDefaultHeadLimit;
    int offset = 0;

    QRegularExpression regex;
    QRegularExpression globRegex;  ///< 无效（未设置 glob）时 pattern 为空
};

/// 一条结果。统一模型让两个后端可以共用全部下游逻辑。
struct GrepEntry {
    QString path;        ///< 绝对路径
    int lineNumber = 0;  ///< 0 表示该模式不带行号
    QString text;
    bool match = true;   ///< false 表示上下文行
};

struct GrepOutcome {
    QList<GrepEntry> entries;
    QString backend;
    QString error;
    QString errorCode;
    bool cancelled = false;
    bool hardLimitReached = false;
    bool typeFilterIgnored = false;
    int skippedLargeFiles = 0;
    int skippedBinaryFiles = 0;
    qint64 scannedEntries = 0;
    QStringList skippedDirs;
};

// ─────────────────────────────────────────────────────────────────────────────
// 公共辅助
// ─────────────────────────────────────────────────────────────────────────────

/// 单行文本折成一行展示（multiline 的匹配片段可能跨行）。
QString flatten(const QString &text) {
    QString result = text;
    result.replace(QLatin1Char('\r'), QString());
    result.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return result;
}

/// 文件相对基目录的路径，统一 '/' 分隔。
QString relativeTo(const QString &baseDir, const QString &absolutePath) {
    const QString relative = QDir::fromNativeSeparators(absolutePath);
    const QString base = QDir::fromNativeSeparators(baseDir);
    if (relative.startsWith(base + QLatin1Char('/'))) {
        return relative.mid(base.size() + 1);
    }
    return relative;
}

/// 按 glob 过滤文件（在读取内容之前，省一次 IO）。
bool passesGlobFilter(const GrepOptions &options, const QString &relativePath) {
    if (options.globPattern.isEmpty()) {
        return true;
    }
    return toolutil::globMatches(options.globRegex, relativePath);
}

// ─────────────────────────────────────────────────────────────────────────────
// native 后端
// ─────────────────────────────────────────────────────────────────────────────

void collectFiles(const QString &dir, const GrepOptions &options, const ToolContext &context,
                  QStringList *files, GrepOutcome *outcome) {
    if (context.isCancelled()) {
        outcome->cancelled = true;
        return;
    }
    QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::NoIteratorFlags);
    QStringList subdirs;
    while (it.hasNext()) {
        const QFileInfo info = it.nextFileInfo();
        ++outcome->scannedEntries;
        if (outcome->scannedEntries > kMaxScannedEntries) {
            outcome->hardLimitReached = true;
            return;
        }
        if (info.isDir()) {
            if (info.isSymLink()) {
                continue;  // 不跟随符号链接目录，避免递归成环
            }
            const QString name = info.fileName();
            if (toolutil::shouldSkipDirectory(name)) {
                if (!outcome->skippedDirs.contains(name)) {
                    outcome->skippedDirs.append(name);
                }
                continue;
            }
            subdirs.append(info.absoluteFilePath());
            continue;
        }
        if (!info.isFile()) {
            continue;
        }
        if (!passesGlobFilter(options, relativeTo(options.basePath, info.absoluteFilePath()))) {
            continue;
        }
        files->append(info.absoluteFilePath());
    }
    subdirs.sort();
    for (const QString &sub : subdirs) {
        collectFiles(sub, options, context, files, outcome);
        if (outcome->cancelled || outcome->hardLimitReached) {
            return;
        }
    }
}

/// 合并区间，保证上下文行不重复输出（`-C 3` 的两个相邻匹配会重叠）。
QList<QPair<int, int>> mergeRanges(QList<QPair<int, int>> ranges) {
    std::sort(ranges.begin(), ranges.end());
    QList<QPair<int, int>> merged;
    for (const auto &range : ranges) {
        if (!merged.isEmpty() && range.first <= merged.last().second + 1) {
            merged.last().second = std::max(merged.last().second, range.second);
        } else {
            merged.append(range);
        }
    }
    return merged;
}

void appendMatchEntries(const QString &path, int lineIndex, const QString &line,
                        const GrepOptions &options, QList<GrepEntry> *entries) {
    if (!options.onlyMatching) {
        entries->append({path, lineIndex + 1, flatten(line), true});
        return;
    }
    // `-o`：一行里的每个匹配各占一条结果。
    QRegularExpressionMatchIterator iterator = options.regex.globalMatch(line);
    bool any = false;
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        entries->append({path, lineIndex + 1, flatten(match.captured(0)), true});
        any = true;
    }
    if (!any) {
        // 理论上不会发生（调用方已确认该行匹配成功），但兜底避免静默丢结果。
        entries->append({path, lineIndex + 1, flatten(line), true});
    }
}

void scanFile(const QString &path, const GrepOptions &options, GrepOutcome *outcome) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(log) << "grep 无法读取文件，跳过:" << path << file.errorString();
        return;
    }
    const QByteArray bytes = file.readAll();
    if (toolutil::looksBinary(bytes)) {
        ++outcome->skippedBinaryFiles;
        return;
    }
    const QString text = QString::fromUtf8(bytes);

    if (options.multiline) {
        // multiline：对整个文件做全局匹配，行号取匹配起点所在行。
        // 由于匹配可能跨行，content 模式下输出的是**匹配片段**（换行折成 `\n`）。
        QRegularExpressionMatchIterator iterator = options.regex.globalMatch(text);
        int matchCount = 0;
        while (iterator.hasNext()) {
            const QRegularExpressionMatch match = iterator.next();
            ++matchCount;
            if (options.outputMode == QLatin1String("content")) {
                const int lineIndex =
                    static_cast<int>(text.left(match.capturedStart()).count(QLatin1Char('\n')));
                outcome->entries.append({path, lineIndex + 1, flatten(match.captured(0)), true});
                if (outcome->entries.size() >= kHardResultLines) {
                    outcome->hardLimitReached = true;
                    return;
                }
            }
        }
        if (matchCount == 0) {
            return;
        }
        if (options.outputMode == QLatin1String("count")) {
            outcome->entries.append({path, 0, QString::number(matchCount), true});
        } else if (options.outputMode == QLatin1String("files_with_matches")) {
            outcome->entries.append({path, 0, path, true});
        }
        return;
    }

    const QStringList lines = Diff::splitLines(text);
    QList<int> matchLines;
    for (int i = 0; i < lines.size(); ++i) {
        if (options.regex.match(lines.at(i)).hasMatch()) {
            matchLines.append(i);
        }
    }
    if (matchLines.isEmpty()) {
        return;
    }

    if (options.outputMode == QLatin1String("files_with_matches")) {
        outcome->entries.append({path, 0, path, true});
        return;
    }
    if (options.outputMode == QLatin1String("count")) {
        outcome->entries.append({path, 0, QString::number(matchLines.size()), true});
        return;
    }

    if (options.before == 0 && options.after == 0) {
        for (int lineIndex : matchLines) {
            appendMatchEntries(path, lineIndex, lines.at(lineIndex), options, &outcome->entries);
            if (outcome->entries.size() >= kHardResultLines) {
                outcome->hardLimitReached = true;
                return;
            }
        }
        return;
    }

    // 带上下文：把每个匹配行扩展成区间后合并，再逐行输出（上下文行带 match=false）。
    QSet<int> matchSet;
    QList<QPair<int, int>> ranges;
    for (int lineIndex : matchLines) {
        matchSet.insert(lineIndex);
        const int from = std::max(0, lineIndex - options.before);
        const int to = std::min(static_cast<int>(lines.size()) - 1, lineIndex + options.after);
        ranges.append({from, to});
    }
    const QList<QPair<int, int>> merged = mergeRanges(ranges);
    for (const auto &range : merged) {
        for (int i = range.first; i <= range.second; ++i) {
            if (matchSet.contains(i)) {
                appendMatchEntries(path, i, lines.at(i), options, &outcome->entries);
            } else {
                outcome->entries.append({path, i + 1, flatten(lines.at(i)), false});
            }
            if (outcome->entries.size() >= kHardResultLines) {
                outcome->hardLimitReached = true;
                return;
            }
        }
    }
}

GrepOutcome nativeScan(const GrepOptions &options, const ToolContext &context) {
    GrepOutcome outcome;
    outcome.backend = QStringLiteral("native");
    if (!options.typeFilter.isEmpty()) {
        // 纯 Qt 实现没有 ripgrep 的 --type 定义表，明确告知而不是静默忽略。
        outcome.typeFilterIgnored = true;
    }

    QStringList files;
    collectFiles(options.basePath, options, context, &files, &outcome);
    if (outcome.cancelled) {
        return outcome;
    }
    std::sort(files.begin(), files.end());

    for (const QString &path : files) {
        if (context.isCancelled()) {
            outcome.cancelled = true;
            return outcome;
        }
        if (outcome.hardLimitReached) {
            return outcome;
        }
        const QFileInfo info(path);
        if (info.size() > kMaxFileBytes) {
            ++outcome.skippedLargeFiles;
            continue;
        }
        scanFile(path, options, &outcome);
        if (options.outputMode == QLatin1String("files_with_matches") &&
            outcome.entries.size() >= kHardResultLines) {
            outcome.hardLimitReached = true;
            return outcome;
        }
    }
    return outcome;
}

/// 跑在 QThreadPool 工作线程里的扫描任务。
class NativeScanTask : public QRunnable {
public:
    NativeScanTask(GrepOptions options, ToolContext context,
                   std::function<void(GrepOutcome)> onDone)
        : options_(std::move(options)), context_(std::move(context)), onDone_(std::move(onDone)) {}

    void run() override {
        GrepOutcome outcome = nativeScan(options_, context_);
        onDone_(std::move(outcome));
    }

private:
    GrepOptions options_;
    ToolContext context_;
    std::function<void(GrepOutcome)> onDone_;
};

// ─────────────────────────────────────────────────────────────────────────────
// ripgrep 后端
// ─────────────────────────────────────────────────────────────────────────────

QStringList buildRipgrepArgs(const GrepOptions &options) {
    QStringList args;
    args << QStringLiteral("--color=never") << QStringLiteral("--no-heading")
         << QStringLiteral("--with-filename");
    if (options.ignoreCase) {
        args << QStringLiteral("-i");
    }
    if (options.onlyMatching) {
        args << QStringLiteral("-o");
    }
    if (options.multiline) {
        args << QStringLiteral("-U") << QStringLiteral("--multiline-dotall");
    }
    if (options.outputMode == QLatin1String("files_with_matches")) {
        args << QStringLiteral("--files-with-matches");
    } else if (options.outputMode == QLatin1String("count")) {
        args << QStringLiteral("--count");
    } else {
        if (options.showLineNumbers) {
            args << QStringLiteral("-n");
        }
        if (options.before > 0) {
            args << QStringLiteral("-B") << QString::number(options.before);
        }
        if (options.after > 0) {
            args << QStringLiteral("-A") << QString::number(options.after);
        }
    }
    if (!options.typeFilter.isEmpty()) {
        args << QStringLiteral("--type") << options.typeFilter;
    }
    if (!options.globPattern.isEmpty()) {
        args << QStringLiteral("--glob") << options.globPattern;
    }
    // `-e` 让以 `-` 开头的 pattern 不会被当成选项；`--` 终止选项解析。
    args << QStringLiteral("-e") << options.pattern << QStringLiteral("--") << options.basePath;
    return args;
}

/// 解析 rg 的一行：`path:line:text`（匹配）或 `path-line-text`（上下文）。
/// 文件名里可能含 ':' 或 '-'，因此用"分隔符后跟数字"来定位，而不是取第一个分隔符。
bool parseRipgrepLine(const QString &line, GrepEntry *entry) {
    static const QRegularExpression pattern(
        QStringLiteral("\\A(.*?)([:\\-])(\\d+)([:\\-])(.*)\\z"));
    const QRegularExpressionMatch match = pattern.match(line);
    if (!match.hasMatch()) {
        return false;
    }
    const QString sep1 = match.captured(2);
    const QString sep2 = match.captured(4);
    if (sep1 != sep2) {
        return false;
    }
    entry->path = match.captured(1);
    entry->lineNumber = match.captured(3).toInt();
    entry->text = match.captured(5);
    entry->match = sep1 == QLatin1String(":");
    return true;
}

GrepOutcome parseRipgrepOutput(const QByteArray &stdoutBytes, const GrepOptions &options) {
    GrepOutcome outcome;
    outcome.backend = QStringLiteral("ripgrep");
    const QStringList lines = QString::fromUtf8(stdoutBytes).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (options.outputMode == QLatin1String("files_with_matches")) {
            outcome.entries.append({line, 0, line, true});
            continue;
        }
        if (options.outputMode == QLatin1String("count")) {
            // rg --count 输出 `path:count`（最后一个 ':' 是分隔符）。
            const qsizetype index = line.lastIndexOf(QLatin1Char(':'));
            if (index <= 0) {
                continue;
            }
            outcome.entries.append(
                {line.left(index), 0, line.mid(index + 1), true});
            continue;
        }
        GrepEntry entry;
        if (parseRipgrepLine(line, &entry)) {
            entry.text = flatten(entry.text);
            outcome.entries.append(entry);
        } else {
            // 解析不了的行（例如 rg 的告警）原样保留，避免静默丢信息。
            outcome.entries.append({QString(), 0, line, true});
        }
    }
    return outcome;
}

}  // namespace

ToolMetadata GrepTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("Grep");
    meta.description = QStringLiteral(
        "Search file contents with a regular expression. Uses ripgrep when "
        "available, otherwise a built-in scanner.");
    meta.modelInstructions = QStringLiteral(
        "output_mode: `content` (matching lines), `files_with_matches` (default) "
        "or `count`. Use -A/-B/-C for context, glob to filter files, head_limit "
        "and offset for paging. -n defaults to true.");
    meta.allowedInPlanMode = true;
    meta.readOnly = true;
    meta.destructive = false;
    meta.concurrentSafe = true;
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

QJsonObject GrepTool::inputSchema() const {
    auto stringProp = [](const QString &description) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                           {QStringLiteral("description"), description}};
    };
    auto numberProp = [](const QString &description) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                           {QStringLiteral("description"), description}};
    };
    auto boolProp = [](const QString &description) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                           {QStringLiteral("description"), description}};
    };

    QJsonObject properties;
    properties.insert(QStringLiteral("pattern"), stringProp(QStringLiteral("要搜索的正则表达式")));
    properties.insert(QStringLiteral("path"), stringProp(QStringLiteral("搜索根目录/文件（默认工作目录）")));
    properties.insert(QStringLiteral("glob"), stringProp(QStringLiteral("只搜索匹配该 glob 的文件，如 `*.cpp`")));
    properties.insert(QStringLiteral("output_mode"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("enum"),
                                   QJsonArray{QStringLiteral("content"),
                                              QStringLiteral("files_with_matches"),
                                              QStringLiteral("count")}},
                                  {QStringLiteral("description"),
                                   QStringLiteral("输出模式，默认 files_with_matches")}});
    properties.insert(QStringLiteral("-B"), numberProp(QStringLiteral("匹配行之前显示的上下文行数")));
    properties.insert(QStringLiteral("-A"), numberProp(QStringLiteral("匹配行之后显示的上下文行数")));
    properties.insert(QStringLiteral("-C"), numberProp(QStringLiteral("匹配行前后各显示的上下文行数")));
    properties.insert(QStringLiteral("context"), numberProp(QStringLiteral("等价于 -C")));
    properties.insert(QStringLiteral("-n"), boolProp(QStringLiteral("是否显示行号，默认 true")));
    properties.insert(QStringLiteral("-i"), boolProp(QStringLiteral("忽略大小写")));
    properties.insert(QStringLiteral("-o"), boolProp(QStringLiteral("只输出匹配到的部分")));
    properties.insert(QStringLiteral("type"), stringProp(QStringLiteral("文件类型过滤（仅 ripgrep 后端支持），如 `cpp`")));
    properties.insert(QStringLiteral("head_limit"),
                      numberProp(QStringLiteral("最多返回多少条结果，默认 250，0 表示不限")));
    properties.insert(QStringLiteral("offset"), numberProp(QStringLiteral("跳过前多少条结果")));
    properties.insert(QStringLiteral("multiline"), boolProp(QStringLiteral("让 `.` 匹配换行（多行匹配）")));
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("pattern")}},
    };
}

QString GrepTool::title(const QJsonObject &input) const {
    const QString pattern = json::str(input, QStringLiteral("pattern"));
    return pattern.isEmpty() ? QStringLiteral("Grep") : QStringLiteral("Grep: ") + pattern;
}

QString GrepTool::permissionCapability() const {
    return QStringLiteral("read");
}

void GrepTool::execute(const QJsonObject &input, const ToolContext &context, ToolCallback done) {
    const ToolMetadata meta = metadata();

    // 恰好回调一次的护栏：rg 路径与 native 路径都可能因为"完成事件 + 超时定时器"
    // 双重触发，shared 状态保证只有一个赢家。
    struct State {
        bool finished = false;
    };
    auto state = std::make_shared<State>();
    auto finish = [state, done](ToolResult result) {
        if (state->finished) {
            return;
        }
        state->finished = true;
        done(std::move(result));
    };

    GrepOptions options;
    options.pattern = json::str(input, QStringLiteral("pattern"));
    options.basePath = json::str(input, QStringLiteral("path"));
    options.globPattern = toolutil::normalizeGlob(json::str(input, QStringLiteral("glob")).trimmed());
    options.typeFilter = json::str(input, QStringLiteral("type")).trimmed();
    options.outputMode = json::str(input, QStringLiteral("output_mode"),
                                   QStringLiteral("files_with_matches"));
    options.ignoreCase = json::boolean(input, QStringLiteral("-i"), false);
    options.showLineNumbers = json::boolean(input, QStringLiteral("-n"), true);
    options.onlyMatching = json::boolean(input, QStringLiteral("-o"), false);
    options.multiline = json::boolean(input, QStringLiteral("multiline"), false);
    options.headLimit = json::integer(input, QStringLiteral("head_limit"), kDefaultHeadLimit);
    options.offset = json::integer(input, QStringLiteral("offset"), 0);

    // 上下文：`context` 等价于同时设置 -B/-A；显式的 -B/-A 优先。
    const int contextLines = json::integer(input, QStringLiteral("context"), 0);
    options.before = json::integer(input, QStringLiteral("-B"), std::max(0, contextLines));
    options.after = json::integer(input, QStringLiteral("-A"), std::max(0, contextLines));
    const int c = json::integer(input, QStringLiteral("-C"), -1);
    if (c >= 0) {
        options.before = c;
        options.after = c;
    }
    options.before = std::max(0, options.before);
    options.after = std::max(0, options.after);
    if (options.headLimit < 0) {
        options.headLimit = kDefaultHeadLimit;
    }
    if (options.offset < 0) {
        options.offset = 0;
    }

    qCDebug(log) << "Grep 参数; pattern=" << toolutil::redactForLog(options.pattern)
                 << "path=" << toolutil::redactForLog(options.basePath)
                 << "glob=" << options.globPattern << "mode=" << options.outputMode
                 << "headLimit=" << options.headLimit << "offset=" << options.offset;

    if (context.isCancelled()) {
        finish(ToolResult::failure(QStringLiteral("执行已取消"), QStringLiteral("cancelled")));
        return;
    }
    if (options.pattern.isEmpty()) {
        finish(ToolResult::failure(QStringLiteral("pattern 不能为空"),
                                   QStringLiteral("invalid_input")));
        return;
    }
    if (options.outputMode != QLatin1String("content") &&
        options.outputMode != QLatin1String("files_with_matches") &&
        options.outputMode != QLatin1String("count")) {
        finish(ToolResult::failure(
            QStringLiteral("output_mode 只能是 content / files_with_matches / count，收到：%1")
                .arg(options.outputMode),
            QStringLiteral("invalid_input")));
        return;
    }

    // 基目录解析：显式 path > workingDirectory > workspace.path。
    QString base;
    if (!options.basePath.isEmpty()) {
        base = context.resolvePath(options.basePath);
        if (base.isEmpty()) {
            finish(ToolResult::failure(
                QStringLiteral("path 非法或超出工作区范围：%1")
                    .arg(toolutil::redactForLog(options.basePath)),
                QStringLiteral("path_outside_workspace")));
            return;
        }
    } else if (!context.workingDirectory.isEmpty()) {
        base = QDir::cleanPath(context.workingDirectory);
    } else {
        base = context.workspace.path;
    }
    if (base.isEmpty() || !QFileInfo::exists(base)) {
        finish(ToolResult::failure(QStringLiteral("搜索路径不存在：%1").arg(base),
                                   QStringLiteral("invalid_search_path")));
        return;
    }
    options.basePath = base;

    QRegularExpression::PatternOptions regexOptions = QRegularExpression::NoPatternOption;
    if (options.ignoreCase) {
        regexOptions |= QRegularExpression::CaseInsensitiveOption;
    }
    if (options.multiline) {
        regexOptions |= QRegularExpression::MultilineOption |
                        QRegularExpression::DotMatchesEverythingOption;
    }
    options.regex = QRegularExpression(options.pattern, regexOptions);
    if (!options.regex.isValid()) {
        // 先本地编译一次：两个后端就能给出同一份错误文案，
        // 也避免把非法 pattern 交给外部进程再去解析它的报错格式。
        finish(ToolResult::failure(
            QStringLiteral("正则表达式非法：%1").arg(options.regex.errorString()),
            QStringLiteral("invalid_pattern")));
        return;
    }
    if (!options.globPattern.isEmpty()) {
        options.globRegex = toolutil::globToRegex(options.globPattern);
    }

    const qint64 startedMs = nowMs();

    // ── 结果汇总：分页 + 格式化 + 元数据 + 回调 ─────────────────────────────
    auto finalize = [finish, options, context, startedMs, meta](GrepOutcome outcome) {
        if (outcome.cancelled) {
            finish(ToolResult::failure(
                QStringLiteral("执行已取消"), QStringLiteral("cancelled"),
                QJsonObject{{QStringLiteral("backend"), outcome.backend}}));
            return;
        }
        if (!outcome.error.isEmpty()) {
            finish(ToolResult::failure(outcome.error, outcome.errorCode,
                                       QJsonObject{{QStringLiteral("backend"), outcome.backend}}));
            return;
        }

        const bool requestedUnlimited = options.headLimit == 0;
        const qsizetype start = std::min<qsizetype>(options.offset, outcome.entries.size());
        QStringList lines;
        QSet<QString> files;
        int matchLines = 0;
        bool truncated = false;

        for (qsizetype i = start; i < outcome.entries.size(); ++i) {
            if (!requestedUnlimited && lines.size() >= options.headLimit) {
                // 还有剩余结果没输出 → 明确标记截断，模型才知道要翻页。
                truncated = true;
                break;
            }
            const GrepEntry &entry = outcome.entries.at(i);
            const QString display = entry.path.isEmpty() ? QString() : context.displayPath(entry.path);
            if (!entry.path.isEmpty()) {
                files.insert(display);
            }
            if (entry.match) {
                ++matchLines;
            }
            QString line;
            if (options.outputMode == QLatin1String("files_with_matches")) {
                line = display;
            } else if (options.outputMode == QLatin1String("count")) {
                line = display + QLatin1Char(':') + entry.text;
            } else {
                const QChar separator = entry.match ? QLatin1Char(':') : QLatin1Char('-');
                if (options.showLineNumbers && entry.lineNumber > 0) {
                    line = display + separator + QString::number(entry.lineNumber) + separator +
                           entry.text;
                } else {
                    line = display + separator + entry.text;
                }
            }
            lines.append(line);
        }
        if (outcome.hardLimitReached) {
            truncated = true;
        }

        toolutil::OutputBudget budget(meta.maxOutputBytes);
        for (const QString &line : lines) {
            budget.appendLine(line);
        }
        QString output = toolutil::chompTrailingNewlines(budget.text());
        if (output.isEmpty()) {
            output = QStringLiteral("(no matches)");
        }
        if (budget.truncated()) {
            truncated = true;
        }

        const qint64 durationMs = nowMs() - startedMs;
        QJsonObject resultMeta;
        resultMeta.insert(QStringLiteral("mode"), options.outputMode);
        resultMeta.insert(QStringLiteral("backend"), outcome.backend);
        resultMeta.insert(QStringLiteral("pattern"), options.pattern);
        resultMeta.insert(QStringLiteral("basePath"), options.basePath);
        resultMeta.insert(QStringLiteral("numFiles"), files.size());
        resultMeta.insert(QStringLiteral("numLines"), lines.size());
        resultMeta.insert(QStringLiteral("matchLines"), matchLines);
        resultMeta.insert(QStringLiteral("appliedLimit"), options.headLimit);
        resultMeta.insert(QStringLiteral("appliedOffset"), options.offset);
        resultMeta.insert(QStringLiteral("truncated"), truncated);
        resultMeta.insert(QStringLiteral("durationMs"), static_cast<double>(durationMs));
        resultMeta.insert(QStringLiteral("scannedEntries"), static_cast<double>(outcome.scannedEntries));
        if (outcome.skippedLargeFiles > 0) {
            resultMeta.insert(QStringLiteral("skippedLargeFiles"), outcome.skippedLargeFiles);
        }
        if (outcome.skippedBinaryFiles > 0) {
            resultMeta.insert(QStringLiteral("skippedBinaryFiles"), outcome.skippedBinaryFiles);
        }
        if (outcome.typeFilterIgnored) {
            resultMeta.insert(QStringLiteral("typeFilterIgnored"), true);
            resultMeta.insert(
                QStringLiteral("typeFilterNote"),
                QStringLiteral("native 后端不支持 type 过滤（未能找到系统 ripgrep），已忽略该参数"));
        }
        QJsonArray skipped;
        for (const QString &name : outcome.skippedDirs) {
            skipped.append(name);
        }
        resultMeta.insert(QStringLiteral("skippedDirectories"), skipped);

        qCInfo(log) << "Grep 完成; backend=" << outcome.backend << "mode=" << options.outputMode
                    << "files=" << files.size() << "lines=" << lines.size()
                    << "truncated=" << truncated << "durationMs=" << durationMs;

        finish(ToolResult::success(output, resultMeta));
    };

    // ── 后端选择 ───────────────────────────────────────────────────────────
    const QString ripgrep = QStandardPaths::findExecutable(QStringLiteral("rg"));
    if (!ripgrep.isEmpty()) {
        auto *process = new QProcess();
        process->setProgram(ripgrep);
        process->setArguments(buildRipgrepArgs(options));
        // 搜索路径可能是单个文件，此时工作目录取它的父目录。
        const QFileInfo baseInfo(options.basePath);
        process->setWorkingDirectory(baseInfo.isDir() ? options.basePath : baseInfo.absolutePath());

        auto *timeoutTimer = new QTimer(process);
        timeoutTimer->setSingleShot(true);
        timeoutTimer->setInterval(meta.timeoutMs > 0 ? meta.timeoutMs : 30000);
        auto *cancelTimer = new QTimer(process);
        cancelTimer->setInterval(100);

        // 非空表示被超时/取消/启动失败打断；finished 处理器据此决定收尾方式。
        auto interrupt = std::make_shared<QString>();

        QObject::connect(cancelTimer, &QTimer::timeout, process, [process, context, interrupt]() {
            if (context.isCancelled() && process->state() != QProcess::NotRunning) {
                *interrupt = QStringLiteral("cancelled");
                process->kill();
            }
        });
        QObject::connect(timeoutTimer, &QTimer::timeout, process, [process, interrupt, meta]() {
            if (process->state() != QProcess::NotRunning) {
                *interrupt = QStringLiteral("timeout");
                qCWarning(log) << "Grep(rg) 超时，已终止; timeoutMs=" << meta.timeoutMs;
                process->kill();
            }
        });
        QObject::connect(process, &QProcess::errorOccurred, process,
                         [process, interrupt, finalize](QProcess::ProcessError error) {
                             if (error != QProcess::FailedToStart) {
                                 return;
                             }
                             *interrupt = QStringLiteral("failed_to_start");
                             GrepOutcome outcome;
                             outcome.backend = QStringLiteral("ripgrep");
                             outcome.error = QStringLiteral("无法启动 ripgrep：%1")
                                                 .arg(process->errorString());
                             outcome.errorCode = QStringLiteral("backend_failed");
                             finalize(std::move(outcome));
                         });
        QObject::connect(process, &QProcess::finished, process,
                         [process, options, interrupt, finalize](int exitCode,
                                                                 QProcess::ExitStatus status) {
                             const QByteArray stdoutBytes = process->readAllStandardOutput();
                             const QByteArray stderrBytes = process->readAllStandardError();
                             process->deleteLater();
                             if (*interrupt == QStringLiteral("cancelled")) {
                                 GrepOutcome outcome;
                                 outcome.backend = QStringLiteral("ripgrep");
                                 outcome.cancelled = true;
                                 finalize(std::move(outcome));
                                 return;
                             }
                             if (*interrupt == QStringLiteral("timeout")) {
                                 GrepOutcome outcome;
                                 outcome.backend = QStringLiteral("ripgrep");
                                 outcome.error = QStringLiteral("ripgrep 搜索超时，已终止进程。");
                                 outcome.errorCode = QStringLiteral("timeout");
                                 finalize(std::move(outcome));
                                 return;
                             }
                             // rg 退出码：0 有匹配，1 无匹配，2 出错。
                             if (exitCode == 1 && status == QProcess::NormalExit) {
                                 GrepOutcome outcome;
                                 outcome.backend = QStringLiteral("ripgrep");
                                 finalize(std::move(outcome));
                                 return;
                             }
                             if (exitCode != 0 || status != QProcess::NormalExit) {
                                 GrepOutcome outcome;
                                 outcome.backend = QStringLiteral("ripgrep");
                                 outcome.error = QStringLiteral("ripgrep 失败（exitCode=%1）：%2")
                                                     .arg(exitCode)
                                                     .arg(QString::fromUtf8(stderrBytes).trimmed());
                                 outcome.errorCode = QStringLiteral("backend_failed");
                                 finalize(std::move(outcome));
                                 return;
                             }
                             finalize(parseRipgrepOutput(stdoutBytes, options));
                         });

        cancelTimer->start();
        timeoutTimer->start();
        // start() 是异步的：启动失败会走 errorOccurred(FailedToStart)，
        // 这里**不阻塞等待**，否则又回到了"在 GUI 线程里等 IO"。
        process->start();
        return;
    }

    // ── native 后端：跑在工作线程，结果 queued 回调用线程 ────────────────────
    // bridge 是调用线程里的 QObject，只作为 queued invoke 的上下文与生命周期锚点：
    // 结果回到调用线程后再 deleteLater，保证回调发生在构造 context 的线程。
    auto *bridge = new QObject();
    const QPointer<QObject> guard(bridge);
    QThreadPool::globalInstance()->start(new NativeScanTask(
        options, context, [guard, finalize](GrepOutcome outcome) {
            if (guard.isNull()) {
                // 调用方已销毁（会话关闭/进程退出）：丢弃结果而不触碰悬空指针。
                return;
            }
            QObject *target = guard.data();
            QMetaObject::invokeMethod(
                target,
                [finalize, outcome = std::move(outcome), target]() mutable {
                    finalize(std::move(outcome));
                    target->deleteLater();
                },
                Qt::QueuedConnection);
        }));
}

}  // namespace zcode

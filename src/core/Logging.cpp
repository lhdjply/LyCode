#include "core/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>
#include <memory>

namespace lycode::logging {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.logging")

QMutex g_mutex;
std::unique_ptr<QFile> g_logFile;

/// 按既定语义的数据目录约定：~/.lycode。
QString resolveLogDirectory() {
    const QString base = qEnvironmentVariable("LYCODE_DATA_BASE_DIR");
    const QString root = base.isEmpty()
                             ? QDir::homePath() + QStringLiteral("/.lycode")
                             : base;
    return root + QStringLiteral("/qt/logs");
}

/// 把 Qt 的日志级别映射为固定宽度文本，便于 grep。
const char *levelName(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:
            return "DEBUG";
        case QtInfoMsg:
            return "INFO ";
        case QtWarningMsg:
            return "WARN ";
        case QtCriticalMsg:
            return "ERROR";
        case QtFatalMsg:
            return "FATAL";
    }
    return "?????";
}

/// 日志落盘的消息处理器。写文件失败时静默降级为仅 stderr，
/// 避免日志系统本身成为崩溃源。
///
/// 注意签名必须匹配 QtMessageHandler（QMessageLogContext），不能自行换成
/// QLoggingCategory —— 分类名从 context.category 读取。
void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    const QString category = QString::fromUtf8(context.category != nullptr ? context.category
                                                                          : "default");
    const QString line = QStringLiteral("%1 %2 [%3] %4")
                             .arg(timestamp, QString::fromLatin1(levelName(type)), category,
                                  message);

    {
        QMutexLocker locker(&g_mutex);
        if (g_logFile && g_logFile->isOpen()) {
            QTextStream stream(g_logFile.get());
            stream << line << '\n';
            stream.flush();
        }
    }

    const QByteArray utf8 = line.toUtf8();
    std::fprintf(stderr, "%s\n", utf8.constData());
    std::fflush(stderr);

    if (type == QtFatalMsg) {
        std::abort();
    }
}

}  // namespace

void init(bool verbose) {
    const QString directory = resolveLogDirectory();
    if (!directory.isEmpty() && QDir().mkpath(directory)) {
        auto file = std::make_unique<QFile>(directory + QStringLiteral("/lycode.log"));
        // 追加模式，保留历史；轮转由外部工具负责，避免日志系统复杂化。
        if (file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QMutexLocker locker(&g_mutex);
            g_logFile = std::move(file);
        }
    }

    // 过滤规则语法是 `<category>[.<type>]=true|false`，type 只能是
    // debug / info / warning / critical。
    //
    // ⚠ 不能写成 `lycode.*=info`——`info` 会被当成类别名，整条规则被判为
    // malformed 并**静默忽略**（Qt 只在 qt.core.logging 里打印一句 warning），
    // 结果是 debug 日志照样落盘。这里用"全开 + 关掉 debug"表达同样的意图。
    const QString rules = verbose
                              ? QStringLiteral("lycode.*=true")
                              : QStringLiteral("lycode.*=true\nlycode.*.debug=false");
    QLoggingCategory::setFilterRules(rules);

    qInstallMessageHandler(messageHandler);
    qCInfo(log) << "日志已初始化; dir=" << (directory.isEmpty() ? QStringLiteral("<none>") : directory)
                << "verbose=" << verbose;
}

QString logDirectory() {
    const QString directory = resolveLogDirectory();
    if (directory.isEmpty()) {
        return {};
    }
    if (!QDir().mkpath(directory)) {
        return {};
    }
    return directory;
}

QString redact(const QString &secret) {
    const QString trimmed = secret.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("<empty>");
    }
    if (trimmed.size() <= 8) {
        return QStringLiteral("***");
    }
    return trimmed.left(4) + QStringLiteral("***") + trimmed.right(4);
}

}  // namespace lycode::logging

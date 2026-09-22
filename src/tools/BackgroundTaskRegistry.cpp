#include "tools/BackgroundTaskRegistry.h"

#include "core/Ids.h"
#include "core/Json.h"
#include "core/Logging.h"
#include "tools/ToolUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace lycode {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.tools.background")

QString dataRoot() {
    const QString base = qEnvironmentVariable("LYCODE_DATA_BASE_DIR");
    return base.isEmpty() ? QDir::homePath() + QStringLiteral("/.lycode") : base;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 存活判定
// ─────────────────────────────────────────────────────────────────────────────

qint64 processStartTicks(int pid) {
#ifdef Q_OS_LINUX
    if (pid <= 0) {
        return 0;
    }
    // /proc/<pid>/stat 的第 22 个字段是进程启动时刻（clock tick）。
    // 进程名可能含空格与括号，所以从最后一个 ')' 之后开始切分。
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QByteArray content = stat.readAll();
    const qsizetype paren = content.lastIndexOf(')');
    if (paren < 0) {
        return 0;
    }
    const QList<QByteArray> fields = content.mid(paren + 2).trimmed().split(' ');
    // 从 ')' 之后第一个字段（state，第 3 个）数起，starttime 是第 20 个 → 下标 19。
    constexpr int kStartTimeIndex = 19;
    if (fields.size() <= kStartTimeIndex) {
        return 0;
    }
    bool ok = false;
    const qint64 ticks = fields.at(kStartTimeIndex).toLongLong(&ok);
    return ok ? ticks : 0;
#else
    Q_UNUSED(pid)
    return 0;
#endif
}

bool processAlive(int pid, qint64 startTicks) {
    if (pid <= 0) {
        return false;
    }
#ifdef Q_OS_UNIX
    // kill(pid, 0) 只回答"这个 pid 是否存在"，不回答"是不是原来那个进程"。
    // pid 会被复用，所以有指纹时必须核对。
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM) {
        return false;
    }
    if (startTicks > 0) {
        const qint64 current = processStartTicks(pid);
        if (current > 0 && current != startTicks) {
            return false;  // pid 被复用了
        }
    }
    return true;
#else
    Q_UNUSED(startTicks)
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// 路径与账本
// ─────────────────────────────────────────────────────────────────────────────

QString BackgroundTaskRegistry::defaultOutputDirectory() {
    const QString directory = dataRoot() + QStringLiteral("/qt/tasks");
    if (!QDir().mkpath(directory)) {
        qCWarning(log) << "后台任务输出目录创建失败:" << directory;
        return {};
    }
    return directory;
}

QString BackgroundTaskRegistry::ledgerPath() {
    const QString directory = defaultOutputDirectory();
    if (directory.isEmpty()) {
        return {};
    }
    return directory + QStringLiteral("/ledger.json");
}

QString BackgroundTaskRegistry::outputPathForTask(const QString &taskId) {
    const QString directory = defaultOutputDirectory();
    if (directory.isEmpty() || taskId.isEmpty()) {
        return {};
    }
    return directory + QLatin1Char('/') + taskId + QStringLiteral(".log");
}

QString BackgroundTaskRegistry::exitCodePathFor(const QString &outputPath) {
    return outputPath.isEmpty() ? QString() : outputPath + QStringLiteral(".exit");
}

namespace {

/// 读退出码旁路文件；不存在或格式不对时返回 -1。
int readExitCodeFile(const QString &path) {
    if (path.isEmpty()) {
        return -1;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return -1;
    }
    bool ok = false;
    const int code = QString::fromUtf8(file.readAll()).trimmed().toInt(&ok);
    return ok ? code : -1;
}

}  // namespace

QString BackgroundTaskRegistry::allocateTaskId() const {
    return newId(QStringLiteral("task"));
}

qint64 BackgroundTask::durationMs() const {
    if (startedAtMs == 0) {
        return 0;
    }
    const TimestampMs end = endedAtMs > 0 ? endedAtMs : nowMs();
    return end - startedAtMs;
}

void BackgroundTaskRegistry::persistLedger() const {
    const QString path = ledgerPath();
    if (path.isEmpty()) {
        return;
    }

    QJsonArray array;
    for (const Entry &entry : entries_) {
        const BackgroundTask &task = entry.task;
        QJsonObject item;
        item.insert(QStringLiteral("id"), task.id);
        item.insert(QStringLiteral("type"), task.type);
        item.insert(QStringLiteral("description"), task.description);
        item.insert(QStringLiteral("command"), task.command);
        item.insert(QStringLiteral("status"), task.status);
        item.insert(QStringLiteral("outputPath"), task.outputPath);
        item.insert(QStringLiteral("exitCode"), task.exitCode);
        item.insert(QStringLiteral("startedAtMs"), static_cast<double>(task.startedAtMs));
        item.insert(QStringLiteral("endedAtMs"), static_cast<double>(task.endedAtMs));
        item.insert(QStringLiteral("killed"), task.killed);
        item.insert(QStringLiteral("autoBackgrounded"), task.autoBackgrounded);
        item.insert(QStringLiteral("pid"), task.pid);
        item.insert(QStringLiteral("pidStartTicks"), static_cast<double>(entry.pidStartTicks));
        item.insert(QStringLiteral("outputBytes"), static_cast<double>(task.outputBytes));
        // 未投递的通知也带上：宿主退出前没来得及投递的通知，重启后不该丢。
        item.insert(QStringLiteral("notification"), entry.notification);
        array.append(item);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("tasks"), array);

    // 原子写：账本损坏会让所有后台任务失去追踪。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(log) << "账本写入失败:" << path << file.errorString();
        return;
    }
    file.write(json::toPrettyBytes(root));
    if (!file.commit()) {
        qCWarning(log) << "账本提交失败:" << path << file.errorString();
    }
}

void BackgroundTaskRegistry::reconcile() {
    const QString path = ledgerPath();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(log) << "账本读取失败:" << path << file.errorString();
        return;
    }
    QString parseError;
    const QJsonObject root = json::parseObject(file.readAll(), &parseError);
    if (!parseError.isEmpty()) {
        qCWarning(log) << "账本格式错误，忽略:" << parseError;
        return;
    }

    const QJsonArray array = json::array(root, QStringLiteral("tasks"));
    int recovered = 0;
    int lost = 0;

    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject item = value.toObject();

        Entry entry;
        entry.task.id = json::str(item, QStringLiteral("id"));
        entry.task.type = json::str(item, QStringLiteral("type"));
        entry.task.description = json::str(item, QStringLiteral("description"));
        entry.task.command = json::str(item, QStringLiteral("command"));
        entry.task.status =
            json::str(item, QStringLiteral("status"), QStringLiteral("running"));
        entry.task.outputPath = json::str(item, QStringLiteral("outputPath"));
        entry.task.exitCode = json::integer(item, QStringLiteral("exitCode"), -1);
        entry.task.startedAtMs = json::integer64(item, QStringLiteral("startedAtMs"));
        entry.task.endedAtMs = json::integer64(item, QStringLiteral("endedAtMs"));
        entry.task.killed = json::boolean(item, QStringLiteral("killed"));
        entry.task.autoBackgrounded = json::boolean(item, QStringLiteral("autoBackgrounded"));
        entry.task.pid = json::integer(item, QStringLiteral("pid"));
        entry.task.outputBytes = json::integer64(item, QStringLiteral("outputBytes"));
        entry.pidStartTicks = json::integer64(item, QStringLiteral("pidStartTicks"));
        entry.notification = json::str(item, QStringLiteral("notification"));

        if (entry.task.id.isEmpty()) {
            continue;
        }

        if (entry.task.status == QLatin1String("running")) {
            if (processAlive(entry.task.pid, entry.pidStartTicks)) {
                // 还活着：认领它。之后靠轮询文件与存活状态跟踪。
                entry.task.recovered = true;
                entry.detached = true;
                entry.task.detached = true;
                entry.finishedEmitted = false;
                pollDetachedOutput(entry);  // 已有输出立刻可见
                ++recovered;
                qCInfo(log) << "认领上次遗留的后台任务; id=" << entry.task.id
                            << "pid=" << entry.task.pid;
            } else {
                // 进程没了，而且**退出码不可知**（孤儿进程的退出状态不会被任何人收）。
                // 如实标成 lost，而不是猜一个 0 让模型以为成功了。
                entry.task.status = QStringLiteral("lost");
                entry.task.exitCode = -1;
                if (entry.task.endedAtMs == 0) {
                    entry.task.endedAtMs = nowMs();
                }
                entry.finishedEmitted = true;
                if (entry.notification.isEmpty()) {
                    entry.notification = buildNotification(entry);
                }
                ++lost;
                qCDebug(log) << "上次遗留的后台任务已结束（退出码不可知）; id="
                             << entry.task.id;
            }
        } else {
            entry.finishedEmitted = true;
        }

        entries_.append(entry);
    }

    qCInfo(log) << "后台任务账本对账完成; 认领=" << recovered << "已结束=" << lost
                << "总条目=" << entries_.size();

    if (recovered > 0) {
        ensureLivenessTimer();
    }
    persistLedger();  // 认领/判死的最新状态立刻回写
}

// ─────────────────────────────────────────────────────────────────────────────
// 构造与析构
// ─────────────────────────────────────────────────────────────────────────────

BackgroundTaskRegistry::BackgroundTaskRegistry(QObject *parent) : QObject(parent) {
    reconcile();
}

BackgroundTaskRegistry::~BackgroundTaskRegistry() {
    // 析构**不杀**分离式任务：它们的设计目标就是活过宿主。
    // 写进账本，下次启动由 reconcile() 认领。
    detachAll();
    for (Entry &entry : entries_) {
        if (entry.logFile != nullptr) {
            entry.logFile->close();
            delete entry.logFile;
            entry.logFile = nullptr;
        }
    }
}

void BackgroundTaskRegistry::ensureLivenessTimer() {
    if (livenessTimer_ == nullptr) {
        livenessTimer_ = new QTimer(this);
        livenessTimer_->setInterval(kLivenessPollIntervalMs);
        connect(livenessTimer_, &QTimer::timeout, this, [this]() {
            for (Entry &entry : entries_) {
                if (entry.task.status != QLatin1String("running") || !entry.detached) {
                    continue;  // 管道式任务由 QProcess 信号驱动，不需要轮询
                }
                pollDetachedOutput(entry);
                if (!processAlive(entry.task.pid, entry.pidStartTicks)) {
                    // 退出码拿不到（孤儿进程没人 wait），如实记 -1。
                    handleFinished(entry, -1, entry.task.killed);
                }
            }
            persistLedger();
        });
    }
    if (!livenessTimer_->isActive()) {
        livenessTimer_->start();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 接管
// ─────────────────────────────────────────────────────────────────────────────

QString BackgroundTaskRegistry::adopt(const AdoptRequest &request) {
    if (request.detached) {
        if (request.pid <= 0) {
            qCWarning(log) << "拒绝接管分离式任务：需要有效 pid";
            return {};
        }
    } else {
        if (request.process == nullptr) {
            qCWarning(log) << "拒绝接管空进程";
            return {};
        }
        if (request.process->state() != QProcess::NotRunning &&
            !request.takeOverRunningProcess) {
            qCWarning(log) << "拒绝接管已经在运行的进程（避免与既有读取器抢输出）";
            return {};
        }
    }

    Entry entry;
    entry.task.id = request.taskId.isEmpty() ? allocateTaskId() : request.taskId;
    entry.task.type = request.type;
    entry.task.description = request.description;
    entry.task.command = request.command;
    entry.task.status = QStringLiteral("running");
    entry.task.startedAtMs = nowMs();
    entry.task.autoBackgrounded = request.autoBackgrounded;
    entry.process = request.detached ? nullptr : request.process;
    entry.detached = request.detached;
    entry.task.detached = request.detached;

    QString path = request.outputPath;
    if (path.isEmpty()) {
        path = outputPathForTask(entry.task.id);
    }
    entry.task.outputPath = path;

    // 自动后台化时把前台已读到的输出带过来，历史不丢。
    if (!request.initialOutput.isEmpty()) {
        const QByteArray seed = request.initialOutput.toUtf8();
        entry.preview = seed.right(kPreviewBytes);
        entry.task.outputPreview = QString::fromUtf8(entry.preview);
        entry.task.outputBytes = seed.size();
    }

    // 分离式任务的输出由 QProcess 直接写文件（调用方在 start 前设好重定向），
    // 我们不再持文件句柄，改为轮询；管道式任务自己开文件边读边写。
    if (!entry.detached && !path.isEmpty()) {
        auto *file = new QFile(path);
        if (file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (!request.initialOutput.isEmpty()) {
                file->write(request.initialOutput.toUtf8());
                file->flush();
            }
            entry.logFile = file;
        } else {
            qCWarning(log) << "后台任务输出文件打开失败:" << path << file->errorString();
            delete file;
        }
    }

    if (!request.detached) {
        request.process->setParent(this);
    }

    entries_.append(std::move(entry));
    Entry *stored = &entries_.last();
    // 分离式任务直接带 pid；管道式任务要等 start 之后才有，调用方随后补记。
    stored->task.pid = request.detached ? request.pid
                                        : static_cast<int>(request.process->processId());
    if (stored->task.pid > 0) {
        stored->pidStartTicks = processStartTicks(stored->task.pid);
    }
    const QString taskId = stored->task.id;

    QProcess *watched = stored->process;
    if (!stored->detached && watched != nullptr) {
        connect(watched, &QProcess::readyReadStandardOutput, this, [this, taskId]() {
            if (Entry *target = findEntry(taskId)) {
                readAvailable(*target);
                emit taskUpdated(taskId);
            }
        });
        connect(watched, &QProcess::readyReadStandardError, this, [this, taskId]() {
            if (Entry *target = findEntry(taskId)) {
                readAvailable(*target);
                emit taskUpdated(taskId);
            }
        });
    }
    if (watched != nullptr) {
    connect(watched, &QProcess::errorOccurred, this,
            [this, taskId](QProcess::ProcessError error) {
                Entry *target = findEntry(taskId);
                if (target == nullptr || target->task.endedAtMs > 0) {
                    return;
                }
                qCWarning(log) << "后台任务进程错误; task=" << taskId
                               << "error=" << static_cast<int>(error);
                if (!target->detached) {
                    readAvailable(*target);
                }
                handleFinished(*target, -1, target->task.killed);
            });
    connect(watched, &QProcess::finished, this,
            [this, taskId](int exitCode, QProcess::ExitStatus status) {
                Entry *target = findEntry(taskId);
                if (target == nullptr) {
                    return;
                }
                if (!target->detached) {
                    readAvailable(*target);
                }
                const bool killed = target->task.killed || status == QProcess::CrashExit;
                handleFinished(*target, exitCode, killed);
            });
    }

    qCInfo(log) << "后台任务已登记; id=" << taskId << "detached=" << stored->detached
                << "auto=" << request.autoBackgrounded << "outputPath=" << path;
    ensureLivenessTimer();
    persistLedger();
    emit taskAdded(taskId);
    return taskId;
}

/// 进程 start 之后补记 pid 与启动指纹（构造时 processId 还是 0）。
void BackgroundTaskRegistry::recordProcessIdentity(const QString &taskId) {
    Entry *entry = findEntry(taskId);
    if (entry == nullptr || entry->process == nullptr) {
        return;  // 分离式任务在 adopt 时就带了 pid，不需要补记
    }
    entry->task.pid = static_cast<int>(entry->process->processId());
    if (entry->task.pid > 0) {
        entry->pidStartTicks = processStartTicks(entry->task.pid);
    }
    persistLedger();
}

void BackgroundTaskRegistry::readAvailable(Entry &entry) {
    if (entry.process == nullptr) {
        return;
    }
    const QByteArray chunk = entry.process->readAllStandardOutput() +
                             entry.process->readAllStandardError();
    if (chunk.isEmpty()) {
        return;
    }

    entry.task.outputBytes += chunk.size();
    if (entry.logFile != nullptr) {
        entry.logFile->write(chunk);
        entry.logFile->flush();  // 落盘即时可见：TaskOutput 与 Read 都可能立刻来读
    }

    // 内存里只留尾部：预览的用途是"判断任务在干什么"，不是完整留档。
    entry.preview.append(chunk);
    if (entry.preview.size() > kPreviewBytes) {
        entry.preview = entry.preview.right(kPreviewBytes);
    }
    entry.task.outputPreview = QString::fromUtf8(entry.preview);
}

void BackgroundTaskRegistry::pollDetachedOutput(Entry &entry) {
    if (entry.task.outputPath.isEmpty()) {
        return;
    }
    QFile file(entry.task.outputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;  // 进程可能还没写出第一块
    }

    const qint64 size = file.size();
    // 文件被截断（例如进程自己重开了日志）时回到开头重读。
    if (size < entry.fileReadOffset) {
        entry.fileReadOffset = 0;
        entry.preview.clear();
        entry.task.outputBytes = 0;
    }
    if (size == entry.fileReadOffset || !file.seek(entry.fileReadOffset)) {
        return;
    }
    const QByteArray chunk = file.read(size - entry.fileReadOffset);
    entry.fileReadOffset = size;
    entry.task.outputBytes += chunk.size();

    entry.preview.append(chunk);
    if (entry.preview.size() > kPreviewBytes) {
        entry.preview = entry.preview.right(kPreviewBytes);
    }
    entry.task.outputPreview = QString::fromUtf8(entry.preview);
}

void BackgroundTaskRegistry::handleFinished(Entry &entry, int exitCode, bool killed) {
    if (entry.finishedEmitted) {
        return;  // 幂等：errorOccurred 与 finished 可能都到达
    }
    entry.finishedEmitted = true;

    // stop() 设置的"主动终止"标记是权威的：被 SIGKILL 的进程会先触发
    // errorOccurred(Crashed)，那条路径不知道是我们杀的，若以它为准就会把
    // 主动终止记成"运行失败"。
    const bool wasKilled = killed || entry.task.killed;

    // 分离式任务没有 wait()，调用方传进来的 exitCode 通常是 -1。
    // 从旁路文件把真实退出码取回来，否则一个成功退出的任务会被报成"失败"。
    int effectiveExitCode = exitCode;
    if (!wasKilled && effectiveExitCode < 0 && entry.detached) {
        const int fromFile = readExitCodeFile(exitCodePathFor(entry.task.outputPath));
        if (fromFile >= 0) {
            effectiveExitCode = fromFile;
        }
    }

    entry.task.endedAtMs = nowMs();
    entry.task.exitCode = effectiveExitCode;
    entry.task.killed = wasKilled;

    if (wasKilled) {
        entry.task.status = QStringLiteral("killed");
    } else if (effectiveExitCode == 0) {
        entry.task.status = QStringLiteral("completed");
    } else {
        // exitCode < 0 且不是我们杀的：进程消失而退出码不可知（孤儿子进程），
        // 或者是崩溃。两者都如实记为 failed，不猜退出码。
        entry.task.status = QStringLiteral("failed");
    }

    // 收尾前把剩余输出读干净。
    if (entry.detached) {
        pollDetachedOutput(entry);
    } else {
        readAvailable(entry);
    }

    if (entry.logFile != nullptr) {
        entry.logFile->flush();
        entry.logFile->close();
    }

    entry.notification = buildNotification(entry);

    const bool ok = !wasKilled && effectiveExitCode == 0;
    qCInfo(log) << "后台任务结束; id=" << entry.task.id << "status=" << entry.task.status
                << "exitCode=" << effectiveExitCode << "bytes=" << entry.task.outputBytes
                << "durationMs=" << entry.task.durationMs();
    persistLedger();
    emit taskUpdated(entry.task.id);
    emit taskFinished(entry.task.id, ok, exitCode);
}

QString BackgroundTaskRegistry::buildNotification(const Entry &entry) {
    // <task-notification> 同形：模型读到的是一段结构化文本，
    // 而不是一句自由描述，便于它稳定地解析出"哪个任务、什么结果、去哪看完整输出"。
    const BackgroundTask &task = entry.task;

    // 尾部预览可能很长，通知里再收一次，避免把上下文挤满。
    const QString tail = json::truncate(task.outputPreview.trimmed(), 2000,
                                        QStringLiteral("\n…[更多输出见 output-path]"));

    QStringList lines;
    lines << QStringLiteral("<task-notification>");
    lines << QStringLiteral("<task-id>%1</task-id>").arg(task.id);
    lines << QStringLiteral("<task-type>%1</task-type>").arg(task.type);
    lines << QStringLiteral("<status>%1</status>").arg(task.status);
    lines << QStringLiteral("<exit-code>%1</exit-code>").arg(task.exitCode);
    if (!task.description.isEmpty()) {
        lines << QStringLiteral("<description>%1</description>").arg(task.description);
    }
    if (!task.outputPath.isEmpty()) {
        lines << QStringLiteral("<output-path>%1</output-path>").arg(task.outputPath);
    }
    lines << QStringLiteral("<bytes>%1</bytes>").arg(task.outputBytes);
    lines << QStringLiteral("<duration-ms>%1</duration-ms>").arg(task.durationMs());
    if (task.recovered) {
        // 恢复出来的任务退出码不可知，明确告诉模型不要把它当成成功。
        lines << QStringLiteral("<recovered>true</recovered>");
    }
    if (!tail.isEmpty()) {
        lines << QStringLiteral("<output-tail>");
        lines << tail;
        lines << QStringLiteral("</output-tail>");
    }
    lines << QStringLiteral("</task-notification>");
    return lines.join(QLatin1Char('\n'));
}

QString BackgroundTaskRegistry::takeNotification(const QString &taskId) {
    Entry *entry = findEntry(taskId);
    if (entry == nullptr || entry->notification.isEmpty()) {
        return {};
    }
    const QString notification = entry->notification;
    entry->notification.clear();
    persistLedger();
    return notification;
}

bool BackgroundTaskRegistry::hasPendingNotification() const {
    for (const Entry &entry : entries_) {
        if (!entry.notification.isEmpty()) {
            return true;
        }
    }
    return false;
}

QList<BackgroundTask> BackgroundTaskRegistry::tasks() const {
    QList<BackgroundTask> result;
    result.reserve(entries_.size());
    for (const Entry &entry : entries_) {
        result.append(entry.task);
    }
    return result;
}

BackgroundTask BackgroundTaskRegistry::task(const QString &taskId) const {
    const Entry *entry = findEntry(taskId);
    return entry != nullptr ? entry->task : BackgroundTask{};
}

bool BackgroundTaskRegistry::contains(const QString &taskId) const {
    return findEntry(taskId) != nullptr;
}

BackgroundTaskRegistry::Entry *BackgroundTaskRegistry::findEntry(const QString &taskId) {
    for (Entry &entry : entries_) {
        if (entry.task.id == taskId) {
            return &entry;
        }
    }
    return nullptr;
}

const BackgroundTaskRegistry::Entry *BackgroundTaskRegistry::findEntry(
    const QString &taskId) const {
    for (const Entry &entry : entries_) {
        if (entry.task.id == taskId) {
            return &entry;
        }
    }
    return nullptr;
}

int BackgroundTaskRegistry::runningCount() const {
    int count = 0;
    for (const Entry &entry : entries_) {
        if (entry.task.isRunning()) {
            ++count;
        }
    }
    return count;
}

bool BackgroundTaskRegistry::stop(const QString &taskId) {
    Entry *entry = findEntry(taskId);
    if (entry == nullptr) {
        qCDebug(log) << "停止后台任务：找不到该 id" << taskId;
        return false;
    }
    if (!entry->task.isRunning()) {
        // 幂等边界：UI 可能重复点停止，已结束的任务直接返回 false 而不是报错。
        return false;
    }

    entry->task.killed = true;
    qCInfo(log) << "停止后台任务; id=" << taskId << "pid=" << entry->task.pid
                << "detached=" << entry->detached;

    if (entry->detached) {
        // 分离式任务的进程可能还是上次宿主启动的，QProcess 那边不会有
        // finished 信号可等，所以直接按 pid 发信号并立即收尾。
        if (!processAlive(entry->task.pid, entry->pidStartTicks)) {
            handleFinished(*entry, -1, true);
            return true;
        }
#ifdef Q_OS_UNIX
        // 进程组也发一次：命令可能自己 fork 过，只杀组长会留孤儿。
        ::kill(-static_cast<pid_t>(entry->task.pid), SIGKILL);
        ::kill(static_cast<pid_t>(entry->task.pid), SIGKILL);
#endif
        handleFinished(*entry, -1, true);
        return true;
    }

    toolutil::killProcessGroup(entry->process);
    // finished 信号会异步到达并完成收尾；这里不直接改状态，
    // 避免"已标记结束但进程仍活着"的窗口。
    return true;
}

void BackgroundTaskRegistry::stopAll() {
    QStringList ids;
    for (const Entry &entry : entries_) {
        if (entry.task.isRunning()) {
            ids.append(entry.task.id);
        }
    }
    for (const QString &id : ids) {
        stop(id);
    }
}

void BackgroundTaskRegistry::detachAll() {
    for (Entry &entry : entries_) {
        // 管道式任务（前台超时后自动后台化的）无法脱离宿主：它的输出来自
        // 我们持有的管道，宿主一退出子进程就会拿到 EPIPE。如实告警，不假装它还能活。
        if (entry.task.isRunning() && !entry.detached) {
            qCWarning(log) << "管道式后台任务无法跨重启存活，将随宿主退出; id="
                           << entry.task.id;
        }
    }
    persistLedger();
}

}  // namespace lycode

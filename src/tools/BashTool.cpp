// ZCode Qt — Bash 工具实现
//
// 关键实现点（每一条都对应一个真实故障场景）：
//   1. `/bin/bash -lc <command>`：`-l` 让登录 shell 的 PATH 生效。GUI 进程从
//      Finder/Dock 启动时 PATH 往往只有 /usr/bin:/bin，不加 -l 会导致
//      `node`、`cargo`、`brew` 全都"找不到命令"。
//   2. setsid + kill(-pid)：命令可能自己再 fork（`npm run dev` 会拉起一堆
//      子进程）。只 kill 直接子进程会留下孤儿进程继续跑；建立独立进程组后
//      对整组发信号才能真正终止。
//   3. stdout/stderr 分开累积：模型需要区分"正常输出"和"报错"，合并成一路
//      会丢失这个信息。展示时再拼接。
//   4. 输出走 OutputBudget：命令可以无限输出（`yes`），必须有硬上限，
//      否则一次工具调用就能吃掉几 GB 内存。
//   5. 取消用 100ms 轮询而不是信号/条件变量：ToolContext 只提供
//      isCancelled()，轮询是唯一无侵入的方式，且 100ms 的延迟对交互无感。
#include "tools/BashTool.h"

#include "core/Json.h"
#include "tools/ToolUtils.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
#include <QTimer>

#include <csignal>
#include <algorithm>
#include <memory>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.tool.bash")

constexpr int kDefaultTimeoutMs = 120000;
constexpr int kMinTimeoutMs = 1000;
constexpr int kMaxTimeoutMs = 600000;
constexpr int kMaxOutputBytes = 10'000'000;

/// 取消轮询间隔。100ms 是"用户感觉不到延迟"与"不浪费 CPU"之间的折中。
constexpr int kCancelPollIntervalMs = 100;

/// 终止整个进程组。
/// setsid() 让子进程成为新会话的组长，其 pgid == pid，因此 kill(-pid) 命中整组。
void killProcessGroup(QProcess *process) {
#ifdef Q_OS_UNIX
    const qint64 pid = process->processId();
    if (pid > 0) {
        // SIGKILL 而不是 SIGTERM：工具被取消/超时后必须立即释放资源，
        // 不做"优雅退出"协商（模型可以自己再发一条命令处理收尾）。
        ::kill(-static_cast<pid_t>(pid), SIGKILL);
    }
#endif
    // 兜底：进程组信号失败（或非 Unix）时至少杀掉直接子进程。
    if (process->state() != QProcess::NotRunning) {
        process->kill();
    }
}

}  // namespace

ToolMetadata BashTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("Bash");
    meta.description = QStringLiteral(
        "Executes a shell command in the workspace. Output is captured and "
        "returned; long-running commands must be kept under the timeout.");
    meta.modelInstructions = QStringLiteral(
        "Commands run through `bash -lc`, so the user's PATH is available. "
        "Avoid interactive commands. timeout is in milliseconds "
        "(default 120000, max 600000). Background execution is not supported yet.");
    meta.allowedInPlanMode = false;
    meta.readOnly = false;
    meta.destructive = true;
    meta.concurrency = ToolMetadata::Concurrency::Serial;
    meta.requiresUserInteraction = false;
    meta.timeoutMs = kDefaultTimeoutMs;
    meta.maxOutputBytes = kMaxOutputBytes;
    meta.sideEffectScope = SideEffectScope::System;
    meta.riskLevel = RiskLevel::High;
    meta.needsApproval = true;
    meta.alwaysAsk = false;
    meta.providerVisible = true;
    meta.stopTurnOnSuccess = false;
    meta.idempotent = false;
    return meta;
}

QJsonObject BashTool::inputSchema() const {
    QJsonObject properties;
    properties.insert(QStringLiteral("command"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("要执行的 shell 命令")}});
    properties.insert(QStringLiteral("timeout"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("超时毫秒数（默认 120000，范围 1000-600000）")}});
    properties.insert(QStringLiteral("description"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("这次命令做什么（5-10 词，用于 UI 展示）")}});
    properties.insert(QStringLiteral("run_in_background"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("后台执行（本阶段未实现，传 true 会失败）")}});
    properties.insert(QStringLiteral("dangerouslyDisableSandbox"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("请求关闭沙箱（当前无沙箱实现，仅记录）")}});
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("command")}},
    };
}

QString BashTool::title(const QJsonObject &input) const {
    QString command = json::str(input, QStringLiteral("command"));
    if (command.isEmpty()) {
        return QStringLiteral("Bash");
    }
    command.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return QStringLiteral("Bash: ") + toolutil::redactForLog(command, 120);
}

QString BashTool::validateInput(const QJsonObject &input) const {
    const QString base = Tool::validateInput(input);
    if (!base.isEmpty()) {
        return base;
    }
    if (json::str(input, QStringLiteral("command")).trimmed().isEmpty()) {
        return QStringLiteral("command 不能为空");
    }
    return {};
}

QString BashTool::permissionCapability() const {
    // 权限能力名与工具名解耦：规则写成 `bash`，与 npm 版词表一致。
    return QStringLiteral("bash");
}

void BashTool::execute(const QJsonObject &input, const ToolContext &context, ToolCallback done) {
    const ToolMetadata meta = metadata();

    // finish 只允许生效一次：超时定时器、取消轮询、finished 信号可能在
    // 同一个事件循环批次里都尝试收尾，护栏保证回调恰好一次。
    struct State {
        bool finished = false;
        bool timedOut = false;
        bool cancelled = false;
    };
    auto state = std::make_shared<State>();
    auto finish = [state, done](ToolResult result) {
        if (state->finished) {
            return;
        }
        state->finished = true;
        done(std::move(result));
    };

    const QString command = json::str(input, QStringLiteral("command"));
    const QString description = json::str(input, QStringLiteral("description"));
    const bool runInBackground = json::boolean(input, QStringLiteral("run_in_background"), false);
    const bool sandboxDisabled =
        json::boolean(input, QStringLiteral("dangerouslyDisableSandbox"), false);
    int timeoutMs = json::integer(input, QStringLiteral("timeout"), kDefaultTimeoutMs);
    timeoutMs = std::max(kMinTimeoutMs, std::min(kMaxTimeoutMs, timeoutMs));

    qCDebug(log) << "Bash 参数; command=" << toolutil::redactForLog(command)
                 << "description=" << toolutil::redactForLog(description, 120)
                 << "timeoutMs=" << timeoutMs << "background=" << runInBackground;
    if (sandboxDisabled) {
        // 当前没有沙箱层：明确记录"模型要求关沙箱"，而不是假装有沙箱可用。
        qCWarning(log) << "模型请求关闭沙箱，但本阶段没有沙箱实现（命令始终直接执行）";
    }

    if (context.isCancelled()) {
        finish(ToolResult::failure(QStringLiteral("执行已取消"), QStringLiteral("cancelled")));
        return;
    }
    if (command.trimmed().isEmpty()) {
        finish(ToolResult::failure(QStringLiteral("command 不能为空"),
                                   QStringLiteral("invalid_input")));
        return;
    }
    if (runInBackground) {
        // 有意未实现：真正的后台任务需要任务表、输出落盘、恢复与清理。
        // 谎报成功会让模型以为进程在跑，从而做出错误的后续决策。
        finish(ToolResult::failure(
            QStringLiteral("run_in_background is not supported yet"),
            QStringLiteral("unsupported")));
        return;
    }

    // 只读模式下 Bash 属于写入集（System），直接拒绝。
    if (toolutil::shouldRejectForReadOnly(meta, context)) {
        qCWarning(log) << "只读模式拒绝 Bash:" << toolutil::redactForLog(command);
        finish(toolutil::readOnlyModeFailure(meta));
        return;
    }

    // 工作目录：workingDirectory 优先，其次工作区路径，最后当前目录。
    QString cwd = context.workingDirectory;
    if (cwd.isEmpty()) {
        cwd = context.workspace.path;
    }
    if (cwd.isEmpty()) {
        cwd = QDir::currentPath();
    }
    if (!QFileInfo(cwd).isDir()) {
        finish(ToolResult::failure(QStringLiteral("工作目录不存在：%1").arg(cwd),
                                   QStringLiteral("invalid_cwd")));
        return;
    }

#ifdef Q_OS_WIN
    const QString program = qEnvironmentVariable("ComSpec", QStringLiteral("cmd.exe"));
    const QStringList arguments{QStringLiteral("/c"), command};
#else
    // `/bin/bash` 在极简容器里可能不存在；退回 /bin/sh 也比直接失败好。
    const QString program = QFileInfo::exists(QStringLiteral("/bin/bash"))
                                ? QStringLiteral("/bin/bash")
                                : QStringLiteral("/bin/sh");
    const QStringList arguments{QStringLiteral("-lc"), command};
#endif

    auto *process = new QProcess();
    process->setProgram(program);
    process->setArguments(arguments);
    process->setWorkingDirectory(cwd);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    // 命令的工作目录（cwd）与 conpty/编码无关，这里不动 locale，
    // 让 shell 继承进程环境，行为与用户终端一致。
#ifdef Q_OS_UNIX
    // 建立独立进程组：超时/取消时可以对整组发信号。
    process->setChildProcessModifier([]() { ::setsid(); });
#endif

    auto stdoutBudget = std::make_shared<toolutil::OutputBudget>(meta.maxOutputBytes);
    auto stderrBudget = std::make_shared<toolutil::OutputBudget>(meta.maxOutputBytes);
    auto stdoutRemainder = std::make_shared<QString>();
    auto startedAt = std::make_shared<QElapsedTimer>();
    startedAt->start();

    // stdout 增量推给 UI：按完整行推送，避免每个字节都触发一次 UI 更新。
    QObject::connect(process, &QProcess::readyReadStandardOutput, process,
                     [process, stdoutBudget, stdoutRemainder, context]() {
                         const QByteArray chunk = process->readAllStandardOutput();
                         stdoutBudget->appendBytes(chunk);
                         if (!context.progress) {
                             return;
                         }
                         *stdoutRemainder += QString::fromUtf8(chunk);
                         int newlineIndex = stdoutRemainder->indexOf(QLatin1Char('\n'));
                         while (newlineIndex >= 0) {
                             const QString line = stdoutRemainder->left(newlineIndex);
                             stdoutRemainder->remove(0, newlineIndex + 1);
                             context.progress(line);
                             newlineIndex = stdoutRemainder->indexOf(QLatin1Char('\n'));
                         }
                     });
    QObject::connect(process, &QProcess::readyReadStandardError, process,
                     [process, stderrBudget]() {
                         stderrBudget->appendBytes(process->readAllStandardError());
                     });

    auto *timeoutTimer = new QTimer(process);
    timeoutTimer->setSingleShot(true);
    timeoutTimer->setInterval(timeoutMs);
    QObject::connect(timeoutTimer, &QTimer::timeout, process, [process, state, timeoutMs]() {
        if (process->state() == QProcess::NotRunning) {
            return;
        }
        state->timedOut = true;
        qCWarning(log) << "Bash 超时，终止进程组; timeoutMs=" << timeoutMs;
        killProcessGroup(process);
    });

    auto *cancelTimer = new QTimer(process);
    cancelTimer->setInterval(kCancelPollIntervalMs);
    QObject::connect(cancelTimer, &QTimer::timeout, process, [process, state, context]() {
        if (!context.isCancelled() || process->state() == QProcess::NotRunning) {
            return;
        }
        state->cancelled = true;
        qCInfo(log) << "Bash 收到取消，终止进程组";
        killProcessGroup(process);
    });

    QObject::connect(process, &QProcess::errorOccurred, process,
                     [process, finish, state, cwd](QProcess::ProcessError error) {
                         if (error != QProcess::FailedToStart) {
                             return;
                         }
                         qCCritical(log) << "Bash 无法启动进程:" << process->errorString();
                         process->deleteLater();
                         QJsonObject metadata;
                         metadata.insert(QStringLiteral("cwd"), cwd);
                         metadata.insert(QStringLiteral("exitCode"), -1);
                         metadata.insert(QStringLiteral("timedOut"), state->timedOut);
                         metadata.insert(QStringLiteral("truncated"), false);
                         finish(ToolResult::failure(
                             QStringLiteral("无法启动命令进程：%1").arg(process->errorString()),
                             QStringLiteral("spawn_failed"), metadata));
                     });

    QObject::connect(process, &QProcess::finished, process,
                     [process, finish, state, stdoutBudget, stderrBudget, stdoutRemainder,
                      startedAt, context, cwd, program, description, sandboxDisabled,
                      timeoutMs](int exitCode, QProcess::ExitStatus exitStatus) {
                         // 读干剩余缓冲：readyRead 可能还没送达最后一块数据。
                         stdoutBudget->appendBytes(process->readAllStandardOutput());
                         stderrBudget->appendBytes(process->readAllStandardError());
                         if (context.progress && !stdoutRemainder->isEmpty()) {
                             context.progress(*stdoutRemainder);
                             stdoutRemainder->clear();
                         }
                         const qint64 durationMs = startedAt->elapsed();
                         process->deleteLater();

                         const bool normalExit = exitStatus == QProcess::NormalExit;
                         const int effectiveExitCode = normalExit ? exitCode : -1;
                         const bool truncated = stdoutBudget->truncated() || stderrBudget->truncated();

                         const QString stdoutText =
                             toolutil::chompTrailingNewlines(stdoutBudget->text());
                         const QString stderrText =
                             toolutil::chompTrailingNewlines(stderrBudget->text());

                         QString combined = stdoutText;
                         if (!stderrText.isEmpty()) {
                             if (!combined.isEmpty()) {
                                 combined += QLatin1Char('\n');
                             }
                             combined += QStringLiteral("[stderr]\n") + stderrText;
                         }
                         if (combined.isEmpty()) {
                             combined = QStringLiteral("(no output)");
                         }

                         QJsonObject metadata;
                         metadata.insert(QStringLiteral("cwd"), cwd);
                         metadata.insert(QStringLiteral("shell"), program);
                         metadata.insert(QStringLiteral("exitCode"), effectiveExitCode);
                         metadata.insert(QStringLiteral("exitStatus"),
                                         normalExit ? QStringLiteral("normal")
                                                    : QStringLiteral("crashed"));
                         metadata.insert(QStringLiteral("durationMs"),
                                         static_cast<double>(durationMs));
                         metadata.insert(QStringLiteral("stdoutBytes"),
                                         static_cast<double>(stdoutBudget->totalBytes()));
                         metadata.insert(QStringLiteral("stderrBytes"),
                                         static_cast<double>(stderrBudget->totalBytes()));
                         metadata.insert(QStringLiteral("timedOut"), state->timedOut);
                         metadata.insert(QStringLiteral("truncated"), truncated);
                         metadata.insert(QStringLiteral("timeoutMs"), timeoutMs);
                         metadata.insert(QStringLiteral("sandbox"), QStringLiteral("none"));
                         metadata.insert(QStringLiteral("sandboxDisabledRequested"), sandboxDisabled);
                         if (!description.isEmpty()) {
                             metadata.insert(QStringLiteral("description"),
                                             toolutil::redactForLog(description, 200));
                         }

                         if (state->cancelled) {
                             qCInfo(log) << "Bash 已取消; durationMs=" << durationMs;
                             ToolResult result = ToolResult::failure(
                                 QStringLiteral("命令已取消"), QStringLiteral("cancelled"),
                                 metadata);
                             result.output = combined;
                             finish(std::move(result));
                             return;
                         }
                         if (state->timedOut) {
                             ToolResult result = ToolResult::failure(
                                 QStringLiteral("命令超时（%1 ms），进程组已被终止。"
                                                "如需更长时间请显式提高 timeout（上限 %2 ms）。")
                                     .arg(timeoutMs)
                                     .arg(kMaxTimeoutMs),
                                 QStringLiteral("timeout"), metadata);
                             result.output = combined;
                             finish(std::move(result));
                             return;
                         }
                         if (!normalExit) {
                             ToolResult result = ToolResult::failure(
                                 QStringLiteral("命令异常结束（未正常退出，可能是段错误或被信号杀死）。"),
                                 QStringLiteral("crashed"), metadata);
                             result.output = combined;
                             finish(std::move(result));
                             return;
                         }
                         if (exitCode != 0) {
                             // 非零退出码是**失败**：模型必须知道命令没成功，
                             // 而不是从输出里自己猜（很多命令失败时输出为空）。
                             qCWarning(log) << "Bash 非零退出码; exitCode=" << exitCode
                                            << "durationMs=" << durationMs;
                             ToolResult result = ToolResult::failure(
                                 QStringLiteral("命令以退出码 %1 结束").arg(exitCode),
                                 QStringLiteral("nonzero_exit"), metadata);
                             result.output = combined;
                             finish(std::move(result));
                             return;
                         }

                         qCInfo(log) << "Bash 完成; exitCode=0"
                                     << "stdoutBytes=" << stdoutBudget->totalBytes()
                                     << "stderrBytes=" << stderrBudget->totalBytes()
                                     << "durationMs=" << durationMs
                                     << "truncated=" << truncated;
                         finish(ToolResult::success(combined, metadata));
                     });

    cancelTimer->start();
    timeoutTimer->start();
    // start() 异步：启动失败走 errorOccurred，不在这里阻塞等待。
    process->start();
}

}  // namespace zcode

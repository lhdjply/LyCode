// LyCode — Bash 工具实现
//
// 关键实现点（每一条都对应一个真实故障场景）：
//   1. `/bin/bash -lc <command>`：`-l` 让登录 shell 的 PATH 生效。GUI 进程从
//      Finder/Dock 启动时 PATH 往往只有 /usr/bin:/bin，不加 -l 会导致
//      `node`、`cargo`、`brew` 全都"找不到命令"。
//   2. setsid + kill(-pid)：命令可能自己再 fork（`make -j` 会拉起一堆
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
#include "tools/BackgroundTaskRegistry.h"
#include "tools/ToolUtils.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <csignal>
#include <algorithm>
#include <memory>

#ifdef Q_OS_UNIX
  #include <unistd.h>
#endif

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.tool.bash")

constexpr int kDefaultTimeoutMs = 120000;
constexpr int kMinTimeoutMs = 1000;
constexpr int kMaxTimeoutMs = 600000;
constexpr int kMaxOutputBytes = 10'000'000;

/// 取消轮询间隔。100ms 是"用户感觉不到延迟"与"不浪费 CPU"之间的折中。
constexpr int kCancelPollIntervalMs = 100;

}  // namespace

ToolMetadata BashTool::metadata() const
{
  ToolMetadata meta;
  meta.name = QStringLiteral("Bash");
  // 描述按平台生成，不能写死一种 shell：工具名叫 Bash，但 Windows 上根本没有
  // bash，告诉模型 `bash -lc` 会让它写出 cmd/PowerShell 解析不了的命令
  // （`for c in gcc; do command -v $c; done` 就是这样失败的，而且报错只剩
  // 一个光秃秃的退出码 1）。契约必须随执行方式走。
  const toolutil::ShellSpec spec = toolutil::shellSpec();
  const bool powershell = !spec.posix && spec.label != QLatin1String("cmd");
  meta.description = QStringLiteral(
                       "Executes a shell command in the workspace. Output is captured and "
                       "returned; long-running commands must be kept under the timeout.");
  if(spec.posix) {
    meta.modelInstructions = QStringLiteral(
                               "Commands run through `%1 -lc`, so the user's PATH is available. "
                               "Avoid interactive commands. timeout is in milliseconds "
                               "(default 120000, max 600000). Background execution is not supported yet.")
                             .arg(spec.label);
  }
  else if(powershell) {
    meta.modelInstructions = QStringLiteral(
                               "This is Windows: commands are executed by %1, which parses "
                               "PowerShell syntax — write PowerShell, NOT bash/POSIX. "
                               "`for c in x; do ...; done`, `command -v`, `/dev/null`, `&&` chains "
                               "and single-quoted strings do not work as they would in bash. "
                               "Use PowerShell idioms (Get-ChildItem, Where-Object, Test-Path, "
                               "Get-Command, $null, 'single quotes', `;` separators); "
                               "separate arguments as an array rather than with backslashes. "
                               "Avoid interactive commands. timeout is in milliseconds "
                               "(default 120000, max 600000). Background execution is not supported yet.")
                             .arg(spec.label);
  }
  else {
    meta.modelInstructions = QStringLiteral(
                               "This is Windows and commands are executed by cmd.exe — write "
                               "cmd syntax, NOT bash/POSIX. `for c in x; do ...; done`, "
                               "`command -v`, `/dev/null` and single-quoted strings do not work. "
                               "Use `where`, `%VAR%`, `>nul 2>&1`, `dir` and `;`-free `&&` chains. "
                               "Avoid interactive commands. timeout is in milliseconds "
                               "(default 120000, max 600000). Background execution is not supported yet.");
  }
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

QJsonObject BashTool::inputSchema() const
{
  QJsonObject properties;
  properties.insert(QStringLiteral("command"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
    {
      QStringLiteral("description"),
      QStringLiteral("要执行的 shell 命令")
    }});
  properties.insert(QStringLiteral("timeout"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
    {
      QStringLiteral("description"),
      QStringLiteral("超时毫秒数（默认 120000，范围 1000-600000）")
    }});
  properties.insert(QStringLiteral("description"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
    {
      QStringLiteral("description"),
      QStringLiteral("这次命令做什么（5-10 词，用于 UI 展示）")
    }});
  properties.insert(QStringLiteral("run_in_background"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
    {
      QStringLiteral("description"),
      QStringLiteral("后台执行（本阶段未实现，传 true 会失败）")
    }});
  properties.insert(QStringLiteral("dangerouslyDisableSandbox"),
  QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
    {
      QStringLiteral("description"),
      QStringLiteral("请求关闭沙箱（当前无沙箱实现，仅记录）")
    }});
  return QJsonObject{
    {QStringLiteral("type"), QStringLiteral("object")},
    {QStringLiteral("properties"), properties},
    {QStringLiteral("required"), QJsonArray{QStringLiteral("command")}},
  };
}

QString BashTool::title(const QJsonObject & input) const
{
  QString command = json::str(input, QStringLiteral("command"));
  if(command.isEmpty()) {
    return QStringLiteral("Bash");
  }
  command.replace(QLatin1Char('\n'), QLatin1Char(' '));
  return QStringLiteral("Bash: ") + toolutil::redactForLog(command, 120);
}

QString BashTool::validateInput(const QJsonObject & input) const
{
  const QString base = Tool::validateInput(input);
  if(!base.isEmpty()) {
    return base;
  }
  if(json::str(input, QStringLiteral("command")).trimmed().isEmpty()) {
    return QStringLiteral("command 不能为空");
  }
  return {};
}

QString BashTool::permissionCapability() const
{
  // 权限能力名与工具名解耦：规则写成 `bash`，与 本实现词表一致。
  return QStringLiteral("bash");
}

void BashTool::execute(const QJsonObject & input, const ToolContext & context, ToolCallback done)
{
  const ToolMetadata meta = metadata();

  // finish 只允许生效一次：超时定时器、取消轮询、finished 信号可能在
  // 同一个事件循环批次里都尝试收尾，护栏保证回调恰好一次。
  struct State {
    bool finished = false;
    /// 前台超时后自动转入后台。
    bool autoBackgrounded = false;
    bool timedOut = false;
    bool cancelled = false;
  };
  auto state = std::make_shared<State>();
  auto finish = [state, done](ToolResult result) {
    if(state->finished) {
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
  if(sandboxDisabled) {
    // 当前没有沙箱层：明确记录"模型要求关沙箱"，而不是假装有沙箱可用。
    qCWarning(log) << "模型请求关闭沙箱，但本阶段没有沙箱实现（命令始终直接执行）";
  }

  if(context.isCancelled()) {
    finish(ToolResult::failure(QStringLiteral("执行已取消"), QStringLiteral("cancelled")));
    return;
  }
  if(command.trimmed().isEmpty()) {
    finish(ToolResult::failure(QStringLiteral("command 不能为空"),
                               QStringLiteral("invalid_input")));
    return;
  }
  // 只读模式下 Bash 属于写入集（System），直接拒绝。
  if(toolutil::shouldRejectForReadOnly(meta, context)) {
    qCWarning(log) << "只读模式拒绝 Bash:" << toolutil::redactForLog(command);
    finish(toolutil::readOnlyModeFailure(meta));
    return;
  }

  // 工作目录：workingDirectory 优先，其次工作区路径，最后当前目录。
  QString cwd = context.workingDirectory;
  if(cwd.isEmpty()) {
    cwd = context.workspace.path;
  }
  if(cwd.isEmpty()) {
    cwd = QDir::currentPath();
  }
  if(!QFileInfo(cwd).isDir()) {
    finish(ToolResult::failure(QStringLiteral("工作目录不存在：%1").arg(cwd),
                               QStringLiteral("invalid_cwd")));
    return;
  }

  // shell 由平台决定，不在这里写 ifdef：Unix 是 `/bin/bash -lc`（登录 shell 的
  // PATH 才包含 node/cargo/brew），Windows 是 PowerShell（pwsh 优先）→ cmd 兜底。
  // 探测逻辑与后台任务包装共用同一份实现，避免"前台能跑、后台换了个 shell"。
  const toolutil::ShellSpec shell = toolutil::shellSpec();
  const QString & program = shell.program;

  // ── 分离式后台任务分支 ──────────────────────────────────────────────────
  // 在创建 QProcess **之前**分流，而且**不持有** QProcess：
  // `~QProcess` 会杀掉仍在运行的进程，那样"活过宿主、跨重启"就无从谈起。
  // 所以用 startDetached 起进程，输出由 shell 自己重定向到文件——
  // 全程没有管道，宿主退出也不会让子进程拿到 EPIPE。
  if(runInBackground) {
    if(context.backgroundTasks == nullptr) {
      finish(ToolResult::failure(
               QStringLiteral("当前环境不支持后台任务（run_in_background）。"),
               QStringLiteral("unsupported")));
      return;
    }

    const QString taskId = context.backgroundTasks->allocateTaskId();
    const QString outputPath = BackgroundTaskRegistry::outputPathForTask(taskId);
    if(outputPath.isEmpty()) {
      finish(ToolResult::failure(QStringLiteral("无法创建后台任务输出目录。"),
                                 QStringLiteral("output_path_unavailable")));
      return;
    }

    // 跑完把退出码写进旁路文件：分离式任务没有 wait() 可取退出码，
    // 没有这一步就永远只能记 -1，一个正常退出的任务会被报成"失败"。
    // 包装写法按 shell 语义分流（POSIX 与 PowerShell/cmd 完全不同）。
    const QString exitPath = BackgroundTaskRegistry::exitCodePathFor(outputPath);
    const QString wrapped =
      toolutil::shellBackgroundWrapper(shell, command, outputPath, exitPath);

    // 用 setsid 起：`QProcess::startDetached` **不会**给子进程建新会话/进程组，
    // 于是记录到的 pid 不是组长，TaskStop 的 kill(-pid) 够不到命令自己 fork 的
    // 子进程（实测漏掉过 `sleep 45`）。setsid exec 掉自己，pid 因此就是组长。
    // Windows 没有 setsid，进程树清理由 TaskStop 走 taskkill /T。
    // 这里的 `-lc` 只在 setsid 分支用：setsid 是 POSIX 程序，能走到这一支就
    // 一定是 Unix + bash，参数不随平台变。
    const QString setsid = QStandardPaths::findExecutable(QStringLiteral("setsid"));
    const bool useSetsid = !setsid.isEmpty();
    const QString launcher = useSetsid ? setsid : program;
    const QStringList launcherArgs =
      useSetsid ? QStringList{program, QStringLiteral("-lc"), wrapped}
      :
      toolutil::shellArgumentsFor(shell, wrapped);
    if(!useSetsid) {
      // 没有 setsid 就只能直起，进程树清理会不完整，如实告警。
      qCWarning(log) << "找不到 setsid，后台任务的子进程可能无法被 TaskStop 一并终止";
    }

    qint64 pid = 0;
    const bool launched = QProcess::startDetached(launcher, launcherArgs, cwd, &pid);
    if(!launched || pid <= 0) {
      qCCritical(log) << "后台任务启动失败; program=" << program << "cwd=" << cwd;
      finish(ToolResult::failure(
               QStringLiteral("后台任务无法启动（program=%1，cwd=%2）。").arg(program, cwd),
               QStringLiteral("spawn_failed")));
      return;
    }

    BackgroundTaskRegistry::AdoptRequest adoptRequest;
    adoptRequest.type = QStringLiteral("bash");
    adoptRequest.description = description;
    adoptRequest.command = command;
    adoptRequest.pid = static_cast<int>(pid);
    adoptRequest.outputPath = outputPath;
    adoptRequest.taskId = taskId;
    adoptRequest.detached = true;
    if(context.backgroundTasks->adopt(adoptRequest).isEmpty()) {
      qCCritical(log) << "后台任务登记失败，进程将成为孤儿; pid=" << pid;
      finish(ToolResult::failure(QStringLiteral("后台任务登记失败。"),
                                 QStringLiteral("background_register_failed")));
      return;
    }

    qCInfo(log) << "后台任务已分离启动; id=" << taskId << "pid=" << pid
                << "output=" << outputPath;
    ToolResult result = ToolResult::success(
                          QStringLiteral("命令已在后台启动（已与宿主分离，可跨重启存活）。\n"
                                         "task_id: %1\n"
                                         "output_path: %2\n"
                                         "pid: %3\n\n"
                                         "用 TaskOutput 读取进度或结果，用 TaskStop 终止。")
                          .arg(taskId, outputPath)
                          .arg(pid));
    result.metadata.insert(QStringLiteral("status"), QStringLiteral("backgrounded"));
    result.metadata.insert(QStringLiteral("backgroundTaskId"), taskId);
    result.metadata.insert(QStringLiteral("outputPath"), outputPath);
    result.metadata.insert(QStringLiteral("pid"), static_cast<double>(pid));
    result.metadata.insert(QStringLiteral("detached"), true);
    result.metadata.insert(QStringLiteral("cwd"), cwd);
    finish(std::move(result));
    return;
  }

  auto * process = new QProcess();
  process->setProgram(program);
  process->setArguments(toolutil::shellArgumentsFor(shell, command));
  process->setWorkingDirectory(cwd);
  process->setProcessChannelMode(QProcess::SeparateChannels);
  // 命令的工作目录（cwd）与 conpty/编码无关，这里不动 locale，
  // 让 shell 继承进程环境，行为与用户终端一致。
  // 建立独立进程组：超时/取消/停止时可以对整组发信号。
  // 与后台任务注册表共用同一份实现，避免两处漂移。
  toolutil::configureProcessGroup(process);

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
    if(!context.progress) {
      return;
    }
    *stdoutRemainder += QString::fromUtf8(chunk);
    int newlineIndex = stdoutRemainder->indexOf(QLatin1Char('\n'));
    while(newlineIndex >= 0) {
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

  // 取消轮询定时器必须**先**创建：超时处理器要停掉它。
  // 否则自动后台化之后，它仍会在用户点中断时杀掉一个已经归后台所有的进程。
  auto * cancelTimer = new QTimer(process);
  cancelTimer->setInterval(kCancelPollIntervalMs);

  auto * timeoutTimer = new QTimer(process);
  timeoutTimer->setSingleShot(true);
  timeoutTimer->setInterval(timeoutMs);
  QObject::connect(timeoutTimer, &QTimer::timeout, process,
                   [process, state, timeoutMs, finish, context, cancelTimer, stdoutBudget,
  stderrBudget, command]() {
    if(process->state() == QProcess::NotRunning) {
      return;
    }

    // 超时不一定要杀掉：命令还在跑，说明它可能只是慢。
    // 有注册表时**自动转入后台**，把控制权还给模型，
    // 让它去做别的事、稍后再读结果（本实现的 assistantAutoBackgrounded）。
    if(context.backgroundTasks == nullptr) {
      state->timedOut = true;
      qCWarning(log) << "Bash 超时且无后台任务支持，终止进程组; timeoutMs="
                     << timeoutMs;
      toolutil::killProcessGroup(process);
      return;
    }

    state->timedOut = true;
    state->autoBackgrounded = true;
    qCInfo(log) << "Bash 超时，自动转入后台; timeoutMs=" << timeoutMs;

    // 停掉前台定时器，并断开前台挂在这个进程上的全部处理器：
    // 不断开的话前台读取器会继续抢走输出，finished 处理器还会
    // deleteLater 掉一个已经归注册表所有的进程。
    if(cancelTimer != nullptr) {
      cancelTimer->stop();
    }
    QObject::disconnect(process, nullptr, process, nullptr);

    BackgroundTaskRegistry::AdoptRequest adoptRequest;
    adoptRequest.type = QStringLiteral("bash");
    // 记用户/模型写的原始命令，不要拿 process->arguments() 拼：PowerShell 分支
    // 那里塞的是 Base64 编码命令，拼出来是一串无法阅读的乱码。
    adoptRequest.command = command;
    adoptRequest.process = process;
    adoptRequest.autoBackgrounded = true;
    // 进程已经在跑，且前台读取器刚被断开，可以由注册表接管。
    adoptRequest.takeOverRunningProcess = true;
    // 把前台已经读到的输出带过去，历史不丢。
    adoptRequest.initialOutput =
      stdoutBudget->text() + stderrBudget->text();
    const QString taskId = context.backgroundTasks->adopt(adoptRequest);
    if(taskId.isEmpty()) {
      qCWarning(log) << "自动后台化失败，改为终止进程组";
      toolutil::killProcessGroup(process);
      return;
    }
    context.backgroundTasks->recordProcessIdentity(taskId);

    const BackgroundTask task = context.backgroundTasks->task(taskId);
    ToolResult result = ToolResult::success(
                          QStringLiteral("命令超过超时（%1ms）仍在运行，已自动转入后台。\n"
                                         "task_id: %2\n"
                                         "output_path: %3\n\n"
                                         "用 TaskOutput 读取进度，用 TaskStop 终止。\n"
                                         "注意：它继承的是前台管道，只在本进程存活期间继续"
                                         "运行；需要跨重启请用 run_in_background 显式启动。")
                          .arg(timeoutMs)
                          .arg(taskId, task.outputPath));
    result.metadata.insert(QStringLiteral("status"),
                           QStringLiteral("backgrounded"));
    result.metadata.insert(QStringLiteral("backgroundTaskId"), taskId);
    result.metadata.insert(QStringLiteral("assistantAutoBackgrounded"), true);
    result.metadata.insert(QStringLiteral("outputPath"), task.outputPath);
    result.metadata.insert(QStringLiteral("timedOut"), true);
    result.metadata.insert(QStringLiteral("autoBackgrounded"), true);
    finish(std::move(result));
  });

  QObject::connect(cancelTimer, &QTimer::timeout, process, [process, state, context]() {
    if(!context.isCancelled() || process->state() == QProcess::NotRunning) {
      return;
    }
    state->cancelled = true;
    qCInfo(log) << "Bash 收到取消，终止进程组";
    toolutil::killProcessGroup(process);
  });

  QObject::connect(process, &QProcess::errorOccurred, process,
  [process, finish, state, cwd](QProcess::ProcessError error) {
    if(error != QProcess::FailedToStart) {
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
    if(context.progress && !stdoutRemainder->isEmpty()) {
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
    if(!stderrText.isEmpty()) {
      if(!combined.isEmpty()) {
        combined += QLatin1Char('\n');
      }
      combined += QStringLiteral("[stderr]\n") + stderrText;
    }
    if(combined.isEmpty()) {
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
    if(!description.isEmpty()) {
      metadata.insert(QStringLiteral("description"),
                      toolutil::redactForLog(description, 200));
    }

    if(state->cancelled) {
      qCInfo(log) << "Bash 已取消; durationMs=" << durationMs;
      ToolResult result = ToolResult::failure(
                            QStringLiteral("命令已取消"), QStringLiteral("cancelled"),
                            metadata);
      result.output = combined;
      finish(std::move(result));
      return;
    }
    if(state->timedOut) {
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
    if(!normalExit) {
      ToolResult result = ToolResult::failure(
                            QStringLiteral("命令异常结束（未正常退出，可能是段错误或被信号杀死）。"),
                            QStringLiteral("crashed"), metadata);
      result.output = combined;
      finish(std::move(result));
      return;
    }
    if(exitCode != 0) {
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

}  // namespace lycode

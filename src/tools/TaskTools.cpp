#include "tools/TaskTools.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QTimer>

#include "core/Json.h"
#include "tools/BackgroundTaskRegistry.h"

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.tool.task")

/// 阻塞等待的默认与上限（与 npm 的 TaskOutput 一致）。
constexpr int kDefaultBlockTimeoutMs = 30000;
constexpr int kMaxBlockTimeoutMs = 600000;
/// 回给模型的输出上限。完整内容在 outputPath 指向的文件里。
constexpr int kOutputPreviewChars = 30000;

/// 把任务快照渲染成模型可读的文本。
QString renderTask(const BackgroundTask &task) {
    QStringList lines;
    lines << QStringLiteral("task_id: %1").arg(task.id);
    lines << QStringLiteral("type: %1").arg(task.type);
    lines << QStringLiteral("status: %1").arg(task.status);
    if (!task.description.isEmpty()) {
        lines << QStringLiteral("description: %1").arg(task.description);
    }
    if (!task.command.isEmpty()) {
        lines << QStringLiteral("command: %1").arg(task.command);
    }
    lines << QStringLiteral("duration_ms: %1").arg(task.durationMs());
    lines << QStringLiteral("output_bytes: %1").arg(task.outputBytes);
    if (!task.isRunning()) {
        lines << QStringLiteral("exit_code: %1").arg(task.exitCode);
    }
    if (!task.outputPath.isEmpty()) {
        lines << QStringLiteral("output_path: %1").arg(task.outputPath);
    }

    const QString preview = task.outputPreview.trimmed();
    if (!preview.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("--- output (tail) ---");
        lines << preview;
    } else {
        lines << QString();
        lines << QStringLiteral("(目前没有输出)");
    }
    return lines.join(QLatin1Char('\n'));
}

QJsonObject taskMetadata(const BackgroundTask &task) {
    QJsonObject metadata;
    metadata.insert(QStringLiteral("taskId"), task.id);
    metadata.insert(QStringLiteral("taskType"), task.type);
    metadata.insert(QStringLiteral("status"), task.status);
    metadata.insert(QStringLiteral("exitCode"), task.exitCode);
    metadata.insert(QStringLiteral("outputPath"), task.outputPath);
    metadata.insert(QStringLiteral("outputBytes"), task.outputBytes);
    metadata.insert(QStringLiteral("durationMs"), task.durationMs());
    return metadata;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// TaskOutput
// ─────────────────────────────────────────────────────────────────────────────

ToolMetadata TaskOutputTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("TaskOutput");
    meta.description =
        QStringLiteral("Read the current output of a background task started by Bash with "
                       "run_in_background. Optionally wait for the task to finish.");
    meta.modelInstructions =
        QStringLiteral("Set block to true to wait for completion (up to timeout ms). "
                       "The full output is written to output_path — read that file when the "
                       "returned tail is not enough.");
    // 只是读任务状态与文件，不碰工作区。
    meta.readOnly = true;
    meta.concurrency = ToolMetadata::Concurrency::Safe;
    meta.sideEffectScope = SideEffectScope::None;
    meta.riskLevel = RiskLevel::Low;
    meta.needsApproval = false;
    meta.timeoutMs = kMaxBlockTimeoutMs + 5000;
    meta.maxOutputBytes = 256 * 1024;
    return meta;
}

QJsonObject TaskOutputTool::inputSchema() const {
    QJsonObject properties;
    properties.insert(QStringLiteral("task_id"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("The background task id returned by Bash.")}});
    properties.insert(QStringLiteral("block"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("Wait for the task to finish. Defaults to "
                                                  "true.")}});
    properties.insert(QStringLiteral("timeout"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("Max wait in milliseconds when block is true "
                                                  "(default 30000, max 600000).")}});

    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), properties);
    schema.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("task_id")});
    return schema;
}

QString TaskOutputTool::title(const QJsonObject &input) const {
    return QStringLiteral("TaskOutput: ") + stringArg(input, QStringLiteral("task_id"));
}

void TaskOutputTool::execute(const QJsonObject &input, const ToolContext &context,
                             ToolCallback done) {
    const QString taskId = stringArg(input, QStringLiteral("task_id")).trimmed();
    if (taskId.isEmpty()) {
        done(ToolResult::failure(QStringLiteral("task_id 不能为空。"),
                                 QStringLiteral("invalid_input")));
        return;
    }
    if (context.backgroundTasks == nullptr) {
        done(ToolResult::failure(QStringLiteral("当前环境不支持后台任务。"),
                                 QStringLiteral("unsupported")));
        return;
    }

    BackgroundTaskRegistry *registry = context.backgroundTasks;
    if (!registry->contains(taskId)) {
        done(ToolResult::failure(
            QStringLiteral("找不到后台任务：%1（可用 TaskOutput 的任务仅限本次会话启动的）")
                .arg(taskId),
            QStringLiteral("task_not_found")));
        return;
    }

    const bool block = boolArg(input, QStringLiteral("block"), true);
    int timeoutMs = intArg(input, QStringLiteral("timeout"), kDefaultBlockTimeoutMs);
    timeoutMs = qBound(0, timeoutMs, kMaxBlockTimeoutMs);

    // 收口只允许一次：finished 信号与超时定时器都可能触发。
    auto state = std::make_shared<bool>(false);
    const auto finishOnce = [state, done](ToolResult result) {
        if (*state) {
            return;
        }
        *state = true;
        done(std::move(result));
    };

    const auto report = [finishOnce, registry, taskId, block](bool timedOut) {
        const BackgroundTask task = registry->task(taskId);
        ToolResult result = ToolResult::success(
            json::truncate(renderTask(task), kOutputPreviewChars));
        result.metadata = taskMetadata(task);
        result.metadata.insert(QStringLiteral("retrievalStatus"),
                               timedOut ? QStringLiteral("timeout")
                                        : (task.isRunning() ? QStringLiteral("not_ready")
                                                           : QStringLiteral("success")));
        if (timedOut && task.isRunning()) {
            // 超时不是失败：任务还在跑，模型可以稍后再读。
            result.metadata.insert(QStringLiteral("timedOut"), true);
        }
        result.metadata.insert(QStringLiteral("blocked"), block);
        finishOnce(std::move(result));
    };

    if (!block || !registry->task(taskId).isRunning()) {
        report(false);
        return;
    }

    // 阻塞等待：订阅一次 taskFinished，同时挂一个超时兜底。
    auto connection = std::make_shared<QMetaObject::Connection>();
    auto *timer = new QTimer(registry);
    timer->setSingleShot(true);
    *connection = QObject::connect(
        registry, &BackgroundTaskRegistry::taskFinished, timer,
        [finishOnce, report, connection, taskId, timer](const Id &finishedId, bool, int) {
            if (finishedId != taskId) {
                return;
            }
            QObject::disconnect(*connection);
            timer->stop();
            timer->deleteLater();
            report(false);
        });

    QObject::connect(timer, &QTimer::timeout, timer, [finishOnce, report, connection, timer]() {
        QObject::disconnect(*connection);
        timer->deleteLater();
        report(true);
    });
    timer->start(timeoutMs);
}

// ─────────────────────────────────────────────────────────────────────────────
// TaskStop
// ─────────────────────────────────────────────────────────────────────────────

ToolMetadata TaskStopTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("TaskStop");
    meta.description = QStringLiteral("Stop a background task started by Bash with "
                                      "run_in_background.");
    meta.readOnly = false;
    // 终止会改变会话内状态，但目标只有一个任务，与其它工具并发是安全的。
    meta.concurrency = ToolMetadata::Concurrency::Serial;
    // 作用域是会话：它只影响本会话自己的后台任务，不碰工作区或外部世界。
    meta.sideEffectScope = SideEffectScope::Session;
    meta.riskLevel = RiskLevel::Low;
    meta.needsApproval = false;
    meta.timeoutMs = 15000;
    meta.maxOutputBytes = 16 * 1024;
    return meta;
}

QJsonObject TaskStopTool::inputSchema() const {
    QJsonObject properties;
    properties.insert(QStringLiteral("task_id"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("The background task id to stop.")}});
    // npm 的 TaskStop 还接受 shell_id（已废弃的别名），这里也接住，
    // 免得旧习惯的调用直接失败。
    properties.insert(QStringLiteral("shell_id"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                  {QStringLiteral("description"),
                                   QStringLiteral("Deprecated alias for task_id.")}});

    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), properties);
    schema.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("task_id")});
    return schema;
}

QString TaskStopTool::title(const QJsonObject &input) const {
    return QStringLiteral("TaskStop: ") + stringArg(input, QStringLiteral("task_id"));
}

void TaskStopTool::execute(const QJsonObject &input, const ToolContext &context,
                           ToolCallback done) {
    QString taskId = stringArg(input, QStringLiteral("task_id")).trimmed();
    if (taskId.isEmpty()) {
        taskId = stringArg(input, QStringLiteral("shell_id")).trimmed();
    }
    if (taskId.isEmpty()) {
        done(ToolResult::failure(QStringLiteral("task_id 不能为空。"),
                                 QStringLiteral("invalid_input")));
        return;
    }
    if (context.backgroundTasks == nullptr) {
        done(ToolResult::failure(QStringLiteral("当前环境不支持后台任务。"),
                                 QStringLiteral("unsupported")));
        return;
    }

    BackgroundTaskRegistry *registry = context.backgroundTasks;
    if (!registry->contains(taskId)) {
        done(ToolResult::failure(QStringLiteral("找不到后台任务：%1").arg(taskId),
                                 QStringLiteral("task_not_found")));
        return;
    }

    const BackgroundTask before = registry->task(taskId);
    if (!before.isRunning()) {
        // 已结束的任务不算错误：告诉模型现状即可，避免它反复重试。
        ToolResult result = ToolResult::success(
            QStringLiteral("任务 %1 已经结束（status=%2, exit_code=%3），无需停止。")
                .arg(taskId, before.status)
                .arg(before.exitCode));
        result.metadata = taskMetadata(before);
        result.metadata.insert(QStringLiteral("alreadyFinished"), true);
        done(std::move(result));
        return;
    }

    if (!registry->stop(taskId)) {
        done(ToolResult::failure(QStringLiteral("停止任务失败：%1").arg(taskId),
                                 QStringLiteral("stop_failed")));
        return;
    }

    // 终止是异步完成的（等 finished 信号），所以这里立刻回执并说明后续
    // 可以用 TaskOutput 确认。不谎报"已终止"。
    qCInfo(log) << "已请求停止后台任务;" << taskId;
    ToolResult result = ToolResult::success(
        QStringLiteral("已向任务 %1 发送终止信号。用 TaskOutput 查看最终状态与退出码。")
            .arg(taskId));
    result.metadata = taskMetadata(before);
    result.metadata.insert(QStringLiteral("stopRequested"), true);
    done(std::move(result));
}

}  // namespace zcode

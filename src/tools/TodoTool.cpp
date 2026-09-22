// LyCode — Todo 工具实现
#include "tools/TodoTool.h"

#include "core/Json.h"
#include "tools/TodoStore.h"
#include "tools/ToolUtils.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QStringList>

namespace lycode {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.tool.todo")

const QString kStatuses[] = {
    QStringLiteral("pending"),
    QStringLiteral("in_progress"),
    QStringLiteral("completed"),
};

const QString kPriorities[] = {
    QStringLiteral("high"),
    QStringLiteral("medium"),
    QStringLiteral("low"),
};

bool isValidStatus(const QString &value) {
    for (const QString &candidate : kStatuses) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

bool isValidPriority(const QString &value) {
    for (const QString &candidate : kPriorities) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

QStringList statusList() {
    QStringList list;
    for (const QString &value : kStatuses) {
        list.append(value);
    }
    return list;
}

QStringList priorityList() {
    QStringList list;
    for (const QString &value : kPriorities) {
        list.append(value);
    }
    return list;
}

QJsonObject todoToJson(const TodoItem &item) {
    QJsonObject object;
    object.insert(QStringLiteral("content"), item.content);
    object.insert(QStringLiteral("status"), item.status);
    object.insert(QStringLiteral("priority"), item.priority);
    return object;
}

QJsonArray todosToJson(const QList<TodoItem> &items) {
    QJsonArray array;
    for (const TodoItem &item : items) {
        array.append(todoToJson(item));
    }
    return array;
}

QString formatTodo(const TodoItem &item, int index) {
    return QStringLiteral("%1. [%2] (%3) %4")
        .arg(index)
        .arg(item.status, item.priority, item.content);
}

QJsonObject summarize(const QList<TodoItem> &items) {
    int pending = 0;
    int inProgress = 0;
    int completed = 0;
    for (const TodoItem &item : items) {
        if (item.status == QLatin1String("in_progress")) {
            ++inProgress;
        } else if (item.status == QLatin1String("completed")) {
            ++completed;
        } else {
            ++pending;
        }
    }
    QJsonObject summary;
    summary.insert(QStringLiteral("total"), static_cast<int>(items.size()));
    summary.insert(QStringLiteral("pending"), pending);
    summary.insert(QStringLiteral("inProgress"), inProgress);
    summary.insert(QStringLiteral("completed"), completed);
    return summary;
}

/// 解析并校验 todos 数组。返回 false 时 errorOut 给出面向模型的说明。
/// 校验细节：
///   * content 必须是非空字符串（空 todo 没有意义，只会污染列表）
///   * status 必须是三值之一（缺失/拼错都拒绝，避免 UI 无法归类）
///   * priority 可缺失，默认 medium（它只影响展示顺序）
bool parseTodos(const QJsonArray &array, QList<TodoItem> *itemsOut, QString *errorOut) {
    QList<TodoItem> items;
    items.reserve(array.size());
    for (int i = 0; i < array.size(); ++i) {
        const QJsonValue value = array.at(i);
        if (!value.isObject()) {
            *errorOut = QStringLiteral("todos[%1] 不是对象").arg(i);
            return false;
        }
        const QJsonObject object = value.toObject();

        const QString content = json::str(object, QStringLiteral("content")).trimmed();
        if (content.isEmpty()) {
            *errorOut = QStringLiteral("todos[%1].content 不能为空").arg(i);
            return false;
        }
        const QString status = json::str(object, QStringLiteral("status")).trimmed();
        if (status.isEmpty()) {
            *errorOut = QStringLiteral("todos[%1].status 缺失（可选值：%2）")
                            .arg(i)
                            .arg(statusList().join(QStringLiteral(" | ")));
            return false;
        }
        if (!isValidStatus(status)) {
            *errorOut = QStringLiteral("todos[%1].status 非法：%2（可选值：%3）")
                            .arg(i)
                            .arg(status, statusList().join(QStringLiteral(" | ")));
            return false;
        }
        QString priority = json::str(object, QStringLiteral("priority")).trimmed();
        if (priority.isEmpty()) {
            priority = QStringLiteral("medium");
        } else if (!isValidPriority(priority)) {
            *errorOut = QStringLiteral("todos[%1].priority 非法：%2（可选值：%3）")
                            .arg(i)
                            .arg(priority, priorityList().join(QStringLiteral(" | ")));
            return false;
        }

        TodoItem item;
        item.content = content;
        item.status = status;
        item.priority = priority;
        items.append(item);
    }
    *itemsOut = items;
    return true;
}

ToolResult storeUnavailable() {
    return ToolResult::failure(
        QStringLiteral("会话 todo 存储不可用（ToolContext::todoStore 为空）。"
                       "这是运行时装配错误，不是模型参数问题。"),
        QStringLiteral("todo_store_unavailable"));
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// TodoRead
// ─────────────────────────────────────────────────────────────────────────────

ToolMetadata TodoReadTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("TodoRead");
    meta.description = QStringLiteral("Reads the current session's todo list.");
    meta.modelInstructions = QStringLiteral(
        "Use it to re-check the plan after context compaction or before starting "
        "a new step. Takes no arguments.");
    meta.allowedInPlanMode = true;
    meta.readOnly = true;
    meta.destructive = false;
    meta.concurrency = ToolMetadata::Concurrency::Safe;
    meta.requiresUserInteraction = false;
    meta.timeoutMs = 30000;
    meta.maxOutputBytes = 256 * 1024;
    meta.sideEffectScope = SideEffectScope::None;
    meta.riskLevel = RiskLevel::Low;
    meta.needsApproval = false;
    meta.alwaysAsk = false;
    meta.providerVisible = true;
    meta.stopTurnOnSuccess = false;
    meta.idempotent = true;
    return meta;
}

QJsonObject TodoReadTool::inputSchema() const {
    // 无参数工具也要给出合法的空 schema：provider 侧对 schema 的合法性有要求。
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{}},
        {QStringLiteral("required"), QJsonArray{}},
    };
}

QList<PermissionRule> TodoReadTool::permissionRules(const QJsonObject &input) const {
    Q_UNUSED(input);
    // todo 读写只动会话内存，不需要"始终允许"这类确认选项。
    return {};
}

QString TodoReadTool::permissionCapability() const {
    return QStringLiteral("todo.read");
}

void TodoReadTool::execute(const QJsonObject &input, const ToolContext &context,
                           ToolCallback done) {
    Q_UNUSED(input);
    auto finish = [&done](ToolResult result) { done(std::move(result)); };

    if (context.isCancelled()) {
        finish(ToolResult::failure(QStringLiteral("执行已取消"), QStringLiteral("cancelled")));
        return;
    }
    if (context.todoStore == nullptr) {
        qCCritical(log) << "TodoRead 缺少注入的 TodoStore";
        finish(storeUnavailable());
        return;
    }
    if (context.sessionId.isEmpty()) {
        finish(ToolResult::failure(QStringLiteral("会话 id 为空，无法读取 todo"),
                                   QStringLiteral("missing_session")));
        return;
    }

    const QList<TodoItem> items = context.todoStore->todos(context.sessionId);
    QJsonObject resultMeta;
    resultMeta.insert(QStringLiteral("todos"), todosToJson(items));

    if (items.isEmpty()) {
        qCInfo(log) << "TodoRead 完成; session=" << context.sessionId << "count=0";
        finish(ToolResult::success(QStringLiteral("(no todos)"), resultMeta));
        return;
    }

    QStringList lines;
    lines.reserve(items.size());
    for (int i = 0; i < items.size(); ++i) {
        lines.append(formatTodo(items.at(i), i + 1));
    }
    qCInfo(log) << "TodoRead 完成; session=" << context.sessionId << "count=" << items.size();
    finish(ToolResult::success(lines.join(QLatin1Char('\n')), resultMeta));
}

// ─────────────────────────────────────────────────────────────────────────────
// TodoWrite
// ─────────────────────────────────────────────────────────────────────────────

ToolMetadata TodoWriteTool::metadata() const {
    ToolMetadata meta;
    meta.name = QStringLiteral("TodoWrite");
    // 注意：这里 readOnly = true 是**声明式语义**上的"不碰外部世界"，
    // 但它确实写会话状态，因此显式声明 Serial、scope = Session。
    // 调度器与权限服务只看这些字段，不会因为名字叫 Write 就当作文件写。
    meta.description = QStringLiteral(
        "Replaces the session todo list with the given items. Use it to plan and "
        "track multi-step work.");
    meta.modelInstructions = QStringLiteral(
        "todos is a full replacement: always send the complete list. Item shape: "
        "`{content, status: pending|in_progress|completed, priority: high|medium|low}`.");
    meta.allowedInPlanMode = true;
    meta.readOnly = true;
    meta.destructive = false;
    meta.concurrency = ToolMetadata::Concurrency::Serial;
    meta.requiresUserInteraction = false;
    meta.timeoutMs = 30000;
    meta.maxOutputBytes = 256 * 1024;
    meta.sideEffectScope = SideEffectScope::Session;
    meta.riskLevel = RiskLevel::Low;
    meta.needsApproval = false;
    meta.alwaysAsk = false;
    meta.providerVisible = true;
    meta.stopTurnOnSuccess = false;
    meta.idempotent = false;
    return meta;
}

QJsonObject TodoWriteTool::inputSchema() const {
    QJsonObject itemProperties;
    itemProperties.insert(QStringLiteral("content"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                      {QStringLiteral("minLength"), 1},
                                      {QStringLiteral("description"),
                                       QStringLiteral("待办内容（祈使句，描述要做什么）")}});
    itemProperties.insert(QStringLiteral("status"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                      {QStringLiteral("enum"),
                                       QJsonArray{QStringLiteral("pending"),
                                                  QStringLiteral("in_progress"),
                                                  QStringLiteral("completed")}},
                                      {QStringLiteral("description"), QStringLiteral("状态")}});
    itemProperties.insert(QStringLiteral("priority"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                      {QStringLiteral("enum"),
                                       QJsonArray{QStringLiteral("high"), QStringLiteral("medium"),
                                                  QStringLiteral("low")}},
                                      {QStringLiteral("description"),
                                       QStringLiteral("优先级，默认 medium")}});

    QJsonObject todosSchema;
    todosSchema.insert(QStringLiteral("type"), QStringLiteral("array"));
    todosSchema.insert(QStringLiteral("description"),
                       QStringLiteral("完整的 todo 列表（整表替换）"));
    todosSchema.insert(QStringLiteral("items"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                   {QStringLiteral("properties"), itemProperties},
                                   {QStringLiteral("required"),
                                    QJsonArray{QStringLiteral("content"), QStringLiteral("status")}}});

    QJsonObject properties;
    properties.insert(QStringLiteral("todos"), todosSchema);
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("todos")}},
    };
}

QList<PermissionRule> TodoWriteTool::permissionRules(const QJsonObject &input) const {
    Q_UNUSED(input);
    return {};
}

QString TodoWriteTool::validateInput(const QJsonObject &input) const {
    const QString base = Tool::validateInput(input);
    if (!base.isEmpty()) {
        return base;
    }
    const QJsonValue value = input.value(QStringLiteral("todos"));
    if (!value.isArray()) {
        return QStringLiteral("todos 必须是数组");
    }
    QList<TodoItem> items;
    QString error;
    if (!parseTodos(value.toArray(), &items, &error)) {
        return error;
    }
    return {};
}

QString TodoWriteTool::title(const QJsonObject &input) const {
    const QJsonArray array = json::array(input, QStringLiteral("todos"));
    return QStringLiteral("TodoWrite: %1 items").arg(array.size());
}

QString TodoWriteTool::permissionCapability() const {
    return QStringLiteral("todo.write");
}

void TodoWriteTool::execute(const QJsonObject &input, const ToolContext &context,
                            ToolCallback done) {
    auto finish = [&done](ToolResult result) { done(std::move(result)); };

    if (context.isCancelled()) {
        finish(ToolResult::failure(QStringLiteral("执行已取消"), QStringLiteral("cancelled")));
        return;
    }
    if (context.todoStore == nullptr) {
        qCCritical(log) << "TodoWrite 缺少注入的 TodoStore";
        finish(storeUnavailable());
        return;
    }
    if (context.sessionId.isEmpty()) {
        finish(ToolResult::failure(QStringLiteral("会话 id 为空，无法写入 todo"),
                                   QStringLiteral("missing_session")));
        return;
    }

    const QJsonArray array = json::array(input, QStringLiteral("todos"));
    QList<TodoItem> items;
    QString error;
    if (!parseTodos(array, &items, &error)) {
        qCWarning(log) << "TodoWrite 参数非法:" << error;
        finish(ToolResult::failure(error, QStringLiteral("invalid_todos")));
        return;
    }

    const QList<TodoItem> oldItems = context.todoStore->todos(context.sessionId);
    // 整表替换：不做差异合并，也不强制"至多一个 in_progress"
    //（本实现已注释掉该校验：并行推进多个步骤是合理的）。
    context.todoStore->setTodos(context.sessionId, items);

    QJsonObject resultMeta;
    resultMeta.insert(QStringLiteral("oldTodos"), todosToJson(oldItems));
    resultMeta.insert(QStringLiteral("todos"), todosToJson(items));
    resultMeta.insert(QStringLiteral("summary"), summarize(items));

    const QJsonObject summary = summarize(items);
    const QString output = QStringLiteral("Todos updated: %1 total (%2 pending, %3 in progress, "
                                          "%4 completed)")
                               .arg(summary.value(QStringLiteral("total")).toInt())
                               .arg(summary.value(QStringLiteral("pending")).toInt())
                               .arg(summary.value(QStringLiteral("inProgress")).toInt())
                               .arg(summary.value(QStringLiteral("completed")).toInt());

    qCInfo(log) << "TodoWrite 完成; session=" << context.sessionId
                << "old=" << oldItems.size() << "new=" << items.size();

    finish(ToolResult::success(output, resultMeta));
}

}  // namespace lycode

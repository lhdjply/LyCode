// ZCode Qt — 会话 todo 存储
//
// todo 的唯一所有者是「会话」而不是工具实例：TodoWrite 工具可能被重新创建
// （换注册表、别名覆盖），若把状态放在工具里，会话一换就会丢。
// 因此由 AgentRuntime 持有一个 TodoStore 并注入到 ToolContext。
//
// 线程约束：内部是普通 QHash + QList，**不是线程安全的**。
// 工具在 GUI 线程执行（Bash 只把异步结果回调回同一线程），所以不需要加锁；
// 若将来有后台线程要读 todo，必须自己保证串行（或在调用方拷贝一份）。
#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include "core/Types.h"

namespace zcode {

/// 一条 todo。字段与 npm 版 todo 条目对齐：
/// `{content, status, priority}`，status/priority 用字面量而不是枚举，
/// 因为工具入参、UI 过滤、持久化三处都要用到同一套字符串，避免来回转换。
struct TodoItem {
    QString content;
    /// pending | in_progress | completed
    QString status = QStringLiteral("pending");
    /// high | medium | low
    QString priority = QStringLiteral("medium");
};

/// 会话 todo 存储。只在 GUI 线程使用。
class TodoStore {
public:
    TodoStore() = default;

    /// 读取某个会话的 todo；未知会话返回空列表。
    QList<TodoItem> todos(const Id &sessionId) const;

    /// 整表替换某个会话的 todo。空列表等价于"清空"。
    void setTodos(const Id &sessionId, const QList<TodoItem> &items);

    /// 删除某个会话的条目（会话关闭时调用，避免内存里累积死会话）。
    void clear(const Id &sessionId);

    /// 当前持有 todo 的会话数（诊断/测试用）。
    int sessionCount() const { return static_cast<int>(store_.size()); }

private:
    QHash<Id, QList<TodoItem>> store_;
};

}  // namespace zcode

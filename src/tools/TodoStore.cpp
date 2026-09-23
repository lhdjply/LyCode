// LyCode — 会话 todo 存储实现
#include "tools/TodoStore.h"

#include <QLoggingCategory>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.tool.todo.store")

}  // namespace

QList<TodoItem> TodoStore::todos(const Id & sessionId) const
{
  if(sessionId.isEmpty()) {
    return {};
  }
  return store_.value(sessionId);
}

void TodoStore::setTodos(const Id & sessionId, const QList<TodoItem> & items)
{
  if(sessionId.isEmpty()) {
    // 空会话 id 说明调用方没注入上下文，静默丢弃比污染全局桶好，
    // 但必须告警，否则表现为"TodoWrite 成功了但读不到"。
    qCWarning(log) << "setTodos 收到空 sessionId，已忽略";
    return;
  }
  if(items.isEmpty()) {
    store_.remove(sessionId);
    qCDebug(log) << "todo 已清空; session=" << sessionId;
    return;
  }
  store_.insert(sessionId, items);
  qCDebug(log) << "todo 已写入; session=" << sessionId << "count=" << items.size();
}

void TodoStore::clear(const Id & sessionId)
{
  if(sessionId.isEmpty()) {
    return;
  }
  store_.remove(sessionId);
}

}  // namespace lycode

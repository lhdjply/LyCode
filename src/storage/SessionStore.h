// LyCode — 会话持久化
//
// 存储选型：SQLite（Qt6::Sql 的 QSQLITE 驱动）。
// 理由：会话是典型的一对多读写模型（session → messages → parts），
// 需要按工作区过滤、按更新时间排序、按标题搜索，用 SQL 表达最直接；
// 同时单文件便于备份与迁移。
//
// 表结构：
//   sessions(id PK, title, workspace_path, workspace_identity, parent_session_id,
//            status, mode, model_id, created_at_ms, updated_at_ms, context_usage_json)
//   messages(id PK, session_id FK, role, status, model_id, created_at_ms,
//            updated_at_ms, usage_json, error_message, seq)
//   parts(id PK, message_id FK, seq, kind, payload_json)
//
// 写入策略：消息在流式过程中会被频繁更新，落盘按"节流 + 终态必写"处理，
// 避免每个 token 都触发一次磁盘写。
#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "core/Types.h"

namespace lycode
{

/// 一次会话列表查询的结果行（不携带消息体，供列表展示）。
struct SessionSummary {
  Session session;
  QString lastMessagePreview;
};

class SessionStore
{
  public:
    SessionStore();
    ~SessionStore();

    SessionStore(const SessionStore &) = delete;
    SessionStore & operator=(const SessionStore &) = delete;

    /// 打开（必要时创建）数据库。path 为空时使用默认数据目录。
    bool open(const QString & path = {});
    void close();
    bool isOpen() const;
    /// 最近一次失败原因。
    QString lastError() const
    {
      return lastError_;
    }
    /// 实际使用的数据库文件路径。
    QString databasePath() const
    {
      return databasePath_;
    }

    // ── 会话 ────────────────────────────────────────────────────────────────
    bool saveSession(const Session & session);
    bool deleteSession(const Id & sessionId);
    /// 按 workspace key 过滤；key 为空表示不过滤（全部工作区）。
    QList<SessionSummary> listSessions(const QString & workspaceKey = {},
                                       int limit = 200) const;
    bool loadSession(const Id & sessionId, Session * sessionOut) const;
    /// 会话的总消息数（用于列表计数，避免加载全部消息）。
    int messageCount(const Id & sessionId) const;

    // ── 消息 ────────────────────────────────────────────────────────────────
    /// 保存或整体替换一条消息（含全部 part）。
    bool saveMessage(const Message & message);
    /// 只更新消息头（状态 / 用量 / 错误），不动 parts。
    /// 流式过程中每个增量都调用它代价太高，仅供终态使用。
    bool updateMessageStatus(const Id & messageId, MessageStatus status,
                             const QJsonObject & usage, const QString & errorMessage);
    bool deleteMessage(const Id & messageId);
    QList<Message> loadMessages(const Id & sessionId) const;
    /// 取最近 N 条消息，按时间正序返回。
    QList<Message> loadRecentMessages(const Id & sessionId, int limit) const;

    // ── 维护 ────────────────────────────────────────────────────────────────
    /// 删除指定工作区下的全部会话。
    bool deleteSessionsForWorkspace(const QString & workspaceKey);
    /// 按标题 / 消息正文做模糊搜索，返回命中的会话摘要。
    QList<SessionSummary> searchSessions(const QString & query, int limit = 50) const;

    /// 默认数据库文件路径：<数据目录>/sessions.db
    static QString defaultDatabasePath();

  private:
    bool ensureSchema();
    bool exec(const QString & sql, const QVariantList & bindings = {});
    bool setError(const QString & context) const;

    mutable QSqlDatabase database_;
    mutable QString lastError_;
    QString databasePath_;
    QString connectionName_;
};

}  // namespace lycode

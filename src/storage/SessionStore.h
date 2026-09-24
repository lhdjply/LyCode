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

/// 一条被上下文压缩归档掉的消息。
///
/// 压缩会把早期消息从**活动历史**里移除（否则上下文不会变小），但用户的原始
/// 记录不该就此消失：归档表保留被移除消息的完整 JSON，供排查与将来的"展开原始
/// 历史"。表结构刻意与 message 表解耦，归档只增不改，schema 演进互不影响。
struct ArchivedMessage {
  Id sessionId;
  Id messageId;
  /// 被归档时的完整消息 JSON（Message::toJson 的产物）。
  QString data;
  /// 归档原因，目前只有 context_compaction。
  QString reason;
  TimestampMs archivedAtMs = 0;
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

    // ── 上下文压缩归档 ──────────────────────────────────────────────────────
    /// 把若干条消息从活动历史移入归档表（单事务）。
    ///
    /// 与 deleteMessage 的区别是**不丢数据**：消息 JSON 先写进 archived_message，
    /// 再从 message 表删除（其 parts 由外键级联带走）。压缩必须走这条路径，
    /// 否则一次压缩就把用户的早期记录永久删掉了。
    bool archiveMessages(const QList<Message> & messages, const QString & reason = {});

    /// 读取某会话被归档的消息（按归档顺序）。`reason` 为空表示不过滤。
    QList<ArchivedMessage> loadArchivedMessages(const Id & sessionId,
                                                const QString & reason = {}) const;

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

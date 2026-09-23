// LyCode — 会话持久化实现（Qt6::Sql / QSQLITE）
//
// ── 必须保持的幂等语义（本实现的约定）────────────────────────────────────────
//   * 写入一律是 `INSERT ... ON CONFLICT(id) DO UPDATE`，重复保存同一条
//     session / message / part 是幂等的。
//   * message / part 的 `sequence` 只在两种情况下才分配：
//       (a) 该 id 在表中不存在（首次插入）；
//       (b) 该 id 已存在但被改绑到了另一个 scope（跨 scope 改绑）。
//     其余情况**原样保留现有 sequence，包括 NULL**。
//     scope 的定义：message 看 session_id，part 看 message_id。
//     这是时间线不漂移的硬约束——如果每次保存都重算队尾，历史消息的顺序
//     会随保存顺序变化，重新打开会话就会看到错乱的对话。
//     实现上把这条规则压进单条 SQL 的 CASE 表达式（见 saveMessage），
//     这样「读旧值 → 算新值 → 写回」不存在中间窗口。
//   * Message 是整体替换语义：saveMessage() 会删除该消息下已不在
//     message.parts 里的 part 行。
//
// ── 有意的取舍 ─────────────────────────────────────────────────────────────
//   * updateMessageStatus() 只写轻量列（status / usage_json / error_message /
//     updated_at_ms），不重写 `data`、不动 parts：流式过程中每个增量都重写整条
//     JSON（可能含 base64 附件）代价过高。代价是 `data` 里的 status / usage /
//     errorMessage 在中间态是陈旧的——读取时由列覆盖（列是权威来源），终态则由
//     saveMessage() 整体落盘补齐，所以陈旧窗口只存在于「流式中」，不会持久化。
//   * loadMessages() 在 part 读取失败时返回**空列表**，而不是「只有头部的
//     消息」。因为 Message 是整体替换语义，把缺 part 的半成品交给调用方，
//     它一旦重新保存就会真正删掉 part 数据。
//   * part 不做 N+1 逐条查询，而是一次性批量读入按 message_id 分组：
//     一个会话几十条消息、每条几个 part，批量读把查询数从 O(n) 降到 2。
//   * `data` 列与 part 表存在冗余（data 里也带一份 parts 快照）。刻意保留：
//     data 是可独立解析的完整快照（便于迁移与排障），part 表是排序/查询的权威。
//   * part_id 为空时现场补一个 id：空串作为主键会让不同消息的匿名 part 互相
//     覆盖。补 id 后「同一条消息重复保存」仍然幂等（同一个 part 对象在内存里
//     已被 Part::fromJson 补过 id；调用方自建的空 id part 每次保存按新 part 处
//     理，与整体替换语义一致）。
#include "storage/SessionStore.h"

#include "core/DataPaths.h"
#include "core/Ids.h"
#include "core/Json.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.storage.sessions")

// ── 列清单 ──────────────────────────────────────────────────────────────────
// 集中一处，保证 loadSession / listSessions / searchSessions 读到的列完全一致，
// 避免「某个查询少读一列 → 该路径下字段静默变默认值」的漂移。

/// session 表的读取列（查询里必须把 session 别名成 s）。
constexpr char kSessionColumns[] =
  "s.id, s.title, s.workspace_path, s.workspace_identity, s.workspace_key, s.remote_session_id, "
  "s.parent_session_id, s.kind, s.status, s.mode, s.model_id, s.provider_id, s.created_at_ms, "
  "s.updated_at_ms, s.archived_at_ms, s.context_usage_json, s.cumulative_usage_json, "
  "s.message_count";

/// message 表的读取列（查询里必须把 message 别名成 m；派生表场景亦同）。
constexpr char kMessageColumns[] =
  "m.id, m.session_id, m.role, m.status, m.model_id, m.provider_id, m.parent_message_id, "
  "m.created_at_ms, m.updated_at_ms, m.usage_json, m.error_message, m.data, m.sequence";

/// 列表类查询的上限。limit 一律 clamp 到 [1, kMaxListLimit]：
/// 0 或负数会让调用方以为「查询成功但没有数据」，静默丢结果是更糟的失败模式。
constexpr int kMaxListLimit = 1000;
/// 搜索上限（搜索要扫 message.data，必须比列表更保守）。
constexpr int kMaxSearchLimit = 200;
/// 会话列表摘要的预览长度。
constexpr int kPreviewChars = 120;

// ── 通用小工具 ──────────────────────────────────────────────────────────────

/// 空串写成 NULL（列可空时）。'' 与「未设置」两种表示混用会让
/// `WHERE col IS NULL` 之类的判断失效，这里统一收敛到 NULL。
QVariant nullable(const QString & value)
{
  return value.isEmpty() ? QVariant() : QVariant(value);
}

/// 绑定 NOT NULL 文本列。
///
/// ⚠ Qt 陷阱（实测踩到）：默认构造的 QString 是 **null** 而不只是空，
/// 且 QVariant 包裹 null QString 之后 `QVariant::isNull()` 仍为 false，
/// 于是 QSQLITE 会把它绑成 SQL NULL，直接触发
/// "NOT NULL constraint failed"。新建会话的 title / workspace_path
/// 恰好就是默认构造的值，所以这里是必须的归一，不是防御性冗余。
/// `QString::fromUtf8("")` 是"空但非 null"，正好满足 NOT NULL。
QVariant notNullText(const QString & value)
{
  return value.isNull() ? QVariant(QString::fromUtf8("")) : QVariant(value);
}

/// 记录一次 QSqlQuery 级失败。
///
/// 注意：Qt **不会**把 QSqlQuery 的错误写到 QSqlDatabase::lastError()（实测
/// 失败后 db.lastError().text() 为空），所以错误文本必须从 query 本身取。
bool recordQueryError(QString * lastErrorOut, const QString & context, const QSqlQuery & query)
{
  const QString detail = query.lastError().text().trimmed();
  const QString message =
    detail.isEmpty() ? context : context + QStringLiteral(": ") + detail;
  if(lastErrorOut != nullptr) {
    *lastErrorOut = message;
  }
  qCWarning(log) << "SQL 失败:" << message;
  return false;
}

/// 准备 + 绑定 + 执行。全部 SQL 都必须走这里，禁止字符串拼接值。
bool runQuery(QSqlQuery & query, const QString & sql, const QVariantList & bindings,
              const QString & context, QString * lastErrorOut)
{
  if(!query.prepare(sql)) {
    return recordQueryError(lastErrorOut, context, query);
  }
  for(const QVariant & value : bindings) {
    query.addBindValue(value);
  }
  if(!query.exec()) {
    return recordQueryError(lastErrorOut, context, query);
  }
  return true;
}

/// 事务的 RAII 守卫：未 commit 就析构则自动 rollback。
/// 用 RAII 而不是在每个失败出口手写 rollback——saveMessage 有十来个出口，
/// 漏写一个就会把连接留在打开的事务里，后续写入全部被拖进那个事务。
class TransactionGuard
{
  public:
    explicit TransactionGuard(QSqlDatabase & database) : database_(database)
    {
      active_ = database_.transaction();
    }
    ~TransactionGuard()
    {
      if(active_) {
        database_.rollback();
      }
    }

    TransactionGuard(const TransactionGuard &) = delete;
    TransactionGuard & operator=(const TransactionGuard &) = delete;

    bool isActive() const
    {
      return active_;
    }
    bool commit()
    {
      if(!active_) {
        return false;
      }
      if(!database_.commit()) {
        return false;  // 保持 active_：析构时 rollback
      }
      active_ = false;
      return true;
    }

  private:
    QSqlDatabase & database_;
    bool active_ = false;
};

/// LIKE 通配符转义。搜索词里的 % / _ / \ 必须转义后再拼进 pattern，
/// 否则用户搜 "100%" 会命中所有记录（写法本身仍走参数绑定，见 searchSessions）。
QString escapeLikePattern(const QString & value)
{
  QString escaped = value;
  escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
  escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
  return escaped;
}

/// 行 → Session::fromJson() 的输入对象。
/// 只在**这里**把列名映射回 JSON 键，loadSession / listSessions / searchSessions
/// 共用同一条解析路径，避免多处解析各自漂移。
QJsonObject sessionJsonFromRow(const QSqlQuery & row)
{
  QJsonObject json;
  json.insert(QStringLiteral("id"), row.value(QStringLiteral("id")).toString());
  json.insert(QStringLiteral("title"), row.value(QStringLiteral("title")).toString());
  json.insert(QStringLiteral("workspacePath"),
              row.value(QStringLiteral("workspace_path")).toString());
  json.insert(QStringLiteral("workspaceIdentity"),
              row.value(QStringLiteral("workspace_identity")).toString());
  json.insert(QStringLiteral("remoteSessionId"),
              row.value(QStringLiteral("remote_session_id")).toString());
  json.insert(QStringLiteral("parentSessionId"),
              row.value(QStringLiteral("parent_session_id")).toString());
  json.insert(QStringLiteral("kind"), row.value(QStringLiteral("kind")).toString());
  json.insert(QStringLiteral("status"), row.value(QStringLiteral("status")).toString());
  json.insert(QStringLiteral("mode"), row.value(QStringLiteral("mode")).toString());
  json.insert(QStringLiteral("modelId"), row.value(QStringLiteral("model_id")).toString());
  json.insert(QStringLiteral("providerId"), row.value(QStringLiteral("provider_id")).toString());
  json.insert(QStringLiteral("createdAtMs"),
              static_cast<double>(row.value(QStringLiteral("created_at_ms")).toLongLong()));
  json.insert(QStringLiteral("updatedAtMs"),
              static_cast<double>(row.value(QStringLiteral("updated_at_ms")).toLongLong()));
  json.insert(QStringLiteral("archivedAtMs"),
              static_cast<double>(row.value(QStringLiteral("archived_at_ms")).toLongLong()));
  json.insert(QStringLiteral("contextUsage"),
              json::parseObject(row.value(QStringLiteral("context_usage_json")).toByteArray()));
  json.insert(QStringLiteral("cumulativeUsage"),
              json::parseObject(row.value(QStringLiteral("cumulative_usage_json")).toByteArray()));
  json.insert(QStringLiteral("messageCount"),
              row.value(QStringLiteral("message_count")).toInt());
  // pendingPermissionCount / pendingInputCount 是纯运行时状态，不落盘；
  // 缺失时 Session::fromJson 取默认值 0，符合「重启后没有待处理交互」的语义。
  return json;
}

/// 行 → Message::fromJson() 的输入对象。
/// `data` 是 Message::toJson() 的完整快照，但**列是权威来源**：
/// updateMessageStatus() 只更新列不重写 data，data 里的状态/用量可能陈旧，
/// 因此这里用列值覆盖 data 的对应键；parts 由调用方从 part 表补齐。
QJsonObject messageJsonFromRow(const QSqlQuery & row)
{
  QJsonObject json = json::parseObject(row.value(QStringLiteral("data")).toByteArray());
  json.insert(QStringLiteral("id"), row.value(QStringLiteral("id")).toString());
  json.insert(QStringLiteral("sessionId"), row.value(QStringLiteral("session_id")).toString());
  json.insert(QStringLiteral("role"), row.value(QStringLiteral("role")).toString());
  json.insert(QStringLiteral("status"), row.value(QStringLiteral("status")).toString());
  json.insert(QStringLiteral("modelId"), row.value(QStringLiteral("model_id")).toString());
  json.insert(QStringLiteral("parentMessageId"),
              row.value(QStringLiteral("parent_message_id")).toString());
  json.insert(QStringLiteral("createdAtMs"),
              static_cast<double>(row.value(QStringLiteral("created_at_ms")).toLongLong()));
  json.insert(QStringLiteral("updatedAtMs"),
              static_cast<double>(row.value(QStringLiteral("updated_at_ms")).toLongLong()));
  json.insert(QStringLiteral("usage"),
              json::parseObject(row.value(QStringLiteral("usage_json")).toByteArray()));
  json.insert(QStringLiteral("errorMessage"), row.value(QStringLiteral("error_message")).toString());
  return json;
}

/// 取「最后一条消息」的数据列，作为会话列表的预览。
/// 排序与 loadMessages 完全一致（sequence 为 NULL 的排最后，再按
/// created_at_ms / rowid），只是方向取反取最新——只按 created_at_ms 取最大会
/// 在「时间戳未设置（0）」的消息上退化成任意一条。
QString lastMessageDataSubquery()
{
  return QStringLiteral(
           "(SELECT m.data FROM message m WHERE m.session_id = s.id "
           "ORDER BY (m.sequence IS NULL) DESC, m.sequence DESC, m.created_at_ms DESC, "
           "m.rowid DESC LIMIT 1) AS last_message_data");
}

/// 预览文本：完整走 Message::fromJson() 解析（唯一解析路径），
/// 再取 plainText() 的前 120 字符（不追加截断标记：列表列宽有限，标记只占位）。
QString previewFromMessageData(const QVariant & data)
{
  if(data.isNull()) {
    return {};
  }
  const QJsonObject json = json::parseObject(data.toByteArray());
  if(json.isEmpty()) {
    return {};
  }
  return Message::fromJson(json).plainText().left(kPreviewChars);
}

/// 批量读取某个会话的全部 part，按 message_id 分组。
/// recentLimit > 0 时只读「最近 recentLimit 条消息」的 part（用于
/// loadRecentMessages），否则读全量。两条 SQL 都是固定文本，值全部绑定。
bool loadPartsBatch(QSqlDatabase & database, const QString & sessionId, int recentLimit,
                    QHash<QString, QList<Part>> * partsByMessage, QString * lastErrorOut)
{
  QString sql;
  QVariantList bindings{sessionId};
  if(recentLimit > 0) {
    sql = QStringLiteral(
            "SELECT p.message_id, p.data FROM part p WHERE p.message_id IN ("
            "SELECT id FROM (SELECT rowid AS rid, id FROM message WHERE session_id = ? "
            "ORDER BY (sequence IS NULL) DESC, sequence DESC, created_at_ms DESC, rid DESC "
            "LIMIT ?)) ORDER BY (p.sequence IS NULL), p.sequence, p.rowid");
    bindings.append(recentLimit);
  }
  else {
    sql = QStringLiteral(
            "SELECT p.message_id, p.data FROM part p WHERE p.message_id IN ("
            "SELECT m.id FROM message m WHERE m.session_id = ?) "
            "ORDER BY (p.sequence IS NULL), p.sequence, p.rowid");
  }

  QSqlQuery query(database);
  if(!runQuery(query, sql, bindings, QStringLiteral("loadParts"), lastErrorOut)) {
    return false;
  }
  while(query.next()) {
    const QString messageId = query.value(0).toString();
    const QJsonObject partJson = json::parseObject(query.value(1).toByteArray());
    (*partsByMessage)[messageId].append(Part::fromJson(partJson));
  }
  return true;
}

/// 打开连接后立刻施加的 PRAGMA。
bool applyPragmas(QSqlDatabase & database, QString * lastErrorOut)
{
  const QStringList pragmas = {
    QStringLiteral("PRAGMA journal_mode=WAL"),     // 读写并发：UI 读不阻塞落盘
    QStringLiteral("PRAGMA synchronous=NORMAL"),   // WAL 下兼顾持久性与吞吐
    QStringLiteral("PRAGMA foreign_keys=ON"),      // deleteSession 的级联依赖它
    QStringLiteral("PRAGMA busy_timeout=5000"),    // 多连接/后台任务短暂冲突时等待
  };
  for(const QString & sql : pragmas) {
    QSqlQuery query(database);
    if(!runQuery(query, sql, {}, QStringLiteral("open/pragma"), lastErrorOut)) {
      return false;
    }
  }

  // foreign_keys 是级联删除的前提，必须回读确认：SQLite 默认关闭且
  // 该 PRAGMA 是 per-connection 的。若没生效就继续跑，删会话会留下孤儿
  // message/part（外键不生效时删除会静默成功），属于数据损坏，宁可拒绝打开。
  QSqlQuery check(database);
  if(!runQuery(check, QStringLiteral("PRAGMA foreign_keys"), {}, QStringLiteral("open/pragma"),
               lastErrorOut)) {
    return false;
  }
  if(!check.next() || check.value(0).toInt() != 1) {
    if(lastErrorOut != nullptr) {
      *lastErrorOut = QStringLiteral("open: PRAGMA foreign_keys 未能开启（级联删除不可靠）");
    }
    return false;
  }
  return true;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 生命周期
// ─────────────────────────────────────────────────────────────────────────────

SessionStore::SessionStore() = default;

SessionStore::~SessionStore()
{
  close();
}

QString SessionStore::defaultDatabasePath()
{
  const QString path = QDir(dataDir()).filePath(QStringLiteral("sessions.db"));

  // 目录不存在时先建好：SQLite 能创建文件，但不会创建父目录。
  const QDir directory = QFileInfo(path).absoluteDir();
  if(!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
    // 不在这里失败——真正的失败由 open() 报告并写入 lastError_。
    qCWarning(log) << "默认数据目录创建失败:" << directory.absolutePath();
  }
  return path;
}

bool SessionStore::open(const QString & path)
{
  const QString requested = path.trimmed();
  const QString target = requested.isEmpty() ? defaultDatabasePath() : requested;

  if(isOpen()) {
    if(databasePath_ == target) {
      return true;  // 已打开同一文件：幂等
    }
    close();  // 换库：先彻底释放旧连接（含 removeDatabase）
  }

  if(target.isEmpty()) {
    lastError_ = QStringLiteral("open: 数据库路径为空");
    qCCritical(log) << lastError_;
    return false;
  }
  if(!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
    lastError_ = QStringLiteral("open: 缺少 QSQLITE 驱动（Qt6::Sql 未安装 sqlite 插件）");
    qCCritical(log) << lastError_;
    return false;
  }

  const QDir directory = QFileInfo(target).absoluteDir();
  if(!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
    lastError_ = QStringLiteral("open: 无法创建数据目录 ") + directory.absolutePath();
    qCCritical(log) << lastError_;
    return false;
  }

  // 连接名唯一化：并行任务/测试会在同一进程里开多个 store，
  // 复用连接名会让 addDatabase 静默替换掉别人的连接。
  connectionName_ = QStringLiteral("lycode_session_store_%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

  database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
  database_.setDatabaseName(target);
  if(!database_.open()) {
    setError(QStringLiteral("open"));
    close();  // 释放半开的连接，避免连接名泄漏
    return false;
  }
  databasePath_ = target;

  if(!applyPragmas(database_, &lastError_)) {
    qCCritical(log) << "open: PRAGMA 初始化失败:" << lastError_;
    close();
    return false;
  }
  if(!ensureSchema()) {
    // ensureSchema() 已经写好 lastError_，这里只补一条 critical 级日志：
    // schema 起不来意味着整个持久化层不可用。
    qCCritical(log) << "open: schema 初始化失败:" << lastError_;
    close();
    return false;
  }

  lastError_.clear();
  qCInfo(log) << "会话库已打开 path=" << databasePath_ << "connection=" << connectionName_;
  return true;
}

void SessionStore::close()
{
  if(database_.isValid() && database_.isOpen()) {
    qCInfo(log) << "会话库关闭 path=" << databasePath_;
    database_.close();
  }
  // 顺序很关键：先让成员放弃连接引用（QSqlDatabase 是值语义的句柄），
  // 再 removeDatabase()。否则 Qt 会告警 "connection is still in use"，
  // 且连接不会被真正移除。
  // 本类的 QSqlQuery 全部是各方法内的局部对象，此调用点必然都已析构。
  database_ = QSqlDatabase();
  if(!connectionName_.isEmpty()) {
    QSqlDatabase::removeDatabase(connectionName_);
    connectionName_.clear();
  }
}

bool SessionStore::isOpen() const
{
  return database_.isValid() && database_.isOpen();
}

// ─────────────────────────────────────────────────────────────────────────────
// 私有辅助
// ─────────────────────────────────────────────────────────────────────────────

bool SessionStore::exec(const QString & sql, const QVariantList & bindings)
{
  QSqlQuery query(database_);
  // exec() 签名里没有调用点标签，就用 SQL 本身当上下文（截断以免日志过长）。
  return runQuery(query, sql, bindings, json::truncate(sql, 200), &lastError_);
}

bool SessionStore::setError(const QString & context) const
{
  // 兜底路径：只用于非查询类失败（open / transaction / commit）。
  // 查询类失败必须走 recordQueryError()，因为 Qt 不会把 QSqlQuery 的错误
  // 传播到 QSqlDatabase::lastError()（实测为空）。
  const QString detail = database_.lastError().text().trimmed();
  lastError_ = detail.isEmpty() ? context : context + QStringLiteral(": ") + detail;
  qCWarning(log) << "存储失败:" << lastError_;
  return false;
}

bool SessionStore::ensureSchema()
{
  // 幂等 DDL：全部 CREATE ... IF NOT EXISTS，可以每次 open() 都跑一遍。
  // schema 的权威定义在 SessionStore.h 的注释与本文件顶部说明里。
  const QStringList statements = {
    QStringLiteral(
    "CREATE TABLE IF NOT EXISTS session ("
    "id TEXT PRIMARY KEY,"
    "title TEXT NOT NULL DEFAULT '',"
    "workspace_path TEXT NOT NULL,"
    "workspace_identity TEXT,"
    "workspace_key TEXT NOT NULL,"
    "remote_session_id TEXT,"
    "parent_session_id TEXT,"
    "kind TEXT NOT NULL DEFAULT 'interactive',"
    "status TEXT NOT NULL DEFAULT 'draft',"
    "mode TEXT NOT NULL DEFAULT 'build',"
    "model_id TEXT,"
    "provider_id TEXT,"
    "created_at_ms INTEGER NOT NULL DEFAULT 0,"
    "updated_at_ms INTEGER NOT NULL DEFAULT 0,"
    "archived_at_ms INTEGER NOT NULL DEFAULT 0,"
    "context_usage_json TEXT NOT NULL DEFAULT '{}',"
    "cumulative_usage_json TEXT NOT NULL DEFAULT '{}',"
    "message_count INTEGER NOT NULL DEFAULT 0)"),
    QStringLiteral(
    "CREATE TABLE IF NOT EXISTS message ("
    "id TEXT PRIMARY KEY,"
    "session_id TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,"
    "role TEXT NOT NULL,"
    "status TEXT NOT NULL,"
    "model_id TEXT,"
    "provider_id TEXT,"
    "parent_message_id TEXT,"
    "created_at_ms INTEGER NOT NULL DEFAULT 0,"
    "updated_at_ms INTEGER NOT NULL DEFAULT 0,"
    "usage_json TEXT NOT NULL DEFAULT '{}',"
    "error_message TEXT,"
    "data TEXT NOT NULL,"
    "sequence INTEGER)"),
    QStringLiteral(
    "CREATE TABLE IF NOT EXISTS part ("
    "id TEXT PRIMARY KEY,"
    "message_id TEXT NOT NULL REFERENCES message(id) ON DELETE CASCADE,"
    "session_id TEXT NOT NULL,"
    "kind TEXT NOT NULL,"
    "data TEXT NOT NULL,"
    "sequence INTEGER,"
    "created_at_ms INTEGER NOT NULL DEFAULT 0,"
    "updated_at_ms INTEGER NOT NULL DEFAULT 0)"),

    // 索引直接对应三处排序/过滤需求：
    //   message 时间线：ORDER BY (sequence IS NULL), sequence, created_at_ms, rowid
    //   part 时间线   ：ORDER BY (sequence IS NULL), sequence, rowid
    //   会话列表      ：WHERE workspace_key = ? ORDER BY updated_at_ms DESC
    // 说明：原本还想把 rowid 显式写进索引尾列，但 SQLite 直接拒绝
    // （CREATE INDEX ... (..., rowid) 报 "no such column: rowid"），
    // 因此退化为不含 rowid 的形式。功能上等价：SQLite 的每个索引项都以
    // rowid 作为隐式最后一列，所以 (session_id, sequence IS NULL, sequence,
    // created_at_ms) 的索引顺序与加 rowid 的排序需求完全一致。
    // (sequence IS NULL) 是表达式索引，SQLite 3.9+ 支持；EXPLAIN QUERY PLAN
    // 实测会走 idx_message_session_order。
    QStringLiteral(
    "CREATE INDEX IF NOT EXISTS idx_message_session_order ON message("
    "session_id, (sequence IS NULL), sequence, created_at_ms)"),
    QStringLiteral(
    "CREATE INDEX IF NOT EXISTS idx_message_session_created ON message(session_id, created_at_ms)"),
    QStringLiteral(
    "CREATE INDEX IF NOT EXISTS idx_part_message_order ON part("
    "message_id, (sequence IS NULL), sequence)"),
    QStringLiteral(
    "CREATE INDEX IF NOT EXISTS idx_session_workspace_updated ON session("
    "workspace_key, updated_at_ms DESC)"),
  };

  for(const QString & sql : statements) {
    if(!exec(sql)) {
      qCCritical(log) << "ensureSchema 失败:" << lastError_;
      return false;
    }
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 会话
// ─────────────────────────────────────────────────────────────────────────────

bool SessionStore::saveSession(const Session & session)
{
  if(!isOpen()) {
    lastError_ = QStringLiteral("saveSession: 数据库未打开");
    qCWarning(log) << lastError_;
    return false;
  }
  if(session.id.isEmpty()) {
    lastError_ = QStringLiteral("saveSession: 会话 id 为空");
    qCWarning(log) << lastError_;
    return false;
  }

  QElapsedTimer timer;
  timer.start();

  // 除 id 与 created_at_ms 外全部列都更新；created_at_ms 只在插入时写入，
  // 更新时保持原值（创建时间不该被后来的保存改写）。
  const QString sql = QStringLiteral(
                        "INSERT INTO session (id, title, workspace_path, workspace_identity, workspace_key, "
                        "remote_session_id, parent_session_id, kind, status, mode, model_id, provider_id, "
                        "created_at_ms, updated_at_ms, archived_at_ms, context_usage_json, "
                        "cumulative_usage_json, message_count) "
                        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                        "ON CONFLICT(id) DO UPDATE SET "
                        "title=excluded.title, workspace_path=excluded.workspace_path, "
                        "workspace_identity=excluded.workspace_identity, workspace_key=excluded.workspace_key, "
                        "remote_session_id=excluded.remote_session_id, "
                        "parent_session_id=excluded.parent_session_id, kind=excluded.kind, "
                        "status=excluded.status, mode=excluded.mode, model_id=excluded.model_id, "
                        "provider_id=excluded.provider_id, updated_at_ms=excluded.updated_at_ms, "
                        "archived_at_ms=excluded.archived_at_ms, "
                        "context_usage_json=excluded.context_usage_json, "
                        "cumulative_usage_json=excluded.cumulative_usage_json, "
                        "message_count=excluded.message_count");

  // workspace_key 是派生列：identity.trim() 非空取 identity，否则取 path
  // （Workspace::key()）。单独存一列是为了让它可索引、可比较，不必在
  // SQL 里重复这段回落逻辑。
  const QVariantList bindings = {
    notNullText(session.id),
    notNullText(session.title),
    notNullText(session.workspace.path),
    nullable(session.workspace.identity),
    notNullText(session.workspace.key()),
    nullable(session.workspace.remoteSessionId),
    nullable(session.parentSessionId),
    notNullText(toToken(session.kind)),
    notNullText(toToken(session.status)),
    notNullText(toToken(session.mode)),
    nullable(session.modelId),
    nullable(session.providerId),
    static_cast<qint64>(session.createdAtMs),
    static_cast<qint64>(session.updatedAtMs),
    static_cast<qint64>(session.archivedAtMs),
    notNullText(QString::fromUtf8(json::toBytes(session.contextUsage))),
    notNullText(QString::fromUtf8(json::toBytes(session.cumulativeUsage.toJson()))),
    session.messageCount,
  };

  QSqlQuery query(database_);
  if(!runQuery(query, sql, bindings, QStringLiteral("saveSession"), &lastError_)) {
    return false;
  }

  qCDebug(log) << "saveSession id=" << session.id << "耗时" << timer.elapsed() << "ms";
  return true;
}

bool SessionStore::loadSession(const Id & sessionId, Session * sessionOut) const
{
  if(sessionOut == nullptr) {
    lastError_ = QStringLiteral("loadSession: sessionOut 为空");
    qCWarning(log) << lastError_;
    return false;
  }
  if(!isOpen()) {
    lastError_ = QStringLiteral("loadSession: 数据库未打开");
    qCWarning(log) << lastError_;
    return false;
  }

  QElapsedTimer timer;
  timer.start();

  const QString sql = QStringLiteral("SELECT %1 FROM session s WHERE s.id = ?")
                      .arg(QString::fromLatin1(kSessionColumns));
  QSqlQuery query(database_);
  if(!runQuery(query, sql, {sessionId}, QStringLiteral("loadSession"), &lastError_)) {
    return false;
  }
  if(!query.next()) {
    lastError_ = QStringLiteral("loadSession: 会话不存在 ") + sessionId;
    qCDebug(log) << lastError_;
    return false;
  }

  // 唯一解析路径：列 → QJsonObject → Session::fromJson()。
  *sessionOut = Session::fromJson(sessionJsonFromRow(query));
  qCDebug(log) << "loadSession id=" << sessionId << "耗时" << timer.elapsed() << "ms";
  return true;
}

QList<SessionSummary> SessionStore::listSessions(const QString & workspaceKey, int limit) const
{
  QList<SessionSummary> result;
  if(!isOpen()) {
    lastError_ = QStringLiteral("listSessions: 数据库未打开");
    qCWarning(log) << lastError_;
    return result;
  }

  QElapsedTimer timer;
  timer.start();

  const int capped = std::clamp(limit, 1, kMaxListLimit);
  QString sql = QStringLiteral("SELECT %1, %2 FROM session s")
                .arg(QString::fromLatin1(kSessionColumns), lastMessageDataSubquery());
  QVariantList bindings;
  if(!workspaceKey.isEmpty()) {
    sql += QStringLiteral(" WHERE s.workspace_key = ?");
    bindings.append(workspaceKey);
  }
  // 次级排序键只为确定性：updated_at_ms 相同（例如同一毫秒内批量写入）时，
  // 没有 tiebreaker 的 SQL 顺序是未定义的，列表会随机跳动。
  sql += QStringLiteral(" ORDER BY s.updated_at_ms DESC, s.created_at_ms DESC, s.rowid DESC LIMIT ?");
  bindings.append(capped);

  QSqlQuery query(database_);
  if(!runQuery(query, sql, bindings, QStringLiteral("listSessions"), &lastError_)) {
    return result;
  }
  while(query.next()) {
    SessionSummary summary;
    summary.session = Session::fromJson(sessionJsonFromRow(query));
    summary.lastMessagePreview =
      previewFromMessageData(query.value(QStringLiteral("last_message_data")));
    result.append(summary);
  }

  qCDebug(log) << "listSessions workspaceKey=" << workspaceKey << "count=" << result.size()
               << "耗时" << timer.elapsed() << "ms";
  return result;
}

bool SessionStore::deleteSession(const Id & sessionId)
{
  if(!isOpen()) {
    lastError_ = QStringLiteral("deleteSession: 数据库未打开");
    qCWarning(log) << lastError_;
    return false;
  }

  QElapsedTimer timer;
  timer.start();

  // message / part 由 ON DELETE CASCADE 带走。前提是打开时
  // PRAGMA foreign_keys=ON 确实生效（open() 里已回读校验，否则拒绝打开）。
  QSqlQuery query(database_);
  if(!runQuery(query, QStringLiteral("DELETE FROM session WHERE id = ?"), {sessionId},
               QStringLiteral("deleteSession"), &lastError_)) {
    return false;
  }
  const int affected = query.numRowsAffected();
  qCDebug(log) << "deleteSession id=" << sessionId << "rows=" << affected << "耗时"
               << timer.elapsed() << "ms";
  if(affected <= 0) {
    lastError_ = QStringLiteral("deleteSession: 会话不存在 ") + sessionId;
    return false;
  }
  return true;
}

int SessionStore::messageCount(const Id & sessionId) const
{
  if(!isOpen()) {
    lastError_ = QStringLiteral("messageCount: 数据库未打开");
    qCWarning(log) << lastError_;
    return 0;
  }

  QElapsedTimer timer;
  timer.start();

  // 以 COUNT(*) 为准：session.message_count 是冗余计数，任何异常路径
  // （外部工具写库、中途崩溃）都会让它变陈旧。
  QSqlQuery countQuery(database_);
  if(!runQuery(countQuery, QStringLiteral("SELECT COUNT(*) FROM message WHERE session_id = ?"),
  {sessionId}, QStringLiteral("messageCount"), &lastError_)) {
    return 0;
  }
  if(!countQuery.next()) {
    return 0;
  }
  const int count = countQuery.value(0).toInt();
  countQuery.finish();

  // 顺手把权威值写回 session 表，避免列表页继续展示陈旧计数。
  // 注意：本方法是 const（头文件契约），database_ / lastError_ 都是 mutable，
  // 因此这里可以回写；失败不影响返回值——COUNT 才是权威结果。
  QSqlQuery refresh(database_);
  if(!runQuery(refresh,
               QStringLiteral("UPDATE session SET message_count = ? WHERE id = ? "
                              "AND message_count <> ?"),
  {count, sessionId, count}, QStringLiteral("messageCount/refresh"), &lastError_)) {
    qCDebug(log) << "messageCount 回写 session 计数失败（不影响返回值）:" << lastError_;
  }

  qCDebug(log) << "messageCount id=" << sessionId << "count=" << count << "耗时"
               << timer.elapsed() << "ms";
  return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// 消息
// ─────────────────────────────────────────────────────────────────────────────

bool SessionStore::saveMessage(const Message & message)
{
  if(!isOpen()) {
    lastError_ = QStringLiteral("saveMessage: 数据库未打开");
    qCWarning(log) << lastError_;
    return false;
  }
  if(message.id.isEmpty() || message.sessionId.isEmpty()) {
    lastError_ = QStringLiteral("saveMessage: message.id / message.sessionId 不能为空");
    qCWarning(log) << lastError_;
    return false;
  }

  QElapsedTimer timer;
  timer.start();

  // 单事务：message 行 + 全部 part 行 + 会话计数要么一起生效，要么一起不生效。
  // 否则读到的「消息已在、part 只写了一半」会被调用方当成完整快照。
  TransactionGuard transaction(database_);
  if(!transaction.isActive()) {
    setError(QStringLiteral("saveMessage: 无法开启事务"));
    return false;
  }

  // 消息没有自己的 providerId 字段（Types.h 的 Message 未建模），
  // 该列按 schema 保留但始终写 NULL；读取路径也不消费它。
  // 会话必须已经存在：message.session_id 有外键约束，未先保存会话会以
  // "FOREIGN KEY constraint failed" 明确失败，而不是留下孤儿消息。
  const QVariantList messageBindings = {
    notNullText(message.id),
    notNullText(message.sessionId),
    notNullText(toToken(message.role)),
    notNullText(toToken(message.status)),
    nullable(message.modelId),
    QVariant(),  // provider_id：Message 无此字段
    nullable(message.parentMessageId),
    static_cast<qint64>(message.createdAtMs),
    static_cast<qint64>(message.updatedAtMs),
    notNullText(QString::fromUtf8(json::toBytes(message.usage.toJson()))),
    nullable(message.errorMessage),
    notNullText(QString::fromUtf8(json::toBytes(message.toJson()))),
    // 队尾分配用的 scope（= session_id），与 ON CONFLICT 里的 CASE 配合。
    notNullText(message.sessionId),
  };

  // sequence 幂等规则（本文件顶部说明的落地形式）：
  //   插入路径：候选值 = 该 session 内 MAX(sequence)+1（全为 NULL 时为 1）。
  //   冲突路径：若新旧 session_id 相同，保留表中现有值（含 NULL）；
  //             只有跨 scope 改绑才采用候选值。
  const QString messageSql = QStringLiteral(
                               "INSERT INTO message (id, session_id, role, status, model_id, provider_id, "
                               "parent_message_id, created_at_ms, updated_at_ms, usage_json, error_message, data, sequence) "
                               "VALUES (?,?,?,?,?,?,?,?,?,?,?,?, "
                               "(SELECT COALESCE(MAX(sequence), 0) + 1 FROM message WHERE session_id = ?)) "
                               "ON CONFLICT(id) DO UPDATE SET "
                               "session_id=excluded.session_id, role=excluded.role, status=excluded.status, "
                               "model_id=excluded.model_id, provider_id=excluded.provider_id, "
                               "parent_message_id=excluded.parent_message_id, updated_at_ms=excluded.updated_at_ms, "
                               "usage_json=excluded.usage_json, error_message=excluded.error_message, data=excluded.data, "
                               "sequence = CASE WHEN message.session_id = excluded.session_id "
                               "THEN message.sequence ELSE excluded.sequence END");

  {
    QSqlQuery query(database_);
    if(!runQuery(query, messageSql, messageBindings, QStringLiteral("saveMessage/message"),
                 &lastError_)) {
      return false;  // 守卫析构 → rollback
    }
  }

  // part 没有自己的时间戳，沿用所属消息的时间（供排障与将来按时间分页）。
  const TimestampMs partCreatedAtMs =
    message.createdAtMs > 0 ? message.createdAtMs : message.updatedAtMs;
  const TimestampMs partUpdatedAtMs =
    message.updatedAtMs > 0 ? message.updatedAtMs : partCreatedAtMs;

  const QString partSql = QStringLiteral(
                            "INSERT INTO part (id, message_id, session_id, kind, data, sequence, created_at_ms, "
                            "updated_at_ms) "
                            "VALUES (?,?,?,?,?, (SELECT COALESCE(MAX(sequence), 0) + 1 FROM part WHERE message_id = ?), "
                            "?,?) "
                            "ON CONFLICT(id) DO UPDATE SET "
                            "message_id=excluded.message_id, session_id=excluded.session_id, kind=excluded.kind, "
                            "data=excluded.data, updated_at_ms=excluded.updated_at_ms, "
                            "sequence = CASE WHEN part.message_id = excluded.message_id "
                            "THEN part.sequence ELSE excluded.sequence END");

  QStringList desiredPartIds;
  desiredPartIds.reserve(message.parts.size());
  for(const Part & part : message.parts) {
    // 空 id 兜底：'' 作为主键会让不同消息的匿名 part 互相覆盖。
    const QString partId = part.id.isEmpty() ? newPartId() : part.id;
    desiredPartIds.append(partId);

    const QVariantList partBindings = {
      notNullText(partId),
      notNullText(message.id),
      notNullText(message.sessionId),
      notNullText(toToken(part.kind)),
      notNullText(QString::fromUtf8(json::toBytes(part.toJson()))),
      notNullText(message.id),  // 队尾分配的 scope = message_id
      static_cast<qint64>(partCreatedAtMs),
      static_cast<qint64>(partUpdatedAtMs),
    };
    QSqlQuery query(database_);
    if(!runQuery(query, partSql, partBindings, QStringLiteral("saveMessage/part"),
                 &lastError_)) {
      return false;
    }
  }

  // 整体替换语义：删掉该消息下不在本次 parts 里的行。
  // 先读完待删 id 再删（不在 SELECT 游标打开时改同一张表），
  // 并且用「逐个 DELETE ... WHERE id = ?」而不是动态拼 IN (?,?,...)：
  // 后者要把占位符数量拼进 SQL，属于本层禁止的字符串拼 SQL 形态。
  {
    QSqlQuery existing(database_);
    if(!runQuery(existing, QStringLiteral("SELECT id FROM part WHERE message_id = ?"),
    {message.id}, QStringLiteral("saveMessage/parts-read"), &lastError_)) {
      return false;
    }
    const QSet<QString> desired(desiredPartIds.begin(), desiredPartIds.end());
    QStringList stalePartIds;
    while(existing.next()) {
      const QString partId = existing.value(0).toString();
      if(!desired.contains(partId)) {
        stalePartIds.append(partId);
      }
    }
    existing.finish();

    for(const QString & staleId : stalePartIds) {
      QSqlQuery remove(database_);
      if(!runQuery(remove, QStringLiteral("DELETE FROM part WHERE id = ?"), {staleId},
                   QStringLiteral("saveMessage/part-delete"), &lastError_)) {
        return false;
      }
    }
    if(!stalePartIds.isEmpty()) {
      qCDebug(log) << "saveMessage 清理过期 part：" << stalePartIds.size();
    }
  }

  // 会话计数与活跃时间。计数用 COUNT(*) 重算（增量维护容易被异常路径带偏）；
  // updated_at_ms 取 MAX(旧值, 消息时间)：只有前进不后退，
  // 否则补写一条历史消息会把旧会话顶到列表最前面。
  const TimestampMs sessionStamp = message.updatedAtMs > 0 ? message.updatedAtMs : nowMs();
  {
    QSqlQuery query(database_);
    const QString sql = QStringLiteral(
                          "UPDATE session SET "
                          "message_count = (SELECT COUNT(*) FROM message WHERE session_id = ?), "
                          "updated_at_ms = MAX(COALESCE(updated_at_ms, 0), ?) WHERE id = ?");
    if(!runQuery(query, sql, {message.sessionId, static_cast<qint64>(sessionStamp),
                              message.sessionId
                             },
                 QStringLiteral("saveMessage/session"), &lastError_)) {
      return false;
    }
  }

  if(!transaction.commit()) {
    setError(QStringLiteral("saveMessage: 提交事务失败"));
    return false;
  }

  qCDebug(log) << "saveMessage id=" << message.id << "parts=" << message.parts.size() << "耗时"
               << timer.elapsed() << "ms";
  return true;
}

bool SessionStore::updateMessageStatus(const Id & messageId, MessageStatus status,
                                       const QJsonObject & usage, const QString & errorMessage)
{
  if(!isOpen()) {
    lastError_ = QStringLiteral("updateMessageStatus: 数据库未打开");
    qCWarning(log) << lastError_;
    return false;
  }

  QElapsedTimer timer;
  timer.start();

  TransactionGuard transaction(database_);
  if(!transaction.isActive()) {
    setError(QStringLiteral("updateMessageStatus: 无法开启事务"));
    return false;
  }

  // 有意只写轻量列，**不重写 data、不动 parts**：本方法服务于流式过程
  // （每个增量更新状态/用量），重写整条 JSON 的代价不可接受；parts 的
  // 权威副本在 part 表，这里也不需要碰。
  // 取舍：data 里的 status/usage/errorMessage 会落后于列值。读取时由
  // messageJsonFromRow() 用列覆盖 data，所以对外的解析结果始终是最新的；
  // 终态会由 saveMessage() 整体落盘，把 data 补成完整快照。
  const qint64 stamp = nowMs();
  {
    QSqlQuery query(database_);
    const QString sql = QStringLiteral(
                          "UPDATE message SET status = ?, usage_json = ?, error_message = ?, updated_at_ms = ? "
                          "WHERE id = ?");
    if(!runQuery(query, sql, {
    toToken(status), QString::fromUtf8(json::toBytes(usage)),
      nullable(errorMessage), stamp, messageId
    },
    QStringLiteral("updateMessageStatus"), &lastError_)) {
      return false;
    }
    if(query.numRowsAffected() <= 0) {
      lastError_ = QStringLiteral("updateMessageStatus: 消息不存在 ") + messageId;
      qCDebug(log) << lastError_;
      return false;  // 守卫析构 → rollback
    }
  }

  // 会话活跃时间同步前进（列表排序依赖它）。
  {
    QSqlQuery query(database_);
    const QString sql = QStringLiteral(
                          "UPDATE session SET updated_at_ms = MAX(COALESCE(updated_at_ms, 0), ?) "
                          "WHERE id = (SELECT session_id FROM message WHERE id = ?)");
    if(!runQuery(query, sql, {stamp, messageId}, QStringLiteral("updateMessageStatus/session"),
                 &lastError_)) {
      return false;
    }
  }

  if(!transaction.commit()) {
    setError(QStringLiteral("updateMessageStatus: 提交事务失败"));
    return false;
  }

  qCDebug(log) << "updateMessageStatus id=" << messageId << "status=" << toToken(status) << "耗时"
               << timer.elapsed() << "ms";
  return true;
}

bool SessionStore::deleteMessage(const Id & messageId)
{
  if(!isOpen()) {
    lastError_ = QStringLiteral("deleteMessage: 数据库未打开");
    qCWarning(log) << lastError_;
    return false;
  }

  QElapsedTimer timer;
  timer.start();

  TransactionGuard transaction(database_);
  if(!transaction.isActive()) {
    setError(QStringLiteral("deleteMessage: 无法开启事务"));
    return false;
  }

  // 先取 session_id：行删掉之后就查不到归属了，而会话计数需要它。
  QString sessionId;
  {
    QSqlQuery query(database_);
    if(!runQuery(query, QStringLiteral("SELECT session_id FROM message WHERE id = ?"),
    {messageId}, QStringLiteral("deleteMessage/session-id"), &lastError_)) {
      return false;
    }
    if(!query.next()) {
      lastError_ = QStringLiteral("deleteMessage: 消息不存在 ") + messageId;
      qCDebug(log) << lastError_;
      return false;
    }
    sessionId = query.value(0).toString();
  }

  {
    // part 由 ON DELETE CASCADE 带走（foreign_keys 已在 open() 校验）。
    QSqlQuery query(database_);
    if(!runQuery(query, QStringLiteral("DELETE FROM message WHERE id = ?"), {messageId},
                 QStringLiteral("deleteMessage"), &lastError_)) {
      return false;
    }
  }

  {
    QSqlQuery query(database_);
    const QString sql = QStringLiteral(
                          "UPDATE session SET "
                          "message_count = (SELECT COUNT(*) FROM message WHERE session_id = ?), "
                          "updated_at_ms = MAX(COALESCE(updated_at_ms, 0), ?) WHERE id = ?");
    if(!runQuery(query, sql, {sessionId, nowMs(), sessionId},
                 QStringLiteral("deleteMessage/session"), &lastError_)) {
      return false;
    }
  }

  if(!transaction.commit()) {
    setError(QStringLiteral("deleteMessage: 提交事务失败"));
    return false;
  }

  qCDebug(log) << "deleteMessage id=" << messageId << "耗时" << timer.elapsed() << "ms";
  return true;
}

QList<Message> SessionStore::loadMessages(const Id & sessionId) const
{
  QList<Message> messages;
  if(!isOpen()) {
    lastError_ = QStringLiteral("loadMessages: 数据库未打开");
    qCWarning(log) << lastError_;
    return messages;
  }

  QElapsedTimer timer;
  timer.start();

  // 排序口径与索引 idx_message_session_order 完全对应：
  // sequence 为 NULL 的排最后，再按 sequence、created_at_ms，最后用 rowid
  // 兜底保证全序（时间戳可能全是 0）。
  const QString sql = QStringLiteral(
                        "SELECT %1 FROM message m WHERE m.session_id = ? "
                        "ORDER BY (m.sequence IS NULL), m.sequence, m.created_at_ms, m.rowid")
                      .arg(QString::fromLatin1(kMessageColumns));

  QList<QJsonObject> rows;
  {
    QSqlQuery query(database_);
    if(!runQuery(query, sql, {sessionId}, QStringLiteral("loadMessages"), &lastError_)) {
      return messages;
    }
    rows.reserve(16);
    while(query.next()) {
      rows.append(messageJsonFromRow(query));
    }
  }  // 游标在这里关闭，下面的批量 part 查询不会与它并存

  // part 一次性批量读（不是每条消息一次查询）。
  QHash<QString, QList<Part>> partsByMessage;
  if(!loadPartsBatch(database_, sessionId, 0, &partsByMessage, &lastError_)) {
    // 见文件头说明：宁可不返回，也不把「缺 part 的消息」交给调用方——
    // Message 是整体替换语义，半成品一旦被重新保存就会删掉真实数据。
    messages.clear();
    return messages;
  }

  messages.reserve(rows.size());
  for(const QJsonObject & row : rows) {
    Message message = Message::fromJson(row);
    const auto it = partsByMessage.constFind(message.id);
    if(it != partsByMessage.constEnd() && !it->isEmpty()) {
      // part 表是权威来源：列覆盖 data 里可能陈旧的 parts 快照。
      message.parts = it.value();
    }
    // 否则保留 data 中的快照：只有 part 表确实没有该消息的行时才走到这里
    // （正常写入路径下两边一致，data 快照只在孤立数据/迁移场景下兜底）。
    messages.append(message);
  }

  qCDebug(log) << "loadMessages sessionId=" << sessionId << "count=" << messages.size() << "耗时"
               << timer.elapsed() << "ms";
  return messages;
}

QList<Message> SessionStore::loadRecentMessages(const Id & sessionId, int limit) const
{
  QList<Message> messages;
  if(!isOpen()) {
    lastError_ = QStringLiteral("loadRecentMessages: 数据库未打开");
    qCWarning(log) << lastError_;
    return messages;
  }

  QElapsedTimer timer;
  timer.start();

  const int capped = std::clamp(limit, 1, kMaxListLimit);

  // 内层按时间线倒序取最近 N 条，外层再正序返回——调用方拿到的仍是
  // 「由旧到新」的对话顺序，与 loadMessages 一致。
  // rowid 以 rid 别名带进派生表：外层排序需要它做全序兜底。
  const QString sql = QStringLiteral(
                        "SELECT %1 FROM (SELECT rowid AS rid, * FROM message "
                        "WHERE session_id = ? "
                        "ORDER BY (sequence IS NULL) DESC, sequence DESC, created_at_ms DESC, "
                        "rid DESC LIMIT ?) AS m "
                        "ORDER BY (m.sequence IS NULL), m.sequence, m.created_at_ms, m.rid")
                      .arg(QString::fromLatin1(kMessageColumns));

  QList<QJsonObject> rows;
  {
    QSqlQuery query(database_);
    if(!runQuery(query, sql, {sessionId, capped}, QStringLiteral("loadRecentMessages"),
                 &lastError_)) {
      return messages;
    }
    rows.reserve(capped);
    while(query.next()) {
      rows.append(messageJsonFromRow(query));
    }
  }

  QHash<QString, QList<Part>> partsByMessage;
  if(!loadPartsBatch(database_, sessionId, capped, &partsByMessage, &lastError_)) {
    messages.clear();
    return messages;
  }

  messages.reserve(rows.size());
  for(const QJsonObject & row : rows) {
    Message message = Message::fromJson(row);
    const auto it = partsByMessage.constFind(message.id);
    if(it != partsByMessage.constEnd() && !it->isEmpty()) {
      message.parts = it.value();
    }
    messages.append(message);
  }

  qCDebug(log) << "loadRecentMessages sessionId=" << sessionId << "limit=" << capped
               << "count=" << messages.size() << "耗时" << timer.elapsed() << "ms";
  return messages;
}

// ─────────────────────────────────────────────────────────────────────────────
// 维护
// ─────────────────────────────────────────────────────────────────────────────

bool SessionStore::deleteSessionsForWorkspace(const QString & workspaceKey)
{
  if(!isOpen()) {
    lastError_ = QStringLiteral("deleteSessionsForWorkspace: 数据库未打开");
    qCWarning(log) << lastError_;
    return false;
  }
  if(workspaceKey.isEmpty()) {
    // listSessions 把空 key 解释为「不过滤」，但删除绝不能沿用这个歧义：
    // 空 key 会删掉全部工作区的会话。这里是显式拒绝，不是功能缺失。
    lastError_ = QStringLiteral("deleteSessionsForWorkspace: workspaceKey 为空，已拒绝执行");
    qCWarning(log) << lastError_;
    return false;
  }

  QElapsedTimer timer;
  timer.start();

  TransactionGuard transaction(database_);
  if(!transaction.isActive()) {
    setError(QStringLiteral("deleteSessionsForWorkspace: 无法开启事务"));
    return false;
  }

  int affected = 0;
  {
    // 会话删除后 message / part 由外键级联清理。
    QSqlQuery query(database_);
    if(!runQuery(query, QStringLiteral("DELETE FROM session WHERE workspace_key = ?"),
    {workspaceKey}, QStringLiteral("deleteSessionsForWorkspace"), &lastError_)) {
      return false;
    }
    affected = query.numRowsAffected();
  }

  if(!transaction.commit()) {
    setError(QStringLiteral("deleteSessionsForWorkspace: 提交事务失败"));
    return false;
  }

  // 0 行不算失败：目标工作区本来就没有会话，语句本身成功了。
  qCInfo(log) << "已删除工作区会话 workspaceKey=" << workspaceKey << "rows=" << affected << "耗时"
              << timer.elapsed() << "ms";
  return true;
}

QList<SessionSummary> SessionStore::searchSessions(const QString & query, int limit) const
{
  QList<SessionSummary> result;
  const QString keyword = query.trimmed();
  if(keyword.isEmpty()) {
    return result;  // 空查询按空结果处理，不做「全量返回」这种昂贵又意外的行为
  }
  if(!isOpen()) {
    lastError_ = QStringLiteral("searchSessions: 数据库未打开");
    qCWarning(log) << lastError_;
    return result;
  }

  QElapsedTimer timer;
  timer.start();

  const int capped = std::clamp(limit, 1, kMaxSearchLimit);
  // 不做 FTS：会话量级是「每工作区几百条」，LIKE 全扫的可接受度远高于
  // 维护一张 FTS5 虚表（及其分词器、重建、随删除同步的复杂度）。
  // 标题直接匹配；正文匹配 message.data（JSON 文本）——对 JSON 文本做
  // LIKE 的副作用是匹配也可能落在 part 的结构字段上，属于可接受的误报。
  // EXISTS 天然去重：一条消息命中不会让同一会话重复出现。
  const QString sql = QStringLiteral(
                        "SELECT %1, %2 FROM session s "
                        "WHERE s.title LIKE ? ESCAPE '\\' OR EXISTS ("
                        "SELECT 1 FROM message m WHERE m.session_id = s.id "
                        "AND m.data LIKE ? ESCAPE '\\') "
                        "ORDER BY s.updated_at_ms DESC, s.created_at_ms DESC, s.rowid DESC "
                        "LIMIT ?")
                      .arg(QString::fromLatin1(kSessionColumns), lastMessageDataSubquery());

  // 值一律走参数绑定（绝无字符串拼接），通配符则转义后再包 %。
  const QString pattern = QStringLiteral("%") + escapeLikePattern(keyword) + QStringLiteral("%");

  QSqlQuery statement(database_);
  if(!runQuery(statement, sql, {pattern, pattern, capped}, QStringLiteral("searchSessions"),
               &lastError_)) {
    return result;
  }
  while(statement.next()) {
    SessionSummary summary;
    summary.session = Session::fromJson(sessionJsonFromRow(statement));
    summary.lastMessagePreview =
      previewFromMessageData(statement.value(QStringLiteral("last_message_data")));
    result.append(summary);
  }

  qCDebug(log) << "searchSessions query=" << keyword << "count=" << result.size() << "耗时"
               << timer.elapsed() << "ms";
  return result;
}

}  // namespace lycode

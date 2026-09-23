// LyCode — JSON 读取辅助
//
// 与外部协议（模型 API、工具输入、持久化文件）打交道时，绝大多数字段都是
// "可能缺失、类型可能不符"的。直接 QJsonObject::value().toString() 会把
// 类型错误静默变成空字符串，导致问题在很远的地方才暴露。
//
// 这里提供一组显式、带默认值的读取函数，把"缺失/类型不符"统一收敛为默认值，
// 并在调试构建下记录一次告警，避免静默数据损坏。
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace lycode::json
{

QString str(const QJsonObject & obj, const QString & key, const QString & fallback = {});
int integer(const QJsonObject & obj, const QString & key, int fallback = 0);
qint64 integer64(const QJsonObject & obj, const QString & key, qint64 fallback = 0);
double number(const QJsonObject & obj, const QString & key, double fallback = 0.0);
bool boolean(const QJsonObject & obj, const QString & key, bool fallback = false);

QJsonObject object(const QJsonObject & obj, const QString & key);
QJsonArray array(const QJsonObject & obj, const QString & key);
QStringList stringList(const QJsonObject & obj, const QString & key);

/// 判断 key 存在且非 null。
bool has(const QJsonObject & obj, const QString & key);

/// 安全的 JSON 文本解析，失败时返回空对象并记录原因。
QJsonObject parseObject(const QByteArray & bytes, QString * errorOut = nullptr);

/// 紧凑序列化（单行），用于 wire 传输。
QByteArray toBytes(const QJsonObject & obj);

/// 带缩进的序列化，用于持久化文件与调试。
QByteArray toPrettyBytes(const QJsonObject & obj);

/// 按长度截断字符串，并在截断处追加标记，用于展示预算控制。
QString truncate(const QString & value, int maxChars, const QString & marker = QStringLiteral("…[truncated]"));

}  // namespace lycode::json

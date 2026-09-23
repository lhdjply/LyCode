// LyCode — 标识符生成
//
// 本实现使用带前缀的随机 id（session_xxx / msg_xxx / call_xxx）便于在日志与
// 数据库里一眼分辨类型。这里保持一致，前缀规则集中在本文件，禁止各处手写。
#pragma once

#include <QString>

namespace lycode
{

/// 生成 `<prefix>_<24位小写base32>` 形式的唯一 id。
/// 使用 QRandomGenerator 的密码学安全源，避免会话 id 可预测。
QString newId(const QString & prefix);

inline QString newSessionId()
{
  return newId(QStringLiteral("session"));
}
inline QString newMessageId()
{
  return newId(QStringLiteral("msg"));
}
inline QString newPartId()
{
  return newId(QStringLiteral("part"));
}
inline QString newToolCallId()
{
  return newId(QStringLiteral("call"));
}
inline QString newPermissionId()
{
  return newId(QStringLiteral("perm"));
}

}  // namespace lycode

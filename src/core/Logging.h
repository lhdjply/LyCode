// LyCode — 日志
//
// 按既定语义的分级约定：
//   debug  — 协议原始数据、流式 chunk、逐条工具更新（高频诊断，默认不落盘）
//   info   — 进程与会话生命周期、权限结果、一次性初始化
//   warn   — 可恢复异常
//   error  — 崩溃、握手失败、鉴权丢失等不可恢复错误
//
// 用法：在 .cpp 顶部 `Q_LOGGING_CATEGORY(log, "lycode.agent")`，
// 然后用 qCDebug(log) / qCInfo(log) / qCWarning(log) / qCCritical(log)。
//
// 绝不写入凭据、真实用户数据或内部服务地址。
#pragma once

#include <QLoggingCategory>
#include <QString>

namespace lycode::logging {

/// 初始化日志规则。默认放行 info 及以上；`--verbose` 打开 debug。
/// 同时把日志写入 `<数据目录>/logs/lycode.log`。
void init(bool verbose);

/// 日志目录（已确保存在）。失败时返回空字符串。
QString logDirectory();

/// 对 API key 等敏感值做脱敏，仅保留首尾少量字符。
QString redact(const QString &secret);

}  // namespace lycode::logging

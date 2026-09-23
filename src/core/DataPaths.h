// LyCode — 数据目录
#pragma once

#include <QString>

namespace lycode
{

/// 数据目录：LYCODE_DATA_BASE_DIR 优先，缺省 Windows 为 `<用户主目录>/lycode`，
/// 其他平台为 `~/.cache/lycode`。
/// 配置、会话、日志、后台任务输出、skills 与用户级 AGENTS.md 都在这里。
QString dataDir();

}  // namespace lycode

// LyCode — 应用配置
//
// 与会话数据分开存放：会话在 SQLite（`<数据目录>/sessions.db`），
// 应用配置在 `<数据目录>/settings.json`。
//
// 分成两个存储的理由：配置是"少写多读、整份替换"的小文档，用 JSON 文件
// 便于用户手工检查与迁移；会话是"高频增量写、按条件查询"的结构化数据，
// 用 SQLite。把两者混在一张表里会让任一侧的演进都变复杂。
//
// 唯一写入路径是 AppConfig::save——任何地方都不允许自行拼 JSON 写盘，
// 否则并发写会互相覆盖。
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "core/Types.h"
#include "mcp/McpProtocol.h"
#include "ui/Theme.h"

namespace lycode::ui
{

/// 全局应用设置。整份替换语义。
struct AppSettings {
  // 外观
  ThemeMode themeMode = ThemeMode::System;
  int uiFontSize = 14;
  int codeFontSize = 14;
  /// 界面语言：`zh-CN` 或 `en-US`。
  QString language = QStringLiteral("zh-CN");

  // 模型
  QList<ProviderConfig> providers;
  ModelSelection lastModel;

  /// 模型能力覆盖，键为 modelOptionKey(providerId, modelId)。
  QHash<QString, ModelOptionOverride> modelOverrides;
  /// 每个模型上次使用的思考档位，键为 modelOptionKey。
  ///
  /// 单独一张表而不是塞进 ModelSelection：档位是"用户对这个模型的偏好"，
  /// 换工作区、换会话都应延续；而 ModelSelection 是会话级的一次性选择。
  QHash<QString, QString> modelReasoningLevel;

  // 工作区
  QStringList recentWorkspaces;
  QString lastWorkspace;
  /// 一个工作区最近使用的模型，键为 Workspace::key()。
  QHash<QString, QString> workspaceLastModel;

  // 行为
  /// 新建会话时的默认模式。
  SessionMode defaultSessionMode = SessionMode::Build;
  /// 是否在会话结束后自动落盘（目前恒为真，保留开关以便将来支持"仅内存会话"）。
  bool persistSessions = true;
  /// 用模型为会话生成标题。
  ///
  /// 默认开启，但**可以关**：它意味着每个新会话多一次模型调用。
  /// 关掉后标题退回"取首条输入的前 40 字符"——无需联网、零成本。
  bool generateSessionTitles = true;

  /// MCP 服务器列表。见 src/mcp/McpProtocol.h。
  QList<lycode::mcp::ServerConfig> mcpServers;

  /// 上一次退出时**处于展开状态**的工作区。
  ///
  /// 不持久化它的话，每次启动只有当前工作区是展开的——用户展开过的其它
  /// 工作区全被收起来，看起来就像"只有一个工作区是打开的"。
  QStringList expandedWorkspaces;

  /// Skills 搜索目录。**为空表示用内置默认**（用户级 + 项目级）。
  /// 非空时只扫这些目录——"我删掉了默认目录"和"我没配过"是两种意图，
  /// 用一个空列表区分不开，所以非空即覆盖。
  QStringList skillDirectories;

  /// 取指定模型的覆盖配置；无记录时返回空覆盖（不是错误）。
  ModelOptionOverride modelOverride(const QString & providerId, const QString & modelId) const;
  /// 写入模型覆盖；空覆盖会删除记录，避免配置文件堆积无用条目。
  void setModelOverride(const QString & providerId, const QString & modelId,
                        const ModelOptionOverride & override);

  /// 取指定模型上次使用的思考档位；无记录返回空（由调用方按模型默认值决定）。
  QString reasoningLevelFor(const QString & providerId, const QString & modelId) const;
  /// 记住某模型使用的思考档位。传空串表示"关闭"，会被持久化。
  void rememberReasoningLevel(const QString & providerId, const QString & modelId,
                              const QString & levelId);

  /// 取指定工作区的最近模型；无记录时返回 lastModel。
  ModelSelection modelForWorkspace(const QString & workspaceKey) const;
  /// 记录工作区最近使用的模型（同时更新 lastModel）。
  void rememberModelForWorkspace(const QString & workspaceKey, const ModelSelection & model);

  QJsonObject toJson() const;
  static AppSettings fromJson(const QJsonObject & json);
};

class AppConfig
{
  public:
    /// 配置文件路径：`<数据目录>/settings.json`。
    static QString configPath();
    /// 配置目录（已确保存在）；失败返回空。
    static QString configDirectory();

    /// 载入配置。文件不存在时返回 true 并给出默认值（首次启动是正常路径，不是错误）。
    /// 只有文件存在但损坏时才返回 false。
    static bool load(AppSettings * settingsOut, QString * errorOut = nullptr);

    /// 原子写入：先写临时文件再 rename，避免写一半掉电留下半个 JSON。
    static bool save(const AppSettings & settings, QString * errorOut = nullptr);
};

}  // namespace lycode::ui

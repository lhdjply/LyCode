#include "ui/AppConfig.h"

#include "core/DataPaths.h"
#include "core/Json.h"
#include "core/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QSaveFile>

namespace lycode::ui
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.config")

}  // namespace

QString AppConfig::configDirectory()
{
  const QString directory = dataDir();
  if(!QDir().mkpath(directory)) {
    qCWarning(log) << "配置目录创建失败:" << directory;
    return {};
  }
  return directory;
}

QString AppConfig::configPath()
{
  const QString directory = configDirectory();
  if(directory.isEmpty()) {
    return {};
  }
  return directory + QStringLiteral("/settings.json");
}

ModelOptionOverride AppSettings::modelOverride(const QString & providerId,
                                               const QString & modelId) const
{
  if(providerId.isEmpty() || modelId.isEmpty()) {
    return {};
  }
  return modelOverrides.value(modelOptionKey(providerId, modelId));
}

void AppSettings::setModelOverride(const QString & providerId, const QString & modelId,
                                   const ModelOptionOverride & override)
{
  if(providerId.isEmpty() || modelId.isEmpty()) {
    return;
  }
  const QString key = modelOptionKey(providerId, modelId);
  if(override.isEmpty()) {
    // 全空等价于"没有覆盖"：删掉记录，别在配置文件里留一堆空对象。
    modelOverrides.remove(key);
    return;
  }
  modelOverrides.insert(key, override);
}

QString AppSettings::reasoningLevelFor(const QString & providerId, const QString & modelId) const
{
  if(providerId.isEmpty() || modelId.isEmpty()) {
    return {};
  }
  return modelReasoningLevel.value(modelOptionKey(providerId, modelId));
}

void AppSettings::rememberReasoningLevel(const QString & providerId, const QString & modelId,
                                         const QString & levelId)
{
  if(providerId.isEmpty() || modelId.isEmpty()) {
    return;
  }
  // 空串是合法值（表示"关闭"），所以这里不删除记录，与 modelOverride 的语义不同。
  modelReasoningLevel.insert(modelOptionKey(providerId, modelId), levelId);
}

ModelSelection AppSettings::modelForWorkspace(const QString & workspaceKey) const
{
  if(!workspaceKey.isEmpty()) {
    const auto iterator = workspaceLastModel.constFind(workspaceKey);
    if(iterator != workspaceLastModel.constEnd()) {
      const ModelSelection parsed = ModelSelection::parseDisplayValue(iterator.value());
      if(parsed.isValid()) {
        return parsed;
      }
      // 记录损坏（例如 provider 被删除）时回退到全局最近模型，而不是返回无效值。
      qCDebug(log) << "工作区最近模型无法解析，回退全局: key=" << workspaceKey;
    }
  }
  return lastModel;
}

void AppSettings::rememberModelForWorkspace(const QString & workspaceKey,
                                            const ModelSelection & model)
{
  if(!model.isValid()) {
    return;
  }
  lastModel = model;
  if(!workspaceKey.isEmpty()) {
    workspaceLastModel.insert(workspaceKey, model.displayValue());
  }
}

QJsonObject AppSettings::toJson() const
{
  QJsonObject result;
  result.insert(QStringLiteral("themeMode"), toToken(themeMode));
  result.insert(QStringLiteral("uiFontSize"), uiFontSize);
  result.insert(QStringLiteral("codeFontSize"), codeFontSize);
  result.insert(QStringLiteral("language"), language);

  QJsonArray providerArray;
  for(const ProviderConfig & provider : providers) {
    providerArray.append(provider.toJson());
  }
  result.insert(QStringLiteral("providers"), providerArray);

  result.insert(QStringLiteral("lastModel"), lastModel.displayValue());
  result.insert(QStringLiteral("recentWorkspaces"),
                QJsonArray::fromStringList(recentWorkspaces));
  result.insert(QStringLiteral("lastWorkspace"), lastWorkspace);

  QJsonObject workspaceModels;
  for(auto it = workspaceLastModel.constBegin(); it != workspaceLastModel.constEnd(); ++it) {
    workspaceModels.insert(it.key(), it.value());
  }
  result.insert(QStringLiteral("workspaceLastModel"), workspaceModels);

  // 局部变量不要叫 modelOverrides：会遮蔽同名成员，导致循环遍历的是空对象，
  // 覆盖配置永远写不出去（实测踩到）。
  QJsonObject overridesJson;
  for(auto it = modelOverrides.constBegin(); it != modelOverrides.constEnd(); ++it) {
    overridesJson.insert(it.key(), it.value().toJson());
  }
  result.insert(QStringLiteral("modelOverrides"), overridesJson);

  QJsonObject reasoningJson;
  for(auto it = modelReasoningLevel.constBegin(); it != modelReasoningLevel.constEnd(); ++it) {
    reasoningJson.insert(it.key(), it.value());
  }
  result.insert(QStringLiteral("modelReasoningLevel"), reasoningJson);

  result.insert(QStringLiteral("defaultSessionMode"), toToken(defaultSessionMode));
  result.insert(QStringLiteral("persistSessions"), persistSessions);
  result.insert(QStringLiteral("generateSessionTitles"), generateSessionTitles);

  QJsonArray servers;
  for(const lycode::mcp::ServerConfig & server : mcpServers) {
    QJsonObject item;
    item.insert(QStringLiteral("id"), server.id);
    item.insert(QStringLiteral("command"), server.command);
    item.insert(QStringLiteral("args"), QJsonArray::fromStringList(server.args));
    item.insert(QStringLiteral("env"), QJsonArray::fromStringList(server.env));
    item.insert(QStringLiteral("enabled"), server.enabled);
    servers.append(item);
  }
  result.insert(QStringLiteral("mcpServers"), servers);
  result.insert(QStringLiteral("expandedWorkspaces"),
                QJsonArray::fromStringList(expandedWorkspaces));
  result.insert(QStringLiteral("skillDirectories"),
                QJsonArray::fromStringList(skillDirectories));
  return result;
}

AppSettings AppSettings::fromJson(const QJsonObject & json)
{
  AppSettings settings;
  settings.themeMode =
    themeModeFromToken(json::str(json, QStringLiteral("themeMode"), QStringLiteral("system")));
  settings.uiFontSize = json::integer(json, QStringLiteral("uiFontSize"), 14);
  settings.codeFontSize = json::integer(json, QStringLiteral("codeFontSize"), 14);
  settings.language = json::str(json, QStringLiteral("language"), QStringLiteral("zh-CN"));

  const QJsonArray providerArray = json::array(json, QStringLiteral("providers"));
  for(const QJsonValue & value : providerArray) {
    if(value.isObject()) {
      settings.providers.append(ProviderConfig::fromJson(value.toObject()));
    }
  }

  settings.lastModel =
    ModelSelection::parseDisplayValue(json::str(json, QStringLiteral("lastModel")));
  settings.recentWorkspaces = json::stringList(json, QStringLiteral("recentWorkspaces"));
  settings.lastWorkspace = json::str(json, QStringLiteral("lastWorkspace"));

  const QJsonObject workspaceModels = json::object(json, QStringLiteral("workspaceLastModel"));
  for(auto it = workspaceModels.constBegin(); it != workspaceModels.constEnd(); ++it) {
    if(it.value().isString()) {
      settings.workspaceLastModel.insert(it.key(), it.value().toString());
    }
  }

  const QJsonObject overridesJson = json::object(json, QStringLiteral("modelOverrides"));
  for(auto it = overridesJson.constBegin(); it != overridesJson.constEnd(); ++it) {
    if(!it.value().isObject()) {
      continue;
    }
    const ModelOptionOverride override = ModelOptionOverride::fromJson(it.value().toObject());
    if(!override.isEmpty()) {
      settings.modelOverrides.insert(it.key(), override);
    }
  }

  const QJsonObject reasoningJson =
    json::object(json, QStringLiteral("modelReasoningLevel"));
  for(auto it = reasoningJson.constBegin(); it != reasoningJson.constEnd(); ++it) {
    if(it.value().isString() && !it.value().toString().isEmpty()) {
      settings.modelReasoningLevel.insert(it.key(), it.value().toString());
    }
  }

  settings.defaultSessionMode = sessionModeFromToken(
                                  json::str(json, QStringLiteral("defaultSessionMode"), QStringLiteral("build")));
  settings.persistSessions = json::boolean(json, QStringLiteral("persistSessions"), true);
  settings.generateSessionTitles =
    json::boolean(json, QStringLiteral("generateSessionTitles"), true);

  for(const QJsonValue & entry : json::array(json, QStringLiteral("expandedWorkspaces"))) {
    const QString path = entry.toString().trimmed();
    if(!path.isEmpty()) {
      settings.expandedWorkspaces.append(path);
    }
  }

  for(const QJsonValue & entry : json::array(json, QStringLiteral("skillDirectories"))) {
    const QString path = entry.toString().trimmed();
    if(!path.isEmpty()) {
      settings.skillDirectories.append(path);
    }
  }

  for(const QJsonValue & value : json::array(json, QStringLiteral("mcpServers"))) {
    const QJsonObject item = value.toObject();
    lycode::mcp::ServerConfig server;
    server.id = json::str(item, QStringLiteral("id"));
    server.command = json::str(item, QStringLiteral("command"));
    for(const QJsonValue & argument : json::array(item, QStringLiteral("args"))) {
      server.args.append(argument.toString());
    }
    for(const QJsonValue & entry : json::array(item, QStringLiteral("env"))) {
      server.env.append(entry.toString());
    }
    server.enabled = json::boolean(item, QStringLiteral("enabled"), true);
    if(server.isValid()) {
      settings.mcpServers.append(server);
    }
    else {
      qCWarning(log) << "忽略配置不完整的 MCP 服务器条目; id=" << server.id;
    }
  }
  return settings;
}

bool AppConfig::load(AppSettings * settingsOut, QString * errorOut)
{
  if(settingsOut == nullptr) {
    return false;
  }

  const QString path = configPath();
  if(path.isEmpty()) {
    if(errorOut != nullptr) {
      *errorOut = QStringLiteral("无法确定配置目录。");
    }
    return false;
  }

  QFile file(path);
  if(!file.exists()) {
    // 首次启动：使用默认值。这是正常路径，不是错误。
    qCInfo(log) << "配置文件不存在，使用默认设置:" << path;
    *settingsOut = AppSettings{};
    return true;
  }

  if(!file.open(QIODevice::ReadOnly)) {
    if(errorOut != nullptr) {
      *errorOut = QStringLiteral("无法读取配置文件：") + file.errorString();
    }
    qCWarning(log) << "配置读取失败:" << path << file.errorString();
    return false;
  }

  QString parseError;
  const QJsonObject json = json::parseObject(file.readAll(), &parseError);
  if(!parseError.isEmpty()) {
    if(errorOut != nullptr) {
      *errorOut = QStringLiteral("配置文件格式错误：") + parseError;
    }
    qCCritical(log) << "配置解析失败:" << path << parseError;
    return false;
  }

  *settingsOut = AppSettings::fromJson(json);
  qCInfo(log) << "配置已载入:" << path << "providers=" << settingsOut->providers.size();
  return true;
}

bool AppConfig::save(const AppSettings & settings, QString * errorOut)
{
  const QString path = configPath();
  if(path.isEmpty()) {
    if(errorOut != nullptr) {
      *errorOut = QStringLiteral("无法确定配置目录。");
    }
    return false;
  }

  // QSaveFile 提供"写临时文件 + 原子 rename"的语义，
  // 中途失败不会留下半个 JSON 把用户配置弄坏。
  QSaveFile file(path);
  if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if(errorOut != nullptr) {
      *errorOut = QStringLiteral("无法写入配置文件：") + file.errorString();
    }
    qCCritical(log) << "配置写入失败:" << path << file.errorString();
    return false;
  }

  const QByteArray payload = json::toPrettyBytes(settings.toJson());
  if(file.write(payload) != payload.size()) {
    if(errorOut != nullptr) {
      *errorOut = QStringLiteral("配置写入不完整。");
    }
    file.cancelWriting();
    return false;
  }

  if(!file.commit()) {
    if(errorOut != nullptr) {
      *errorOut = QStringLiteral("配置提交失败：") + file.errorString();
    }
    qCCritical(log) << "配置提交失败:" << path << file.errorString();
    return false;
  }

  qCInfo(log) << "配置已保存:" << path;
  return true;
}

}  // namespace lycode::ui

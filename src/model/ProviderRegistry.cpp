// LyCode — Provider 注册表实现
//
// 唯一写入路径是 replaceAll() / upsert()：所有 provider 实例都挂在本对象下，
// 按 id 解析；配置不完整的实例仍会保留（便于设置页展示与修复），
// 但 resolve() 会拒绝把它当作可用选择。
#include "model/ProviderRegistry.h"

#include "model/AnthropicProvider.h"
#include "model/OpenAICompatibleProvider.h"

#include <QLoggingCategory>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.model.registry")

/// 按 ProviderKind 构造具体实现并灌入配置。
/// 未知 kind 返回 nullptr；配置不完整只告警，不阻止实例创建。
ModelProvider * createProvider(const ProviderConfig & config, QObject * parent)
{
  ModelProvider * provider = nullptr;
  switch(config.kind) {
    case ProviderKind::Anthropic:
      provider = new AnthropicProvider(parent);
      break;
    case ProviderKind::OpenAICompatible:
      provider = new OpenAICompatibleProvider(parent);
      break;
  }
  if(provider == nullptr) {
    qCWarning(log) << "无法为 provider 创建实现; providerId=" << config.id;
    return nullptr;
  }
  if(!provider->configure(config)) {
    qCWarning(log) << "provider 配置不完整，实例已保留但暂不可用; providerId=" << config.id;
  }
  return provider;
}

}  // namespace

ProviderRegistry::ProviderRegistry(QObject * parent) : QObject(parent) {}

ProviderRegistry::~ProviderRegistry() = default;

void ProviderRegistry::replaceAll(const QList<ProviderConfig> & configs)
{
  const QList<ModelProvider *> previous = providers_.values();
  providers_.clear();
  for(ModelProvider * provider : previous) {
    provider->deleteLater();
  }

  for(const ProviderConfig & config : configs) {
    if(config.id.isEmpty()) {
      qCWarning(log) << "provider 配置缺少 id，已跳过";
      continue;
    }
    ModelProvider * provider = createProvider(config, this);
    if(provider == nullptr) {
      continue;
    }
    // 同一批里出现重复 id 时后者覆盖前者。
    ModelProvider * duplicate = providers_.take(config.id);
    if(duplicate != nullptr) {
      duplicate->deleteLater();
    }
    providers_.insert(config.id, provider);
  }

  qCInfo(log) << "provider 集合已替换; count=" << providers_.size();
  emit changed();
}

void ProviderRegistry::upsert(const ProviderConfig & config)
{
  if(config.id.isEmpty()) {
    qCWarning(log) << "provider 配置缺少 id，已跳过";
    return;
  }

  ModelProvider * existing = providers_.value(config.id, nullptr);
  if(existing != nullptr && existing->kind() == config.kind) {
    // kind 未变则原地更新配置，避免无谓重建网络栈。
    if(!existing->configure(config)) {
      qCWarning(log) << "provider 配置不完整; providerId=" << config.id;
    }
    emit changed();
    return;
  }

  ModelProvider * provider = createProvider(config, this);
  if(provider == nullptr) {
    return;
  }
  if(existing != nullptr) {
    providers_.remove(config.id);
    existing->deleteLater();
  }
  providers_.insert(config.id, provider);
  qCInfo(log) << "provider 已写入; providerId=" << config.id << "kind=" << toToken(config.kind);
  emit changed();
}

ModelProvider * ProviderRegistry::provider(const QString & providerId) const
{
  return providers_.value(providerId, nullptr);
}

ModelProvider * ProviderRegistry::resolve(const ModelSelection & selection, ModelInfo * infoOut) const
{
  if(!selection.isValid()) {
    return nullptr;
  }
  ModelProvider * found = provider(selection.providerId);
  if(found == nullptr) {
    qCWarning(log) << "模型选择指向不存在的 provider; providerId=" << selection.providerId;
    return nullptr;
  }
  if(!found->configuration().isUsable()) {
    qCWarning(log) << "provider 未配置完整，无法解析模型选择; providerId="
                   << selection.providerId;
    return nullptr;
  }
  if(infoOut != nullptr) {
    *infoOut = found->modelInfo(selection.modelId);
    applyOverride(infoOut);
  }
  return found;
}

bool ProviderRegistry::isUsable(const ModelSelection & selection) const
{
  if(!selection.isValid()) {
    return false;
  }
  ModelProvider * found = provider(selection.providerId);
  if(found == nullptr) {
    return false;   // Provider 已被删除/改名：旧选择成了悬空引用
  }
  if(!found->configuration().isUsable()) {
    return false;   // 存在但没配全（缺 apiKey / 被禁用）
  }
  const QStringList models = found->configuration().models;
  if(models.isEmpty()) {
    // 没声明模型列表时没有"存在性"可判，交给 provider 自己决定。
    return true;
  }
  return models.contains(selection.modelId);
}

void ProviderRegistry::setModelOverrides(
  const QHash<QString, ModelOptionOverride> & overrides)
{
  overrides_ = overrides;
  qCInfo(log) << "模型能力覆盖已更新; count=" << overrides_.size();
  emit changed();
}

ModelInfo ProviderRegistry::effectiveModelInfo(const QString & providerId,
                                               const QString & modelId) const
{
  ModelInfo info;
  info.providerId = providerId;
  info.modelId = modelId;
  info.displayName = modelId;
  if(ModelProvider * found = provider(providerId)) {
    info = found->modelInfo(modelId);
  }
  applyOverride(&info);
  return info;
}

void ProviderRegistry::applyOverride(ModelInfo * info) const
{
  if(info == nullptr) {
    return;
  }
  const auto iterator =
    overrides_.constFind(modelOptionKey(info->providerId, info->modelId));
  if(iterator == overrides_.constEnd()) {
    return;
  }
  iterator.value().applyTo(info);
}

QStringList ProviderRegistry::providerIds() const
{
  QStringList ids = providers_.keys();
  ids.sort();
  return ids;
}

QList<ProviderConfig> ProviderRegistry::configurations() const
{
  const QStringList ids = providerIds();
  QList<ProviderConfig> result;
  result.reserve(ids.size());
  for(const QString & id : ids) {
    ModelProvider * provider = providers_.value(id, nullptr);
    if(provider != nullptr) {
      result.append(provider->configuration());
    }
  }
  return result;
}

QList<ModelInfo> ProviderRegistry::allModels() const
{
  QList<ModelInfo> result;
  const QStringList ids = providerIds();
  for(const QString & id : ids) {
    ModelProvider * provider = providers_.value(id, nullptr);
    if(provider == nullptr) {
      continue;
    }
    const QStringList models = provider->modelIds();
    result.reserve(result.size() + models.size());
    for(const QString & modelId : models) {
      ModelInfo info = provider->modelInfo(modelId);
      applyOverride(&info);
      result.append(info);
    }
  }
  return result;
}

bool ProviderRegistry::hasUsableProvider() const
{
  for(ModelProvider * provider : providers_) {
    if(provider->configuration().isUsable()) {
      return true;
    }
  }
  return false;
}

}  // namespace lycode

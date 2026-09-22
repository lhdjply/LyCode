// LyCode — Provider 注册表
//
// 持有全部已配置的 provider 实例，按 id 解析。
// 唯一写入路径：replaceAll() / upsert()，避免多个地方各自改配置导致漂移。
#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>

#include "core/Types.h"
#include "model/ModelProvider.h"

namespace lycode {

class ProviderRegistry : public QObject {
    Q_OBJECT

public:
    explicit ProviderRegistry(QObject *parent = nullptr);
    ~ProviderRegistry() override;

    /// 用给定配置整体替换现有 provider 集合。
    /// 会按 ProviderKind 创建对应实现；配置不完整的 provider 仍会被创建，
    /// 但在 stream() 前会因 configure() 失败而被跳过。
    void replaceAll(const QList<ProviderConfig> &configs);

    /// 新增或更新单个 provider。
    void upsert(const ProviderConfig &config);

    /// 按 id 取 provider；不存在返回 nullptr。
    ModelProvider *provider(const QString &providerId) const;

    /// 解析模型选择到具体 provider + 模型元信息。
    /// 返回 nullptr 表示选择无效（provider 不存在或未配置）。
    ModelProvider *resolve(const ModelSelection &selection, ModelInfo *infoOut = nullptr) const;

    QStringList providerIds() const;
    QList<ProviderConfig> configurations() const;

    /// 全部 provider 的全部模型，供设置页与模型选择器展示。
    /// 返回的每一项都已应用用户的模型能力覆盖。
    QList<ModelInfo> allModels() const;

    /// 设置模型能力覆盖（键为 modelOptionKey）。覆盖会在 resolve/allModels
    /// 返回前应用到模型元信息上，因此**所有**读模型信息的路径都自动生效——
    /// 这是让"上下文窗口设置"只需一处写入的关键。
    void setModelOverrides(const QHash<QString, ModelOptionOverride> &overrides);
    /// 当前覆盖集合。
    QHash<QString, ModelOptionOverride> modelOverrides() const { return overrides_; }
    /// 取单个模型的有效元信息（已应用覆盖）。provider 不存在时返回默认值。
    ModelInfo effectiveModelInfo(const QString &providerId, const QString &modelId) const;

    /// 是否有任何可用（配置完整）的 provider。
    bool hasUsableProvider() const;

signals:
    /// 集合或配置发生变化。
    void changed();

private:
    /// 把覆盖应用到一份模型元信息上。集中在一处，避免每个读取点各写一遍。
    void applyOverride(ModelInfo *info) const;

    QHash<QString, ModelProvider *> providers_;
    QHash<QString, ModelOptionOverride> overrides_;
};

}  // namespace lycode

// ZCode Qt — OpenAI Chat Completions 兼容流式 Provider
//
// 面向 OpenAI 官方与一切兼容网关（自建代理、vLLM、DashScope 兼容模式等）。
// 协议要点（对齐 npm 版）：
//   POST {baseUrl}/chat/completions
//   Authorization: Bearer <key>
//   stream + stream_options.include_usage（否则拿不到流式 usage）
#pragma once

#include "model/ModelProvider.h"

class QNetworkAccessManager;

namespace zcode {

class OpenAICompatibleProvider : public ModelProvider {
    Q_OBJECT

public:
    explicit OpenAICompatibleProvider(QObject *parent = nullptr);
    ~OpenAICompatibleProvider() override;

    QString providerId() const override;
    ProviderKind kind() const override;
    QStringList modelIds() const override;

    ModelStream *stream(const ModelRequest &request) override;

    bool configure(const ProviderConfig &config) override;
    ProviderConfig configuration() const override;

    ModelInfo modelInfo(const QString &modelId) const override;

private:
    ProviderConfig config_;
    /// 每个 Provider 实例独占一个网络管理器，生命周期跟随本对象。
    QNetworkAccessManager *network_ = nullptr;
};

}  // namespace zcode

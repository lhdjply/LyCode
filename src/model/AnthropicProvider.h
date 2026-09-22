// LyCode — Anthropic Messages API 流式 Provider
//
// 协议要点（按既定语义）：
//   POST {baseUrl}/v1/messages
//   x-api-key + anthropic-version: 2023-06-01
//   SSE 事件 message_start / content_block_* / message_delta / message_stop / error / ping
#pragma once

#include "model/ModelProvider.h"

class QNetworkAccessManager;

namespace lycode {

class AnthropicProvider : public ModelProvider {
    Q_OBJECT

public:
    explicit AnthropicProvider(QObject *parent = nullptr);
    ~AnthropicProvider() override;

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

}  // namespace lycode

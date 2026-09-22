// ZCode Qt — OpenAI Chat Completions 兼容流式客户端实现
//
// 兼容范围：OpenAI 官方 /chat/completions，以及一切同形状网关
// （自建代理、vLLM、DashScope 兼容模式等）。
// 工具调用的分片靠 `index` 关联（首片才带 id/name），这一点与 Anthropic 不同。
#include "model/OpenAICompatibleProvider.h"

#include "core/Json.h"
#include "core/Logging.h"
#include "model/SseParser.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.model.openai")

constexpr int kTransferTimeoutMs = 600000;
constexpr int kIdleTimeoutMs = 60000;
constexpr int kErrorBodyLimit = 2048;

/// baseUrl 归一：去尾部 '/'。
QString normalizeBaseUrl(const QString &baseUrl) {
    QString base = baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return base;
}

/// 拼接 chat/completions 端点；baseUrl 已以 /v1 结尾时不再重复拼 /v1。
QString chatCompletionsEndpoint(const QString &baseUrl) {
    const QString base = normalizeBaseUrl(baseUrl);
    if (base.endsWith(QStringLiteral("/v1"))) {
        return base + QStringLiteral("/chat/completions");
    }
    return base + QStringLiteral("/v1/chat/completions");
}

/// 自建网关的附加请求头；非字符串值忽略。
void applyExtraHeaders(QNetworkRequest &request, const QJsonObject &headers) {
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        if (it.value().isString()) {
            request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
        } else {
            qCWarning(log) << "extraHeaders 值不是字符串，已忽略; key=" << it.key();
        }
    }
}

/// 工具入参：优先用已解析的 input；否则尝试解析流式累积的原始文本。
QJsonObject toolInputObject(const ToolPart &tool) {
    if (!tool.input.isEmpty()) {
        return tool.input;
    }
    const QString raw = tool.inputText.trimmed();
    if (raw.isEmpty()) {
        return {};
    }
    QString error;
    const QJsonObject parsed = json::parseObject(raw.toUtf8(), &error);
    if (!error.isEmpty()) {
        qCWarning(log) << "工具入参原始文本解析失败，按空对象回传; callId=" << tool.callId
                       << "error=" << error;
        return {};
    }
    return parsed;
}

/// 工具结果文本；空输出退回错误信息，再退回占位符。
QString toolResultContent(const ToolPart &tool) {
    QString content = tool.output;
    if (content.isEmpty()) {
        content = tool.error;
    }
    if (content.isEmpty()) {
        content = QStringLiteral("(no output)");
    }
    return content;
}

/// 拼接消息里所有 Text part（OpenAI 的 user/assistant content 是单一字符串）。
QString joinTextParts(const Message &message) {
    QStringList chunks;
    for (const Part &part : message.parts) {
        if (part.kind == PartKind::Text && !part.text.text.isEmpty()) {
            chunks.append(part.text.text);
        }
    }
    return chunks.join(QStringLiteral("\n"));
}

/// 领域消息 → OpenAI messages 数组。
QJsonArray buildMessages(const QString &systemPrompt, const QList<Message> &messages) {
    QJsonArray result;

    // system 是独立角色，固定放最前。
    if (!systemPrompt.isEmpty()) {
        QJsonObject system;
        system.insert(QStringLiteral("role"), QStringLiteral("system"));
        system.insert(QStringLiteral("content"), systemPrompt);
        result.append(system);
    }

    for (const Message &message : messages) {
        if (message.role == MessageRole::System) {
            // 本地记录（压缩摘要等）不回传给 provider。
            continue;
        }

        if (message.role == MessageRole::User) {
            // 工具结果必须是独立的 role=tool 消息，且要紧跟对应的 assistant
            // tool_calls，因此先放 tool 消息，再放同一消息里的用户正文。
            for (const Part &part : message.parts) {
                if (part.kind != PartKind::Tool) {
                    continue;
                }
                if (part.tool.callId.isEmpty()) {
                    qCWarning(log) << "工具结果缺少 callId，已跳过";
                    continue;
                }
                QJsonObject tool;
                tool.insert(QStringLiteral("role"), QStringLiteral("tool"));
                tool.insert(QStringLiteral("tool_call_id"), part.tool.callId);
                tool.insert(QStringLiteral("content"), toolResultContent(part.tool));
                result.append(tool);
            }

            const QString text = joinTextParts(message);
            if (!text.isEmpty()) {
                QJsonObject user;
                user.insert(QStringLiteral("role"), QStringLiteral("user"));
                user.insert(QStringLiteral("content"), text);
                result.append(user);
            }
            continue;
        }

        // assistant：正文 + tool_calls（有工具调用时 content 必须为 null）。
        QJsonArray toolCalls;
        for (const Part &part : message.parts) {
            if (part.kind != PartKind::Tool) {
                continue;
            }
            if (part.tool.callId.isEmpty() || part.tool.name.isEmpty()) {
                qCWarning(log) << "assistant 工具块缺少 callId/name，已跳过; callId="
                               << part.tool.callId;
                continue;
            }
            QJsonObject function;
            function.insert(QStringLiteral("name"), part.tool.name);
            function.insert(QStringLiteral("arguments"),
                            QString::fromUtf8(json::toBytes(toolInputObject(part.tool))));
            QJsonObject call;
            call.insert(QStringLiteral("id"), part.tool.callId);
            call.insert(QStringLiteral("type"), QStringLiteral("function"));
            call.insert(QStringLiteral("function"), function);
            toolCalls.append(call);
        }

        const QString text = joinTextParts(message);
        if (toolCalls.isEmpty() && text.isEmpty()) {
            qCDebug(log) << "assistant 消息内容为空，已从请求中跳过";
            continue;
        }

        QJsonObject assistant;
        assistant.insert(QStringLiteral("role"), QStringLiteral("assistant"));
        if (toolCalls.isEmpty()) {
            assistant.insert(QStringLiteral("content"), text);
        } else {
            assistant.insert(QStringLiteral("content"), QJsonValue::Null);
            assistant.insert(QStringLiteral("tool_calls"), toolCalls);
        }
        result.append(assistant);
    }
    return result;
}

/// 工具声明 → OpenAI tools 数组。
QJsonArray buildTools(const QList<ToolSpec> &tools) {
    QJsonArray result;
    for (const ToolSpec &spec : tools) {
        if (spec.name.isEmpty()) {
            qCWarning(log) << "工具声明缺少 name，已跳过";
            continue;
        }
        QJsonObject function;
        function.insert(QStringLiteral("name"), spec.name);
        if (!spec.description.isEmpty()) {
            function.insert(QStringLiteral("description"), spec.description);
        }
        QJsonObject parameters = spec.inputSchema;
        if (parameters.isEmpty()) {
            parameters.insert(QStringLiteral("type"), QStringLiteral("object"));
            parameters.insert(QStringLiteral("properties"), QJsonObject());
        }
        function.insert(QStringLiteral("parameters"), parameters);

        QJsonObject entry;
        entry.insert(QStringLiteral("type"), QStringLiteral("function"));
        entry.insert(QStringLiteral("function"), function);
        result.append(entry);
    }
    return result;
}

/// 组装请求体。
QJsonObject buildRequestBody(const ModelRequest &request) {
    QJsonObject body;
    body.insert(QStringLiteral("model"), request.modelId);
    body.insert(QStringLiteral("messages"), buildMessages(request.systemPrompt, request.messages));
    body.insert(QStringLiteral("stream"), true);

    // 不带 include_usage 时，流式响应不会返回任何 usage。
    QJsonObject streamOptions;
    streamOptions.insert(QStringLiteral("include_usage"), true);
    body.insert(QStringLiteral("stream_options"), streamOptions);

    const QJsonArray tools = buildTools(request.tools);
    if (!tools.isEmpty()) {
        body.insert(QStringLiteral("tools"), tools);
    }
    body.insert(QStringLiteral("temperature"), request.temperature);
    body.insert(QStringLiteral("max_tokens"), request.maxOutputTokens);

    // 思考强度：OpenAI 兼容协议用 reasoning_effort 表达，与 Anthropic 的
    // thinking.budget_tokens 是同一件事的两种写法。空值不传——部分兼容网关
    // 见到未知字段会直接 400。
    if (!request.reasoningEffort.isEmpty()) {
        body.insert(QStringLiteral("reasoning_effort"), request.reasoningEffort);
    }
    return body;
}

/// 错误体截断为可读文本。
QString truncateBody(const QByteArray &body) {
    QString text = QString::fromUtf8(body.left(kErrorBodyLimit)).trimmed();
    if (body.size() > kErrorBodyLimit) {
        text += QStringLiteral("…[truncated]");
    }
    return text;
}

/// finish_reason 归一：tool_calls→tool_use，stop→end_turn，length→max_tokens，其余原样。
QString normalizeFinishReason(const QString &reason) {
    if (reason == QStringLiteral("tool_calls")) {
        return QStringLiteral("tool_use");
    }
    if (reason == QStringLiteral("stop")) {
        return QStringLiteral("end_turn");
    }
    if (reason == QStringLiteral("length")) {
        return QStringLiteral("max_tokens");
    }
    static const QStringList known = {QStringLiteral("content_filter"), QStringLiteral("function_call")};
    if (!known.contains(reason)) {
        qCWarning(log) << "未知 finish_reason，原样透传:" << reason;
    }
    return reason;
}

// ─────────────────────────────────────────────────────────────────────────────
// 单次流式调用
// ─────────────────────────────────────────────────────────────────────────────

class OpenAiStream final : public ModelStream {
public:
    OpenAiStream(QNetworkAccessManager *network, QUrl url, QByteArray payload, QByteArray apiKey,
                 QJsonObject extraHeaders)
        : network_(network),
          url_(std::move(url)),
          payload_(std::move(payload)),
          apiKey_(std::move(apiKey)),
          extraHeaders_(std::move(extraHeaders)) {
        idleTimer_ = new QTimer(this);
        idleTimer_->setSingleShot(true);
        idleTimer_->setInterval(kIdleTimeoutMs);
        connect(idleTimer_, &QTimer::timeout, this, [this] {
            fail(QStringLiteral("流空闲超时：%1 毫秒内未收到任何数据").arg(kIdleTimeoutMs));
        });
    }

    void start() {
        if (terminal_) {
            return;
        }
        if (network_ == nullptr || url_.isEmpty()) {
            fail(QStringLiteral("OpenAI 兼容 provider 未正确配置 baseUrl"));
            return;
        }

        QNetworkRequest request(url_);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("accept", "text/event-stream");
        request.setRawHeader("authorization", QByteArrayLiteral("Bearer ") + apiKey_);
        applyExtraHeaders(request, extraHeaders_);
        request.setTransferTimeout(kTransferTimeoutMs);

        qCInfo(log) << "请求开始; url=" << url_.toString();
        reply_ = network_->post(request, payload_);
        if (reply_ == nullptr) {
            fail(QStringLiteral("无法创建网络请求"));
            return;
        }
        connect(reply_, &QNetworkReply::readyRead, this, [this] { onReadyRead(); });
        connect(reply_, &QNetworkReply::finished, this, [this] { onReplyFinished(); });
        connect(reply_, &QObject::destroyed, this, [this] {
            if (terminal_) {
                return;
            }
            reply_ = nullptr;
            fail(QStringLiteral("网络管理器已销毁，请求中断"));
        });
        idleTimer_->start();
    }

    void abort() override {
        if (terminal_ || isFinished()) {
            return;
        }
        abortRequested_ = true;
        // 注意：abort() 可能同步发出 finished()，从而在 onReplyFinished 里把
        // reply_ 置空并终止本流，因此必须用局部指针并先检查 terminal_。
        QNetworkReply *reply = reply_;
        if (reply != nullptr) {
            reply->abort();
            if (!terminal_ && reply->isFinished()) {
                onReplyFinished();
            }
        } else {
            fail(QStringLiteral("请求已取消"));
        }
    }

private:
    /// 单个工具调用的流式累积状态。
    struct ToolCallState {
        QString id;
        QString name;
        QString arguments;
        bool started = false;
        bool closed = false;
    };

    void onReadyRead() {
        if (terminal_ || reply_ == nullptr || !reply_->isOpen()) {
            return;
        }
        const QByteArray chunk = reply_->readAll();
        if (chunk.isEmpty()) {
            return;
        }
        idleTimer_->start();

        const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400) {
            errorBody_.append(chunk);
            if (errorBody_.size() > kErrorBodyLimit * 4) {
                errorBody_ = errorBody_.left(kErrorBodyLimit * 4);
            }
            return;
        }
        parser_.feed(chunk);
        processParser();
    }

    void onReplyFinished() {
        if (terminal_ || reply_ == nullptr) {
            return;
        }
        idleTimer_->stop();

        const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError netError = reply_->error();
        const QString netErrorString = reply_->errorString();

        // finished 之后仍可能有未读字节（[DONE] 之后不收尾的网关）。
        // 被 abort 的 reply 已关闭，此时不能读，否则 Qt 会打印设备未打开的告警。
        QByteArray rest;
        if (reply_->isOpen() && reply_->bytesAvailable() > 0) {
            rest = reply_->readAll();
        }
        if (!rest.isEmpty()) {
            if (status >= 400) {
                errorBody_.append(rest);
            } else {
                parser_.feed(rest);
            }
        }

        if (status >= 400) {
            if (status == 401 || status == 403) {
                qCCritical(log) << "鉴权失败; HTTP" << status;
            }
            fail(QStringLiteral("HTTP %1: %2").arg(status).arg(truncateBody(errorBody_)));
            return;
        }

        if (netError != QNetworkReply::NoError) {
            if (abortRequested_ || netError == QNetworkReply::OperationCanceledError) {
                fail(QStringLiteral("请求已取消"));
            } else {
                qCCritical(log) << "连接失败:" << netErrorString;
                fail(netErrorString);
            }
            return;
        }

        parser_.finish();
        processParser();
        if (terminal_) {
            return;
        }
        // 连接正常关闭：补齐工具调用、发 usage、收尾。
        finishStream();
    }

    void processParser() {
        while (!terminal_ && parser_.hasNext()) {
            handleEvent(parser_.next());
        }
    }

    void handleEvent(const SseEvent &event) {
        if (terminal_) {
            return;
        }
        const QString data = event.data.trimmed();
        if (data.isEmpty()) {
            return;
        }
        qCDebug(log) << "SSE chunk; data=" << data.left(500);

        if (data == QStringLiteral("[DONE]")) {
            // [DONE] 是标准收尾信号：补齐未闭合的工具调用、发 Usage、发 Completed。
            finishStream();
            return;
        }

        QString error;
        const QJsonObject chunk = json::parseObject(data.toUtf8(), &error);
        if (!error.isEmpty()) {
            fail(QStringLiteral("无法解析流式 chunk JSON: %1; 原始片段: %2")
                     .arg(error, json::truncate(data, 500)));
            return;
        }
        handleChunk(chunk);
    }

    void handleChunk(const QJsonObject &chunk) {
        // 与 Anthropic 的 message_start 对齐：首个成功解析的 chunk 发出 Started，
        // 让两个 provider 对上层暴露一致的"流已开始"信号（对齐 AI SDK 的 start 事件）。
        if (!startedEmitted_) {
            startedEmitted_ = true;
            emitSafe(StreamEvent::started());
        }

        bool usagePresent = false;
        if (json::has(chunk, QStringLiteral("usage"))) {
            const QJsonObject usage = json::object(chunk, QStringLiteral("usage"));
            if (!usage.isEmpty()) {
                applyUsage(usage);
                usagePresent = true;
            }
        }

        const QJsonArray choices = json::array(chunk, QStringLiteral("choices"));
        if (!choices.isEmpty()) {
            const QJsonValue first = choices.at(0);
            if (first.isObject()) {
                const QJsonObject choice = first.toObject();
                const QJsonObject delta = json::object(choice, QStringLiteral("delta"));
                handleTextDelta(delta);
                handleReasoningDelta(delta);
                handleToolCallDeltas(delta);

                const QString finishReason = json::str(choice, QStringLiteral("finish_reason"));
                if (!finishReason.isEmpty()) {
                    handleFinishReason(finishReason);
                }
            }
        }

        // choices 为空的 chunk 通常只带 usage，是 OpenAI 的收尾用法。
        if (usagePresent) {
            emitUsageIfNeeded();
        }
    }

    void handleTextDelta(const QJsonObject &delta) {
        const QJsonValue content = delta.value(QStringLiteral("content"));
        if (content.isString()) {
            const QString text = content.toString();
            if (!text.isEmpty()) {
                emitSafe(StreamEvent::textDelta(text));
            }
        } else if (!content.isUndefined() && !content.isNull()) {
            qCWarning(log) << "delta.content 不是字符串，已忽略; type="
                           << static_cast<int>(content.type());
        }
    }

    void handleReasoningDelta(const QJsonObject &delta) {
        // 部分兼容网关用 reasoning_content，另一部分用 reasoning。
        QString text = json::str(delta, QStringLiteral("reasoning_content"));
        if (text.isEmpty()) {
            text = json::str(delta, QStringLiteral("reasoning"));
        }
        if (!text.isEmpty()) {
            emitSafe(StreamEvent::reasoningDelta(text));
        }
    }

    void handleToolCallDeltas(const QJsonObject &delta) {
        const QJsonArray toolCalls = json::array(delta, QStringLiteral("tool_calls"));
        for (const QJsonValue &value : toolCalls) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject item = value.toObject();
            const int index = json::integer(item, QStringLiteral("index"));

            ToolCallState &state = toolCalls_[index];
            const QString id = json::str(item, QStringLiteral("id"));
            if (!id.isEmpty()) {
                state.id = id;
            }
            const QJsonObject function = json::object(item, QStringLiteral("function"));
            const QString name = json::str(function, QStringLiteral("name"));
            if (!name.isEmpty()) {
                state.name = name;
            }

            const QString argumentsDelta = json::str(function, QStringLiteral("arguments"));
            if (!state.started && (!state.name.isEmpty() || !argumentsDelta.isEmpty())) {
                // 首片携带 id/name；若网关把参数排在 name 之前，这里先开出调用，
                // 保证后续 delta 有归属（name 缺失只告警，不影响流继续）。
                if (state.name.isEmpty()) {
                    qCWarning(log) << "工具调用分片缺少 function.name; index=" << index;
                }
                state.started = true;
                StreamEvent start;
                start.kind = StreamEventKind::ToolCallStart;
                start.toolCallId = state.id;
                start.toolName = state.name;
                emitSafe(start);
            }

            if (!argumentsDelta.isEmpty()) {
                state.arguments += argumentsDelta;
                StreamEvent event;
                event.kind = StreamEventKind::ToolCallDelta;
                event.toolCallId = state.id;
                event.argumentsDelta = argumentsDelta;
                emitSafe(event);
            }
        }
    }

    void handleFinishReason(const QString &reason) {
        finishReason_ = normalizeFinishReason(reason);
        // 先把仍未闭合的工具调用按 index 顺序收口。
        closeAllToolCalls();
        if (terminal_) {
            return;
        }
        // 用量若已到（少数网关在同片给出），可以直接收尾；否则等最后的
        // usage-only chunk 或 [DONE]，避免 Usage 事件因为已终止而被丢弃。
        if (usageSent_) {
            finishStream();
        }
    }

    void closeAllToolCalls() {
        QList<int> indices = toolCalls_.keys();
        std::sort(indices.begin(), indices.end());
        for (int index : indices) {
            if (terminal_) {
                return;
            }
            ToolCallState &state = toolCalls_[index];
            if (state.closed) {
                continue;
            }
            state.closed = true;

            if (!state.started) {
                // 没有 name 也从没见过首片：补一个 Start，避免消费者只收到 End。
                state.started = true;
                StreamEvent start;
                start.kind = StreamEventKind::ToolCallStart;
                start.toolCallId = state.id;
                start.toolName = state.name;
                emitSafe(start);
                if (terminal_) {
                    return;
                }
            }

            QJsonObject input;
            const QString raw = state.arguments.trimmed();
            if (!raw.isEmpty()) {
                QString error;
                input = json::parseObject(raw.toUtf8(), &error);
                if (!error.isEmpty()) {
                    fail(QStringLiteral("工具入参 JSON 解析失败: %1; 原始片段: %2")
                             .arg(error, json::truncate(raw, 300)));
                    return;
                }
            }

            StreamEvent end;
            end.kind = StreamEventKind::ToolCallEnd;
            end.toolCallId = state.id;
            end.toolName = state.name;
            end.toolInput = input;
            emitSafe(end);
        }
    }

    /// 统一收尾：[补齐工具调用] → [Usage（若尚未发）] → [Completed]。
    void finishStream() {
        if (terminal_) {
            return;
        }
        closeAllToolCalls();
        if (terminal_) {
            return;
        }
        emitUsageIfNeeded();
        if (terminal_) {
            return;
        }
        const QString reason = finishReason_.isEmpty() ? QStringLiteral("end_turn") : finishReason_;
        qCInfo(log) << "请求结束; finishReason=" << reason << "inputTokens=" << usage_.inputTokens
                    << "outputTokens=" << usage_.outputTokens
                    << "reasoningTokens=" << usage_.reasoningTokens
                    << "cacheReadTokens=" << usage_.cacheReadTokens;
        terminate(StreamEvent::completed(reason));
    }

    void emitUsageIfNeeded() {
        if (usageSent_ || terminal_) {
            return;
        }
        usageSent_ = true;
        StreamEvent event;
        event.kind = StreamEventKind::Usage;
        event.usage = usage_;
        emitSafe(event);
    }

    void applyUsage(const QJsonObject &usage) {
        usage_.inputTokens =
            json::integer(usage, QStringLiteral("prompt_tokens"), usage_.inputTokens);
        usage_.outputTokens =
            json::integer(usage, QStringLiteral("completion_tokens"), usage_.outputTokens);
        usage_.totalTokens =
            json::integer(usage, QStringLiteral("total_tokens"), usage_.totalTokens);
        const QJsonObject completionDetails =
            json::object(usage, QStringLiteral("completion_tokens_details"));
        usage_.reasoningTokens = json::integer(completionDetails, QStringLiteral("reasoning_tokens"),
                                               usage_.reasoningTokens);
        const QJsonObject promptDetails =
            json::object(usage, QStringLiteral("prompt_tokens_details"));
        usage_.cacheReadTokens =
            json::integer(promptDetails, QStringLiteral("cached_tokens"), usage_.cacheReadTokens);
    }

    void fail(const QString &message) {
        if (terminal_) {
            return;
        }
        qCWarning(log) << "流失败:" << message;
        terminate(StreamEvent::failed(message));
    }

    void emitSafe(const StreamEvent &event) {
        if (!terminal_) {
            emitEvent(event);
        }
    }

    /// 唯一的终止出口：保证恰好一次终止事件，并在终止后释放网络资源。
    void terminate(const StreamEvent &event) {
        if (terminal_) {
            return;
        }
        terminal_ = true;
        idleTimer_->stop();
        if (reply_ != nullptr) {
            reply_->disconnect(this);
            reply_->abort();
            reply_->deleteLater();
            reply_ = nullptr;
        }
        deleteLater();
        emitEvent(event);
    }

    QNetworkAccessManager *network_ = nullptr;
    QUrl url_;
    QByteArray payload_;
    QByteArray apiKey_;
    QJsonObject extraHeaders_;

    QNetworkReply *reply_ = nullptr;
    QTimer *idleTimer_ = nullptr;
    SseParser parser_;

    bool terminal_ = false;
    bool abortRequested_ = false;
    bool startedEmitted_ = false;
    bool usageSent_ = false;

    Usage usage_;
    QString finishReason_;
    QHash<int, ToolCallState> toolCalls_;
    QByteArray errorBody_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// OpenAICompatibleProvider
// ─────────────────────────────────────────────────────────────────────────────

OpenAICompatibleProvider::OpenAICompatibleProvider(QObject *parent)
    : ModelProvider(parent), network_(new QNetworkAccessManager(this)) {}

OpenAICompatibleProvider::~OpenAICompatibleProvider() = default;

QString OpenAICompatibleProvider::providerId() const {
    return config_.id;
}

ProviderKind OpenAICompatibleProvider::kind() const {
    return ProviderKind::OpenAICompatible;
}

QStringList OpenAICompatibleProvider::modelIds() const {
    return config_.models;
}

bool OpenAICompatibleProvider::configure(const ProviderConfig &config) {
    config_ = config;
    const QString baseUrl = normalizeBaseUrl(config_.baseUrl);
    if (baseUrl.isEmpty()) {
        qCWarning(log) << "配置缺少 baseUrl; providerId=" << config_.id;
        return false;
    }
    // OpenAI 兼容网关常常内网直连、不需要 key，因此这里只告警不判失败。
    if (config_.apiKey.trimmed().isEmpty()) {
        qCWarning(log) << "配置未提供 apiKey（自建网关可忽略）; providerId=" << config_.id;
    }
    qCInfo(log) << "配置完成; providerId=" << config_.id << "baseUrl=" << baseUrl
                << "apiKey=" << logging::redact(config_.apiKey)
                << "models=" << config_.models.size();
    return true;
}

ProviderConfig OpenAICompatibleProvider::configuration() const {
    return config_;
}

ModelStream *OpenAICompatibleProvider::stream(const ModelRequest &request) {
    const QByteArray payload = json::toBytes(buildRequestBody(request));
    qCInfo(log) << "发起调用; model=" << request.modelId << "messages=" << request.messages.size()
                << "tools=" << request.tools.size() << "reasoning=" << request.enableReasoning;

    auto *stream = new OpenAiStream(network_, QUrl(chatCompletionsEndpoint(config_.baseUrl)),
                                    payload, config_.apiKey.toUtf8(), config_.extraHeaders);
    // 延迟到事件循环再启动，确保调用方先连上信号。
    QTimer::singleShot(0, stream, [stream] { stream->start(); });
    return stream;
}

ModelInfo OpenAICompatibleProvider::modelInfo(const QString &modelId) const {
    ModelInfo info = ModelProvider::modelInfo(modelId);
    const QString lower = modelId.toLower();
    if (lower.contains(QStringLiteral("claude"))) {
        info.contextWindow = 200000;
        info.supportsReasoning = true;
    } else if (lower.contains(QStringLiteral("gpt-4")) || lower.contains(QStringLiteral("o1")) ||
               lower.contains(QStringLiteral("o3"))) {
        info.contextWindow = 128000;
        info.supportsReasoning = true;
    }
    return info;
}

}  // namespace zcode

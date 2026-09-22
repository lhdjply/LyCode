// LyCode — 模型 Provider 抽象
//
// 一次模型调用是"流式"的：provider 在收到 HTTP 分块时就产出增量事件，
// Agent 主循环消费这些事件并实时更新 UI。
//
// 线程模型：所有 provider 都跑在 GUI 线程（QNetworkAccessManager 在 GUI 线程
// 创建），事件通过信号直接投递到主循环。工具执行才是真正的异步密集点，
// 由 QProcess 单独处理。
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include "core/Types.h"

namespace lycode {

/// 暴露给模型的工具声明（JSON Schema 形式）。
struct ToolSpec {
    QString name;
    QString description;
    /// JSON Schema 对象，形如 {"type":"object","properties":{...},"required":[...]}
    QJsonObject inputSchema;
};

/// 一次模型调用的完整请求。
struct ModelRequest {
    QString modelId;
    QString systemPrompt;
    /// 只包含 user / assistant 消息；system 走 systemPrompt 字段。
    ///
    /// ⚠ 工具结果的表示约定（provider 实现必须照此投影）：
    /// 一条 **role == User 且全部 part 都是 Tool 类型** 的消息表示「上一轮工具调用的
    /// 结果」。provider 应把它展开成各自的协议形状：
    ///   - Anthropic：同一 user 消息里的多个 `tool_result` 内容块
    ///   - OpenAI：每个工具结果一条 `{"role":"tool","tool_call_id":...}` 消息
    /// 把工具结果挂成独立的 user 消息（而不是塞回 assistant 消息）的原因：
    /// 两种协议的线格式都是这样要求的，且这样 assistant 消息可以整体作为
    /// 历史保留，便于重放与 cache 前缀稳定。
    QList<Message> messages;
    QList<ToolSpec> tools;

    int maxOutputTokens = 8192;
    double temperature = 1.0;
    /// 是否要求模型输出思考内容（仅部分模型支持）。
    bool enableReasoning = false;
    /// 思考预算 token 数（Anthropic 的 thinking.budget_tokens）。
    int reasoningBudgetTokens = 0;
    /// 思考强度（OpenAI 兼容协议的 reasoning_effort）。空表示不传该字段。
    /// 与 reasoningBudgetTokens 并存：两种协议对"思考强度"的表达方式不同，
    /// 由 Agent 循环按档位同时填好，provider 各取所需。
    QString reasoningEffort;
};

/// 流式事件类型。
enum class StreamEventKind {
    Started,         ///< 已建立连接，模型开始产出
    TextDelta,       ///< 正文增量，累加到 text
    ReasoningDelta,  ///< 思考增量，累加到 text
    ToolCallStart,   ///< 新的工具调用开始，携带 toolCallId 与 toolName
    ToolCallDelta,   ///< 工具入参 JSON 片段，累加到 argumentsDelta
    ToolCallEnd,     ///< 单个工具调用结束，toolInput 已组装完成
    Usage,           ///< token 用量
    Completed,       ///< 本轮正常结束，finishReason 说明原因
    Failed,          ///< 本轮失败，errorMessage 说明原因
};

QString toToken(StreamEventKind kind);

/// 单个流式事件。
///
/// 用一个宽结构而不是多态层次：事件在队列里被大量搬运，
/// 且消费者总是按 kind 分派，宽结构的拷贝成本可忽略而分派最简单。
struct StreamEvent {
    StreamEventKind kind = StreamEventKind::Started;

    /// TextDelta / ReasoningDelta 的增量文本。
    QString text;
    /// ReasoningDelta 的签名（部分 provider 需要回传）。
    QString signature;

    QString toolCallId;
    QString toolName;
    /// ToolCallDelta 的原始 JSON 片段。
    QString argumentsDelta;
    /// ToolCallEnd 时已解析完成的入参。
    QJsonObject toolInput;

    Usage usage;
    /// Completed 的结束原因：end_turn / tool_use / max_tokens / stop_sequence。
    QString finishReason;
    QString errorMessage;

    static StreamEvent started();
    static StreamEvent textDelta(const QString &value);
    static StreamEvent reasoningDelta(const QString &value);
    static StreamEvent completed(const QString &finishReason);
    static StreamEvent failed(const QString &message);
    /// 该事件是否终止本轮流。
    bool isTerminal() const;
};

/// 一次进行中的流式调用。
///
/// 生命周期由调用方持有（Agent 主循环），abort() 后仍会收到 Failed 或
/// Completed 事件，保证恰好一次终止事件。
class ModelStream : public QObject {
    Q_OBJECT

public:
    explicit ModelStream(QObject *parent = nullptr);
    ~ModelStream() override;

    /// 请求取消。幂等；已终止的流调用无副作用。
    virtual void abort() = 0;
    /// 是否已终止（收到过终止事件）。
    bool isFinished() const { return finished_; }

signals:
    void event(const lycode::StreamEvent &streamEvent);
    /// 在最后一个 event 之后恰好发出一次。
    void finished();

protected:
    /// 子类投递事件；终止事件后自动置位 finished_ 并发出 finished()。
    void emitEvent(const StreamEvent &streamEvent);

private:
    bool finished_ = false;
};

/// Provider 抽象。
class ModelProvider : public QObject {
    Q_OBJECT

public:
    explicit ModelProvider(QObject *parent = nullptr);
    ~ModelProvider() override;

    virtual QString providerId() const = 0;
    virtual ProviderKind kind() const = 0;
    /// 该 provider 配置的可用模型 id 列表。
    virtual QStringList modelIds() const = 0;

    /// 发起一次流式调用。调用方负责 delete（或用 deleteLater）。
    /// 实现必须立即返回，不得阻塞 GUI 线程。
    virtual ModelStream *stream(const ModelRequest &request) = 0;

    /// 配置 baseUrl / apiKey 等。返回 false 表示配置不完整。
    virtual bool configure(const ProviderConfig &config) = 0;
    virtual ProviderConfig configuration() const = 0;

    /// 该 provider 的模型元信息；未知模型返回带默认值的结构。
    virtual ModelInfo modelInfo(const QString &modelId) const;
};

}  // namespace lycode

Q_DECLARE_METATYPE(lycode::StreamEvent)
